// =============================================================================
// gsp_s63_audit_bt.cpp -- Validate GSP class S63 protection against same tape
// =============================================================================
//
// Build (Mac/Linux):
//   clang++ -O3 -std=c++17 -o backtest/gsp_s63_audit_bt \
//           backtest/gsp_s63_audit_bt.cpp
//
// Run:
//   ./backtest/gsp_s63_audit_bt /path/to/XAUUSD_combined.csv
//   or
//   ./backtest/gsp_s63_audit_bt   (default = Dukascopy combined)
//
// PURPOSE:
//   The validated GSP best-config sweep ($15,084 / PF 1.45 / 71% WR / 5436
//   trades over 26 months -- see backtest/gold_scalp_pyramid_results.txt)
//   was run by backtest/gold_scalp_pyramid_bt.cpp, which has its OWN inline
//   re-implementation of the entry+management logic. That harness has zero
//   references to LOSS_CUT_PCT / BE_ARM_PCT / BE_BUFFER_PCT -- i.e. the
//   $15K result is for a NO-S63 version of GSP.
//
//   The GoldScalpPyramidEngine class (used by the live binary) has S63
//   added on TOP of that shape, with LOSS_CUT_PCT=0.05 / BE_ARM_PCT=0.03
//   / BE_BUFFER_PCT=0.012 set in engine_init.hpp:372-374. Those values
//   have never been independently swept.
//
//   This harness drives the class directly and runs two configs:
//     Config A: S63 OFF  (LOSS_CUT_PCT = BE_ARM_PCT = BE_BUFFER_PCT = 0)
//     Config B: S63 ON   (the live values 0.05 / 0.03 / 0.012)
//
//   Both use the GSP best-config core: LB=8, SL=1.5, TP=3.0, Trail=0.12,
//   Pyr=Y, session 07-21 UTC, NO L2 (l2_real=false).
//
// What we learn:
//   * Does S63 ON match A's PnL? -> S63 is harmless on this tape, keep it.
//   * Does S63 ON drop PnL significantly? -> S63 hurts the validated edge,
//     consider disabling it on live GSP.
//   * Does S63 ON LIFT PnL? -> S63 adds value, the live config is correct.
//
// COST CONVENTION (S99h):
//   The GSP class (post-S99h fix) reports tr.pnl in pts*lots. This harness
//   multiplies by tick_value=100 at summarize() time to display USD.
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
#include <chrono>
#include <algorithm>
#include <functional>

#include "../include/GoldScalpPyramidEngine.hpp"

// -----------------------------------------------------------------------------
// Sweep config: the two S63 states
// -----------------------------------------------------------------------------
struct Config {
    bool   s63_on;
    double loss_cut_pct;
    double be_arm_pct;
    double be_buffer_pct;
    const char* label;
};

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
    int    n_pyramid      = 0;
    double gross_pnl_usd  = 0.0;
    double max_dd_usd     = 0.0;
    double profit_factor  = 0.0;
    double wr_pct         = 0.0;
    double avg_win        = 0.0;
    double avg_loss       = 0.0;
};

// -----------------------------------------------------------------------------
// Driver: streams ticks into the GSP class, accumulates trades
// -----------------------------------------------------------------------------
struct Driver {
    omega::GoldScalpPyramidEngine engine;

    std::vector<omega::TradeRecord> trades;
    double cum_pnl_usd = 0.0;
    double peak_pnl    = 0.0;
    double max_dd      = 0.0;

    static constexpr double XAUUSD_TICK_VALUE = 100.0;

    Driver() {
        engine.on_close_cb = [this](const omega::TradeRecord& tr) {
            trades.push_back(tr);
            // S99h: tr.pnl is pts*lots; production scales by tick_value=100.
            const double pnl_usd = tr.pnl * XAUUSD_TICK_VALUE;
            cum_pnl_usd += pnl_usd;
            if (cum_pnl_usd > peak_pnl) peak_pnl = cum_pnl_usd;
            const double dd = peak_pnl - cum_pnl_usd;
            if (dd > max_dd) max_dd = dd;
        };
    }

