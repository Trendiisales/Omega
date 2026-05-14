// =============================================================================
//  EmaPullbackBacktest.cpp -- dedicated harness for the EmaPullback portfolio.
//
//  Created 2026-05-14 (part-X follow-up to S76 EmaPullback S63 activation).
//
//  Purpose
//  -------
//  S76 activated S63 in-flight protection on g_ema_pullback with values
//  reasoned from a single shadow trade. Operator-directed: validate the
//  S63 values against the EmaPullback validation corpus via a proper C++
//  harness, NOT a Python sim. This harness instantiates the actual
//  omega::EpbPortfolio (no logic duplication, no SweepableEnginesCRTP
//  rebuild) and drives it over the same H1 OHLC tape the engine was
//  validated on (phase1/signal_discovery/tsmom_warmup_H1.csv).
//
//  Structurally mirrors TsmomCellBacktest.cpp's single-engine pattern
//  (same XAUUSD H1 OHLC corpus, same shadow_mode wiring, same TradeRecord
//  ledger format) with the XauTrendFollowBacktest.cpp CLI shape for the
//  three S63 PCT overrides. Engine code unchanged.
//
//  CLI
//  ---
//      ./build/EmaPullbackBacktest [options]
//
//      --csv <path>            H1 OHLC corpus
//                              (default: phase1/signal_discovery/tsmom_warmup_H1.csv)
//      --trades <path>         per-trade ledger CSV
//                              (default: epb_trades.csv)
//      --report <path>         summary report CSV
//                              (default: epb_report.csv)
//      --loss-cut <pct>        LOSS_CUT_PCT override   (default: 0.0 -- S63 OFF)
//      --be-arm   <pct>        BE_ARM_PCT override     (default: 0.0)
//      --be-buffer<pct>        BE_BUFFER_PCT override  (default: 0.0)
//      --risk-pct <r>          risk_pct override       (default: 0.005)
//      --max-lot  <lot>        max_lot_cap override    (default: 0.05)
//      --start-eq <eq>         start_equity override   (default: 10000.0)
//      --shadow    yes|no      shadow_mode override    (default: yes)
//      --sweep                 ignore single-run flags; run 27-cell 3x3x3
//                              grid over (LOSS_CUT, BE_ARM, BE_BUFFER) and
//                              write per-cell summary to --report (no trades
//                              CSV in this mode). Grid (S81 tight-zone rev):
//                                  LOSS_CUT_PCT  : 0.0, 0.05, 0.10
//                                                  (matches XAU engine cluster:
//                                                   XauusdFvg/XauThreeBar30m
//                                                   use 0.05%, PDHL uses 0.04%,
//                                                   FX opens use 0.03%, indices
//                                                   IFlow/VWR use 0.07-0.08%)
//                                  BE_ARM_PCT    : 0.0, 0.20, 0.40
//                                  BE_BUFFER_PCT : 0.0, 0.025, 0.05
//                              The (0,0,0) cell is the pre-S63 baseline.
//                              The (0.0, 0.40, 0.05) cell is the current
//                              S80 engine_init.hpp activation.
//      --wide-sweep            S77 historical grid for backward comparison:
//                                  LOSS_CUT_PCT  : 0.0, 0.5, 1.0
//                                  BE_ARM_PCT    : 0.0, 0.20, 0.40
//                                  BE_BUFFER_PCT : 0.0, 0.025, 0.05
//      --quiet                 suppress engine printf chatter
//      --help / -h
//
//  Outputs
//  -------
//  Single-run mode writes BOTH a trade ledger and a single-row report:
//      trades.csv columns:
//        id,symbol,side,engine,entryTs,exitTs,entryPrice,exitPrice,sl,tp,size,
//        pnl,mfe,mae,atr_at_entry,spreadAtEntry,exitReason,regime,shadow
//      report.csv columns:
//        loss_cut_pct,be_arm_pct,be_buffer_pct,trades,wins,losses,win_rate,
//        gross_pnl,profit_factor,sl_hit,tp_hit,time_exit,be_cut,loss_cut,
//        other,equity_end,peak_equity,max_dd_pct
//
//  Sweep mode writes only the report, with one row per cell (27 rows).
//
//  Build
//  -----
//      cmake --build build --target EmaPullbackBacktest --config Release
//      ./build/EmaPullbackBacktest               # single-run baseline
//      ./build/EmaPullbackBacktest --sweep       # 27-cell grid
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>
#endif

