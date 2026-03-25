#pragma once
// ==============================================================================
// OmegaAdaptiveRisk.hpp
// Adaptive risk layer — closes the gap between Omega and institutional quant systems.
//
// Components:
//   1. SymbolPerformanceTracker  — rolling win-rate, expectancy, Sharpe per symbol
//   2. KellySizer                — Kelly-fraction + vol-normalised lot sizing
//   3. DrawdownThrottle          — progressive size reduction as daily loss grows
//   4. CorrelationHeatGuard      — prevents correlated symbols stacking exposure
//
// Usage in main.cpp:
//   #include "OmegaAdaptiveRisk.hpp"
//   static omega::risk::AdaptiveRiskManager g_adaptive_risk;
//
//   On each closed trade:
//     g_adaptive_risk.record_trade(symbol, net_pnl, hold_sec);
//
//   Before sending an order:
//     double lot = g_adaptive_risk.adjusted_size(symbol, base_lot, daily_pnl_so_far, daily_limit);
//     if (!g_adaptive_risk.corr_heat_ok(symbol)) return;  // skip — too much correlated exposure
// ==============================================================================

#include <string>
#include <deque>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <unordered_set>
#include <functional>
#include <iomanip>
#include <sstream>

namespace omega { namespace risk {

// ─────────────────────────────────────────────────────────────────────────────
// 1. SymbolPerformanceTracker
//    Rolling window of the last N trades per symbol.
//    Computes: win_rate, avg_win, avg_loss, expectancy, Sharpe estimate.
//    Thread-safe via internal mutex.
// ─────────────────────────────────────────────────────────────────────────────
struct SymbolPerformanceTracker {
    // Window sizes
    static constexpr int WINDOW_SHORT = 20;   // recent edge (Kelly uses this)
    static constexpr int WINDOW_LONG  = 100;  // stability check

    struct TradeResult {
        double pnl;       // net PnL in USD
        double hold_sec;  // how long position was open
        int64_t ts;       // unix seconds
    };

    void record(double net_pnl, double hold_sec) {
        std::lock_guard<std::mutex> lk(mtx_);
        const int64_t now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        buf_.push_back({net_pnl, hold_sec, now});
        if ((int)buf_.size() > WINDOW_LONG) buf_.pop_front();
    }

    // Win rate over last N trades (0–1)
    double win_rate(int n = WINDOW_SHORT) const {
        std::lock_guard<std::mutex> lk(mtx_);
        int wins = 0, total = 0;
        for (auto it = buf_.rbegin(); it != buf_.rend() && total < n; ++it, ++total)
            if (it->pnl > 0) ++wins;
        return total > 0 ? (double)wins / total : 0.5;
    }

    // Average win (positive USD)
    double avg_win(int n = WINDOW_SHORT) const {
        std::lock_guard<std::mutex> lk(mtx_);
        double sum = 0; int cnt = 0, total = 0;
        for (auto it = buf_.rbegin(); it != buf_.rend() && total < n; ++it, ++total)
            if (it->pnl > 0) { sum += it->pnl; ++cnt; }
        return cnt > 0 ? sum / cnt : 0.0;
    }

    // Average loss (positive USD — absolute value)
    double avg_loss(int n = WINDOW_SHORT) const {
        std::lock_guard<std::mutex> lk(mtx_);
        double sum = 0; int cnt = 0, total = 0;
        for (auto it = buf_.rbegin(); it != buf_.rend() && total < n; ++it, ++total)
            if (it->pnl <= 0) { sum += std::fabs(it->pnl); ++cnt; }
        return cnt > 0 ? sum / cnt : 1.0;
    }

    // Expectancy per trade in USD
    double expectancy(int n = WINDOW_SHORT) const {
        double wr = win_rate(n);
        double aw = avg_win(n);
        double al = avg_loss(n);
        return (wr * aw) - ((1.0 - wr) * al);
    }

