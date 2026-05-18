// =============================================================================
// gold_session_breakout_bt.cpp -- Backtest harness for GoldSessionBreakoutEngine
// =============================================================================
//
// Build (Mac/Linux):
//   clang++ -O3 -std=c++17 -o backtest/gold_session_breakout_bt \
//           backtest/gold_session_breakout_bt.cpp
//
// Run:
//   ./backtest/gold_session_breakout_bt /path/to/XAUUSD_combined.csv
//   or
//   ./backtest/gold_session_breakout_bt   (default = Dukascopy combined)
//
// What this does:
//   1. Streams Dukascopy combined CSV (timestamp_ms,askPrice,bidPrice).
//   2. Builds synthetic M5 bars from ticks (same as the engine internally).
//   3. Builds synthetic H4 bars from ticks (4-hour UTC-aligned). Maintains
//      H4 EMA9 + EMA50 from H4 closes. H4 trend state = +1 if EMA9>EMA50,
//      -1 if EMA9<EMA50, 0 if either unprimed.
//      In production this same state comes from g_bars_gold.h4.ind
//      .trend_state -- the harness reproduces the value, not the engine's
//      internal model.
//   4. Drives GoldSessionBreakoutEngine::on_tick() with bid/ask/ts +
//      computed h4_trend.
//   5. Sweep grid: 24 configs varying the three new filters (+ pyramid).
//   6. Reports per-config: N, WR%, PnL$, PF, DD$, exit-mix.
//
// Cost / PnL convention (S99h):
//   Engine reports tr.pnl in pts*lots. trade_lifecycle.hpp would multiply
//   by tick_value_multiplier=100 in production. Harness does the same
//   scaling at summarize() time so dollar figures are comparable to live.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <functional>

#include "../include/GoldSessionBreakoutEngine.hpp"

// -----------------------------------------------------------------------------
// Sweep config
// -----------------------------------------------------------------------------
struct Config {
    bool   h4_gate_enabled;
    int    session_start_hr;
    int    session_end_hr;
    double atr_spike_mult;
    bool   pyramid_on;
};

static const char* h4_label(bool on) { return on ? "H4y" : "H4n"; }
static const char* pyr_label(bool on) { return on ? "PyrY" : "PyrN"; }

// -----------------------------------------------------------------------------
// Per-config result
// -----------------------------------------------------------------------------
struct Result {
    Config cfg;
    int    n_trades       = 0;
    int    n_wins         = 0;
    int    n_losses       = 0;
    int    n_trail_hit    = 0;
    int    n_tp_hit       = 0;
    int    n_sl_hit       = 0;
    int    n_loss_cut     = 0;
    int    n_be_cut       = 0;
    int    n_time_stop    = 0;
    int    max_layers     = 0;
    double gross_pnl_usd  = 0.0;
    double max_dd_usd     = 0.0;
    double profit_factor  = 0.0;
    double wr_pct         = 0.0;
    double avg_win        = 0.0;
    double avg_loss       = 0.0;
    int    n_pyramid      = 0;  // trades with > 1 layer
};

// -----------------------------------------------------------------------------
// H4 trend tracker -- builds H4 bars from ticks, computes EMA9/EMA50 crossover
// -----------------------------------------------------------------------------
struct H4TrendTracker {
    static constexpr int64_t H4_SECS = 4 * 3600;

    int64_t cur_anchor_h4 = -1;
    double  cur_close_h4  = 0.0;

    struct EMA {
        int period = 0; double value = 0.0; double alpha = 0.0;
        int count = 0; bool primed = false;
        void init(int p) { period = p; alpha = 2.0/(p+1.0); value = 0.0; count = 0; primed = false; }
        void push(double v) {
            if (!primed) {
                value += v; ++count;
                if (count >= period) { value /= period; primed = true; }
            } else {
                value = alpha * v + (1.0 - alpha) * value;
            }
        }
    } ema9, ema50;

    bool inited = false;

    void reset() {
        cur_anchor_h4 = -1;
        cur_close_h4  = 0.0;
        ema9.init(9);
        ema50.init(50);
        inited = true;
    }

    // Feed every tick; H4 bars are detected by anchor change.
    void on_tick(int64_t ts_ms, double mid) {
        if (!inited) reset();
        const int64_t ts_s   = ts_ms / 1000;
        const int64_t anchor = (ts_s / H4_SECS) * H4_SECS;

        if (cur_anchor_h4 < 0) {
            cur_anchor_h4 = anchor;
            cur_close_h4  = mid;
        } else if (anchor != cur_anchor_h4) {
            // Prior H4 closed -- push its close into EMAs
            ema9 .push(cur_close_h4);
            ema50.push(cur_close_h4);
            cur_anchor_h4 = anchor;
            cur_close_h4  = mid;
        } else {
            cur_close_h4 = mid;
        }
    }

