// =============================================================================
// XauThreeBar30mBacktest.cpp -- Standalone backtest for XauThreeBar30mEngine
// =============================================================================
//
// Strategy: 3 consecutive directional 30-minute bars + breakout confirmation.
// If bars i-3, i-2, i-1 all bullish AND bar-i close > bar i-1 high → LONG.
// If bars i-3, i-2, i-1 all bearish AND bar-i close < bar i-1 low → SHORT.
//
// DATA FORMAT: Dukascopy combined CSV: timestamp_ms,askPrice,bidPrice
//
// COMPILATION:
//   g++ -std=c++17 -O2 -o backtest/xau_3bar_bt backtest/XauThreeBar30mBacktest.cpp
//   ./backtest/xau_3bar_bt ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <array>
#include <deque>

namespace cfg {
    constexpr double LOT_SIZE       = 0.01;
    constexpr double PNL_PER_PT     = LOT_SIZE * 100.0;

    constexpr int64_t BAR_PERIOD_MS = 1800000;  // 30 minutes
    constexpr int    ATR_PERIOD     = 14;
    constexpr double ATR_INIT       = 5.0;
    constexpr double ATR_MIN_CLAMP  = 0.5;

    // SL/TP geometry (from engine defaults)
    constexpr double SL_ATR_MULT    = 2.0;
    constexpr double TP_ATR_MULT    = 4.0;

    // Cooldown: 1 bar after close
    constexpr int    COOLDOWN_BARS  = 1;

    // v1: no session filter
    constexpr bool   USE_SESSION_FILTER = false;

    // v2: LONG ONLY (short side PF=0.84 in v1 dragged overall)
    constexpr bool   LONG_ONLY = true;

    constexpr double OOS_SPLIT_FRAC = 0.60;

    inline bool is_weekend(int64_t ts_ms) noexcept {
        const int64_t sec = ts_ms / 1000;
        const int dow = ((sec / 86400) + 4) % 7;
        if (dow == 0 || dow == 6) return true;
        if (dow == 5) {
            const int hour = static_cast<int>((sec % 86400) / 3600);
            if (hour >= 21) return true;
        }
        return false;
    }
}

struct Bar30m {
    int64_t open_ms    = 0;
    double  open       = 0.0;
    double  high       = 0.0;
    double  low        = 99999999.0;
    double  close      = 0.0;
    int     tick_count = 0;

    void reset(int64_t ts, double price) noexcept {
        open_ms = ts; open = price; high = price; low = price; close = price; tick_count = 1;
    }
    void update(double price) noexcept {
        if (price > high) high = price;
        if (price < low)  low  = price;
        close = price; tick_count++;
    }
    bool is_bullish() const noexcept { return close > open; }
    bool is_bearish() const noexcept { return close < open; }
};

struct ThreeBarIndicators {
    Bar30m current_bar{};
    std::deque<Bar30m> bars;
    bool bar_active = false;
    int  total_bars = 0;

    double atr_pts = cfg::ATR_INIT;
    std::deque<double> tr_history;

    bool on_tick(double mid, int64_t ts_ms) noexcept {
        if (!bar_active) {
            current_bar.reset(ts_ms, mid);
            bar_active = true;
            return false;
        }
        int64_t bar_boundary = (current_bar.open_ms / cfg::BAR_PERIOD_MS) * cfg::BAR_PERIOD_MS + cfg::BAR_PERIOD_MS;
        if (ts_ms >= bar_boundary) {
            bars.push_back(current_bar);
            total_bars++;
            _on_bar_close();
            current_bar.reset(ts_ms, mid);
            while ((int)bars.size() > 260) bars.pop_front();
            return true;
        }
        current_bar.update(mid);
        return false;
    }

private:
    void _on_bar_close() noexcept {
        if (bars.size() < 2) return;
        const Bar30m& bar = bars.back();
        const Bar30m& prev = bars[bars.size() - 2];

        const double tr = std::max({
            bar.high - bar.low,
            std::fabs(bar.high - prev.close),
            std::fabs(bar.low - prev.close)
        });
        tr_history.push_back(tr);
        if ((int)tr_history.size() > cfg::ATR_PERIOD) tr_history.pop_front();

        if ((int)tr_history.size() >= cfg::ATR_PERIOD) {
            atr_pts = atr_pts * (cfg::ATR_PERIOD - 1.0) / cfg::ATR_PERIOD + tr / cfg::ATR_PERIOD;
        } else {
            double sum = 0.0;
            for (double t : tr_history) sum += t;
            atr_pts = sum / tr_history.size();
        }
        atr_pts = std::max(cfg::ATR_MIN_CLAMP, atr_pts);
    }
};

