#pragma once
// =============================================================================
// OmegaEdges.hpp — Seven institutional-grade trading edges
//
//  1. CumulativeVolumeDelta (CVD)
//     Tick-classified buy/sell volume running total. Divergence between price
//     and CVD reveals institutional flow direction. Stronger signal than L2
//     imbalance (which shows intent) because CVD shows actual execution.
//
//  2. TimeOfDayGate
//     Win-rate and EV tracking by 30-minute UTC bucket from live trades.
//     Auto-blocks engines during demonstrably-negative windows after warmup.
//
//  3. RelativeSpreadGate
//     Z-score of current spread vs 200-tick rolling median per symbol.
//     Replaces fixed max_spread_pct — detects anomalous liquidity conditions
//     regardless of absolute spread level.
//
//  4. RoundNumberFilter
//     Detects proximity to psychological price levels ($X00, $X50, $X25, $X75).
//     Breakout targets hitting a round number have higher TP probability;
//     breakout origins AT a round number are likely to be defended/rejected.
//
//  5. PreviousDayLevels
//     Tracks prior-session high, low, and VWAP per symbol (rolling UTC-day).
//     Compression breakouts near PDH/PDL have higher continuation probability.
//
//  6. FxFixWindowEngine
//     WM/Reuters 4pm London fix (21:00 UTC) and Tokyo fix (00:55 UTC).
//     Mechanical directional bias from known institutional rebalancing flows.
//     USDJPY 00:50-01:05 UTC and major FX pairs at 20:55-21:10 UTC.
//
//  7. FillQualityTracker
//     Tracks fill price vs signal mid for every entry.
//     Detects adverse selection (consistently filling worse than mid) which
//     indicates front-running or execution leakage.
//
// Usage: #include "OmegaEdges.hpp"
//        static omega::edges::EdgeContext g_edges;
//        // each tick: g_edges.cvd.update(sym, bid, ask, prev_bid, prev_ask, mid);
//        // entry:     if (!g_edges.tod.allow(sym, now_utc_hour, now_utc_min)) skip;
//        // entry:     if (!g_edges.spread.ok(sym, ask-bid)) skip;
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omega { namespace edges {

// ─────────────────────────────────────────────────────────────────────────────
// 1. CumulativeVolumeDelta (CVD)
// ─────────────────────────────────────────────────────────────────────────────
// Classifies each tick as buyer-initiated (price >= prev_ask) or
// seller-initiated (price <= prev_bid). Accumulates a running signed total.
//
// CVD divergence signal:
//   Price makes new high, CVD does NOT make new high → distribution → SHORT
//   Price makes new low,  CVD does NOT make new low  → absorption  → LONG
//
// Per-symbol. Thread-safe via mutex. Resets at UTC day rollover.
// ─────────────────────────────────────────────────────────────────────────────
struct CVDState {
    double  cvd          = 0.0;   // running cumulative delta
    double  session_high = 0.0;   // highest CVD this session
    double  session_low  = 0.0;   // lowest CVD this session
    double  price_high   = 0.0;   // price high since last reset
    double  price_low    = 1e18;  // price low since last reset

    // Rolling buffers for divergence detection
    static constexpr int WINDOW = 50;
    std::deque<double> cvd_window;    // last N CVD values
    std::deque<double> price_window;  // last N mid prices
    double prev_bid = 0.0;
    double prev_ask = 0.0;

    // Update on every tick
    // tick_size: minimum price increment for lot estimation (e.g. 0.10 for gold)
    void update(double bid, double ask, double tick_size = 0.10) noexcept {
        if (bid <= 0 || ask <= 0) return;
        const double mid = (bid + ask) * 0.5;

        // Tick classification (Lee-Ready heuristic):
        //   mid >= prev_ask → buyer initiated (lifted offer)
        //   mid <= prev_bid → seller initiated (hit bid)
        //   otherwise       → use midpoint direction (quote rule)
        double delta = 0.0;
        if (prev_bid > 0 && prev_ask > 0) {
            // Estimate trade size from spread/tick relationship (unit = 1 lot equivalent)
            const double spread   = ask - bid;
            const double vol_est  = (tick_size > 0 && spread > 0)
                ? std::max(0.01, tick_size / spread) : 1.0;

            if (mid >= prev_ask)                      delta = +vol_est;   // buyer
            else if (mid <= prev_bid)                 delta = -vol_est;   // seller
            else if (mid > (prev_bid + prev_ask)*0.5) delta = +vol_est*0.5; // quote rule up
            else                                      delta = -vol_est*0.5; // quote rule down
        }

        cvd += delta;
        if (cvd > session_high) session_high = cvd;
        if (cvd < session_low)  session_low  = cvd;
        if (mid > price_high)   price_high   = mid;
        if (mid < price_low)    price_low    = mid;

        cvd_window.push_back(cvd);
        price_window.push_back(mid);
        if ((int)cvd_window.size()   > WINDOW) cvd_window.pop_front();
        if ((int)price_window.size() > WINDOW) price_window.pop_front();

        prev_bid = bid;
        prev_ask = ask;
    }

    // Bearish divergence: price made new high but CVD did not
    // → institutions are distributing into the rally
    bool bearish_divergence() const noexcept {
        if ((int)price_window.size() < WINDOW) return false;
        const double p_now  = price_window.back();
        const double p_old  = price_window.front();
        const double c_now  = cvd_window.back();
        const double c_old  = cvd_window.front();
        const bool price_up = p_now > p_old * 1.0002;   // price made progress
        const bool cvd_down = c_now < c_old;             // CVD diverging down
        return price_up && cvd_down;
    }

    // Bullish divergence: price made new low but CVD did not
    // → institutions are absorbing selling
    bool bullish_divergence() const noexcept {
        if ((int)price_window.size() < WINDOW) return false;
        const double p_now  = price_window.back();
        const double p_old  = price_window.front();
        const double c_now  = cvd_window.back();
        const double c_old  = cvd_window.front();
        const bool price_dn = p_now < p_old * 0.9998;   // price fell
        const bool cvd_up   = c_now > c_old;             // CVD diverging up
        return price_dn && cvd_up;
    }

    // Normalised CVD: cvd / (session_high - session_low), range -1..+1
    // Tells you where CVD is relative to its own session range
    double normalised() const noexcept {
        const double range = session_high - session_low;
        if (range < 1e-9) return 0.0;
        return (cvd - (session_high + session_low) * 0.5) / (range * 0.5);
    }

    // Direction: +1 = buying dominates last WINDOW ticks, -1 = selling, 0 = neutral
    int direction() const noexcept {
        if ((int)cvd_window.size() < 10) return 0;
        const double c_now = cvd_window.back();
        const double c_old = cvd_window[cvd_window.size() - 10];
        if (c_now > c_old + 0.05) return  1;
        if (c_now < c_old - 0.05) return -1;
        return 0;
    }

    void reset_session() noexcept {
        cvd = 0; session_high = 0; session_low = 0;
        price_high = 0; price_low = 1e18;
        cvd_window.clear(); price_window.clear();
        prev_bid = prev_ask = 0;
    }
};

class CumulativeVolumeDelta {
public:
    mutable std::mutex mtx;

    // tick_sizes: per-symbol minimum tick (gold=0.10, silver=0.001, indices=0.25, fx=0.00001)
    std::unordered_map<std::string, double> tick_sizes = {
        {"GOLD.F",  0.10}, {"XAGUSD",  0.001}, {"USOIL.F", 0.01}, {"BRENT",   0.01},
        {"US500.F", 0.25}, {"USTEC.F", 0.25},  {"DJ30.F",  1.0},  {"NAS100",  0.25},
        {"GER40",   0.5},  {"UK100",   0.5},   {"ESTX50",  0.5},
        {"EURUSD",  0.00001},{"GBPUSD", 0.00001},{"AUDUSD", 0.00001},
        {"NZDUSD",  0.00001},{"USDJPY", 0.001},
    };

    void update(const std::string& sym, double bid, double ask) noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        double ts = 0.10;
        auto it = tick_sizes.find(sym);
        if (it != tick_sizes.end()) ts = it->second;
        state_[sym].update(bid, ask, ts);
    }

    // Returns snapshot (copy) — safe to read outside lock after update()
    CVDState get(const std::string& sym) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = state_.find(sym);
        if (it == state_.end()) return {};
        return it->second;
    }

    void reset_all() noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& kv : state_) kv.second.reset_session();
    }