    void reset() {
        engine.m_pos = decltype(engine.m_pos){};
        trades.clear();
        cum_pnl_usd = 0.0;
        peak_pnl    = 0.0;
        max_dd      = 0.0;
    }

    void on_tick(int64_t ts_ms, double bid, double ask) {
        // Feed stub L2 (l2_real=false -> all L2 filters in the engine
        // degrade to neutral, matching the Dukascopy no-DOM environment).
        engine.on_tick(bid, ask, ts_ms,
                       /*can_enter=*/true,
                       /*l2_imbalance=*/0.5,
                       /*book_slope=*/0.0,
                       /*vacuum_ask=*/false,
                       /*vacuum_bid=*/false,
                       /*wall_above=*/false,
                       /*wall_below=*/false,
                       /*l2_real=*/false,
                       /*ext_close=*/nullptr);
    }
};

// -----------------------------------------------------------------------------
// CSV streamer
// -----------------------------------------------------------------------------
static int64_t parse_csv_and_run(Driver& drv, const char* path, const Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[GSP-AUDIT] FAIL: cannot open %s\n", path);
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
                "[GSP-AUDIT] %lldM ticks (%s) cum_pnl=$%.2f n_trades=%zu\n",
                (long long)(n_ticks / 1000000LL),
                cfg.label, drv.cum_pnl_usd, drv.trades.size());
        }
    }

    std::fprintf(stderr, "[GSP-AUDIT] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

// -----------------------------------------------------------------------------
// Apply config to engine -- holds GSP best-config core constant, varies S63
// -----------------------------------------------------------------------------
static void apply_config(omega::GoldScalpPyramidEngine& e, const Config& c) {
    // ---- GSP best-config core (held constant across both configs) ----
    e.LOOKBACK     = 8;
    e.SL_ATR_MULT  = 1.5;
    e.TP_ATR_MULT  = 3.0;
    e.TRAIL_TIGHT  = 0.12;
    e.PYRAMID_ON   = true;

    // ---- S63 -- the variable under test ----
    e.LOSS_CUT_PCT  = c.loss_cut_pct;
    e.BE_ARM_PCT    = c.be_arm_pct;
    e.BE_BUFFER_PCT = c.be_buffer_pct;

    // ---- Shadow + enable (harness only) ----
    e.shadow_mode = true;
    e.enabled     = true;
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

        if (t.size > 0.051) ++r.n_pyramid;
    }
    r.wr_pct        = (r.n_trades > 0) ? (100.0 * r.n_wins / r.n_trades) : 0.0;
    r.profit_factor = (gross_loss > 1e-9) ? (gross_win / gross_loss) : 0.0;
    r.avg_win       = (r.n_wins   > 0) ? (gross_win  / r.n_wins)   : 0.0;
    r.avg_loss      = (r.n_losses > 0) ? (-gross_loss / r.n_losses) : 0.0;
    return r;
}

