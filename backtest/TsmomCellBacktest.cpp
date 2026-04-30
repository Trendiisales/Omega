// =============================================================================
//  TsmomCellBacktest.cpp -- Phase 2a dual-engine harness.
//
//  Status: Phase 2 -- ADDITIVE ONLY. No production paths touched.
//
//  Drives V1 (TsmomPortfolio from TsmomEngine.hpp) and V2
//  (TsmomPortfolioV2 = CellPortfolio<TsmomStrategy>) over the same H1
//  OHLC corpus and emits two CSV ledgers we can diff.
//
//  Default corpus:
//      phase1/signal_discovery/tsmom_warmup_H1.csv
//          (~6,156 H1 bars, 2025-04-01 -> 2026-04-01, XAUUSD)
//
//  Phase 2a contract: with --max-pos 1 (default), the two ledgers MUST be
//  byte-identical. The harness exits nonzero if they aren't, and prints
//  the first divergent row.
//
//  Phase 2b contract: with --max-pos 10, V2 will open positions V1 wouldn't
//  have. Divergence is expected and the parity check is skipped (just
//  prints the diff stats).
//
//  Build (Mac / clang):
//      cmake --build build --target TsmomCellBacktest
//      ./build/TsmomCellBacktest
//
//  Build (Windows / MSVC):
//      cmake --build build --target TsmomCellBacktest --config Release
//      .\build\Release\TsmomCellBacktest.exe
//
//  Options:
//      --csv     <path>   Input H1 OHLC corpus      (default: phase1/.../tsmom_warmup_H1.csv)
//      --v1-out  <path>   V1 ledger path            (default: bt_tsmom_v1.csv)
//      --v2-out  <path>   V2 ledger path            (default: bt_tsmom_v2.csv)
//      --max-pos <n>      Max positions per cell    (default: 1, Phase 2a)
//      --quiet            Suppress per-trade printf from V1
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
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>      // dup, dup2, close (used by --quiet stdout redirect)
#endif

#include "../include/CellPrimitives.hpp"
#include "../include/OmegaTradeLedger.hpp"
#include "../include/TsmomEngine.hpp"      // V1: TsmomPortfolio
#include "../include/CellEngine.hpp"       // V2 base: CellBase / CellPortfolio
#include "../include/TsmomStrategy.hpp"    // V2 strategy + topology builder

namespace {

// -----------------------------------------------------------------------------
//  Bar row used by the local CSV reader. Same layout as omega::cell::Bar
//  but kept as a POD here to keep this TU self-contained.
// -----------------------------------------------------------------------------
struct H1Row {
    int64_t bar_start_ms;
    double  open, high, low, close;
};

// -----------------------------------------------------------------------------
//  CSV reader for the 5-col warmup format:
//      bar_start_ms,open,high,low,close
//  '#'-prefixed lines and blank lines are skipped.
// -----------------------------------------------------------------------------
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
//  CSV ledger writer. Field order is FROZEN -- changing it breaks the V1/V2
//  byte-for-byte diff. Floating-point format is fixed-precision so that
//  identical doubles always serialise to identical strings.
// -----------------------------------------------------------------------------
static const char* kLedgerHeader =
    "id,symbol,side,engine,entryTs,exitTs,entryPrice,exitPrice,sl,tp,size,"
    "pnl,mfe,mae,atr_at_entry,spreadAtEntry,exitReason,regime,shadow\n";

static void write_ledger_row(std::FILE* f, const ::omega::TradeRecord& tr) {
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
//  Args
// -----------------------------------------------------------------------------
struct Args {
    std::string csv_path = "phase1/signal_discovery/tsmom_warmup_H1.csv";
    std::string v1_out   = "bt_tsmom_v1.csv";
    std::string v2_out   = "bt_tsmom_v2.csv";
    int         max_pos  = 1;       // Phase 2a default
    bool        quiet    = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto take = [&](const char* /*name*/) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ERROR: '%s' needs an argument\n", argv[i]);
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if      (s == "--csv")     a.csv_path = take("--csv");
        else if (s == "--v1-out")  a.v1_out   = take("--v1-out");
        else if (s == "--v2-out")  a.v2_out   = take("--v2-out");
        else if (s == "--max-pos") a.max_pos  = std::atoi(take("--max-pos").c_str());
        else if (s == "--quiet")   a.quiet    = true;
        else if (s == "--help" || s == "-h") {
            std::printf(
                "TsmomCellBacktest -- Phase 2a dual-engine harness\n"
                "\n"
                "Usage: %s [options]\n"
                "  --csv     <path>   H1 OHLC corpus  (default: phase1/.../tsmom_warmup_H1.csv)\n"
                "  --v1-out  <path>   V1 ledger CSV   (default: bt_tsmom_v1.csv)\n"
                "  --v2-out  <path>   V2 ledger CSV   (default: bt_tsmom_v2.csv)\n"
                "  --max-pos <n>      max_positions_per_cell (Phase 2a=1, Phase 2b=10)\n"
                "  --quiet            suppress per-trade V1 printf\n",
                argv[0]);
            std::exit(0);
        }
        else {
            std::fprintf(stderr, "ERROR: unknown arg '%s'\n", s.c_str());
            std::exit(2);
        }
    }
    return a;
}

// -----------------------------------------------------------------------------
//  Byte-for-byte CSV comparator. Returns 0 if identical, else line# of first
//  divergence (1-based); prints both lines to stderr.
// -----------------------------------------------------------------------------
static int diff_ledgers(const std::string& a_path, const std::string& b_path) {
    std::ifstream fa(a_path), fb(b_path);
    if (!fa.is_open() || !fb.is_open()) {
        std::fprintf(stderr, "ERROR: cannot open ledger(s) for diff\n");
        return -1;
    }
    int line_no = 0;
    std::string la, lb;
    while (true) {
        const bool ga = static_cast<bool>(std::getline(fa, la));
        const bool gb = static_cast<bool>(std::getline(fb, lb));
        ++line_no;
        if (!ga && !gb) return 0;          // both ended -- match
        if (!ga || !gb) {
            std::fprintf(stderr,
                "[DIFF] length mismatch at line %d: V1 %s end, V2 %s end\n",
                line_no, ga ? "did not"   : "reached",
                         gb ? "did not"   : "reached");
            return line_no;
        }
        if (la != lb) {
            std::fprintf(stderr, "[DIFF] line %d:\n  V1: %s\n  V2: %s\n",
                         line_no, la.c_str(), lb.c_str());
            return line_no;
        }
    }
}

}  // namespace