private:
    std::unordered_map<std::string, CVDState> state_;
};


// ─────────────────────────────────────────────────────────────────────────────
// 2. TimeOfDayGate
// ─────────────────────────────────────────────────────────────────────────────
// Records each trade outcome per 30-minute UTC bucket per symbol/engine pair.
// After MIN_TRADES samples, blocks entry during buckets where win-rate is
// below threshold OR EV (avg net P&L per trade) is negative.
//
// This is the "adaptive session filter" used by firms like Virtu and Optiver —
// they measure edge empirically and shut off engines during proven dead windows.
// ─────────────────────────────────────────────────────────────────────────────
struct TODBucket {
    int     trades    = 0;
    int     wins      = 0;
    double  total_pnl = 0.0;  // sum of net_pnl
    double  win_rate() const { return trades > 0 ? (double)wins / trades : 0.5; }
    double  avg_ev()   const { return trades > 0 ? total_pnl / trades    : 0.0; }
};

class TimeOfDayGate {
public:
    int    min_trades     = 30;    // minimum samples before blocking
    double min_win_rate   = 0.40;  // block if WR < 40% (after min_trades)
    double min_avg_ev_usd = -1.50; // block if avg net P&L < -$1.50/trade
    bool   enabled        = true;

    // key = sym + ":" + engine + ":" + bucket (0..47 = 30-min UTC slots)
    std::unordered_map<std::string, TODBucket> buckets;
    mutable std::mutex mtx;

    // Record trade outcome — call from handle_closed_trade
    void record(const std::string& sym, const std::string& engine,
                int64_t entry_ts_sec, double net_pnl) noexcept {
        const int bucket = utc_bucket(entry_ts_sec);
        const std::string key = sym + ":" + engine + ":" + std::to_string(bucket);
        std::lock_guard<std::mutex> lk(mtx);
        auto& b = buckets[key];
        ++b.trades;
        if (net_pnl > 0) ++b.wins;
        b.total_pnl += net_pnl;
    }

    // Check if entry is allowed — returns false if bucket is known-negative
    bool allow(const std::string& sym, const std::string& engine,
               int64_t now_sec) const noexcept {
        if (!enabled) return true;
        const int bucket = utc_bucket(now_sec);
        const std::string key = sym + ":" + engine + ":" + std::to_string(bucket);
        std::lock_guard<std::mutex> lk(mtx);
        auto it = buckets.find(key);
        if (it == buckets.end()) return true;  // no data yet = allow
        const auto& b = it->second;
        if (b.trades < min_trades) return true; // not enough data
        if (b.win_rate() < min_win_rate)   return false;
        if (b.avg_ev()   < min_avg_ev_usd) return false;
        return true;
    }

    // Print worst buckets for diagnostics
    void print_worst(int top_n = 10) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<std::pair<std::string, const TODBucket*>> sorted;
        for (const auto& kv : buckets)
            if (kv.second.trades >= min_trades)
                sorted.push_back({kv.first, &kv.second});
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b){ return a.second->avg_ev() < b.second->avg_ev(); });
        std::printf("[TOD-GATE] Worst %d buckets (min_trades=%d):\n", top_n, min_trades);
        for (int i = 0; i < std::min((int)sorted.size(), top_n); ++i) {
            const auto& [k, b] = sorted[i];
            std::printf("  %-40s  trades=%3d  wr=%.1f%%  avg_ev=$%.2f  %s\n",
                k.c_str(), b->trades, b->win_rate()*100, b->avg_ev(),
                b->avg_ev() < min_avg_ev_usd ? "BLOCKED" : "ok");
        }
    }

    // Save/load from CSV for persistence across restarts
    void save_csv(const std::string& path) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        fprintf(f, "key,trades,wins,total_pnl\n");
        for (const auto& kv : buckets)
            fprintf(f, "%s,%d,%d,%.4f\n",
                kv.first.c_str(), kv.second.trades, kv.second.wins, kv.second.total_pnl);
        fclose(f);
    }

    void load_csv(const std::string& path) noexcept {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return;
        char line[256];
        fgets(line, sizeof(line), f); // skip header
        while (fgets(line, sizeof(line), f)) {
            char key[128]; int trades, wins; double pnl;
            if (sscanf(line, "%127[^,],%d,%d,%lf", key, &trades, &wins, &pnl) == 4) {
                std::lock_guard<std::mutex> lk(mtx);
                auto& b = buckets[key];
                b.trades += trades; b.wins += wins; b.total_pnl += pnl;
            }
        }
        fclose(f);
        std::printf("[TOD-GATE] Loaded %zu buckets from %s\n", buckets.size(), path.c_str());
    }

    // TOD-based lot size multiplier.
    // Instead of binary block/allow, returns a continuous scale based on
    // the bucket's historical win rate and EV vs the target thresholds.
    //
    // Scale mapping (applied AFTER allow() returns true — never blocks):
    //   WR >= 0.60 AND EV > 0         → 1.00 (full size — strong bucket)
    //   WR >= 0.55 AND EV > 0         → 0.90
    //   WR >= 0.50 AND EV > -0.50     → 0.75
    //   WR >= min_win_rate (0.40)      → 0.60 (marginal — reduce but allow)
    //   < min_trades (cold bucket)    → 1.00 (no data = no penalty)
    double size_scale(const std::string& sym, const std::string& engine,
                      int64_t now_sec) const noexcept {
        if (!enabled) return 1.0;
        const int bucket = utc_bucket(now_sec);
        const std::string key = sym + ":" + engine + ":" + std::to_string(bucket);
        std::lock_guard<std::mutex> lk(mtx);
        auto it = buckets.find(key);
        if (it == buckets.end()) return 1.0;
        const auto& b = it->second;
        if (b.trades < min_trades) return 1.0;  // cold bucket — no penalty
        const double wr = b.win_rate();
        const double ev = b.avg_ev();
        if (wr >= 0.60 && ev > 0.0)   return 1.00;
        if (wr >= 0.55 && ev > 0.0)   return 0.90;
        if (wr >= 0.50 && ev > -0.50) return 0.75;
        return 0.60;  // marginal bucket — allow but reduce
    }