    // Simplified Sharpe estimate: mean(pnl) / stdev(pnl) over window
    // Annualised using average hold_sec → trades_per_year approximation
    double sharpe(int n = WINDOW_SHORT) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if ((int)buf_.size() < 4) return 0.0;
        int use = std::min(n, (int)buf_.size());
        std::vector<double> pnls;
        pnls.reserve(use);
        for (auto it = buf_.rbegin(); it != buf_.rend() && (int)pnls.size() < use; ++it)
            pnls.push_back(it->pnl);

        double mean = 0;
        for (double v : pnls) mean += v;
        mean /= pnls.size();

        double var = 0;
        for (double v : pnls) var += (v - mean) * (v - mean);
        var /= pnls.size();
        const double stdev = std::sqrt(var);
        if (stdev < 1e-9) return 0.0;

        // Annualise: assume ~250 trading days, average trades/day from hold time
        double avg_hold = 0;
        for (auto it = buf_.rbegin(); it != buf_.rend() && (int)pnls.size() > 0; ++it)
            avg_hold += it->hold_sec;
        avg_hold /= pnls.size();
        if (avg_hold < 1.0) avg_hold = 1.0;

        const double trades_per_year = (250.0 * 8 * 3600.0) / avg_hold;
        return (mean / stdev) * std::sqrt(trades_per_year);
    }

    int trade_count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return (int)buf_.size();
    }

    // Persist trade results to CSV — append-only
    // Format: timestamp,pnl,hold_sec
    // Overwrite (not append) — write the current ring buffer as the canonical state.
    // append caused the file to double on every restart, loading 5x data after 5 restarts.
    void save_csv(const std::string& path) const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ofstream f(path, std::ios::trunc);  // overwrite — ring buffer IS the state
        if (!f.is_open()) return;
        for (const auto& r : buf_)
            f << r.ts << "," << r.pnl << "," << r.hold_sec << "\n";
    }

    // Load trade history from CSV on startup.
    // Deduplicates by timestamp — skips records already in buf_ to prevent
    // double-counting if save/load cycle runs multiple times per session.
    void load_csv(const std::string& path) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ifstream f(path);
        if (!f.is_open()) return;
        // Build set of existing timestamps for dedup
        std::unordered_set<int64_t> existing;
        for (const auto& r : buf_) existing.insert(r.ts);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            int64_t ts = 0; double pnl = 0, hold = 0;
            if (sscanf(line.c_str(), "%lld,%lf,%lf",
                       reinterpret_cast<long long*>(&ts), &pnl, &hold) == 3) {
                if (existing.count(ts)) continue;  // skip duplicate
                buf_.push_back({pnl, hold, ts});
                if ((int)buf_.size() > WINDOW_LONG) buf_.pop_front();
            }
        }
    }

    // Confidence: 0 (cold/unreliable) → 1.0 (full window, stable)
    // Used to blend between fixed sizing and Kelly sizing during warmup
    double confidence() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::min(1.0, (double)buf_.size() / WINDOW_SHORT);
    }

private:
    mutable std::mutex      mtx_;
    std::deque<TradeResult> buf_;
};

// ─────────────────────────────────────────────────────────────────────────────
// 2. KellySizer
//    Kelly fraction: f* = (W/L * P - (1-P)) / (W/L)
//    where P = win_rate, W = avg_win, L = avg_loss
//
//    We use FRACTIONAL Kelly (default 25%) to avoid overbetting.
//    Size is further clamped between 50%–150% of the base risk amount.
//    Only applies when we have >= MIN_TRADES samples.
// ─────────────────────────────────────────────────────────────────────────────
struct KellySizer {
    double kelly_fraction = 0.25;   // fractional Kelly (25% of full Kelly)
    double min_scale      = 0.50;   // never size below 50% of base
    double max_scale      = 1.50;   // never size above 150% of base
    int    min_trades     = 15;     // minimum trades before Kelly activates