    int trend_state() const {
        if (!ema9.primed || !ema50.primed) return 0;
        if (ema9.value > ema50.value) return +1;
        if (ema9.value < ema50.value) return -1;
        return 0;
    }
};

// -----------------------------------------------------------------------------
// Driver: streams ticks, runs engine, accumulates trades
// -----------------------------------------------------------------------------
struct Driver {
    omega::GoldSessionBreakoutEngine engine;
    H4TrendTracker                   h4;

    std::vector<omega::TradeRecord>  trades;
    double cum_pnl_usd = 0.0;
    double peak_pnl    = 0.0;
    double max_dd      = 0.0;

    static constexpr double XAUUSD_TICK_VALUE = 100.0;

    Driver() {
        engine.on_close_cb = [this](const omega::TradeRecord& tr) {
            trades.push_back(tr);
            // tr.pnl is pts*lots; production multiplies by tick_value=100.
            const double pnl_usd = tr.pnl * XAUUSD_TICK_VALUE;
            cum_pnl_usd += pnl_usd;
            if (cum_pnl_usd > peak_pnl) peak_pnl = cum_pnl_usd;
            const double dd = peak_pnl - cum_pnl_usd;
            if (dd > max_dd) max_dd = dd;
        };
    }

    void reset() {
        engine.m_pos = decltype(engine.m_pos){};
        h4.reset();
        trades.clear();
        cum_pnl_usd = 0.0;
        peak_pnl    = 0.0;
        max_dd      = 0.0;
    }

    void on_tick(int64_t ts_ms, double bid, double ask) {
        const double mid = (bid + ask) * 0.5;
        h4.on_tick(ts_ms, mid);

        engine.on_tick(bid, ask, ts_ms,
                       /*can_enter=*/true,
                       h4.trend_state(),
                       /*l2_imbalance=*/0.5,
                       /*l2_real=*/false);
    }
};