private:
    // 30-minute UTC bucket index (0=00:00-00:30, 1=00:30-01:00, ... 47=23:30-24:00)
    static int utc_bucket(int64_t ts_sec) noexcept {
        const int mins = static_cast<int>((ts_sec % 86400) / 60);
        return mins / 30;
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// 3. RelativeSpreadGate
// ─────────────────────────────────────────────────────────────────────────────
// Tracks rolling 200-tick median spread per symbol.
// Blocks entry when current spread is Z standard deviations above median.
// This replaces fixed max_spread_pct thresholds.
//
// A $2.00 spread on gold in London = normal (Z≈0). The same spread at 05:30
// UTC = Z≈3.5 = anomalous = likely a liquidity gap or quote stale.
// ─────────────────────────────────────────────────────────────────────────────
class RelativeSpreadGate {
public:
    int    window_ticks  = 200;
    double max_z_score   = 2.5;   // block if spread > median + 2.5σ
    double min_median    = 0.0;   // don't block if median itself is tiny (warmup)
    bool   enabled       = true;

    mutable std::mutex mtx;

    struct SpreadState {
        std::deque<double> history;
        double median  = 0.0;
        double std_dev = 0.0;
        bool   ready   = false;
    };

    void update(const std::string& sym, double spread) noexcept {
        if (spread <= 0) return;
        std::lock_guard<std::mutex> lk(mtx);
        auto& s = state_[sym];
        s.history.push_back(spread);
        if ((int)s.history.size() > window_ticks) s.history.pop_front();
        // FIX: require minimum 50 ticks before gate activates.
        // With only 20 ticks, a single wide startup spread dominates std_dev
        // and makes every subsequent normal spread look like a 5σ anomaly.
        // Log showed: USOIL spread=0.08 z=5.48 on tick ~21 — false positive.
        // 50 ticks = enough to build a stable median and std_dev.
        if ((int)s.history.size() >= 50) {
            // Compute median
            std::vector<double> sorted(s.history.begin(), s.history.end());
            std::sort(sorted.begin(), sorted.end());
            const int n = (int)sorted.size();
            s.median = (n % 2 == 0)
                ? (sorted[n/2-1] + sorted[n/2]) * 0.5
                : sorted[n/2];
            // Compute std dev
            double sq = 0.0;
            for (double v : sorted) sq += (v - s.median) * (v - s.median);
            s.std_dev = std::sqrt(sq / n);
            s.ready = true;
        }
    }

    // Returns true if current spread is within normal range
    bool ok(const std::string& sym, double current_spread) const noexcept {
        if (!enabled) return true;
        std::lock_guard<std::mutex> lk(mtx);
        auto it = state_.find(sym);
        if (it == state_.end() || !it->second.ready) return true;
        const auto& s = it->second;
        if (s.median < min_median) return true; // not yet liquid enough to judge
        if (s.std_dev < 1e-9)     return true; // degenerate
        const double z = (current_spread - s.median) / s.std_dev;
        return z <= max_z_score;
    }

    // Z-score of current spread (useful for logging)
    double z_score(const std::string& sym, double current_spread) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = state_.find(sym);
        if (it == state_.end() || !it->second.ready) return 0.0;
        const auto& s = it->second;
        if (s.std_dev < 1e-9) return 0.0;
        return (current_spread - s.median) / s.std_dev;
    }

    double median(const std::string& sym) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = state_.find(sym);
        return (it != state_.end()) ? it->second.median : 0.0;
    }

    // Reset all spread history — call on reconnect so stale pre-reconnect
    // medians (computed during low-liquidity period) don't block the first
    // legitimate entries on a fresh connection. History rebuilds in 20 ticks.
    void reset_all() noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        state_.clear();
    }

private:
    mutable std::unordered_map<std::string, SpreadState> state_;
};


// ─────────────────────────────────────────────────────────────────────────────
// 4. RoundNumberFilter
// ─────────────────────────────────────────────────────────────────────────────
// Detects proximity to psychological price levels.
// Dealers defend big figures; order clusters at round numbers create
// absorption zones that either stop breakouts or become powerful confluences.
//
// For gold at $2000: big figure = $2000, half = $1950, quarters = $1925/$1975
// For FX EURUSD at 1.0850: big figure = 1.0800, half = 1.0850, etc.
//
// Usage patterns:
//   - Breakout TARGET hits a round number → higher TP hit probability
//   - Breakout ORIGIN is at a round number → likely defended, lower probability
//   - Bracket arm level is just below a round number → stop-hunt risk
// ─────────────────────────────────────────────────────────────────────────────
class RoundNumberFilter {
public:
    // Per-symbol round number granularity (price units)
    // Gold: every $50 is a big figure ($2000, $2050 etc), $25 is a half
    // FX: every 100 pips is a big figure
    std::unordered_map<std::string, double> round_increments = {
        {"GOLD.F",  50.0},   {"XAGUSD",   0.50},  {"USOIL.F",  1.0},  {"BRENT",   1.0},
        {"US500.F", 100.0},  {"USTEC.F",  500.0}, {"DJ30.F",   500.0},{"NAS100",  500.0},
        {"GER40",   100.0},  {"UK100",    100.0}, {"ESTX50",   50.0},
        {"EURUSD",  0.0100}, {"GBPUSD",   0.0100},{"AUDUSD",   0.0100},
        {"NZDUSD",  0.0100}, {"USDJPY",   1.00},
    };