    // Returns a multiplier to apply to base lot size (0.5 – 1.5)
    // confidence: 0 = use 1.0 (no adjustment), 1.0 = full Kelly
    double size_multiplier(const SymbolPerformanceTracker& tracker) const {
        if (tracker.trade_count() < min_trades) return 1.0;

        const double conf = tracker.confidence();
        const double wr  = tracker.win_rate();
        const double aw  = tracker.avg_win();
        const double al  = tracker.avg_loss();

        if (al < 1e-9) return 1.0;  // no loss data yet

        const double payoff_ratio = aw / al;
        // Full Kelly fraction
        const double full_kelly = (payoff_ratio * wr - (1.0 - wr)) / payoff_ratio;
        const double frac_kelly = full_kelly * kelly_fraction;

        // full_kelly baseline of 1.0 maps to our scale range
        // Normalise: if full_kelly == 0.20 → scale = 1.0 (50/50 with fixed)
        // positive kelly → scale > 1.0, negative kelly → scale < 1.0
        const double raw_scale = 1.0 + (frac_kelly - 0.05) * 4.0; // centre at kelly=0.05
        const double clamped   = std::max(min_scale, std::min(max_scale, raw_scale));

        // Blend: during warmup linearly interpolate toward 1.0
        return 1.0 + conf * (clamped - 1.0);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 3. DrawdownThrottle
//    Progressive size reduction based on current daily PnL vs daily limit.
//    Stages:
//      daily_loss < 40% of limit  → 100% size (normal)
//      daily_loss 40–60%          → 75% size
//      daily_loss 60–80%          → 50% size
//      daily_loss 80–100%         → 25% size
//      daily_loss >= 100%         → 0% (hard stop — handled upstream)
//    Also tracks consecutive loss streak for intra-session throttle.
// ─────────────────────────────────────────────────────────────────────────────
struct DrawdownThrottle {
    // Returns size multiplier (0.25 – 1.0) based on how deep into daily limit
    double size_scale(double daily_loss_usd,   // current session loss (positive = loss)
                      double daily_limit_usd   // daily_loss_limit from config
                      ) const noexcept {
        if (daily_limit_usd <= 0) return 1.0;
        const double used = daily_loss_usd / daily_limit_usd;
        if (used < 0.40) return 1.00;
        if (used < 0.60) return 0.75;
        if (used < 0.80) return 0.50;
        return 0.25;
    }

    // Consecutive-loss streak throttle — independent of daily PnL
    // Reduces size after runs of losses even if total PnL is still small
    double streak_scale(int consec_losses) const noexcept {
        if (consec_losses < 2) return 1.00;
        if (consec_losses < 4) return 0.75;
        if (consec_losses < 6) return 0.50;
        return 0.25;
    }

    // Combined: use the more restrictive of the two
    double combined_scale(double daily_loss_usd, double daily_limit_usd,
                          int consec_losses) const noexcept {
        return std::min(size_scale(daily_loss_usd, daily_limit_usd),
                        streak_scale(consec_losses));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 3b. MultiDayDrawdownThrottle
//     Persistent memory of session-end PnL across UTC days.
//     Written to logs/day_results.csv at rollover; read back on startup.
//     Logic: 3+ consecutive losing days → halve all sizes next session.
//            1 winning day resets the streak.
//
//     File format (append-only, one row per day):
//       unix_day_number,YYYY-MM-DD,net_pnl_usd
//
//     Usage:
//       At startup:   g_adaptive_risk.multiday.load(path);
//       At rollover:  g_adaptive_risk.multiday.record_day(date_str, net_pnl, path);
//       Before entry: double scale = g_adaptive_risk.multiday.size_scale();
// ─────────────────────────────────────────────────────────────────────────────
struct MultiDayDrawdownThrottle {
    int    trigger_days   = 3;     // consecutive losing days before throttle fires
    double throttle_scale = 0.50;  // multiply sizes by this when throttle active

    struct DayRecord {
        int    day_number;   // unix day (time_t / 86400)
        char   date_str[16]; // "YYYY-MM-DD"
        double net_pnl;
    };

    std::deque<DayRecord> history;   // up to last 30 days
    mutable std::mutex    mtx;

    // Load history from CSV on startup. Safe to call if file doesn't exist yet.
    void load(const std::string& path) noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        history.clear();
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            int day_num = 0; char date[16] = {}; double pnl = 0.0;
            if (sscanf(line.c_str(), "%d,%15[^,],%lf", &day_num, date, &pnl) == 3) {
                DayRecord r;
                r.day_number = day_num;
                strncpy(r.date_str, date, 15); r.date_str[15] = '\0';
                r.net_pnl = pnl;
                history.push_back(r);
            }
        }
        // Keep only last 30 days
        while ((int)history.size() > 30) history.pop_front();
        std::printf("[MULTIDAY-THROTTLE] Loaded %d day records from %s\n",
                    (int)history.size(), path.c_str());
    }

    // Record today's session result and append to CSV. Call at UTC rollover.
    void record_day(const std::string& date_str, double net_pnl,
                    const std::string& path) noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        const int64_t t = static_cast<int64_t>(std::time(nullptr));
        const int day_num = static_cast<int>(t / 86400);

        // Avoid duplicate for same day
        if (!history.empty() && history.back().day_number == day_num) {
            history.back().net_pnl = net_pnl; // update in-place
        } else {
            DayRecord r;
            r.day_number = day_num;
            strncpy(r.date_str, date_str.c_str(), 15); r.date_str[15] = '\0';
            r.net_pnl = net_pnl;
            history.push_back(r);
            while ((int)history.size() > 30) history.pop_front();
        }

        // Append to CSV
        std::ofstream f(path, std::ios::app);
        if (f.is_open()) {
            f << day_num << ',' << date_str << ',' << std::fixed
              << std::setprecision(2) << net_pnl << '\n';
            f.flush();
        }

        // Log streak state
        const int streak = consecutive_losing_days_locked();
        std::printf("[MULTIDAY-THROTTLE] Day %s net=$%.2f  consec_loss_streak=%d  scale=%.2f\n",
                    date_str.c_str(), net_pnl, streak,
                    streak >= trigger_days ? throttle_scale : 1.0);
    }

    // How many consecutive losing days are at the tail of history?
    int consecutive_losing_days() const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        return consecutive_losing_days_locked();
    }