// =============================================================================
//  Engine wiring helpers
// =============================================================================

// V1 wiring: matches engine_init.hpp lines 492-501 for shadow_mode=true,
// risk_pct=0.005, max_lot_cap=0.05. max_concurrent set wide so the per-cell
// cap is the binding constraint (Phase 2a needs cell-level multi-pos = 1).
static void configure_v1(::omega::TsmomPortfolio& v1, const Args& a) {
    v1.shadow_mode             = true;
    v1.enabled                 = true;
    v1.max_concurrent          = a.max_pos * 5 + 5;   // headroom over per-cell cap
    v1.max_positions_per_cell  = a.max_pos;
    v1.risk_pct                = 0.005;
    v1.start_equity            = 10000.0;
    v1.margin_call             = 1000.0;
    v1.max_lot_cap             = 0.05;
    v1.block_on_risk_off       = false;   // backtest has no macro feed
    v1.warmup_csv_path         = "";       // we drive bars directly, no warmup phase
    v1.init();
}

static void configure_v2(::omega::cell::TsmomPortfolioV2& v2, const Args& a) {
    v2.shadow_mode             = true;
    v2.enabled                 = true;
    v2.max_concurrent          = a.max_pos * 5 + 5;
    v2.max_positions_per_cell  = a.max_pos;
    v2.risk_pct                = 0.005;
    v2.start_equity            = 10000.0;
    v2.margin_call             = 1000.0;
    v2.max_lot_cap             = 0.05;
    v2.usd_per_pt_per_lot      = 100.0;
    v2.block_on_risk_off       = false;
    v2.warmup_csv_path         = "";
    v2.symbol                  = "XAUUSD";
    v2.regime_label            = "TSMOM";
    ::omega::cell::build_default_tsmom_topology(v2);
    v2.init();
}

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
    std::printf("[BT] loaded %zu H1 bars (rejected=%d) from %s\n",
                rows.size(), rejected, a.csv_path.c_str());
    std::printf("[BT] first bar_ms=%lld  last bar_ms=%lld  max_pos_per_cell=%d\n",
                static_cast<long long>(rows.front().bar_start_ms),
                static_cast<long long>(rows.back().bar_start_ms),
                a.max_pos);
    std::fflush(stdout);

    // Optional: silence V1's printf chatter. Both engines are CPU-bound on
    // big runs; V1's per-trade log adds noticeable I/O time. With --quiet we
    // redirect stdout for the duration of the run.
    int saved_stdout_fd = -1;
    std::FILE* devnull = nullptr;
    if (a.quiet) {
        std::fflush(stdout);
#ifdef _WIN32
        // Quiet mode on Windows: leave stdout alone; harness lines from the
        // tail of main() will print correctly because we don't rely on
        // synchronisation. (V1 still prints, but compile-out is overkill.)
#else
        saved_stdout_fd = ::dup(1);
        devnull = std::fopen("/dev/null", "w");
        if (devnull) ::dup2(::fileno(devnull), 1);
#endif
    }

    // ---- V1 ledger ----
    std::FILE* f_v1 = std::fopen(a.v1_out.c_str(), "w");
    if (!f_v1) { std::fprintf(stderr, "ERROR: cannot open %s\n", a.v1_out.c_str()); return 4; }
    std::fputs(kLedgerHeader, f_v1);

    ::omega::TsmomPortfolio v1;
    configure_v1(v1, a);

    auto v1_cb = [f_v1](const ::omega::TradeRecord& tr) {
        write_ledger_row(f_v1, tr);
    };

    // ---- V2 ledger ----
    std::FILE* f_v2 = std::fopen(a.v2_out.c_str(), "w");
    if (!f_v2) { std::fprintf(stderr, "ERROR: cannot open %s\n", a.v2_out.c_str()); return 4; }
    std::fputs(kLedgerHeader, f_v2);

    ::omega::cell::TsmomPortfolioV2 v2;
    configure_v2(v2, a);

    auto v2_cb = [f_v2](const ::omega::TradeRecord& tr) {
        write_ledger_row(f_v2, tr);
    };

    // -------------------------------------------------------------------------
    //  Drive the bar stream through both engines in lockstep.
    //  bid = ask = close (zero spread).
    //  now_ms = bar_start_ms + 3600*1000 (close of the H1 bar).
    //  atr14 passed as 0.0 -> internal atr_h1_.value() fallback (used by both
    //  V1 and V2 identically).
    // -------------------------------------------------------------------------
    int v1_trades = 0;

    for (const H1Row& r : rows) {
        const int64_t now_ms = r.bar_start_ms + 3600LL * 1000LL;
        const double  bid    = r.close;
        const double  ask    = r.close;

        // V1 needs a TsmomBar; V2 needs an omega::cell::Bar. Same layout but
        // distinct struct types until Phase 3 retires the V1 type.
        ::omega::TsmomBar bv1;
        bv1.bar_start_ms = r.bar_start_ms;
        bv1.open = r.open; bv1.high = r.high; bv1.low = r.low; bv1.close = r.close;

        ::omega::cell::Bar bv2;
        bv2.bar_start_ms = r.bar_start_ms;
        bv2.open = r.open; bv2.high = r.high; bv2.low = r.low; bv2.close = r.close;

        const auto v1_count_before = v1.h1_long_.trade_id_ + v1.h2_long_.trade_id_
                                   + v1.h4_long_.trade_id_ + v1.h6_long_.trade_id_
                                   + v1.d1_long_.trade_id_;
        v1.on_h1_bar(bv1, bid, ask, 0.0, now_ms, v1_cb);
        const auto v1_count_after  = v1.h1_long_.trade_id_ + v1.h2_long_.trade_id_
                                   + v1.h4_long_.trade_id_ + v1.h6_long_.trade_id_
                                   + v1.d1_long_.trade_id_;
        v1_trades += (v1_count_after - v1_count_before);

        v2.on_h1_bar(bv2, bid, ask, 0.0, now_ms, v2_cb);
    }

    // Force-close any residual open positions at the end of the corpus
    // so they appear in the ledger. Both engines do this identically.
    {
        const H1Row& last = rows.back();
        const int64_t end_ms = last.bar_start_ms + 3600LL * 1000LL;
        v1.force_close_all(last.close, last.close, end_ms, v1_cb);
        v2.force_close_all(last.close, last.close, end_ms, v2_cb);
    }

    std::fclose(f_v1);
    std::fclose(f_v2);

    // Restore stdout if we redirected it.