    // Distance within which a price is considered "at" a round number
    // Expressed as a fraction of the round_increment (e.g. 0.10 = 10% of increment)
    double proximity_frac = 0.10;

    // Returns distance to nearest round number as a fraction of increment
    // 0.0 = exactly on round number, 0.5 = halfway between
    double distance_frac(const std::string& sym, double price) const noexcept {
        const double inc = get_increment(sym);
        if (inc <= 0) return 0.5;
        const double rem = std::fmod(price, inc);
        const double dist = std::min(rem, inc - rem);
        return dist / inc;
    }

    // Returns the nearest round number
    double nearest(const std::string& sym, double price) const noexcept {
        const double inc = get_increment(sym);
        if (inc <= 0) return price;
        return std::round(price / inc) * inc;
    }

    // Is price within proximity_frac of a round number?
    bool is_near(const std::string& sym, double price) const noexcept {
        return distance_frac(sym, price) <= proximity_frac;
    }

    // Is target price confluent with a round number? (increases TP probability)
    bool target_confluent(const std::string& sym, double entry,
                          bool is_long, double tp) const noexcept {
        // TP is confluent if it is near a round number AND the round number
        // is in the direction of the trade (between entry and TP)
        const double rn = nearest(sym, tp);
        if (!is_near(sym, rn)) return false;
        return is_long ? (rn > entry && rn <= tp) : (rn < entry && rn >= tp);
    }

    // Is entry near a round number? (increases rejection / false-break risk)
    bool entry_at_round(const std::string& sym, double entry) const noexcept {
        return is_near(sym, entry);
    }

    // Confluence score for a trade: +1 if TP confluent, -1 if entry at round, 0 otherwise
    int confluence_score(const std::string& sym, double entry,
                         bool is_long, double tp) const noexcept {
        int score = 0;
        if (target_confluent(sym, entry, is_long, tp)) ++score;
        if (entry_at_round(sym, entry))                --score;
        return score;
    }

private:
    double get_increment(const std::string& sym) const noexcept {
        auto it = round_increments.find(sym);
        return (it != round_increments.end()) ? it->second : 0.0;
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// 5. PreviousDayLevels
// ─────────────────────────────────────────────────────────────────────────────
// Tracks the prior UTC-day's high, low, and close per symbol.
// Compression breakouts near PDH/PDL have higher follow-through because:
//   - PDH = known prior resistance → break above = momentum with conviction
//   - PDL = known prior support   → break below = capitulation
//   - Midpoint (PDM) = VWAP proxy for daily mean reversion
//
// Also flags "inside day" setups (current range contained within prior range).
// ─────────────────────────────────────────────────────────────────────────────
struct DayLevels {
    double high  = 0.0;
    double low   = 1e18;
    double close = 0.0;
    double open  = 0.0;
    int    day   = -1;  // UTC day number

    bool valid() const { return high > 0 && low < 1e17 && high > low; }
    double midpoint() const { return valid() ? (high + low) * 0.5 : 0.0; }
    double range()    const { return valid() ? (high - low) : 0.0; }
};

class PreviousDayLevels {
public:
    // How close to PDH/PDL before we consider it "near" (fraction of prior range)
    double proximity_frac = 0.05;  // within 5% of prior range

    mutable std::mutex mtx;

    void update(const std::string& sym, double mid, int64_t ts_sec) noexcept {
        const int today = static_cast<int>(ts_sec / 86400);
        std::lock_guard<std::mutex> lk(mtx);
        auto& cur = current_[sym];
        auto& prev = previous_[sym];

        if (cur.day < 0) {
            // First tick ever
            cur.day  = today;
            cur.open = mid;
            cur.high = mid;
            cur.low  = mid;
            return;
        }

        if (today != cur.day) {
            // Day rolled — save current as previous, start fresh
            cur.close = mid;
            prev = cur;
            cur.day  = today;
            cur.open = mid;
            cur.high = mid;
            cur.low  = mid;
            return;
        }

        if (mid > cur.high) cur.high = mid;
        if (mid < cur.low)  cur.low  = mid;
        cur.close = mid;
    }

    DayLevels previous(const std::string& sym) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = previous_.find(sym);
        return (it != previous_.end()) ? it->second : DayLevels{};
    }

    DayLevels current(const std::string& sym) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = current_.find(sym);
        return (it != current_.end()) ? it->second : DayLevels{};
    }

    // Is price near PDH? (breakout above = bullish continuation)
    bool near_pdh(const std::string& sym, double price) const noexcept {
        const auto p = previous(sym);
        if (!p.valid()) return false;
        return std::fabs(price - p.high) <= p.range() * proximity_frac;
    }

    // Is price near PDL? (break below = bearish continuation)
    bool near_pdl(const std::string& sym, double price) const noexcept {
        const auto p = previous(sym);
        if (!p.valid()) return false;
        return std::fabs(price - p.low) <= p.range() * proximity_frac;
    }

    // Is current price inside prior day's range? (lower breakout conviction)
    bool inside_prior_range(const std::string& sym, double price) const noexcept {
        const auto p = previous(sym);
        if (!p.valid()) return false;
        return price >= p.low && price <= p.high;
    }

    // Structural confluence score for a breakout:
    //  +2 = price breaking above PDH (strong continuation signal)
    //  +1 = price near PDH (approaching key level)
    //  -1 = price inside prior range (no prior structure break)
    //  -2 = price breaking below PDL (short continuation)
    int breakout_score(const std::string& sym, double price, bool is_long) const noexcept {
        const auto p = previous(sym);
        if (!p.valid()) return 0;
        if (is_long) {
            if (price > p.high)            return  2;  // PDH broken bullish
            if (near_pdh(sym, price))      return  1;  // approaching PDH
            if (inside_prior_range(sym, price)) return -1; // inside = no structure
        } else {
            if (price < p.low)             return  2;  // PDL broken bearish
            if (near_pdl(sym, price))      return  1;  // approaching PDL
            if (inside_prior_range(sym, price)) return -1;
        }
        return 0;
    }

private:
    mutable std::unordered_map<std::string, DayLevels> current_;
    mutable std::unordered_map<std::string, DayLevels> previous_;
};