    // Returns the size multiplier to apply at the START of a new session.
    // (0.50 when throttle active, 1.0 otherwise)
    double size_scale() const noexcept {
        const int streak = consecutive_losing_days();
        if (streak >= trigger_days) {
            std::printf("[MULTIDAY-THROTTLE] ACTIVE — %d consecutive losing days → scale=%.2f\n",
                        streak, throttle_scale);
            return throttle_scale;
        }
        return 1.0;
    }

    // Is the multi-day throttle currently active?
    bool is_active() const noexcept {
        return consecutive_losing_days() >= trigger_days;
    }

private:
    // Called with lock already held
    int consecutive_losing_days_locked() const noexcept {
        int streak = 0;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->net_pnl < 0.0) ++streak;
            else break;
        }
        return streak;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 4. CorrelationHeatGuard
//    Prevents stacking highly correlated positions simultaneously.
//    Symbols are grouped by asset class / correlation cluster.
//    Max concurrent positions per cluster is enforced.
//
//    Clusters:
//      US_EQUITY  : US500, USTEC, DJ30, NAS100  — near-perfect correlation
//      EU_EQUITY  : GER40, UK100, ESTX50         — high correlation
//      OIL        : USOIL.F, BRENT               — tight spread
//      METALS     : GOLD.F, XAGUSD               — correlated but different vol
//      JPY_RISK   : USDJPY, AUDUSD, NZDUSD       — carry/risk-off cluster
//      EUR_GBP    : EURUSD, GBPUSD               — G10 major correlation
//
//    Default max open per cluster = 2 (allows diversification within cluster
//    but prevents all 4 US equity engines firing simultaneously).
// ─────────────────────────────────────────────────────────────────────────────
enum class CorrCluster {
    US_EQUITY,   // US500 USTEC DJ30 NAS100
    EU_EQUITY,   // GER40 UK100 ESTX50
    OIL,         // USOIL.F BRENT
    METALS,      // GOLD.F XAGUSD
    JPY_RISK,    // USDJPY AUDUSD NZDUSD
    EUR_GBP,     // EURUSD GBPUSD
    STANDALONE,  // no correlation cluster (treat independently)
};