#include "../include/OmegaTradeLedger.hpp"
#include "../include/EmaPullbackEngine.hpp"

namespace {

// -----------------------------------------------------------------------------
//  Input row -- 5-col format identical to tsmom_warmup_H1.csv:
//      bar_start_ms,open,high,low,close
// -----------------------------------------------------------------------------
struct H1Row {
    int64_t bar_start_ms;
    double  open, high, low, close;
};

static std::vector<H1Row> load_csv(const std::string& path, int& rejected_out) {
    std::vector<H1Row> rows;
    rejected_out = 0;

    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "ERROR: cannot open '%s'\n", path.c_str());
        return rows;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::array<std::string, 5> tok;
        int idx = 0;
        std::stringstream ss(line);
        std::string field;
        while (idx < 5 && std::getline(ss, field, ',')) tok[idx++] = field;
        if (idx < 5) { ++rejected_out; continue; }

        char* endp = nullptr;
        const long long ms_ll = std::strtoll(tok[0].c_str(), &endp, 10);
        if (endp == tok[0].c_str() || *endp != '\0') { ++rejected_out; continue; }

        char* ep_o = nullptr; const double o = std::strtod(tok[1].c_str(), &ep_o);
        char* ep_h = nullptr; const double h = std::strtod(tok[2].c_str(), &ep_h);
        char* ep_l = nullptr; const double l = std::strtod(tok[3].c_str(), &ep_l);
        char* ep_c = nullptr; const double c = std::strtod(tok[4].c_str(), &ep_c);
        if (ep_o == tok[1].c_str() || ep_h == tok[2].c_str()
            || ep_l == tok[3].c_str() || ep_c == tok[4].c_str()) { ++rejected_out; continue; }
        if (!std::isfinite(o) || !std::isfinite(h)
            || !std::isfinite(l) || !std::isfinite(c))           { ++rejected_out; continue; }

        rows.push_back(H1Row{
            static_cast<int64_t>(ms_ll), o, h, l, c
        });
    }
    return rows;
}

// -----------------------------------------------------------------------------
//  Per-trade CSV writer -- same schema as TsmomCellBacktest for downstream
//  tooling compatibility.
// -----------------------------------------------------------------------------
static const char* kTradesHeader =
    "id,symbol,side,engine,entryTs,exitTs,entryPrice,exitPrice,sl,tp,size,"
    "pnl,mfe,mae,atr_at_entry,spreadAtEntry,exitReason,regime,shadow\n";

static void write_trade_row(std::FILE* f, const ::omega::TradeRecord& tr) {
    std::fprintf(f,
        "%d,%s,%s,%s,%lld,%lld,%.6f,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%s,%d\n",
        tr.id,
        tr.symbol.c_str(),
        tr.side.c_str(),
        tr.engine.c_str(),
        static_cast<long long>(tr.entryTs),
        static_cast<long long>(tr.exitTs),
        tr.entryPrice,
        tr.exitPrice,
        tr.sl,
        tr.tp,
        tr.size,
        tr.pnl,
        tr.mfe,
        tr.mae,
        tr.atr_at_entry,
        tr.spreadAtEntry,
        tr.exitReason.c_str(),
        tr.regime.c_str(),
        tr.shadow ? 1 : 0);
}

// -----------------------------------------------------------------------------
//  Per-run aggregate -- one row in the report CSV.
// -----------------------------------------------------------------------------
struct RunStats {
    // Config
    double loss_cut_pct = 0.0;
    double be_arm_pct   = 0.0;
    double be_buffer_pct = 0.0;
    // Trade counts
    int trades   = 0;
    int wins     = 0;
    int losses   = 0;
    // P&L (in raw dollars after the ledger's *100 conversion is applied below)
    double gross_pnl = 0.0;
    double gross_win = 0.0;
    double gross_loss = 0.0;
    // Exit reasons
    int sl_hit    = 0;
    int tp_hit    = 0;
    int time_exit = 0;
    int be_cut    = 0;
    int loss_cut  = 0;
    int other     = 0;
    // Portfolio
    double equity_end  = 0.0;
    double peak_equity = 0.0;
    double max_dd_pct  = 0.0;