// ─────────────────────────────────────────────────────────────────────────────
// 6. FxFixWindowEngine
// ─────────────────────────────────────────────────────────────────────────────
// The WM/Reuters FX benchmark fixes are the most predictable directional
// events in currency markets. Institutional rebalancing creates known
// directional pressure at known times.
//
// Two fix windows:
//   Tokyo fix:  00:55-01:05 UTC daily
//               JPY repatriation flows. USDJPY direction bias based on
//               accumulated intraday CVD direction (JPY bought = fall in USDJPY)
//
//   London fix: 15:55-16:05 UTC (20:55-21:05 UTC in summer/21:55-22:05 winter)
//               WM/Reuters benchmark. Largest currency fixing in the world.
//               All major pairs affected. Direction from pre-fix momentum.
//
// Signal: enter 2 minutes before fix in the direction of accumulated flow,
//         exit at or just after the fix. 5-minute TP, 3-minute SL.
// ─────────────────────────────────────────────────────────────────────────────
struct FixSignal {
    bool   valid    = false;
    bool   is_long  = false;
    double entry    = 0.0;
    double tp       = 0.0;
    double sl       = 0.0;
    const char* reason = "";
};

class FxFixWindowEngine {
public:
    bool   enabled          = true;
    double tp_pips          = 15.0;   // 15 pips target
    double sl_pips          = 8.0;    // 8 pips stop
    double min_cvd_signal   = 0.10;   // min normalised CVD to enter
    int    cooldown_sec     = 7200;   // 2h between fix entries

    // Call every USDJPY tick for Tokyo fix
    FixSignal on_tick_tokyo(double bid, double ask, double cvd_normalised,
                            int64_t now_sec) noexcept {
        if (!enabled) return {};
        if (now_sec - last_tokyo_sec_ < cooldown_sec) return {};

        const int utc_min = static_cast<int>((now_sec % 86400) / 60);
        // Tokyo fix: 00:55-01:00 UTC (arm at 00:53, execute at 00:55)
        const bool in_arm_window  = (utc_min == 53);     // 00:53 UTC
        const bool in_fix_window  = (utc_min >= 55 && utc_min <= 57); // 00:55-00:57

        if (in_arm_window) {
            // Record the directional bias just before the fix
            tokyo_bias_  = cvd_normalised;
            armed_tokyo_ = true;
        }

        if (!armed_tokyo_ || !in_fix_window) return {};
        if (std::fabs(tokyo_bias_) < min_cvd_signal) return {};

        armed_tokyo_ = false;
        last_tokyo_sec_ = now_sec;

        const double mid = (bid + ask) * 0.5;
        const double pip = 0.01;  // USDJPY pip
        FixSignal sig;
        sig.valid   = true;
        sig.is_long = (tokyo_bias_ < 0);  // CVD falling = JPY bid = USDJPY falls = short
        sig.entry   = mid;
        sig.tp      = sig.is_long ? mid + tp_pips * pip : mid - tp_pips * pip;
        sig.sl      = sig.is_long ? mid - sl_pips * pip : mid + sl_pips * pip;
        sig.reason  = "TOKYO_FIX";
        printf("[FX-FIX] TOKYO fix signal: %s USDJPY entry=%.3f tp=%.3f sl=%.3f cvd_bias=%.3f\n",
               sig.is_long ? "LONG" : "SHORT", sig.entry, sig.tp, sig.sl, tokyo_bias_);
        return sig;
    }

    // Call every major FX tick for London fix (EURUSD, GBPUSD)
    // London WM/Reuters fix: 20:55-21:00 UTC (winter EST+5 = 15:55-16:00 London)
    // Arm at day_min=1253 (20:53 UTC), fire at 1255-1257 (20:55-20:57 UTC)
    FixSignal on_tick_london(const char* sym, double bid, double ask,
                              double cvd_normalised, int64_t now_sec) noexcept {
        if (!enabled) return {};
        if (now_sec - last_london_sec_ < cooldown_sec) return {};

        const int day_min      = static_cast<int>((now_sec % 86400) / 60);
        const bool arm_window  = (day_min == 1253);               // 20:53 UTC — arm
        const bool fire_window = (day_min >= 1255 && day_min <= 1257); // 20:55-20:57 UTC

        if (arm_window) {
            london_bias_  = cvd_normalised;
            armed_london_ = true;
        }

        if (!armed_london_ || !fire_window) return {};
        if (std::fabs(london_bias_) < min_cvd_signal) return {};

        armed_london_    = false;
        last_london_sec_ = now_sec;

        const double pip = 0.0001;  // both EURUSD and GBPUSD are 4-decimal pairs
        const double mid = (bid + ask) * 0.5;
        FixSignal sig;
        sig.valid   = true;
        sig.is_long = (london_bias_ > 0);  // positive CVD = buyers dominate = go long
        sig.entry   = mid;
        sig.tp      = sig.is_long ? mid + tp_pips * pip : mid - tp_pips * pip;
        sig.sl      = sig.is_long ? mid - sl_pips * pip : mid + sl_pips * pip;
        sig.reason  = "LONDON_FIX";
        printf("[FX-FIX] LONDON fix signal: %s %s entry=%.5f tp=%.5f sl=%.5f cvd_bias=%.3f\n",
               sig.is_long ? "LONG" : "SHORT", sym, sig.entry, sig.tp, sig.sl, london_bias_);
        return sig;
    }

private:
    bool    armed_tokyo_    = false;
    bool    armed_london_   = false;
    double  tokyo_bias_     = 0.0;
    double  london_bias_    = 0.0;
    int64_t last_tokyo_sec_ = 0;
    int64_t last_london_sec_= 0;
};


// ─────────────────────────────────────────────────────────────────────────────
// 7. FillQualityTracker
// ─────────────────────────────────────────────────────────────────────────────
// Records fill price vs signal mid price for every entry.
// Tracks:
//   fill_slippage = |fill_price - signal_mid| / signal_mid * 10000 (bps)
//   adverse_selection: fill consistently worse than mid = being front-run
//
// When rolling adverse selection score exceeds threshold, sets a warning flag.
// The adaptive risk system can use this to reduce size or skip entries.
// ─────────────────────────────────────────────────────────────────────────────
struct FillRecord {
    double signal_mid;  // mid price at signal generation
    double fill_price;  // actual fill price
    bool   is_long;
    int64_t ts_sec;

    // Positive = adverse (filled worse than mid), negative = improvement
    double slippage_bps() const noexcept {
        if (signal_mid <= 0) return 0;
        const double diff = is_long
            ? (fill_price - signal_mid)   // long: higher fill = worse
            : (signal_mid - fill_price);  // short: lower fill = worse
        return diff / signal_mid * 10000.0;
    }
};

