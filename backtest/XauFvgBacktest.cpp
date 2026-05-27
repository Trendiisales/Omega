// =============================================================================
// XauFvgBacktest.cpp -- Standalone backtest for XauusdFvgEngine (Fair Value Gap)
// =============================================================================
//
// Replicates the XauusdFvgEngine signal logic on XAUUSD tick data.
// Strategy: detect 3-bar Fair Value Gaps on 15-minute bars, enter on
// mitigation (price returns to fill the gap), score-based filtering.
//
// DATA FORMAT: Dukascopy combined CSV: timestamp_ms,askPrice,bidPrice
//
// COMPILATION:
//   g++ -std=c++17 -O2 -o backtest/xau_fvg_bt backtest/XauFvgBacktest.cpp
//   ./backtest/xau_fvg_bt ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
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

// =============================================================================
// CONFIG
// =============================================================================
namespace cfg {
    constexpr double LOT_SIZE       = 0.01;
    constexpr double PNL_PER_PT     = LOT_SIZE * 100.0;  // XAUUSD: $1/pt at 0.01 lot

    // Bar period: 15 minutes
    constexpr int64_t BAR_PERIOD_MS = 900000;  // 15 * 60 * 1000

    // ATR
    constexpr int    ATR_PERIOD     = 14;
    constexpr double ATR_INIT       = 5.0;
    constexpr double ATR_MIN_CLAMP  = 0.5;

    // FVG detection
    constexpr double GAP_MIN_ATR    = 0.10;   // minimum gap = 0.10 * ATR
    constexpr double GAP_MAX_ATR    = 5.0;    // maximum gap = 5.0 * ATR
    constexpr double SCORE_CUTOFF   = 0.48;   // minimum score to enter
    constexpr int    MAX_PENDING    = 64;
    constexpr int    MAX_AGE_BARS   = 500;

    // Score weights (sum = 3.5)
    constexpr double W_GAP  = 1.5;
    constexpr double W_DISP = 1.0;
    constexpr double W_TV   = 1.0;
    constexpr double W_SUM  = 3.5;

    // SL/TP geometry
    constexpr double SL_ATR_MULT    = 2.5;
    constexpr double TP_ATR_MULT    = 5.0;

    // Time stop: 60 bars (15 hours)
    constexpr int    TIME_STOP_BARS = 60;

    // v1 DISCOVERY: no session filter
    constexpr bool   USE_SESSION_FILTER = false;

    // OOS split: 26 months * 0.60 = ~15.6 months = ~474 days
    constexpr double OOS_SPLIT_FRAC = 0.60;

    // Weekend gate
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

// =============================================================================
// BAR (15-minute)
// =============================================================================
struct Bar15m {
    int64_t open_ms    = 0;
    double  open       = 0.0;
    double  high       = 0.0;
    double  low        = 99999999.0;
    double  close      = 0.0;
    int     tick_count = 0;

    void reset(int64_t ts, double price) noexcept {
        open_ms    = ts;
        open       = price;
        high       = price;
        low        = price;
        close      = price;
        tick_count = 1;
    }
    void update(double price) noexcept {
        if (price > high) high = price;
        if (price < low)  low  = price;
        close = price;
        tick_count++;
    }
};

// =============================================================================
// FVG ZONE (pending gap waiting for mitigation)
// =============================================================================
struct FvgZone {
    bool   is_long    = true;     // true = bullish FVG (long entry)
    double zone_low   = 0.0;
    double zone_high  = 0.0;
    double score      = 0.0;
    double atr_at_form = 0.0;
    int    formed_bar = 0;        // bar index when formed
    int64_t formed_ts = 0;
    bool   active     = true;
};

// =============================================================================
// INDICATORS (ATR + tick volume rolling mean)
// =============================================================================
struct FvgIndicators {
    Bar15m current_bar{};
    std::deque<Bar15m> bars;
    bool bar_active = false;
    int  total_bars = 0;

    double atr_pts = cfg::ATR_INIT;
    std::deque<double> tr_history;

    // Rolling 20-bar tick count mean for tick-volume score
    std::deque<int> tick_counts;
    double rolling_tv_mean = 100.0;