    void record(const ::omega::TradeRecord& tr) noexcept {
        // EpbCell._close already converts pnl to "$100 per point" terms via the
        // *100 in the printf. TradeRecord.pnl itself stores raw pnl_pts
        // (price-move * direction * lot). Convert to dollars here, matching
        // the engine's own console print (and matching how EpbPortfolio updates
        // equity_ via the wrap callback at line ~520).
        const double pnl_usd = tr.pnl * 100.0;
        ++trades;
        gross_pnl += pnl_usd;
        if (pnl_usd > 0.0) {
            ++wins;
            gross_win += pnl_usd;
        } else if (pnl_usd < 0.0) {
            ++losses;
            gross_loss += -pnl_usd;
        }
        const std::string& r = tr.exitReason;
        if      (r == "SL_HIT")      ++sl_hit;
        else if (r == "TP_HIT")      ++tp_hit;
        else if (r == "TIME_EXIT")   ++time_exit;
        else if (r == "BE_CUT")      ++be_cut;
        else if (r == "LOSS_CUT")    ++loss_cut;
        else                          ++other;
    }

    double win_rate() const noexcept {
        return trades > 0 ? (100.0 * wins) / trades : 0.0;
    }
    double profit_factor() const noexcept {
        return gross_loss > 0.0 ? gross_win / gross_loss
                                : (gross_win > 0.0 ? 999.0 : 0.0);
    }
};

static const char* kReportHeader =
    "loss_cut_pct,be_arm_pct,be_buffer_pct,trades,wins,losses,win_rate,"
    "gross_pnl,profit_factor,sl_hit,tp_hit,time_exit,be_cut,loss_cut,"
    "other,equity_end,peak_equity,max_dd_pct\n";

static void write_report_row(std::FILE* f, const RunStats& s) {
    std::fprintf(f,
        "%.4f,%.4f,%.4f,%d,%d,%d,%.2f,"
        "%.2f,%.4f,%d,%d,%d,%d,%d,"
        "%d,%.2f,%.2f,%.4f\n",
        s.loss_cut_pct, s.be_arm_pct, s.be_buffer_pct,
        s.trades, s.wins, s.losses, s.win_rate(),
        s.gross_pnl, s.profit_factor(),
        s.sl_hit, s.tp_hit, s.time_exit, s.be_cut, s.loss_cut,
        s.other, s.equity_end, s.peak_equity, s.max_dd_pct * 100.0);
}

// -----------------------------------------------------------------------------
//  Args
// -----------------------------------------------------------------------------
struct Args {
    std::string csv_path     = "phase1/signal_discovery/tsmom_warmup_H1.csv";
    std::string trades_path  = "epb_trades.csv";
    std::string report_path  = "epb_report.csv";
    double      loss_cut_pct = 0.0;
    double      be_arm_pct   = 0.0;
    double      be_buffer_pct = 0.0;
    double      risk_pct     = 0.005;
    double      max_lot      = 0.05;
    double      start_equity = 10000.0;
    bool        shadow_mode  = true;
    bool        sweep        = false;
    bool        wide_sweep   = false;
    bool        quiet        = false;
};