class FillQualityTracker {
public:
    int    window_trades    = 50;    // rolling window for adverse selection detection
    double adverse_threshold_bps = 2.0;  // avg slippage > 2bp = adverse selection warning
    bool   enabled          = true;

    mutable std::mutex mtx;

    // Record a fill — call when order fills (on ExecutionReport with fill price)
    void record_fill(const std::string& sym, double signal_mid,
                     double fill_price, bool is_long, int64_t ts_sec) noexcept {
        if (!enabled || signal_mid <= 0) return;
        std::lock_guard<std::mutex> lk(mtx);
        auto& q = queues_[sym];
        q.push_back({signal_mid, fill_price, is_long, ts_sec});
        if ((int)q.size() > window_trades) q.pop_front();
    }

    // Rolling average slippage in bps for a symbol
    double avg_slippage_bps(const std::string& sym) const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = queues_.find(sym);
        if (it == queues_.end() || it->second.empty()) return 0.0;
        double total = 0.0;
        for (const auto& r : it->second) total += r.slippage_bps();
        return total / it->second.size();
    }

    // Is this symbol showing signs of adverse selection?
    bool adverse_selection_detected(const std::string& sym) const noexcept {
        if (!enabled) return false;
        const auto& q = [&]() -> const std::deque<FillRecord>& {
            static const std::deque<FillRecord> empty;
            std::lock_guard<std::mutex> lk(mtx);
            auto it = queues_.find(sym);
            return (it != queues_.end()) ? it->second : empty;
        }();
        if ((int)q.size() < 10) return false;  // need enough data
        return avg_slippage_bps(sym) > adverse_threshold_bps;
    }

    void print_summary() const noexcept {
        std::lock_guard<std::mutex> lk(mtx);
        printf("[FILL-QUALITY] Fill quality summary (%d-trade window):\n", window_trades);
        for (const auto& kv : queues_) {
            if (kv.second.size() < 5) continue;
            double total = 0;
            for (const auto& r : kv.second) total += r.slippage_bps();
            const double avg = total / kv.second.size();
            printf("  %-10s  fills=%3d  avg_slip=%.2fbps  %s\n",
                kv.first.c_str(), (int)kv.second.size(), avg,
                avg > adverse_threshold_bps ? "⚠ ADVERSE" : "ok");
        }
    }

private:
    mutable std::unordered_map<std::string, std::deque<FillRecord>> queues_;
};


// =============================================================================
// 8. VolumeProfileTracker
// =============================================================================
// Tracks time-at-price (tick count per bucket) as a proxy for volume profile.
// At each price bucket, counts how many ticks price has spent there.
//
// HIGH-NODE ENTRY (price has spent a lot of time here):
//   - Market Profile theory: high-node areas are VALUE areas — price returns here.
//   - A breakout from a high-node is a lower-probability move; market tends to
//     pull back to that node. Score: -1.
//
// LOW-NODE ENTRY (price rarely visits here — "thin" area):
//   - Price moves fast through thin areas with little resistance or support.
//   - Breakouts into thin areas have strong follow-through. Score: +1.
//
// Implementation: bucket = floor(price / bucket_size) × bucket_size
//   bucket_size calibrated per instrument so there are ~50-100 meaningful buckets:
//   GOLD: $2.00 buckets, SILVER: $0.25, SP500: 5pts, FX: 20 pips
// =============================================================================
class VolumeProfileTracker {
public:
    // Bucket sizes per instrument class (price points)
    static constexpr double BUCKET_GOLD    = 2.00;
    static constexpr double BUCKET_SILVER  = 0.25;
    static constexpr double BUCKET_OIL     = 0.50;
    static constexpr double BUCKET_INDEX   = 5.0;    // SP500, NQ, DJ30, NAS100
    static constexpr double BUCKET_EU_IDX  = 3.0;    // GER40, UK100, ESTX50
    static constexpr double BUCKET_FX      = 0.0020; // 20 pip buckets
    static constexpr double BUCKET_USDJPY  = 0.20;   // 20 pip buckets in JPY

    // Window: max ticks to keep in profile (rolling — old ticks fall off)
    static constexpr int PROFILE_WINDOW = 2000;  // ~5-10 minutes at typical rates

    // Thresholds: what fraction of max-node count is HIGH vs LOW
    static constexpr double HIGH_NODE_FRAC = 0.60;  // bucket >= 60% of max = high node
    static constexpr double LOW_NODE_FRAC  = 0.12;  // bucket <= 12% of max = thin area

    struct SymState {
        std::unordered_map<int, int> profile;  // bucket_idx → tick_count
        std::deque<int>              history;  // ordered bucket_idx of last N ticks
        int                          max_count = 0;
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, SymState> state_;

    static double bucket_size(const std::string& sym) noexcept {
        if (sym == "GOLD.F")  return BUCKET_GOLD;
        if (sym == "XAGUSD")  return BUCKET_SILVER;
        if (sym == "USOIL.F" || sym == "BRENT") return BUCKET_OIL;
        if (sym == "US500.F" || sym == "USTEC.F" || sym == "DJ30.F" || sym == "NAS100")
            return BUCKET_INDEX;
        if (sym == "GER40" || sym == "UK100" || sym == "ESTX50")
            return BUCKET_EU_IDX;
        if (sym == "USDJPY") return BUCKET_USDJPY;
        return BUCKET_FX;  // all FX pairs
    }

    void update(const std::string& sym, double mid) noexcept {
        if (mid <= 0.0) return;
        const double bs = bucket_size(sym);
        if (bs <= 0.0) return;
        const int bucket = static_cast<int>(std::floor(mid / bs));

        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = state_[sym];

        // Add to profile
        s.profile[bucket]++;
        s.history.push_back(bucket);

        // Update max
        if (s.profile[bucket] > s.max_count)
            s.max_count = s.profile[bucket];

        // Evict oldest tick if over window
        if ((int)s.history.size() > PROFILE_WINDOW) {
            const int old_bucket = s.history.front();
            s.history.pop_front();
            auto it = s.profile.find(old_bucket);
            if (it != s.profile.end()) {
                it->second--;
                if (it->second <= 0) s.profile.erase(it);
            }
            // Recompute max (amortised — only when max bucket was evicted)
            if (s.max_count > 0) {
                int new_max = 0;
                for (const auto& kv : s.profile)
                    if (kv.second > new_max) new_max = kv.second;
                s.max_count = new_max;
            }
        }
    }