    // Returns true when a new bar closes
    bool on_tick(double mid, int64_t ts_ms) noexcept {
        if (!bar_active) {
            current_bar.reset(ts_ms, mid);
            bar_active = true;
            return false;
        }
        // Align bars to 15-min boundaries
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
        const Bar15m& bar = bars.back();
        const Bar15m& prev = bars[bars.size() - 2];

        const double tr = std::max({
            bar.high - bar.low,
            std::fabs(bar.high - prev.close),
            std::fabs(bar.low - prev.close)
        });
        tr_history.push_back(tr);
        if ((int)tr_history.size() > cfg::ATR_PERIOD) tr_history.pop_front();

        // Wilder RMA: ATR = prev_ATR * (N-1)/N + TR/N
        if ((int)tr_history.size() >= cfg::ATR_PERIOD) {
            atr_pts = atr_pts * (cfg::ATR_PERIOD - 1.0) / cfg::ATR_PERIOD + tr / cfg::ATR_PERIOD;
        } else {
            double sum = 0.0;
            for (double t : tr_history) sum += t;
            atr_pts = sum / tr_history.size();
        }
        atr_pts = std::max(cfg::ATR_MIN_CLAMP, atr_pts);

        // Rolling tick volume mean
        tick_counts.push_back(bar.tick_count);
        if ((int)tick_counts.size() > 20) tick_counts.pop_front();
        double tc_sum = 0.0;
        for (int tc : tick_counts) tc_sum += tc;
        rolling_tv_mean = tc_sum / tick_counts.size();
    }
};

// =============================================================================
// SIMULATED TRADE
// =============================================================================
struct SimTrade {
    bool    active       = false;
    bool    is_long      = true;
    double  entry_px     = 0.0;
    double  sl_px        = 0.0;
    double  tp_px        = 0.0;
    double  atr_at_entry = 5.0;
    int64_t entry_time   = 0;
    int     entry_bar    = 0;
    double  mfe_pts      = 0.0;
    double  mae_pts      = 0.0;
};

// =============================================================================
// PERFORMANCE METRICS
// =============================================================================
struct PerformanceMetrics {
    int    total_trades    = 0;
    int    wins            = 0;
    int    losses          = 0;
    double total_pnl       = 0.0;
    double gross_wins      = 0.0;
    double gross_losses    = 0.0;
    double max_drawdown    = 0.0;
    double peak_equity     = 0.0;
    std::vector<double> pnl_series;

    void record(double pnl) {
        total_trades++;
        total_pnl += pnl;
        pnl_series.push_back(pnl);
        if (pnl > 0) { wins++; gross_wins += pnl; }
        else         { losses++; gross_losses += std::fabs(pnl); }
        if (total_pnl > peak_equity) peak_equity = total_pnl;
        const double dd = peak_equity - total_pnl;
        if (dd > max_drawdown) max_drawdown = dd;
    }

    double win_rate() const { return total_trades > 0 ? (double)wins / total_trades : 0.0; }
    double profit_factor() const { return gross_losses > 0 ? gross_wins / gross_losses : 999.0; }
    double avg_pnl() const { return total_trades > 0 ? total_pnl / total_trades : 0.0; }
    double avg_win() const { return wins > 0 ? gross_wins / wins : 0.0; }
    double avg_loss() const { return losses > 0 ? gross_losses / losses : 0.0; }

    void print(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  "
                   "avg=$%.3f  maxDD=$%.2f\n",
                   label, total_trades,
                   win_rate() * 100.0, profit_factor(),
                   total_pnl, avg_pnl(), max_drawdown);
    }
    void print_detail(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  "
                   "avgW=$%.3f  avgL=$%.3f  maxDD=$%.2f\n",
                   label, total_trades,
                   win_rate() * 100.0, profit_factor(),
                   total_pnl, avg_win(), avg_loss(), max_drawdown);
    }
};

struct ExitDiagnostics {
    int sl_hit = 0, tp_hit = 0, time_stop = 0;
    void print() const {
        std::printf("  Exit breakdown: SL=%d  TP=%d  TimeStop=%d\n", sl_hit, tp_hit, time_stop);
    }
};

// =============================================================================
// FVG SCORING
// =============================================================================
inline double clamp01(double x) { return std::max(0.0, std::min(1.0, x)); }