struct SimTrade {
    bool active = false; bool is_long = true;
    double entry_px = 0, sl_px = 0, tp_px = 0, atr_at_entry = 5.0;
    int64_t entry_time = 0; int entry_bar = 0;
    double mfe_pts = 0, mae_pts = 0;
};

struct PerformanceMetrics {
    int total_trades = 0, wins = 0, losses = 0;
    double total_pnl = 0, gross_wins = 0, gross_losses = 0, max_drawdown = 0, peak_equity = 0;
    std::vector<double> pnl_series;

    void record(double pnl) {
        total_trades++; total_pnl += pnl; pnl_series.push_back(pnl);
        if (pnl > 0) { wins++; gross_wins += pnl; }
        else { losses++; gross_losses += std::fabs(pnl); }
        if (total_pnl > peak_equity) peak_equity = total_pnl;
        double dd = peak_equity - total_pnl;
        if (dd > max_drawdown) max_drawdown = dd;
    }
    double win_rate() const { return total_trades > 0 ? (double)wins / total_trades : 0.0; }
    double profit_factor() const { return gross_losses > 0 ? gross_wins / gross_losses : 999.0; }
    double avg_pnl() const { return total_trades > 0 ? total_pnl / total_trades : 0.0; }
    double avg_win() const { return wins > 0 ? gross_wins / wins : 0.0; }
    double avg_loss() const { return losses > 0 ? gross_losses / losses : 0.0; }