    // Returns: +1 if entry is in a thin (low-node) area → fast follow-through
    //           0 if neutral
    //          -1 if entry is at a high-node (value area) → likely to revert
    int score(const std::string& sym, double entry) const noexcept {
        const double bs = bucket_size(sym);
        if (bs <= 0.0) return 0;
        const int bucket = static_cast<int>(std::floor(entry / bs));

        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        if (it == state_.end() || it->second.max_count < 20) return 0;  // not warmed up

        const auto& s = it->second;
        const auto pit = s.profile.find(bucket);
        const int count = (pit != s.profile.end()) ? pit->second : 0;
        const double frac = static_cast<double>(count) / s.max_count;

        if (frac >= HIGH_NODE_FRAC) return -1;  // high-value area — avoid
        if (frac <= LOW_NODE_FRAC)  return +1;  // thin area — fast moves
        return 0;
    }

    void reset(const std::string& sym) {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.erase(sym);
    }
    void reset_all() {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.clear();
    }
};

// =============================================================================
// 9. OrderFlowAbsorptionDetector
// =============================================================================
// Detects when price and order book imbalance diverge — the institutional
// signature of a failing breakout.
//
// Pattern: price rising (long breakout candidate) but L2 is ask-heavy
//   (imbalance < 0.40) for N consecutive ticks = sellers absorbing the move.
//   The breakout is being sold into. Hard block on longs.
//
// Pattern: price falling (short breakout candidate) but L2 is bid-heavy
//   (imbalance > 0.60) for N consecutive ticks = buyers absorbing the move.
//   Hard block on shorts.
//
// Based on: Jovanovic & Menkveld (2016), Kirilenko et al. (2017) — absorption
// of aggressive order flow by passive large traders is the primary reversal signal.
// =============================================================================
class OrderFlowAbsorptionDetector {
public:
    int    CONFIRM_TICKS    = 4;    // consecutive divergent ticks needed
    double ABSORB_THRESHOLD = 0.40; // imbalance below this = ask-heavy (sellers)
    double ABSORB_LONG_MAX  = 0.40; // if price up AND imbalance < this = absorption
    double ABSORB_SHORT_MIN = 0.60; // if price down AND imbalance > this = absorption

    struct SymState {
        double prev_mid    = 0.0;
        int    absorb_long_count  = 0;  // ticks where price rising but book sell-heavy
        int    absorb_short_count = 0;  // ticks where price falling but book buy-heavy
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, SymState> state_;

    // Call every tick with current mid and L2 imbalance for this symbol
    void update(const std::string& sym, double mid, double l2_imbalance) noexcept {
        if (mid <= 0.0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = state_[sym];

        if (s.prev_mid > 0.0) {
            const bool price_up   = (mid > s.prev_mid);
            const bool price_down = (mid < s.prev_mid);

            // Price rising but ask-heavy book = sellers absorbing longs
            if (price_up && l2_imbalance < ABSORB_LONG_MAX)
                s.absorb_long_count++;
            else
                s.absorb_long_count = 0;

            // Price falling but bid-heavy book = buyers absorbing shorts
            if (price_down && l2_imbalance > ABSORB_SHORT_MIN)
                s.absorb_short_count++;
            else
                s.absorb_short_count = 0;
        }
        s.prev_mid = mid;
    }

    // Returns true if absorption is detected for the given direction
    // is_long=true: are sellers absorbing the upward move?
    bool is_absorbing(const std::string& sym, bool is_long) const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        if (it == state_.end()) return false;
        const auto& s = it->second;
        if (is_long)  return s.absorb_long_count  >= CONFIRM_TICKS;
        return              s.absorb_short_count >= CONFIRM_TICKS;
    }