static inline CorrCluster symbol_to_cluster(const std::string& sym) {
    if (sym == "US500.F" || sym == "USTEC.F" || sym == "DJ30.F"  || sym == "NAS100")
        return CorrCluster::US_EQUITY;
    if (sym == "GER40"   || sym == "UK100"   || sym == "ESTX50")
        return CorrCluster::EU_EQUITY;
    if (sym == "USOIL.F" || sym == "BRENT")
        return CorrCluster::OIL;
    if (sym == "GOLD.F"  || sym == "XAGUSD")
        return CorrCluster::METALS;
    if (sym == "USDJPY"  || sym == "AUDUSD"  || sym == "NZDUSD")
        return CorrCluster::JPY_RISK;
    if (sym == "EURUSD"  || sym == "GBPUSD")
        return CorrCluster::EUR_GBP;
    return CorrCluster::STANDALONE;
}

struct CorrelationHeatGuard {
    // Max concurrent positions per correlation cluster
    int max_per_cluster_us_equity = 2;
    int max_per_cluster_eu_equity = 2;
    int max_per_cluster_oil       = 1;
    int max_per_cluster_metals    = 2;
    int max_per_cluster_jpy_risk  = 2;
    int max_per_cluster_eur_gbp   = 1;

    // Open positions per cluster (set externally each tick from actual engine states)
    std::unordered_map<int, int> cluster_open;

    // Call this once per tick with total open positions per cluster
    void update(CorrCluster cluster, int open_count) {
        cluster_open[static_cast<int>(cluster)] = open_count;
    }

    // Returns true if a new position in this symbol is allowed within correlation budget
    bool ok(const std::string& symbol) const {
        const CorrCluster c = symbol_to_cluster(symbol);
        if (c == CorrCluster::STANDALONE) return true;

        int max_allowed = 2;
        switch (c) {
            case CorrCluster::US_EQUITY: max_allowed = max_per_cluster_us_equity; break;
            case CorrCluster::EU_EQUITY: max_allowed = max_per_cluster_eu_equity; break;
            case CorrCluster::OIL:       max_allowed = max_per_cluster_oil;       break;
            case CorrCluster::METALS:    max_allowed = max_per_cluster_metals;    break;
            case CorrCluster::JPY_RISK:  max_allowed = max_per_cluster_jpy_risk;  break;
            case CorrCluster::EUR_GBP:   max_allowed = max_per_cluster_eur_gbp;   break;
            default: break;
        }

        auto it = cluster_open.find(static_cast<int>(c));
        const int open = (it != cluster_open.end()) ? it->second : 0;
        return open < max_allowed;
    }