// -----------------------------------------------------------------------------
// CSV streamer
// -----------------------------------------------------------------------------
static int64_t parse_csv_and_run(Driver& drv, const char* path, const Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[GSB-BT] FAIL: cannot open %s\n", path);
        return -1;
    }

    std::string line;
    int64_t n_ticks = 0, n_skipped = 0;

    if (std::getline(f, line)) {
        if (!line.empty() && !std::isdigit((unsigned char)line[0])) {
            // header, drop
        } else {
            f.clear();
            f.seekg(0);
        }
    }

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        char* p1;
        const int64_t ts_ms = std::strtoll(line.c_str(), &p1, 10);
        if (!p1 || *p1 != ',') { ++n_skipped; continue; }
        char* p2;
        const double ask = std::strtod(p1 + 1, &p2);
        if (!p2 || *p2 != ',') { ++n_skipped; continue; }
        char* p3;
        double bid = std::strtod(p2 + 1, &p3);
        (void)p3;

        if (!std::isfinite(ask) || !std::isfinite(bid)) { ++n_skipped; continue; }
        if (ask <= 0.0 || bid <= 0.0) { ++n_skipped; continue; }
        double a = ask, b = bid;
        if (b > a) std::swap(a, b);
        const double spread = a - b;
        if (spread > 5.0) { ++n_skipped; continue; }

        drv.on_tick(ts_ms, b, a);
        ++n_ticks;

        if (n_ticks % 20000000LL == 0) {
            std::fprintf(stderr,
                "[GSB-BT] %lldM ticks (%s ses=%d-%d ATRx=%.1f %s) "
                "cum_pnl=$%.2f n_trades=%zu\n",
                (long long)(n_ticks / 1000000LL),
                h4_label(cfg.h4_gate_enabled),
                cfg.session_start_hr, cfg.session_end_hr,
                cfg.atr_spike_mult, pyr_label(cfg.pyramid_on),
                drv.cum_pnl_usd, drv.trades.size());
        }
    }

    std::fprintf(stderr, "[GSB-BT] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

// -----------------------------------------------------------------------------
// Apply config to engine
// -----------------------------------------------------------------------------
static void apply_config(omega::GoldSessionBreakoutEngine& e, const Config& c) {
    e.H4_GATE_ENABLED        = c.h4_gate_enabled;
    e.SESSION_START_HOUR_UTC = c.session_start_hr;
    e.SESSION_END_HOUR_UTC   = c.session_end_hr;
    e.ATR_SPIKE_MULT         = c.atr_spike_mult;
    e.PYRAMID_ON             = c.pyramid_on;
    // Core (hold at GSP best)
    e.LOOKBACK     = 8;
    e.SL_ATR_MULT  = 1.5;
    e.TP_ATR_MULT  = 3.0;
    e.TRAIL_TIGHT  = 0.12;
    // S63 protection DISABLED for sweep -- the validated GSP $15K/PF1.45
    // result was on a no-S63 version of the engine. LOSS_CUT_PCT=0.05 at
    // base_entry=4500 = 2.25pt cold cut, which fires AT OR BEFORE the hard
    // SL at 1.5*ATR for most ATR values, amputating the trail before it
    // can develop. Setting these to 0.0 reproduces the shape the GSP
    // sweep validated. If we want to test S63 separately, we add it as a
    // fifth knob in the sweep grid -- but baseline first.
    e.LOSS_CUT_PCT  = 0.0;
    e.BE_ARM_PCT    = 0.0;
    e.BE_BUFFER_PCT = 0.0;
    e.L2_GATE_ENABLED = false;
    e.shadow_mode = true;
    e.enabled     = true;       // harness override -- production keeps false
}

// -----------------------------------------------------------------------------
// Summarise
// -----------------------------------------------------------------------------
static Result summarize(const Config& cfg,
                        const std::vector<omega::TradeRecord>& trades,
                        double max_dd_usd)
{
    constexpr double XAUUSD_TICK_VALUE = 100.0;

    Result r;
    r.cfg        = cfg;
    r.max_dd_usd = max_dd_usd;
    r.n_trades   = (int)trades.size();

    double gross_win = 0.0, gross_loss = 0.0;
    for (const auto& t : trades) {
        const double pnl_usd = t.pnl * XAUUSD_TICK_VALUE;
        r.gross_pnl_usd += pnl_usd;
        if (pnl_usd > 0.0)      { ++r.n_wins;   gross_win  += pnl_usd; }
        else if (pnl_usd < 0.0) { ++r.n_losses; gross_loss += -pnl_usd; }

        if      (t.exitReason == "TRAIL_HIT") ++r.n_trail_hit;
        else if (t.exitReason == "TP_HIT")    ++r.n_tp_hit;
        else if (t.exitReason == "SL_HIT")    ++r.n_sl_hit;
        else if (t.exitReason == "LOSS_CUT")  ++r.n_loss_cut;
        else if (t.exitReason == "BE_CUT")    ++r.n_be_cut;
        else if (t.exitReason == "TIME_STOP") ++r.n_time_stop;

        // Detect pyramid use via size > base lot
        if (t.size > 0.051) ++r.n_pyramid;
    }
    r.wr_pct        = (r.n_trades > 0) ? (100.0 * r.n_wins / r.n_trades) : 0.0;
    r.profit_factor = (gross_loss > 1e-9) ? (gross_win / gross_loss) : 0.0;
    r.avg_win       = (r.n_wins   > 0) ? (gross_win  / r.n_wins)   : 0.0;
    r.avg_loss      = (r.n_losses > 0) ? (-gross_loss / r.n_losses) : 0.0;
    return r;
}

// -----------------------------------------------------------------------------
// Main: parameter sweep
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";

    std::fprintf(stderr, "[GSB-BT] CSV: %s\n", csv_path);

    // ---- Sweep grid: 24 configs --------------------------------------------
    // Cross product:
    //   h4_gate           : {false, true}                                (2)
    //   session window    : {(7, 21), (12, 17)}                          (2)
    //   atr_spike_mult    : {1.5, 2.0, 99.0}   (99 = filter disabled)    (3)
    //   pyramid_on        : {false, true}                                (2)
    std::vector<Config> configs;
    const bool   h4_grid[]     = {false, true};
    struct SessionPair { int s; int e; };
    const SessionPair ses_grid[]  = {{7, 21}, {12, 17}};
    const double atr_grid[]    = {1.5, 2.0, 99.0};
    const bool   pyr_grid[]    = {false, true};

    for (bool h4 : h4_grid)
        for (SessionPair sp : ses_grid)
            for (double a : atr_grid)
                for (bool p : pyr_grid)
                    configs.push_back({h4, sp.s, sp.e, a, p});

    std::printf("============================================================================\n");
    std::printf("  GoldSessionBreakoutEngine sweep -- %zu configs\n", configs.size());
    std::printf("  Core held at GSP best: LB=8 SL=1.5 TP=3.0 Trail=0.12\n");
    std::printf("  CSV: %s\n", csv_path);
    std::printf("============================================================================\n\n");

    std::vector<Result> results;
    results.reserve(configs.size());

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr,
            "[GSB-BT] === Cfg %zu/%zu: H4=%d Ses=%d-%d ATRx=%.1f Pyr=%d ===\n",
            i + 1, configs.size(),
            (int)cfg.h4_gate_enabled,
            cfg.session_start_hr, cfg.session_end_hr,
            cfg.atr_spike_mult, (int)cfg.pyramid_on);

        Driver drv;
        drv.reset();
        apply_config(drv.engine, cfg);

        const auto t0 = std::chrono::steady_clock::now();
        const int64_t n_ticks = parse_csv_and_run(drv, csv_path, cfg);
        const auto t1 = std::chrono::steady_clock::now();
        if (n_ticks < 0) {
            std::fprintf(stderr, "[GSB-BT] CSV read failed, aborting\n");
            return 1;
        }
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        Result r = summarize(cfg, drv.trades, drv.max_dd);
        results.push_back(r);

        std::fprintf(stderr,
            "[GSB-BT] Cfg %zu done in %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f dd=$%.2f "
            "(TR=%d TP=%d SL=%d LC=%d BE=%d TS=%d pyr=%d)\n\n",
            i + 1, secs,
            r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor, r.max_dd_usd,
            r.n_trail_hit, r.n_tp_hit, r.n_sl_hit,
            r.n_loss_cut, r.n_be_cut, r.n_time_stop, r.n_pyramid);
    }

    // Sort by PnL descending
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) {
                  return a.gross_pnl_usd > b.gross_pnl_usd;
              });

    std::printf("\n=========================================================================================================================\n");
    std::printf("  RESULTS (sorted by gross PnL)\n");
    std::printf("=========================================================================================================================\n");
    std::printf("%-4s %-6s %-7s %-5s %-6s %-7s %-12s %-7s %-9s %-7s %-8s %-5s %-5s %-5s %-5s %-5s\n",
                "H4", "Ses", "ATRx", "Pyr",
                "N", "WR%", "PnL$", "PF", "DD$",
                "AvgWin", "AvgLoss",
                "TR", "TP", "SL", "LC", "BE");
    std::printf("-------------------------------------------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        char ses_buf[16];
        std::snprintf(ses_buf, sizeof(ses_buf), "%d-%d",
                     r.cfg.session_start_hr, r.cfg.session_end_hr);
        std::printf("%-4s %-6s %-7.1f %-5s %-6d %-7.1f %-+12.2f %-7.2f %-9.2f %+7.2f %+8.2f %-5d %-5d %-5d %-5d %-5d\n",
                    h4_label(r.cfg.h4_gate_enabled), ses_buf,
                    r.cfg.atr_spike_mult, pyr_label(r.cfg.pyramid_on),
                    r.n_trades, r.wr_pct, r.gross_pnl_usd,
                    r.profit_factor, r.max_dd_usd,
                    r.avg_win, r.avg_loss,
                    r.n_trail_hit, r.n_tp_hit, r.n_sl_hit,
                    r.n_loss_cut, r.n_be_cut);
    }
    std::printf("=========================================================================================================================\n");

    if (!results.empty()) {
        const auto& best = results.front();
        std::printf("\nBest config:\n");
        std::printf("  H4_GATE_ENABLED   = %s\n", best.cfg.h4_gate_enabled ? "true" : "false");
        std::printf("  SESSION_WINDOW    = %d-%d UTC\n", best.cfg.session_start_hr, best.cfg.session_end_hr);
        std::printf("  ATR_SPIKE_MULT    = %.2f%s\n",
                    best.cfg.atr_spike_mult,
                    (best.cfg.atr_spike_mult >= 99.0) ? "  (filter disabled)" : "");
        std::printf("  PYRAMID_ON        = %s\n", best.cfg.pyramid_on ? "true" : "false");
        std::printf("  N=%d  WR=%.1f%%  PnL=$%+.2f  PF=%.2f  DD=$%.2f\n",
                    best.n_trades, best.wr_pct, best.gross_pnl_usd,
                    best.profit_factor, best.max_dd_usd);
        std::printf("  AvgWin=$%+.2f  AvgLoss=$%+.2f  Pyramid-trades=%d\n",
                    best.avg_win, best.avg_loss, best.n_pyramid);
        std::printf("  Exits: TRAIL=%d  TP=%d  SL=%d  LOSS_CUT=%d  BE_CUT=%d  TIME=%d\n",
                    best.n_trail_hit, best.n_tp_hit, best.n_sl_hit,
                    best.n_loss_cut, best.n_be_cut, best.n_time_stop);
    }

    return 0;
}