    void reset_all() {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.clear();
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// EdgeContext — single object holding all edge systems
// Declare one global: static omega::edges::EdgeContext g_edges;
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// VPINDetector — Volume-synchronised Probability of Informed trading
//
// Standard VPIN uses equal-volume buckets. We use a simplified tick-based
// version that is computationally cheap and works without real volume data:
//   - Divide the tick stream into N-tick buckets (default 50 ticks)
//   - For each bucket: count buy_ticks (price up) and sell_ticks (price down)
//   - VPIN = rolling_mean(|buy - sell| / bucket_size) over last W buckets
//   - VPIN in [0, 1]: 0 = balanced flow, 1 = one-sided (informed)
//
// When VPIN > high_threshold (default 0.60): informed flow detected.
//   → Reduce lot size by 50% (via size_scale() < 1.0)
//   → Optionally block entry entirely (block_above = 0.75)
//
// This catches institutional flow that doesn't show as L2 wall/vacuum.
// Two Sigma / Citadel measure VPIN every bucket and reduce size on elevation.
// ─────────────────────────────────────────────────────────────────────────────
struct VPINDetector {
    int    bucket_size     = 50;    // ticks per bucket
    int    window_buckets  = 10;    // rolling window (10 × 50 = 500 ticks)
    double high_threshold  = 0.60;  // VPIN above this = informed flow
    double block_threshold = 0.80;  // VPIN above this = block entry entirely
    bool   enabled         = true;

    struct SymState {
        double prev_mid    = 0.0;
        int    buy_ticks   = 0;
        int    sell_ticks  = 0;
        int    tick_count  = 0;
        std::deque<double> bucket_vpins;  // rolling bucket VPIN values
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, SymState> state_;

    // Call on every tick with mid price
    void update(const std::string& sym, double mid) {
        if (!enabled || mid <= 0.0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = state_[sym];
        if (s.prev_mid > 0.0) {
            if (mid > s.prev_mid)      ++s.buy_ticks;
            else if (mid < s.prev_mid) ++s.sell_ticks;
        }
        s.prev_mid = mid;
        ++s.tick_count;

        if (s.tick_count >= bucket_size) {
            const int total = s.buy_ticks + s.sell_ticks;
            const double bucket_vpin = (total > 0)
                ? static_cast<double>(std::abs(s.buy_ticks - s.sell_ticks)) / total
                : 0.0;
            s.bucket_vpins.push_back(bucket_vpin);
            if (static_cast<int>(s.bucket_vpins.size()) > window_buckets)
                s.bucket_vpins.pop_front();
            // Reset bucket counters
            s.buy_ticks = s.sell_ticks = s.tick_count = 0;
        }
    }

    // Current VPIN for a symbol (0.0 = no data / balanced, 1.0 = fully informed)
    double vpin(const std::string& sym) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        if (it == state_.end() || it->second.bucket_vpins.empty()) return 0.0;
        const auto& bv = it->second.bucket_vpins;
        double sum = 0.0;
        for (double v : bv) sum += v;
        return sum / bv.size();
    }

    // Size multiplier: 1.0 (normal) → 0.5 (high informed flow) → 0.0 (block)
    double size_scale(const std::string& sym) const {
        if (!enabled) return 1.0;
        const double v = vpin(sym);
        if (v >= block_threshold) return 0.0;  // block entry entirely
        if (v >= high_threshold)  return 0.5;  // halve size
        return 1.0;
    }

    // True if entry should be completely blocked due to extreme informed flow
    bool is_blocked(const std::string& sym) const {
        if (!enabled) return false;
        return vpin(sym) >= block_threshold;
    }

    void reset_on_reconnect(const std::string& sym) {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.erase(sym);
    }
};

struct EdgeContext {
    CumulativeVolumeDelta      cvd;
    TimeOfDayGate              tod;
    RelativeSpreadGate         spread_gate;
    RoundNumberFilter          round_numbers;
    PreviousDayLevels          prev_day;
    FxFixWindowEngine          fx_fix;
    FillQualityTracker         fill_quality;
    VolumeProfileTracker       vol_profile;       // Edge 8: time-at-price
    OrderFlowAbsorptionDetector absorption;       // Edge 9: institutional absorption
    VPINDetector               vpin;              // Edge 10: informed order flow toxicity

    // ── entry_score: original 4-signal score (-4..+4) ──────────────────────
    int entry_score(const std::string& sym, double entry, bool is_long,
                    double tp, int64_t now_sec) const noexcept {
        int score = 0;
        score += round_numbers.confluence_score(sym, entry, is_long, tp);
        score += prev_day.breakout_score(sym, entry, is_long);
        const CVDState cvd_s = cvd.get(sym);
        const int cvd_dir = cvd_s.direction();
        if (is_long  && cvd_dir == +1) ++score;
        if (!is_long && cvd_dir == -1) ++score;
        if (is_long  && cvd_s.bullish_divergence()) ++score;
        if (!is_long && cvd_s.bearish_divergence()) ++score;
        return score;
    }

    // ── entry_score_l2: full microstructure score including L2 signals ──────
    // Extended version that incorporates:
    //   - All original signals (CVD, PDH/PDL, round numbers)
    //   - Microprice bias confirmation/contradiction
    //   - Liquidity vacuum in direction (fast move likely)
    //   - Liquidity wall between entry and TP (target blocked)
    //   - Order flow absorption (institutional selling into longs)
    //   - Volume profile (thin area = follow-through; node = reversion)
    //
    // Score range: -7..+7
    // Hard block threshold: <= -3 (same as before — now more selective)
    // Lot boost threshold:  >= +3 (more signals required for boost)
    int entry_score_l2(const std::string& sym,
                       double entry,
                       bool   is_long,
                       double tp,
                       int64_t now_sec,
                       // L2 microstructure inputs from MacroContext
                       double microprice_bias,   // >0 = upward pressure
                       double l2_imbalance,      // 0..1, 0.5 = neutral
                       bool   vacuum_in_dir,     // liquidity vacuum in trade direction
                       bool   wall_to_tp         // large resting order between entry and TP
                       ) const noexcept {

        // Start with base score (CVD, PDH/PDL, round numbers)
        int score = entry_score(sym, entry, is_long, tp, now_sec);

        // ── Microprice bias ───────────────────────────────────────────────────
        // Microprice = weighted midpoint accounting for queue sizes.
        // If microprice is above mid (bias > 0), next tick is more likely up.
        // Confirms longs, contradicts shorts, and vice versa.
        // Threshold: 0.05pts for FX/indices, 0.50pts for gold — scale by price.
        // Use a relative threshold: bias as pct of entry.
        if (entry > 0.0) {
            const double bias_pct = std::fabs(microprice_bias) / entry * 100.0;
            if (bias_pct > 0.001) {  // meaningful: >0.1bp from mid
                const bool bias_long  = (microprice_bias > 0.0);
                if (is_long  && bias_long)  ++score;   // confirms long
                if (!is_long && !bias_long) ++score;   // confirms short
                if (is_long  && !bias_long) --score;   // contradicts long
                if (!is_long && bias_long)  --score;   // contradicts short
            }
        }

        // ── Liquidity vacuum ──────────────────────────────────────────────────
        // Thin top-3 levels on the side we're heading toward = fast move.
        // vacuum_in_dir: caller passes vacuum_ask for longs, vacuum_bid for shorts.
        // This is one of the strongest short-term momentum confirmations available.
        if (vacuum_in_dir) score += 2;

        // ── Wall to TP ────────────────────────────────────────────────────────
        // Large resting order between entry and TP = target may not be reached.
        // Penalty -2 (was -3): walls are frequently absorbed by institutional flow,
        // -3 made a single wall an unconditional block regardless of other signals.
        // Combined with absorption (-2) still reaches the -3 block threshold.
        if (wall_to_tp) score -= 2;

        // ── Order flow absorption ─────────────────────────────────────────────
        // Institutional signature: price moving in breakout direction but book
        // is heavily loaded against the move. Someone big is fading this.
        // Only apply when absorption has been confirmed (N consecutive ticks).
        if (absorption.is_absorbing(sym, is_long)) score -= 2;

        // ── Volume profile ────────────────────────────────────────────────────
        // +1 if entry is in thin area (fast follow-through)
        // -1 if entry is at high-volume node (price magnet, likely to revert)
        score += vol_profile.score(sym, entry);

        return score;
    }

    // ── VWAP chop gate ────────────────────────────────────────────────────────
    // Returns true (allow entry) if price is sufficiently far from VWAP.
    // Entries near VWAP have no directional edge — VWAP is the mean-reversion
    // anchor; breakouts from inside the VWAP zone chop back constantly.
    // Threshold: 0.05% of price (5bp) — tight enough to catch real chop,
    // loose enough not to block genuine momentum that happens to be near VWAP.
    bool vwap_gate(double entry, double vwap, double threshold_pct = 0.05) const noexcept {
        if (vwap <= 0.0) return true;  // no VWAP data — allow
        const double dist_pct = std::fabs(entry - vwap) / vwap * 100.0;
        return dist_pct >= threshold_pct;
    }

    // Reset daily state
    void reset_daily() noexcept {
        cvd.reset_all();
        vol_profile.reset_all();
        absorption.reset_all();
        // prev_day handled by update() detecting day change
    }
};

}} // namespace omega::edges