double score_fvg(const Bar15m& b0, const Bar15m& b1, const Bar15m& b2,
                 double gap, double atr, double rolling_tv_mean) {
    // s_gap: gap size relative to ATR
    double s_gap = clamp01((gap / atr - 0.10) / 1.90);

    // s_disp: displacement bar quality (body fraction * range/ATR)
    double body_b1 = std::fabs(b1.close - b1.open);
    double range_b1 = b1.high - b1.low;
    double body_frac = (range_b1 > 0.001) ? body_b1 / range_b1 : 0.0;
    double s_disp = clamp01(body_frac * clamp01(range_b1 / atr / 2.0));

    // s_tv: tick volume of displacement bar vs rolling mean
    double tv_ratio = (rolling_tv_mean > 1.0) ? (double)b1.tick_count / rolling_tv_mean : 1.0;
    double s_tv = clamp01((tv_ratio - 1.0) / 2.0);

    return (s_gap * cfg::W_GAP + s_disp * cfg::W_DISP + s_tv * cfg::W_TV) / cfg::W_SUM;
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <tick_csv> [--verbose]\n", argv[0]);
        return 1;
    }

    std::string csv_path = argv[1];
    bool verbose = (argc > 2 && std::strcmp(argv[2], "--verbose") == 0);

    PerformanceMetrics overall, long_trades, short_trades;
    std::array<PerformanceMetrics, 24> per_hour;
    PerformanceMetrics is_metrics, oos_metrics;
    ExitDiagnostics exit_diag;

    int fvgs_detected = 0, fvgs_mitigated = 0, fvgs_expired = 0, fvgs_low_score = 0;

    FvgIndicators ind;
    SimTrade trade;
    std::vector<FvgZone> pending_fvgs;

    int64_t oos_split_ts = 0;
    bool oos_split_set = false;
    int64_t first_ts = 0, last_ts = 0;
    size_t total_ticks = 0;
    double min_px = 99999999, max_px = 0;

    std::printf("[FVG-BT] Streaming XAUUSD FVG backtest from %s...\n", csv_path.c_str());
    std::printf("[FVG-BT] Config: 15m bars, SL=%.1f*ATR  TP=%.1f*ATR  score_cutoff=%.2f\n",
               cfg::SL_ATR_MULT, cfg::TP_ATR_MULT, cfg::SCORE_CUTOFF);
    std::printf("[FVG-BT] Time stop: %d bars (%.1f hours)  |  Direction: BOTH\n\n",
               cfg::TIME_STOP_BARS, cfg::TIME_STOP_BARS * 15.0 / 60.0);

    std::ifstream f(csv_path);
    if (!f.is_open()) {
        std::fprintf(stderr, "Cannot open %s\n", csv_path.c_str());
        return 1;
    }

    // Skip header if present
    std::string first_line;
    std::getline(f, first_line);
    bool has_header = (first_line.find("timestamp") != std::string::npos ||
                       first_line.find("Time") != std::string::npos);

    auto parse_duka = [](const std::string& line, int64_t& ts, double& bid, double& ask) -> bool {
        return std::sscanf(line.c_str(), "%lld,%lf,%lf",
                           (long long*)&ts, &ask, &bid) == 3;
    };

    auto process_tick = [&](int64_t ts_ms, double bid, double ask) {
        if (bid <= 0 || ask <= 0 || ask < bid) return;
        const double mid = (bid + ask) * 0.5;
        if (mid < 1000.0 || mid > 5000.0) return;  // XAUUSD sanity

        total_ticks++;
        if (mid < min_px) min_px = mid;
        if (mid > max_px) max_px = mid;
        if (first_ts == 0) first_ts = ts_ms;
        last_ts = ts_ms;

        if (!oos_split_set && first_ts > 0) {
            const double total_range_ms = 26.0 * 30.0 * 24.0 * 3600.0 * 1000.0; // ~26 months
            oos_split_ts = first_ts + (int64_t)(total_range_ms * cfg::OOS_SPLIT_FRAC);
            oos_split_set = true;
        }

        if (cfg::is_weekend(ts_ms)) return;

        bool new_bar = ind.on_tick(mid, ts_ms);
        if (ind.total_bars < 16) return;  // warmup

        // ── Manage open position (per-tick) ──────────────────────────
        if (trade.active) {
            const double fav = trade.is_long ? (mid - trade.entry_px)
                                             : (trade.entry_px - mid);
            if (fav > trade.mfe_pts) trade.mfe_pts = fav;
            if (fav < trade.mae_pts) trade.mae_pts = fav;

            bool sl_hit = trade.is_long ? (bid <= trade.sl_px) : (ask >= trade.sl_px);
            bool tp_hit = trade.is_long ? (bid >= trade.tp_px) : (ask <= trade.tp_px);

            // Time stop check on bar close
            bool time_stop = false;
            if (new_bar && (ind.total_bars - trade.entry_bar) >= cfg::TIME_STOP_BARS) {
                time_stop = true;
            }

            if (sl_hit || tp_hit || time_stop) {
                // S37 audit fix: fill at touch price (bid for long exit,
                // ask for short exit). Filling at literal tp_px/sl_px
                // overstates winners by ~half the spread per trade.
                double exit_px;
                if (tp_hit || sl_hit) exit_px = trade.is_long ? bid : ask;
                else exit_px = trade.is_long ? bid : ask;  // time stop at market

                const double pnl_pts = trade.is_long ? (exit_px - trade.entry_px)
                                                     : (trade.entry_px - exit_px);
                const double pnl_usd = pnl_pts * cfg::PNL_PER_PT;

                overall.record(pnl_usd);
                if (trade.is_long) long_trades.record(pnl_usd);
                else short_trades.record(pnl_usd);

                if (trade.entry_time < oos_split_ts) is_metrics.record(pnl_usd);
                else oos_metrics.record(pnl_usd);

                const int h = static_cast<int>((trade.entry_time / 1000 % 86400) / 3600);
                per_hour[h].record(pnl_usd);

                if (tp_hit) exit_diag.tp_hit++;
                else if (sl_hit) exit_diag.sl_hit++;
                else exit_diag.time_stop++;

                if (verbose) {
                    std::printf("[FVG-CLOSE] %s %s pnl=$%.3f mfe=%.1f mae=%.1f\n",
                               trade.is_long ? "LONG" : "SHORT",
                               tp_hit ? "TP" : (sl_hit ? "SL" : "TIME"),
                               pnl_usd, trade.mfe_pts, trade.mae_pts);
                }
                trade.active = false;
            }
            return;
        }

        // ── On bar close: detect new FVGs + check mitigation ─────────
        if (!new_bar) return;
        if ((int)ind.bars.size() < 3) return;

        // Expire old FVGs
        for (auto& z : pending_fvgs) {
            if (z.active && (ind.total_bars - z.formed_bar) > cfg::MAX_AGE_BARS) {
                z.active = false;
                fvgs_expired++;
            }
        }

        // Detect new FVG from last 3 closed bars
        const Bar15m& b0 = ind.bars[ind.bars.size() - 3];  // oldest
        const Bar15m& b1 = ind.bars[ind.bars.size() - 2];  // displacement
        const Bar15m& b2 = ind.bars[ind.bars.size() - 1];  // formation (just closed)

        // Bullish FVG: b0.high < b2.low (gap up)
        if (b0.high < b2.low) {
            double gap = b2.low - b0.high;
            if (gap >= cfg::GAP_MIN_ATR * ind.atr_pts && gap <= cfg::GAP_MAX_ATR * ind.atr_pts) {
                double sc = score_fvg(b0, b1, b2, gap, ind.atr_pts, ind.rolling_tv_mean);
                FvgZone z;
                z.is_long = true;
                z.zone_low = b0.high;
                z.zone_high = b2.low;
                z.score = sc;
                z.atr_at_form = ind.atr_pts;
                z.formed_bar = ind.total_bars;
                z.formed_ts = ts_ms;
                if ((int)pending_fvgs.size() < cfg::MAX_PENDING) {
                    pending_fvgs.push_back(z);
                    fvgs_detected++;
                }
            }
        }

        // Bearish FVG: b0.low > b2.high (gap down)
        if (b0.low > b2.high) {
            double gap = b0.low - b2.high;
            if (gap >= cfg::GAP_MIN_ATR * ind.atr_pts && gap <= cfg::GAP_MAX_ATR * ind.atr_pts) {
                double sc = score_fvg(b0, b1, b2, gap, ind.atr_pts, ind.rolling_tv_mean);
                FvgZone z;
                z.is_long = false;
                z.zone_low = b2.high;
                z.zone_high = b0.low;
                z.score = sc;
                z.atr_at_form = ind.atr_pts;
                z.formed_bar = ind.total_bars;
                z.formed_ts = ts_ms;
                if ((int)pending_fvgs.size() < cfg::MAX_PENDING) {
                    pending_fvgs.push_back(z);
                    fvgs_detected++;
                }
            }
        }

        // Check mitigation: does the just-closed bar touch any pending FVG?
        const Bar15m& latest = ind.bars.back();
        for (auto& z : pending_fvgs) {
            if (!z.active) continue;
            if (z.score < cfg::SCORE_CUTOFF) {
                z.active = false;
                fvgs_low_score++;
                continue;
            }

            bool mitigated = (latest.high >= z.zone_low && latest.low <= z.zone_high);
            if (!mitigated) continue;

            fvgs_mitigated++;
            z.active = false;

            // Open trade
            trade.active       = true;
            trade.is_long      = z.is_long;
            trade.entry_px     = z.is_long ? z.zone_high : z.zone_low;
            trade.atr_at_entry = ind.atr_pts;
            trade.entry_time   = ts_ms;
            trade.entry_bar    = ind.total_bars;
            trade.mfe_pts      = 0.0;
            trade.mae_pts      = 0.0;

            const double sl_pts = ind.atr_pts * cfg::SL_ATR_MULT;
            const double tp_pts = ind.atr_pts * cfg::TP_ATR_MULT;

            if (z.is_long) {
                trade.sl_px = trade.entry_px - sl_pts;
                trade.tp_px = trade.entry_px + tp_pts;
            } else {
                trade.sl_px = trade.entry_px + sl_pts;
                trade.tp_px = trade.entry_px - tp_pts;
            }

            if (verbose) {
                std::printf("[FVG-ENTRY] %s @ %.2f sl=%.2f tp=%.2f score=%.2f atr=%.1f\n",
                           z.is_long ? "LONG" : "SHORT",
                           trade.entry_px, trade.sl_px, trade.tp_px,
                           z.score, ind.atr_pts);
            }
            break;  // one trade at a time
        }

        // Clean up inactive zones
        pending_fvgs.erase(
            std::remove_if(pending_fvgs.begin(), pending_fvgs.end(),
                           [](const FvgZone& z) { return !z.active; }),
            pending_fvgs.end());
    };

    // Process first line if not header
    if (!has_header) {
        int64_t ts; double bid, ask;
        if (parse_duka(first_line, ts, bid, ask)) process_tick(ts, bid, ask);
    }

    std::string line;
    size_t line_count = 0;
    while (std::getline(f, line)) {
        int64_t ts; double bid, ask;
        if (parse_duka(line, ts, bid, ask)) process_tick(ts, bid, ask);
        if (++line_count % 10000000 == 0) std::printf("  ... %zuM ticks processed\n", line_count / 1000000);
    }

    // ── RESULTS ──────────────────────────────────────────────────────────
    const double hours = (last_ts > first_ts) ? (last_ts - first_ts) / 3600000.0 : 0.0;

    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  XAUUSD FVG BACKTEST RESULTS (15m bars, BOTH DIRECTIONS)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    std::printf("  Ticks: %zu  |  Period: %.1f days\n", total_ticks, hours / 24.0);
    std::printf("  Price range: %.1f - %.1f\n", min_px, max_px);
    std::printf("  Bars: %d (15-minute)  |  Final ATR: %.2f\n\n", ind.total_bars, ind.atr_pts);

    std::printf("  FVGs detected: %d  |  Mitigated (traded): %d\n", fvgs_detected, fvgs_mitigated);
    std::printf("  Low score filtered: %d  |  Expired: %d\n\n", fvgs_low_score, fvgs_expired);

    std::printf("── OVERALL ─────────────────────────────────────────────────\n");
    overall.print("TOTAL");
    overall.print_detail("TOTAL (detail)");
    long_trades.print("LONG");
    short_trades.print("SHORT");
    exit_diag.print();
    std::printf("\n");

    std::printf("── PF BY HOUR (UTC) ───────────────────────────────────────\n");
    std::printf("  Hour  Trades   WR%%    PF      PnL\n");
    for (int h = 0; h < 24; ++h) {
        if (per_hour[h].total_trades > 0) {
            std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%8.2f  %s\n",
                       h, per_hour[h].total_trades,
                       per_hour[h].win_rate() * 100.0,
                       per_hour[h].profit_factor(),
                       per_hour[h].total_pnl,
                       per_hour[h].profit_factor() >= 1.0 ? " <<<" : "");
        }
    }
    std::printf("\n");

    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  IN-SAMPLE / OUT-OF-SAMPLE (60/40 split)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");
    is_metrics.print("IS TOTAL");
    oos_metrics.print("OOS TOTAL");
    const bool oos_pass = (oos_metrics.total_trades >= 8 && oos_metrics.profit_factor() >= 1.20);
    std::printf("\n  OOS VERDICT: %s\n",
               oos_pass ? "PASS" : "REVIEW — needs filtering or insufficient trades.");
    if (oos_metrics.total_trades > 0 && is_metrics.total_trades > 0) {
        const double pf_decay = 1.0 - oos_metrics.profit_factor() / is_metrics.profit_factor();
        std::printf("  PF decay IS->OOS: %.0f%%\n", pf_decay * 100.0);
    }
    std::printf("\n");

    return 0;
}