#ifndef _WIN32
    if (saved_stdout_fd >= 0) {
        std::fflush(stdout);
        ::dup2(saved_stdout_fd, 1);
        ::close(saved_stdout_fd);
        if (devnull) std::fclose(devnull);
    }
#endif

    // -------------------------------------------------------------------------
    //  Summary + parity check
    // -------------------------------------------------------------------------
    auto v1_status = v1.status();
    auto v2_status = v2.status();
    std::printf("\n[BT] === V1 (TsmomPortfolio) ===\n"
                  "  equity        = %.2f\n"
                  "  peak          = %.2f\n"
                  "  max_dd_pct    = %.2f%%\n"
                  "  open_at_end   = %d\n"
                  "  ledger_path   = %s\n",
                v1_status.equity, v1_status.peak, v1_status.max_dd_pct * 100.0,
                v1_status.open_count, a.v1_out.c_str());
    std::printf("\n[BT] === V2 (CellPortfolio<TsmomStrategy>) ===\n"
                  "  equity        = %.2f\n"
                  "  peak          = %.2f\n"
                  "  max_dd_pct    = %.2f%%\n"
                  "  open_at_end   = %d\n"
                  "  ledger_path   = %s\n",
                v2_status.equity, v2_status.peak, v2_status.max_dd_pct * 100.0,
                v2_status.open_count, a.v2_out.c_str());

    if (a.max_pos == 1) {
        std::printf("\n[BT] Phase 2a parity check (max_pos=1): "
                    "V1 vs V2 ledgers must be byte-identical.\n");
        const int diff_line = diff_ledgers(a.v1_out, a.v2_out);
        if (diff_line == 0) {
            std::printf("[BT] *** PARITY OK *** -- ledgers byte-identical.\n");
            return 0;
        }
        if (diff_line < 0) return 5;
        std::fprintf(stderr,
            "[BT] *** PARITY FAIL *** -- first divergence at line %d.\n", diff_line);
        return 6;
    } else {
        std::printf("\n[BT] Phase 2b run (max_pos=%d): parity check skipped.\n",
                    a.max_pos);
        std::printf("[BT] V1 trade count = %d ; compare ledgers manually.\n",
                    v1_trades);
        return 0;
    }
}