static void usage(const char* argv0) {
    std::printf(
        "EmaPullbackBacktest -- single-engine harness for the EmaPullback portfolio\n"
        "\n"
        "Usage: %s [options]\n"
        "  --csv <path>          H1 OHLC corpus (default: phase1/.../tsmom_warmup_H1.csv)\n"
        "  --trades <path>       per-trade ledger CSV (default: epb_trades.csv)\n"
        "  --report <path>       summary report CSV (default: epb_report.csv)\n"
        "  --loss-cut <pct>      LOSS_CUT_PCT override (default: 0.0)\n"
        "  --be-arm <pct>        BE_ARM_PCT override   (default: 0.0)\n"
        "  --be-buffer <pct>     BE_BUFFER_PCT override (default: 0.0)\n"
        "  --risk-pct <r>        risk_pct override     (default: 0.005)\n"
        "  --max-lot <l>         max_lot_cap override  (default: 0.05)\n"
        "  --start-eq <e>        start_equity override (default: 10000.0)\n"
        "  --shadow yes|no       shadow_mode override  (default: yes)\n"
        "  --sweep               run 27-cell 3x3x3 grid over (LOSS_CUT, BE_ARM,\n"
        "                        BE_BUFFER); writes only --report, one row per cell.\n"
        "                        Default (TIGHT zone, matches other XAU engines):\n"
        "                          LC={0.0, 0.05, 0.10}  ARM={0.0, 0.20, 0.40}\n"
        "                          BUF={0.0, 0.025, 0.05}\n"
        "  --wide-sweep          S77 historical sweep grid (WIDE LC zone for\n"
        "                        back-comparison):\n"
        "                          LC={0.0, 0.5, 1.0}  ARM={0.0, 0.20, 0.40}\n"
        "                          BUF={0.0, 0.025, 0.05}\n"
        "  --quiet               suppress engine printf chatter\n"
        "  --help / -h           this help\n",
        argv0);
}

static bool parse_yesno(const std::string& s) {
    return s == "yes" || s == "1" || s == "true";
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto take = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ERROR: '%s' needs an argument\n", argv[i]);
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if      (s == "--csv")        a.csv_path     = take();
        else if (s == "--trades")     a.trades_path  = take();
        else if (s == "--report")     a.report_path  = take();
        else if (s == "--loss-cut")   a.loss_cut_pct = std::strtod(take().c_str(), nullptr);
        else if (s == "--be-arm")     a.be_arm_pct   = std::strtod(take().c_str(), nullptr);
        else if (s == "--be-buffer")  a.be_buffer_pct = std::strtod(take().c_str(), nullptr);
        else if (s == "--risk-pct")   a.risk_pct     = std::strtod(take().c_str(), nullptr);
        else if (s == "--max-lot")    a.max_lot      = std::strtod(take().c_str(), nullptr);
        else if (s == "--start-eq")   a.start_equity = std::strtod(take().c_str(), nullptr);
        else if (s == "--shadow")     a.shadow_mode  = parse_yesno(take());
        else if (s == "--sweep")      a.sweep        = true;
        else if (s == "--wide-sweep") { a.sweep      = true; a.wide_sweep = true; }
        else if (s == "--quiet")      a.quiet        = true;
        else if (s == "--help" || s == "-h") { usage(argv[0]); std::exit(0); }
        else {
            std::fprintf(stderr, "ERROR: unknown arg '%s'\n", s.c_str());
            std::exit(2);
        }
    }
    return a;
}

// -----------------------------------------------------------------------------
//  Configure an EpbPortfolio with the engine_init.hpp baseline settings plus
//  the harness overrides. Keep this in sync with engine_init.hpp:1285-1306
//  (g_ema_pullback init block).
// -----------------------------------------------------------------------------
static void configure_portfolio(::omega::EpbPortfolio& p, const Args& a,
                                double loss_cut, double be_arm, double be_buf) {
    p.shadow_mode       = a.shadow_mode;
    p.enabled           = true;
    p.max_concurrent    = 4;
    p.risk_pct          = a.risk_pct;
    p.start_equity      = a.start_equity;
    p.margin_call       = 1000.0;
    p.max_lot_cap       = a.max_lot;
    p.block_on_risk_off = false;          // harness has no macro feed
    p.warmup_csv_path   = "";              // we drive bars directly
    p.LOSS_CUT_PCT      = loss_cut;
    p.BE_ARM_PCT        = be_arm;
    p.BE_BUFFER_PCT     = be_buf;
    p.init();
}