    void print(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  avg=$%.3f  maxDD=$%.2f\n",
                   label, total_trades, win_rate()*100, profit_factor(), total_pnl, avg_pnl(), max_drawdown);
    }
    void print_detail(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  avgW=$%.3f  avgL=$%.3f\n",
                   label, total_trades, win_rate()*100, profit_factor(), total_pnl, avg_win(), avg_loss());
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) { std::fprintf(stderr, "Usage: %s <tick_csv> [--verbose]\n", argv[0]); return 1; }

    std::string csv_path = argv[1];
    bool verbose = (argc > 2 && std::strcmp(argv[2], "--verbose") == 0);

    PerformanceMetrics overall, long_m, short_m, is_metrics, oos_metrics;
    std::array<PerformanceMetrics, 24> per_hour;
    int sl_hit = 0, tp_hit = 0;
    int signals_fired = 0, cooldown_blocked = 0;

    ThreeBarIndicators ind;
    SimTrade trade;
    int cooldown_remaining = 0;

    int64_t oos_split_ts = 0; bool oos_split_set = false;
    int64_t first_ts = 0, last_ts = 0;
    size_t total_ticks = 0;
    double min_px = 99999999, max_px = 0;

    std::printf("[3BAR-BT] Streaming XAUUSD ThreeBar30m backtest from %s...\n", csv_path.c_str());
    std::printf("[3BAR-BT] Config: 30m bars, SL=%.1f*ATR  TP=%.1f*ATR  Direction: %s\n\n",
               cfg::SL_ATR_MULT, cfg::TP_ATR_MULT, cfg::LONG_ONLY ? "LONG ONLY" : "BOTH");

    std::ifstream f(csv_path);
    if (!f.is_open()) { std::fprintf(stderr, "Cannot open %s\n", csv_path.c_str()); return 1; }

    std::string first_line;
    std::getline(f, first_line);
    bool has_header = (first_line.find("timestamp") != std::string::npos ||
                       first_line.find("Time") != std::string::npos);

    auto parse_duka = [](const std::string& line, int64_t& ts, double& bid, double& ask) -> bool {
        return std::sscanf(line.c_str(), "%lld,%lf,%lf", (long long*)&ts, &ask, &bid) == 3;
    };

    auto process_tick = [&](int64_t ts_ms, double bid, double ask) {
        if (bid <= 0 || ask <= 0 || ask < bid) return;
        const double mid = (bid + ask) * 0.5;
        if (mid < 1000.0 || mid > 5000.0) return;

        total_ticks++;
        if (mid < min_px) min_px = mid;
        if (mid > max_px) max_px = mid;
        if (first_ts == 0) first_ts = ts_ms;
        last_ts = ts_ms;

        if (!oos_split_set && first_ts > 0) {
            const double total_range_ms = 26.0 * 30.0 * 24.0 * 3600.0 * 1000.0;
            oos_split_ts = first_ts + (int64_t)(total_range_ms * cfg::OOS_SPLIT_FRAC);
            oos_split_set = true;
        }

        if (cfg::is_weekend(ts_ms)) return;

        bool new_bar = ind.on_tick(mid, ts_ms);

        // Manage open trade
        if (trade.active) {
            const double fav = trade.is_long ? (mid - trade.entry_px) : (trade.entry_px - mid);
            if (fav > trade.mfe_pts) trade.mfe_pts = fav;
            if (fav < trade.mae_pts) trade.mae_pts = fav;

            bool sl = trade.is_long ? (bid <= trade.sl_px) : (ask >= trade.sl_px);
            bool tp = trade.is_long ? (bid >= trade.tp_px) : (ask <= trade.tp_px);

            if (sl || tp) {
                double exit_px = tp ? trade.tp_px : trade.sl_px;
                double pnl_pts = trade.is_long ? (exit_px - trade.entry_px) : (trade.entry_px - exit_px);
                double pnl_usd = pnl_pts * cfg::PNL_PER_PT;

                overall.record(pnl_usd);
                if (trade.is_long) long_m.record(pnl_usd); else short_m.record(pnl_usd);
                if (trade.entry_time < oos_split_ts) is_metrics.record(pnl_usd);
                else oos_metrics.record(pnl_usd);
                int h = static_cast<int>((trade.entry_time / 1000 % 86400) / 3600);
                per_hour[h].record(pnl_usd);
                if (tp) tp_hit++; else sl_hit++;

                trade.active = false;
                cooldown_remaining = cfg::COOLDOWN_BARS;
            }
            return;
        }

        // Entry evaluation on bar close
        if (!new_bar) return;
        if ((int)ind.bars.size() < 4) return;
        if (ind.total_bars < 16) return;

        if (cooldown_remaining > 0) { cooldown_remaining--; cooldown_blocked++; return; }

        const Bar30m& b3 = ind.bars[ind.bars.size() - 4];  // i-3
        const Bar30m& b2 = ind.bars[ind.bars.size() - 3];  // i-2
        const Bar30m& b1 = ind.bars[ind.bars.size() - 2];  // i-1
        const Bar30m& b0 = ind.bars[ind.bars.size() - 1];  // i (just closed)

        int signal = 0;
        // Bullish: 3 consecutive bullish bars + close breaks above prev high
        if (b3.is_bullish() && b2.is_bullish() && b1.is_bullish() && b0.close > b1.high)
            signal = +1;
        // Bearish: 3 consecutive bearish bars + close breaks below prev low
        // v2: LONG ONLY — short side PF=0.84 in v1, kills overall edge
        if (!cfg::LONG_ONLY && b3.is_bearish() && b2.is_bearish() && b1.is_bearish() && b0.close < b1.low)
            signal = -1;

        if (signal == 0) return;
        signals_fired++;

        bool is_long = (signal == +1);
        double sl_pts = ind.atr_pts * cfg::SL_ATR_MULT;
        double tp_pts = ind.atr_pts * cfg::TP_ATR_MULT;

        trade.active = true;
        trade.is_long = is_long;
        trade.entry_px = is_long ? ask : bid;
        trade.atr_at_entry = ind.atr_pts;
        trade.entry_time = ts_ms;
        trade.entry_bar = ind.total_bars;
        trade.mfe_pts = 0; trade.mae_pts = 0;

        if (is_long) {
            trade.sl_px = trade.entry_px - sl_pts;
            trade.tp_px = trade.entry_px + tp_pts;
        } else {
            trade.sl_px = trade.entry_px + sl_pts;
            trade.tp_px = trade.entry_px - tp_pts;
        }

        if (verbose) {
            std::printf("[3BAR-ENTRY] %s @ %.2f sl=%.2f tp=%.2f atr=%.1f\n",
                       is_long ? "LONG" : "SHORT", trade.entry_px, trade.sl_px, trade.tp_px, ind.atr_pts);
        }
    };

    if (!has_header) {
        int64_t ts; double bid, ask;
        if (parse_duka(first_line, ts, bid, ask)) process_tick(ts, bid, ask);
    }

    std::string line;
    size_t lc = 0;
    while (std::getline(f, line)) {
        int64_t ts; double bid, ask;
        if (parse_duka(line, ts, bid, ask)) process_tick(ts, bid, ask);
        if (++lc % 10000000 == 0) std::printf("  ... %zuM ticks\n", lc / 1000000);
    }

    const double hours = (last_ts > first_ts) ? (last_ts - first_ts) / 3600000.0 : 0.0;

    std::printf("\n═══════════════════════════════════════════════════════════════\n");
    std::printf("  XAUUSD THREE-BAR 30m BACKTEST RESULTS (%s)\n", cfg::LONG_ONLY ? "LONG ONLY" : "BOTH DIRECTIONS");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    std::printf("  Ticks: %zu  |  Period: %.1f days\n", total_ticks, hours / 24.0);
    std::printf("  Price: %.1f - %.1f  |  Bars: %d (30m)  |  ATR: %.2f\n\n", min_px, max_px, ind.total_bars, ind.atr_pts);
    std::printf("  Signals fired: %d  |  Cooldown blocked: %d\n\n", signals_fired, cooldown_blocked);

    std::printf("── OVERALL ─────────────────────────────────────────────────\n");
    overall.print("TOTAL"); overall.print_detail("TOTAL (detail)");
    long_m.print("LONG"); short_m.print("SHORT");
    std::printf("  Exit breakdown: SL=%d  TP=%d\n\n", sl_hit, tp_hit);

    std::printf("── PF BY HOUR (UTC) ───────────────────────────────────────\n");
    std::printf("  Hour  Trades   WR%%    PF      PnL\n");
    for (int h = 0; h < 24; ++h) {
        if (per_hour[h].total_trades > 0) {
            std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%8.2f  %s\n",
                       h, per_hour[h].total_trades, per_hour[h].win_rate()*100,
                       per_hour[h].profit_factor(), per_hour[h].total_pnl,
                       per_hour[h].profit_factor() >= 1.0 ? " <<<" : "");
        }
    }
    std::printf("\n");

    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  IN-SAMPLE / OUT-OF-SAMPLE (60/40 split)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");
    is_metrics.print("IS TOTAL"); oos_metrics.print("OOS TOTAL");
    const bool oos_pass = (oos_metrics.total_trades >= 8 && oos_metrics.profit_factor() >= 1.20);
    std::printf("\n  OOS VERDICT: %s\n", oos_pass ? "PASS" : "REVIEW");
    if (oos_metrics.total_trades > 0 && is_metrics.total_trades > 0) {
        double decay = 1.0 - oos_metrics.profit_factor() / is_metrics.profit_factor();
        std::printf("  PF decay: %.0f%%\n", decay * 100.0);
    }
    std::printf("\n");

    return 0;
}