    // For logging — returns cluster name
    static const char* cluster_name(const std::string& symbol) {
        switch (symbol_to_cluster(symbol)) {
            case CorrCluster::US_EQUITY: return "US_EQUITY";
            case CorrCluster::EU_EQUITY: return "EU_EQUITY";
            case CorrCluster::OIL:       return "OIL";
            case CorrCluster::METALS:    return "METALS";
            case CorrCluster::JPY_RISK:  return "JPY_RISK";
            case CorrCluster::EUR_GBP:   return "EUR_GBP";
            default:                     return "STANDALONE";
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 5. VolatilityRegimeScaler
//    Computes a 20-period ATR per symbol from tick stream.
//    When ATR is elevated vs its own 100-period average → reduce size.
//    When ATR is compressed → normal size (breakout premium).
//
//    This is separate from ATR in BracketEngine (which gates minimum range).
//    This one gates LOT SIZE — high vol = same dollar risk but smaller position.
// ─────────────────────────────────────────────────────────────────────────────
struct VolatilityRegimeScaler {
    static constexpr int ATR_FAST   = 20;
    static constexpr int ATR_SLOW   = 100;

    double high_vol_scale  = 0.70;  // size × 0.70 when ATR > 1.5× baseline
    double normal_scale    = 1.00;
    double low_vol_scale   = 1.10;  // slight size boost when ATR < 0.7× baseline (compressed)

    struct SymATR {
        std::deque<double> ranges;  // |high - low| per tick or |tick_delta|
        double atr_fast = 0;        // EWM fast
        double atr_slow = 0;        // EWM slow
        bool   ready    = false;
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, SymATR> state_;

    void update(const std::string& sym, double mid_price_change_abs) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = state_[sym];
        const double alpha_fast = 2.0 / (ATR_FAST + 1);
        const double alpha_slow = 2.0 / (ATR_SLOW + 1);
        if (s.atr_fast == 0) { s.atr_fast = mid_price_change_abs; s.atr_slow = mid_price_change_abs; }
        else {
            s.atr_fast = alpha_fast * mid_price_change_abs + (1 - alpha_fast) * s.atr_fast;
            s.atr_slow = alpha_slow * mid_price_change_abs + (1 - alpha_slow) * s.atr_slow;
        }
        s.ranges.push_back(mid_price_change_abs);
        if ((int)s.ranges.size() > ATR_SLOW) s.ranges.pop_front();
        s.ready = ((int)s.ranges.size() >= ATR_FAST);
    }

    double size_scale(const std::string& sym) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        if (it == state_.end() || !it->second.ready) return 1.0;
        const auto& s = it->second;
        if (s.atr_slow < 1e-12) return 1.0;
        const double ratio = s.atr_fast / s.atr_slow;
        if (ratio > 1.50) return high_vol_scale;
        if (ratio < 0.70) return low_vol_scale;
        return normal_scale;
    }

    double atr_fast(const std::string& sym) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        return (it != state_.end()) ? it->second.atr_fast : 0.0;
    }

    double atr_slow(const std::string& sym) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        return (it != state_.end()) ? it->second.atr_slow : 0.0;
    }

    // ATR-normalised SL floor.
    // Problem: when compression range is very tight (CRUSH regime),
    // sl_dist = comp_range × 0.4 is tiny → lot size balloons dangerously.
    // A $50 risk with sl_abs = 0.5pts on GOLD = 1.0 lot (should be ~0.05).
    //
    // Solution: never size from an SL smaller than ATR_SLOW × atr_sl_mult.
    // Default atr_sl_mult = 0.5: SL floor = half of the slow ATR baseline.
    // This is the same principle used by top prop firms (Tower, Virtu, DRW):
    //   "size to the market's natural tick noise, not to the setup width"
    //
    // When ATR is not warmed up (< ATR_FAST samples), returns sl_abs unchanged.
    double atr_sl_floor(const std::string& sym, double sl_abs,
                        double atr_sl_mult = 0.5) const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        if (it == state_.end() || !it->second.ready) return sl_abs;
        const double floor_sl = it->second.atr_slow * atr_sl_mult;
        if (floor_sl <= 0.0) return sl_abs;
        return std::max(sl_abs, floor_sl);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AdaptiveRiskManager — unified facade used by main.cpp
// ─────────────────────────────────────────────────────────────────────────────
class AdaptiveRiskManager {
public:
    // ── Config (set once at startup or from config file) ─────────────────────
    bool   kelly_enabled          = true;
    bool   dd_throttle_enabled    = true;
    bool   corr_heat_enabled      = true;
    bool   vol_regime_enabled     = true;
    bool   multiday_throttle_enabled = true;
    bool   fill_quality_enabled   = true;

    // Fill quality size reduction when adverse selection detected (default 70%)
    double fill_quality_scale     = 0.70;
    // Minimum fills before fill-quality reduction fires (mirrors FillQualityTracker::window)
    int    fill_quality_min_fills = 10;