// -----------------------------------------------------------------------------
//  Drive one full run. Returns the RunStats; optionally writes per-trade
//  ledger if trades_file != nullptr.
// -----------------------------------------------------------------------------
static RunStats run_one(const std::vector<H1Row>& rows, const Args& a,
                        double loss_cut, double be_arm, double be_buf,
                        std::FILE* trades_file) {
    RunStats stats;
    stats.loss_cut_pct = loss_cut;
    stats.be_arm_pct   = be_arm;
    stats.be_buffer_pct = be_buf;

    ::omega::EpbPortfolio p;
    configure_portfolio(p, a, loss_cut, be_arm, be_buf);

    auto cb = [&stats, trades_file](const ::omega::TradeRecord& tr) {
        stats.record(tr);
        if (trades_file) write_trade_row(trades_file, tr);
    };

    for (const H1Row& r : rows) {
        const int64_t now_ms = r.bar_start_ms + 3600LL * 1000LL;
        const double  bid    = r.close;
        const double  ask    = r.close;

        ::omega::EpbBar bar;
        bar.bar_start_ms = r.bar_start_ms;
        bar.open  = r.open;
        bar.high  = r.high;
        bar.low   = r.low;
        bar.close = r.close;

        // atr14 = 0.0 -> engine's internal atr_h1_.value() fallback kicks in.
        p.on_h1_bar(bar, bid, ask, 0.0, now_ms, cb);
    }

    // Force-close residual positions at end of corpus so they appear in stats.
    if (!rows.empty()) {
        const H1Row& last = rows.back();
        const int64_t end_ms = last.bar_start_ms + 3600LL * 1000LL;
        p.force_close_all(last.close, last.close, end_ms, cb);
    }

    stats.equity_end  = p.equity_;
    stats.peak_equity = p.peak_equity_;
    stats.max_dd_pct  = p.max_dd_pct_;
    return stats;
}

}  // namespace

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    int rejected = 0;
    auto rows = load_csv(a.csv_path, rejected);
    if (rows.empty()) {
        std::fprintf(stderr, "ERROR: 0 H1 bars loaded from %s\n", a.csv_path.c_str());
        return 3;
    }
    std::printf("[EPB-BT] loaded %zu H1 bars (rejected=%d) from %s\n",
                rows.size(), rejected, a.csv_path.c_str());
    std::printf("[EPB-BT] first bar_ms=%lld  last bar_ms=%lld\n",
                static_cast<long long>(rows.front().bar_start_ms),
                static_cast<long long>(rows.back().bar_start_ms));
    std::fflush(stdout);

    // Optional stdout silencing -- the engine prints per-trade chatter and
    // per-init ARMED lines that swamp big runs.
    int saved_stdout_fd = -1;
    std::FILE* devnull = nullptr;
    if (a.quiet) {
        std::fflush(stdout);
#ifndef _WIN32
        saved_stdout_fd = ::dup(1);
        devnull = std::fopen("/dev/null", "w");
        if (devnull) ::dup2(::fileno(devnull), 1);
#endif
    }

    // -------------------------------------------------------------------------
    //  Sweep mode -- 27-cell 3x3x3 grid. Writes only the report.
    // -------------------------------------------------------------------------
    if (a.sweep) {
        std::FILE* report = std::fopen(a.report_path.c_str(), "w");
        if (!report) {
            std::fprintf(stderr, "ERROR: cannot open %s for write\n", a.report_path.c_str());
            return 4;
        }
        std::fputs(kReportHeader, report);

        // S81: default sweep now covers the TIGHT LOSS_CUT zone (0.05-0.10%),
        // matching the cluster other XAU engines actually use (XauusdFvg /
        // XauThreeBar30m = 0.05%, PDHL = 0.04%). The original S77 wide grid
        // (LC = 0.5, 1.0) was 6-20x looser than the cluster other engines
        // operate in, so it never tested the regime the operator was asking
        // about ("cut trades when they go bad").
        //
        // Pass --wide-sweep to run the original S77 grid for back-comparison.
        const double tight_lc[3] = { 0.0, 0.05, 0.10 };  // S81 default (tight zone)
        const double wide_lc[3]  = { 0.0, 0.5,  1.0  };  // S77 historical (wide zone)
        const double* lc_vals    = a.wide_sweep ? wide_lc : tight_lc;
        const double arm_vals[3] = { 0.0, 0.20, 0.40 };
        const double buf_vals[3] = { 0.0, 0.025, 0.05 };

        int cell = 0;
        std::fprintf(stderr, "[EPB-BT] sweep mode: %s LC zone\n",
                     a.wide_sweep ? "WIDE (S77 historical)" : "TIGHT (S81 default)");
        for (int i = 0; i < 3; ++i) {
            const double lc = lc_vals[i];
            for (double arm : arm_vals) {
                for (double buf : buf_vals) {
                    ++cell;
                    RunStats s = run_one(rows, a, lc, arm, buf, nullptr);
                    write_report_row(report, s);
                    std::fflush(report);
                    // Cell summary to stderr so it's visible under --quiet too.
                    std::fprintf(stderr,
                        "[EPB-BT][sweep %2d/27] LC=%.3f ARM=%.3f BUF=%.4f "
                        "-> trades=%d wr=%.1f%% pnl=$%.2f pf=%.3f "
                        "sl=%d tp=%d to=%d be=%d lc_cut=%d eq=$%.2f\n",
                        cell, lc, arm, buf,
                        s.trades, s.win_rate(), s.gross_pnl, s.profit_factor(),
                        s.sl_hit, s.tp_hit, s.time_exit, s.be_cut, s.loss_cut,
                        s.equity_end);
                }
            }
        }
        std::fclose(report);

#ifndef _WIN32
        if (saved_stdout_fd >= 0) {
            std::fflush(stdout);
            ::dup2(saved_stdout_fd, 1);
            ::close(saved_stdout_fd);
            if (devnull) std::fclose(devnull);
        }
#endif

        std::printf("\n[EPB-BT] *** sweep done *** 27 cells -> %s\n",
                    a.report_path.c_str());
        if (a.wide_sweep) {
            std::printf("[EPB-BT] WIDE-sweep mode (S77 historical grid).\n"
                        "[EPB-BT] Compare cells against the (0,0,0) baseline.\n");
        } else {
            std::printf("[EPB-BT] TIGHT-sweep mode (S81 default grid, matches\n"
                        "[EPB-BT] other XAU engines' LOSS_CUT range 0.04-0.05%%).\n"
                        "[EPB-BT] Compare against (0,0,0) baseline and current\n"
                        "[EPB-BT] S80 cell (LC=0.0 ARM=0.40 BUF=0.050).\n");
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    //  Single-run mode -- writes both trades CSV and a 1-row report.
    // -------------------------------------------------------------------------
    std::FILE* trades = std::fopen(a.trades_path.c_str(), "w");
    if (!trades) {
        std::fprintf(stderr, "ERROR: cannot open %s for write\n", a.trades_path.c_str());
        return 4;
    }
    std::fputs(kTradesHeader, trades);

    std::FILE* report = std::fopen(a.report_path.c_str(), "w");
    if (!report) {
        std::fprintf(stderr, "ERROR: cannot open %s for write\n", a.report_path.c_str());
        std::fclose(trades);
        return 4;
    }
    std::fputs(kReportHeader, report);

    RunStats s = run_one(rows, a, a.loss_cut_pct, a.be_arm_pct, a.be_buffer_pct, trades);
    write_report_row(report, s);

    std::fclose(trades);
    std::fclose(report);

#ifndef _WIN32
    if (saved_stdout_fd >= 0) {
        std::fflush(stdout);
        ::dup2(saved_stdout_fd, 1);
        ::close(saved_stdout_fd);
        if (devnull) std::fclose(devnull);
    }
#endif

    std::printf("\n[EPB-BT] === Run summary ===\n"
                "  config       : LC=%.4f%% ARM=%.4f%% BUF=%.4f%%\n"
                "  trades       : %d  (wins=%d  losses=%d  WR=%.2f%%)\n"
                "  gross_pnl    : $%.2f\n"
                "  profit_factor: %.4f\n"
                "  exits        : SL=%d TP=%d TIME=%d BE_CUT=%d LOSS_CUT=%d OTHER=%d\n"
                "  equity_end   : $%.2f  peak=$%.2f  max_dd=%.2f%%\n"
                "  ledger       : %s\n"
                "  report       : %s\n",
                s.loss_cut_pct, s.be_arm_pct, s.be_buffer_pct,
                s.trades, s.wins, s.losses, s.win_rate(),
                s.gross_pnl, s.profit_factor(),
                s.sl_hit, s.tp_hit, s.time_exit, s.be_cut, s.loss_cut, s.other,
                s.equity_end, s.peak_equity, s.max_dd_pct * 100.0,
                a.trades_path.c_str(), a.report_path.c_str());
    return 0;
}