// -----------------------------------------------------------------------------
// Main: run 2 configs, A=S63-OFF and B=S63-ON, print side-by-side comparison
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";

    std::fprintf(stderr, "[GSP-AUDIT] CSV: %s\n", csv_path);

    const std::vector<Config> configs = {
        // A: S63 OFF -- matches the validated GSP backtest harness shape
        { false, 0.0,  0.0,  0.0,   "A:S63-OFF" },
        // B: S63 ON  -- matches the live engine_init.hpp:372-374 values
        { true,  0.05, 0.03, 0.012, "B:S63-ON"  },
    };

    std::printf("============================================================================\n");
    std::printf("  GSP S63 Audit -- compare live S63 config vs validated no-S63 shape\n");
    std::printf("  Core held at GSP best: LB=8 SL=1.5 TP=3.0 Trail=0.12 Pyr=Y\n");
    std::printf("  CSV: %s\n", csv_path);
    std::printf("============================================================================\n\n");

    std::vector<Result> results;
    results.reserve(configs.size());

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr,
            "[GSP-AUDIT] === Cfg %zu/%zu: %s LC=%.3f BE=%.3f BUF=%.3f ===\n",
            i + 1, configs.size(),
            cfg.label, cfg.loss_cut_pct, cfg.be_arm_pct, cfg.be_buffer_pct);

        Driver drv;
        drv.reset();
        apply_config(drv.engine, cfg);

        const auto t0 = std::chrono::steady_clock::now();
        const int64_t n_ticks = parse_csv_and_run(drv, csv_path, cfg);
        const auto t1 = std::chrono::steady_clock::now();
        if (n_ticks < 0) return 1;
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        Result r = summarize(cfg, drv.trades, drv.max_dd);
        results.push_back(r);

        std::fprintf(stderr,
            "[GSP-AUDIT] Cfg %zu (%s) done in %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f dd=$%.2f\n\n",
            i + 1, cfg.label, secs,
            r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor, r.max_dd_usd);
    }

    // -------------------------------------------------------------------------
    // Side-by-side comparison
    // -------------------------------------------------------------------------
    std::printf("\n=========================================================================================================================\n");
    std::printf("  RESULTS\n");
    std::printf("=========================================================================================================================\n");
    std::printf("%-12s %-7s %-7s %-12s %-7s %-9s %-8s %-8s %-5s %-5s %-5s %-5s %-5s %-5s\n",
                "Config", "N", "WR%", "PnL$", "PF", "DD$",
                "AvgWin", "AvgLoss",
                "TR", "TP", "SL", "LC", "BE", "Pyr");
    std::printf("-------------------------------------------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        std::printf("%-12s %-7d %-7.1f %-+12.2f %-7.2f %-9.2f %+8.2f %+8.2f %-5d %-5d %-5d %-5d %-5d %-5d\n",
                    r.cfg.label,
                    r.n_trades, r.wr_pct, r.gross_pnl_usd,
                    r.profit_factor, r.max_dd_usd,
                    r.avg_win, r.avg_loss,
                    r.n_trail_hit, r.n_tp_hit, r.n_sl_hit,
                    r.n_loss_cut, r.n_be_cut, r.n_pyramid);
    }
    std::printf("=========================================================================================================================\n\n");

    // -------------------------------------------------------------------------
    // Verdict
    // -------------------------------------------------------------------------
    if (results.size() == 2) {
        const auto& a = results[0];  // S63 OFF
        const auto& b = results[1];  // S63 ON
        const double pnl_delta = b.gross_pnl_usd - a.gross_pnl_usd;
        const double pf_delta  = b.profit_factor - a.profit_factor;
        const double dd_delta  = b.max_dd_usd - a.max_dd_usd;

        std::printf("VERDICT:\n");
        std::printf("  PnL: S63-ON minus S63-OFF = $%+.2f  (%s)\n",
                    pnl_delta,
                    pnl_delta > 200.0   ? "S63 HELPS materially" :
                    pnl_delta < -200.0  ? "S63 HURTS materially" :
                                          "S63 effectively neutral");
        std::printf("  PF:  S63-ON minus S63-OFF = %+.2f   (%s)\n",
                    pf_delta,
                    pf_delta >  0.05    ? "S63 improves risk-adjusted" :
                    pf_delta < -0.05    ? "S63 degrades risk-adjusted" :
                                          "S63 effectively neutral");
        std::printf("  DD:  S63-ON minus S63-OFF = $%+.2f  (%s)\n",
                    dd_delta,
                    dd_delta < -100.0   ? "S63 reduces drawdown materially" :
                    dd_delta >  100.0   ? "S63 increases drawdown materially" :
                                          "S63 effectively neutral");

        std::printf("\nRECOMMENDATION:\n");
        if (pnl_delta < -500.0 && pf_delta < -0.05) {
            std::printf("  -> Disable S63 on live GSP (engine_init.hpp:372-374 set to 0.0).\n");
            std::printf("     Validated edge is recovered without S63 amputation.\n");
        } else if (pnl_delta > 200.0 || pf_delta > 0.05) {
            std::printf("  -> Keep S63 ON on live GSP. Current engine_init values add value.\n");
        } else {
            std::printf("  -> S63 is roughly neutral. Either keep it (for tail protection) or\n");
            std::printf("     disable it (to match the validated config). Operator's choice.\n");
        }
    }

    return 0;
}