    // Per-symbol performance trackers — keyed by canonical symbol name
    std::unordered_map<std::string, SymbolPerformanceTracker> perf;

    KellySizer                  kelly;
    DrawdownThrottle            dd_throttle;
    MultiDayDrawdownThrottle    multiday;
    CorrelationHeatGuard        corr_heat;
    VolatilityRegimeScaler      vol_scaler;

    // Non-owning pointer to OmegaEdges::FillQualityTracker set by main.cpp at startup.
    // Declared as void* to avoid circular include; cast on use via a template accessor.
    // In practice main.cpp calls set_fill_quality_tracker(&g_edges.fill_quality).
    void* fill_quality_tracker_ptr = nullptr;

    // Setter called by main.cpp — pass &g_edges.fill_quality
    template<typename T>
    void set_fill_quality_tracker(T* ptr) { fill_quality_tracker_ptr = static_cast<void*>(ptr); }

    // ── Record a closed trade ─────────────────────────────────────────────────
    void record_trade(const std::string& symbol, double net_pnl, double hold_sec) {
        perf[symbol].record(net_pnl, hold_sec);
    }

    // ── Update volatility state (call each tick per symbol with |price_delta|) ─
    void update_vol(const std::string& symbol, double abs_price_delta) {
        if (vol_regime_enabled)
            vol_scaler.update(symbol, abs_price_delta);
    }

    // ── Update correlation cluster counts (call once per on_tick) ────────────
    void update_cluster_counts(
        int us_equity_open, int eu_equity_open, int oil_open,
        int metals_open, int jpy_risk_open, int eur_gbp_open) {
        if (!corr_heat_enabled) return;
        corr_heat.update(CorrCluster::US_EQUITY, us_equity_open);
        corr_heat.update(CorrCluster::EU_EQUITY, eu_equity_open);
        corr_heat.update(CorrCluster::OIL,       oil_open);
        corr_heat.update(CorrCluster::METALS,    metals_open);
        corr_heat.update(CorrCluster::JPY_RISK,  jpy_risk_open);
        corr_heat.update(CorrCluster::EUR_GBP,   eur_gbp_open);
    }

    // ── Main entry point: compute adjusted lot size ───────────────────────────
    // base_lot          : raw lot size from compute_size()
    // daily_loss_usd    : current session loss (positive = loss amount)
    // daily_limit_usd   : config daily_loss_limit
    // consec_losses     : from SymbolRiskState
    double adjusted_lot(const std::string& symbol,
                        double base_lot,
                        double daily_loss_usd,   // positive = we are in drawdown
                        double daily_limit_usd,
                        int    consec_losses) {
        double lot = base_lot;

        // 1. Kelly scaling
        if (kelly_enabled) {
            auto it = perf.find(symbol);
            if (it != perf.end()) {
                const double kelly_mult = kelly.size_multiplier(it->second);
                lot *= kelly_mult;
            }
        }

        // 2. Volatility regime scaling
        if (vol_regime_enabled) {
            lot *= vol_scaler.size_scale(symbol);
        }

        // 3. Intra-session drawdown throttle
        if (dd_throttle_enabled) {
            const double dd_mult = dd_throttle.combined_scale(
                daily_loss_usd, daily_limit_usd, consec_losses);
            if (dd_mult < 1.0) {
                std::printf("[ADAPTIVE-RISK] %s dd_throttle=%.2f (loss=$%.0f/%.0f, streak=%d)\n",
                            symbol.c_str(), dd_mult, daily_loss_usd, daily_limit_usd, consec_losses);
                lot *= dd_mult;
            }
        }

        // 4. Multi-day drawdown throttle (applied on top of intra-session)
        if (multiday_throttle_enabled) {
            const double md_mult = multiday.size_scale();
            if (md_mult < 1.0) {
                std::printf("[ADAPTIVE-RISK] %s multiday_throttle=%.2f (consec_loss_days=%d)\n",
                            symbol.c_str(), md_mult, multiday.consecutive_losing_days());
                lot *= md_mult;
            }
        }

        // 5. Fill quality adverse selection reduction
        // Wired via fill_quality_tracker_ptr to avoid circular include.
        // The FillQualityTracker interface is duck-typed via a small inline lambda.
        if (fill_quality_enabled && fill_quality_tracker_ptr) {
            // We call adverse_selection_detected() via a function pointer trick.
            // To avoid including OmegaEdges.hpp here, we use a registered callback.
            if (fill_quality_check_fn && fill_quality_check_fn(symbol)) {
                std::printf("[ADAPTIVE-RISK] %s fill_quality_reduction=%.2f (adverse selection)\n",
                            symbol.c_str(), fill_quality_scale);
                lot *= fill_quality_scale;
            }
        }

        // Floor to 0.01 lots, round to 2dp
        lot = std::max(0.01, std::floor(lot * 100.0 + 0.5) / 100.0);
        return lot;
    }

    // Callback registered by main.cpp: returns true if adverse selection detected
    // for this symbol. Avoids circular include between AdaptiveRisk and OmegaEdges.
    std::function<bool(const std::string&)> fill_quality_check_fn;

    // ── Correlation heat check ─────────────────────────────────────────────────
    // Returns false if opening a new position in this symbol would exceed
    // the correlation cluster budget. Call before every entry.
    bool corr_heat_ok(const std::string& symbol) const {
        if (!corr_heat_enabled) return true;
        const bool ok = corr_heat.ok(symbol);
        if (!ok) {
            std::printf("[ADAPTIVE-RISK] %s BLOCKED by corr-heat cluster=%s\n",
                        symbol.c_str(), CorrelationHeatGuard::cluster_name(symbol));
        }
        return ok;
    }

    // ── Persist/restore Kelly performance data ─────────────────────────────
    // Called at startup (load) and UTC rollover (save) alongside TOD gate.
    void save_perf(const std::string& dir) const {
        for (const auto& kv : perf) {
            std::string safe_sym = kv.first;
            for (char& c : safe_sym) if (c == '.' || c == '/') c = '_';
            {
                std::string p = dir;
                p += (p.back() == '/' || p.back() == '\\') ? "" : "/";
                p += safe_sym + ".csv";
                kv.second.save_csv(p);
            }
        }
    }
    void load_perf(const std::string& dir) noexcept {
        static const char* SYMS[] = {
            "GOLD.F","XAGUSD","US500.F","USTEC.F","DJ30.F","NAS100",
            "GER40","UK100","ESTX50","USOIL.F","BRENT",
            "EURUSD","GBPUSD","AUDUSD","NZDUSD","USDJPY", nullptr
        };
        for (int i = 0; SYMS[i]; ++i) {
            std::string safe = SYMS[i];
            for (char& c : safe) if (c == '.' || c == '/') c = '_';
            {
                std::string p = dir;
                p += (p.back() == '/' || p.back() == '\\') ? "" : "/";
                p += safe + ".csv";
                perf[SYMS[i]].load_csv(p);
            }
        }
        std::printf("[ADAPTIVE-RISK] Kelly perf loaded from %s\n", dir.c_str());
    }

    // ── Performance summary (for GUI / logging) ───────────────────────────────
    void print_summary() const {
        for (const auto& kv : perf) {
            const auto& sym = kv.first;
            const auto& t   = kv.second;
            if (t.trade_count() < 3) continue;
            std::printf("[ADAPTIVE-RISK] %s  n=%d  wr=%.1f%%  exp=$%.2f  sharpe=%.2f  kelly_mult=%.2f  vol_scale=%.2f\n",
                        sym.c_str(),
                        t.trade_count(),
                        t.win_rate() * 100.0,
                        t.expectancy(),
                        t.sharpe(),
                        kelly.size_multiplier(t),
                        const_cast<VolatilityRegimeScaler&>(vol_scaler).size_scale(sym));
        }
    }
};

}} // namespace omega::risk
