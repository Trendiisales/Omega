// =============================================================================
// XauTrendFollowBacktest.cpp -- dedicated harness for the XAU trend-follow trio
// =============================================================================
// Created 2026-05-14 (session 2026-05-14 part-W, follow-up to part-V handoff
// SESSION_HANDOFF_2026-05-14k.md §"Recommended next-session focus" item 1).
//
// HARNESS STATUS (2026-05-14 part-Z):
//   All three dispatch paths (--engine 2h | 4h | d1) are now fully wired.
//   The 4h dispatch (run_4h_engine) and d1 dispatch (run_d1_engine) were
//   filled in part-Z by mirroring run_2h_engine with the appropriate
//   bar-type / bucket / signature substitutions. Each is documented inline
//   above its function definition. The harness is no longer a skeleton.
//
//   - --engine 2h  : H1 bar feed -> XauTrendFollow2hEngine::on_h1_bar
//   - --engine 4h  : H4 bar feed -> XauTrendFollow4hEngine::on_h4_bar
//                    (atr14_external = 0.0 -> engine computes locally)
//   - --engine d1  : H4 bar feed -> XauTrendFollowD1Engine::on_h4_bar
//                    (D1 synthesises daily bars from H4 internally)
//
// Purpose:
//   The XauTrendFollow trio (2h, 4h, D1) was added in S33 (2026-05-11) with
//   four ATR-based bracket cells per timeframe. As of 2026-05-14 part-W,
//   each engine has been transitioned from state E -> state B with respect
//   to S63 in-flight protection: the LOSS_CUT_PCT / BE_ARM_PCT / BE_BUFFER_PCT
//   fields and the gated management-path check have been added to each
//   engine header, with class defaults all set to 0.0 (S63 OFF). This
//   harness exercises the engine in a tick-replay loop with CLI-overridable
//   S63 values, supporting the Phase 3 walk-forward sweep that decides
//   whether the S63-adverse pattern from VWR USTEC (S71) and UTF5m USTEC
//   (S73) generalises to XAU trend-follow.
//
//   This harness instantiates the actual omega::XauTrendFollow*Engine
//   directly, the way backtest/UstecTrendFollow5mBacktest.cpp does for
//   UstecTrendFollow5mEngine. Logic drift from production is structurally
//   impossible.
//
// CLI:
//   ./build/XauTrendFollowBacktest <ticks.csv> [options]
//     --engine <eng>     2h | 4h | d1                       (default: 2h)
//     --mode <m>         baseline | tuned                   (default: baseline)
//                        baseline: LOSS_CUT_PCT=0, BE_ARM_PCT=0, BE_BUFFER_PCT=0
//                                  (S63 OFF -- engine runs at class defaults).
//                        tuned:    LOSS_CUT_PCT=0.05, BE_ARM_PCT=0.03,
//                                  BE_BUFFER_PCT=0.012 (XAU-scaled, mirrors
//                                  XauusdFvg / XauThreeBar30m defaults; the
//                                  Phase 1 sweep can override via the per-PCT
//                                  flags below).
//     --loss-cut <pct>   override LOSS_CUT_PCT  (sweep dimension)
//     --be-arm   <pct>   override BE_ARM_PCT    (sweep dimension)
//     --be-buffer<pct>   override BE_BUFFER_PCT (sweep dimension)
//     --lot <val>        override lot          (default: engine = 0.01)
//     --max-spread <pts> override max_spread   (default: engine = 1.0)
//     --trades <file>    trades CSV output     (default: xtf_trades.csv)
//     --report <file>    summary report CSV    (default: xtf_report.csv)
//     --limit <N>        max ticks to process  (default: unlimited)
//     --warmup <N>       warmup ticks pre-trade(default: 5000)
//     --quiet            suppress engine stdout chatter
//
// Tick CSV formats supported (auto-detected from the first data row):
//   A: timestamp_ms,bid,ask                          (numeric ms epoch)
//   B: timestamp_ms,bid,ask,vol                      (numeric ms epoch)
//   C: YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol           (Dukascopy)
//   D: YYYYMMDD HHMMSSmmm,bid,ask,vol                (HistData M1 tick)
//   E: timestamp_ms,open,high,low,close,vol          (OHLCV -- bid=close, ask=close)
//
//   The MinimalH4Gold fresh-tape converter at scripts/duka_to_legacy.py
//   produces YYYYMMDD,HH:MM:SS,bid,ask format; that converter does not
//   apply here -- XTF harness consumes Dukascopy directly or pre-merged
//   tick formats. Add a new format here if a future tape demands it.
//
// Output schema:
//   trades.csv columns mirror UstecTrendFollow5mBacktest exactly so the
//   walk-forward driver (scripts/xtf_*_wf_t1.py) can be adapted from
//   utf5m_wf_t1.py with minimal flag substitutions:
//     entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,
//     mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score
//
//   Note: XauTrendFollow*Engines do NOT populate TradeRecord.spreadAtEntry
//   or a confluence score; both columns are emitted as 0 for schema parity
//   with VWR / UTF5m. The cell name is suffixed onto tr.engine
//   ("XauTrendFollow2h_2h_Keltner_K2_sl2.0tp4.0" etc) per the engine's
//   S34 P1 fix #3 so per-cell P&L can be grouped downstream.
//
//   report.csv columns mirror UstecTrendFollow5mBacktest report:
//     metric,value rows for: engine_name, n_trades, wins, win_rate_pct,
//     gross_pnl, sum_pos, sum_neg, pf, n_tp_hit, n_sl_hit, n_loss_cut,
//     n_be_cut, n_other, worst_trade, best_trade, plus a final row each
//     for loss_cut_pct, be_arm_pct, be_buffer_pct -- the effective S63
//     values for this run.
//
// Build:
//   cmake --build build --target XauTrendFollowBacktest --config Release
//   (Mac/Linux uses -O3 -std=c++20; OmegaTimeShim is force-included via -include
//   in the CMake target so std::chrono::steady_clock / system_clock route
//   to simulated time. Same pattern as UstecTrendFollow5mBacktest and VWR.)
//
// AUTHORITY: NEW file, not core code. Engine headers were edited in the
// same session (part-W) to add S63 fields + management-path -- those
// edits are separate commits with their own evidence. CMakeLists.txt
// gets one add_executable block (non-core configuration file edit).
// All other source files untouched.
//
// PART-Z STATUS NOTES:
//   Items 1 + 2 from the original part-W TODO list are now complete (4h
//   and d1 dispatch wired -- see run_4h_engine / run_d1_engine).
//   Item 3 (2h baseline validation) was completed in part-W (the 2h path
//   shipped operational). Item 4 (CMakeLists.txt entry) was completed in
//   part-W too. Item 5 (scripts/xtf_2h_wf_t1.py) shipped in part-W.
//
//   Part-Z added: scripts/xtf_4h_wf_t1.py (4h sibling of xtf_2h_wf_t1.py,
//   5-line substitution per the part-W skeleton-state comment in the 2h
//   driver). scripts/xtf_d1_wf_t1.py remains as a queued follow-up
//   (mechanically identical patch to swap "4h" -> "d1" in the 4h driver,
//   plus the small-n caveat documented in XauTrendFollowD1Engine.hpp:174-179
//   should be considered for a per-window trade-count floor).
//
//   Decision rule (Phase 3, applied per-WF run): aggregate PF >= 1.20 AND
//   >= 3/4 windows with avg_pnl >= +0.001. Same protocol as VWR/UTF5m.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <deque>
#include <limits>

#include "OmegaTimeShim.hpp"

#include "../include/XauTrendFollow2hEngine.hpp"
#include "../include/XauTrendFollow4hEngine.hpp"
#include "../include/XauTrendFollowD1Engine.hpp"
#include "../include/OmegaTradeLedger.hpp"

// =============================================================================
// Tick row + CSV parsing -- mirrors UstecTrendFollow5mBacktest.cpp:142-340
// =============================================================================

struct TickRow {
    int64_t ts_ms = 0;
    double  bid   = 0.0;
    double  ask   = 0.0;
};

enum class TickFmt { Unknown, A_TBA, B_TBAV, C_DUKA, D_HIST, E_OHLCV };

static TickFmt detect_format(const std::string& line) {
    int commas = 0;
    for (char c : line) if (c == ',') ++commas;

    if (line.size() >= 10 &&
        std::isdigit(static_cast<unsigned char>(line[0])) &&
        std::isdigit(static_cast<unsigned char>(line[1])) &&
        std::isdigit(static_cast<unsigned char>(line[2])) &&
        std::isdigit(static_cast<unsigned char>(line[3])) &&
        line[4] == '.' &&
        std::isdigit(static_cast<unsigned char>(line[5])) &&
        std::isdigit(static_cast<unsigned char>(line[6])) &&
        line[7] == '.') {
        return TickFmt::C_DUKA;
    }
    if (line.size() >= 19 && line[8] == ' ' &&
        std::isdigit(static_cast<unsigned char>(line[0])) &&
        std::isdigit(static_cast<unsigned char>(line[7])) &&
        std::isdigit(static_cast<unsigned char>(line[9])) &&
        std::isdigit(static_cast<unsigned char>(line[17]))) {
        return TickFmt::D_HIST;
    }
    if (commas == 2) return TickFmt::A_TBA;
    if (commas == 3) return TickFmt::B_TBAV;
    if (commas == 5) return TickFmt::E_OHLCV;
    return TickFmt::Unknown;
}

static int64_t parse_dukascopy_ts(const char* p) {
    // YYYY.MM.DD,HH:MM:SS.mmm,...
    int Y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int M = (p[5]-'0')*10 + (p[6]-'0');
    int D = (p[8]-'0')*10 + (p[9]-'0');
    if (p[10] != ',') return -1;
    int h = (p[11]-'0')*10 + (p[12]-'0');
    int m = (p[14]-'0')*10 + (p[15]-'0');
    int s = (p[17]-'0')*10 + (p[18]-'0');
    if (p[19] != '.') return -1;
    int ms = 0;
    if (std::isdigit(static_cast<unsigned char>(p[20]))) ms = (p[20]-'0') * 100;
    if (std::isdigit(static_cast<unsigned char>(p[21]))) ms += (p[21]-'0') * 10;
    if (std::isdigit(static_cast<unsigned char>(p[22]))) ms += (p[22]-'0');
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    return (int64_t)timegm(&tm) * 1000 + ms;
}

static int64_t parse_histdata_ts(const char* p) {
    // YYYYMMDD HHMMSSmmm,bid,ask,vol
    int Y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int M = (p[4]-'0')*10 + (p[5]-'0');
    int D = (p[6]-'0')*10 + (p[7]-'0');
    int h = (p[9]-'0')*10 + (p[10]-'0');
    int m = (p[11]-'0')*10 + (p[12]-'0');
    int s = (p[13]-'0')*10 + (p[14]-'0');
    int ms = (p[15]-'0')*100 + (p[16]-'0')*10 + (p[17]-'0');
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    return (int64_t)timegm(&tm) * 1000 + ms;
}

static bool parse_row(const std::string& line, TickFmt fmt, TickRow& out) {
    if (line.empty()) return false;
    if (fmt != TickFmt::C_DUKA && fmt != TickFmt::D_HIST) {
        for (size_t i = 0; i < std::min<size_t>(12, line.size()); ++i) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (std::isalpha(c)) return false;
        }
    }
    const char* p = line.c_str();
    char* e = nullptr;
    if (fmt == TickFmt::A_TBA || fmt == TickFmt::B_TBAV) {
        out.ts_ms = std::strtoll(p, &e, 10);
        if (!e || *e != ',') return false;
        p = e + 1;
        out.bid = std::strtod(p, &e);
        if (!e || *e != ',') return false;
        p = e + 1;
        out.ask = std::strtod(p, &e);
        return true;
    }
    if (fmt == TickFmt::C_DUKA) {
        if (line.size() < 23) return false;
        out.ts_ms = parse_dukascopy_ts(p);
        if (out.ts_ms < 0) return false;
        // After 23 chars: ",bid,ask,vol"
        const char* q = line.c_str() + 23;
        if (*q != ',') return false;
        ++q;
        out.bid = std::strtod(q, &e);
        if (!e || *e != ',') return false;
        q = e + 1;
        out.ask = std::strtod(q, &e);
        return true;
    }
    if (fmt == TickFmt::D_HIST) {
        if (line.size() < 19) return false;
        out.ts_ms = parse_histdata_ts(p);
        if (out.ts_ms < 0) return false;
        const char* q = line.c_str() + 18;
        if (*q != ',') return false;
        ++q;
        out.bid = std::strtod(q, &e);
        if (!e || *e != ',') return false;
        q = e + 1;
        out.ask = std::strtod(q, &e);
        return true;
    }
    if (fmt == TickFmt::E_OHLCV) {
        out.ts_ms = std::strtoll(p, &e, 10);
        if (!e || *e != ',') return false;
        // skip open, high, low
        for (int i = 0; i < 3; ++i) {
            if (*e != ',') return false;
            p = e + 1;
            std::strtod(p, &e);
        }
        if (*e != ',') return false;
        p = e + 1;
        const double close = std::strtod(p, &e);
        out.bid = out.ask = close;
        return true;
    }
    return false;
}

// =============================================================================
// CLI parsing
// =============================================================================

struct Args {
    std::string tape;
    std::string engine_kind = "2h";   // "2h" | "4h" | "d1"
    std::string mode        = "baseline"; // "baseline" | "tuned"
    double loss_cut_override = std::numeric_limits<double>::quiet_NaN();
    double be_arm_override   = std::numeric_limits<double>::quiet_NaN();
    double be_buffer_override= std::numeric_limits<double>::quiet_NaN();
    double lot_override      = std::numeric_limits<double>::quiet_NaN();
    double max_spread_override= std::numeric_limits<double>::quiet_NaN();
    std::string trades_path  = "xtf_trades.csv";
    std::string report_path  = "xtf_report.csv";
    int64_t limit = -1;
    int     warmup = 5000;
    bool    quiet  = false;
};

static int parse_args(int argc, char** argv, Args& args) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <ticks.csv> [--engine 2h|4h|d1] [--mode baseline|tuned]\n"
            "  [--loss-cut <pct>] [--be-arm <pct>] [--be-buffer <pct>]\n"
            "  [--lot <v>] [--max-spread <pts>]\n"
            "  [--trades <f>] [--report <f>] [--limit <N>] [--warmup <N>] [--quiet]\n",
            argv[0]);
        return 2;
    }
    args.tape = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if      (a == "--engine")     args.engine_kind = need("--engine");
        else if (a == "--mode")       args.mode        = need("--mode");
        else if (a == "--loss-cut")   args.loss_cut_override  = std::strtod(need("--loss-cut"), nullptr);
        else if (a == "--be-arm")     args.be_arm_override    = std::strtod(need("--be-arm"), nullptr);
        else if (a == "--be-buffer")  args.be_buffer_override = std::strtod(need("--be-buffer"), nullptr);
        else if (a == "--lot")        args.lot_override       = std::strtod(need("--lot"), nullptr);
        else if (a == "--max-spread") args.max_spread_override= std::strtod(need("--max-spread"), nullptr);
        else if (a == "--trades")     args.trades_path = need("--trades");
        else if (a == "--report")     args.report_path = need("--report");
        else if (a == "--limit")      args.limit  = std::strtoll(need("--limit"), nullptr, 10);
        else if (a == "--warmup")     args.warmup = (int)std::strtol(need("--warmup"), nullptr, 10);
        else if (a == "--quiet")      args.quiet  = true;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    return 0;
}

// =============================================================================
// H1 bar construction from ticks. The XTF engines consume H1 bars
// (XauTrendFollow2h synthesises H2 from H1 internally; XauTrendFollow4h
// consumes the H1 we aggregate -> H4 here; XauTrendFollowD1 consumes H4).
// =============================================================================

struct H1Bar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
    int     n_ticks = 0;
};

static int64_t h1_bucket_ms(int64_t ts_ms) {
    return (ts_ms / 3600000LL) * 3600000LL;
}
static int64_t h4_bucket_ms(int64_t ts_ms) {
    return (ts_ms / 14400000LL) * 14400000LL;
}

// =============================================================================
// Trade collection
// =============================================================================

struct TradeOut {
    int64_t entry_ts = 0;
    int64_t exit_ts  = 0;
    std::string symbol;
    std::string side;
    std::string engine;
    double entry = 0.0;
    double exit  = 0.0;
    double pnl   = 0.0;
    double mfe   = 0.0;
    double mae   = 0.0;
    std::string exit_reason;
};

// =============================================================================
// run_2h_engine -- fully wired path
// =============================================================================
static int run_2h_engine(const Args& args) {
    using namespace omega;
    XauTrendFollow2hEngine eng;
    eng.shadow_mode = true;
    eng.enabled     = true;
    if (!std::isnan(args.lot_override))         eng.lot        = args.lot_override;
    if (!std::isnan(args.max_spread_override))  eng.max_spread = args.max_spread_override;

    // S63 mode + overrides
    if (args.mode == "tuned") {
        eng.LOSS_CUT_PCT  = 0.05;
        eng.BE_ARM_PCT    = 0.03;
        eng.BE_BUFFER_PCT = 0.012;
    } else {
        eng.LOSS_CUT_PCT  = 0.0;
        eng.BE_ARM_PCT    = 0.0;
        eng.BE_BUFFER_PCT = 0.0;
    }
    if (!std::isnan(args.loss_cut_override))    eng.LOSS_CUT_PCT  = args.loss_cut_override;
    if (!std::isnan(args.be_arm_override))      eng.BE_ARM_PCT    = args.be_arm_override;
    if (!std::isnan(args.be_buffer_override))   eng.BE_BUFFER_PCT = args.be_buffer_override;

    eng.init();

    std::vector<TradeOut> trades;
    auto on_close = [&](const omega::TradeRecord& tr) {
        TradeOut t;
        t.entry_ts = tr.entryTs;
        t.exit_ts  = tr.exitTs;
        t.symbol   = tr.symbol;
        t.side     = tr.side;
        t.engine   = tr.engine;
        t.entry    = tr.entryPrice;
        t.exit     = tr.exitPrice;
        t.pnl      = tr.pnl;
        t.mfe      = tr.mfe;
        t.mae      = tr.mae;
        t.exit_reason = tr.exitReason;
        trades.push_back(std::move(t));
    };

    std::ifstream in(args.tape);
    if (!in) {
        std::fprintf(stderr, "ERROR: cannot open tape: %s\n", args.tape.c_str());
        return 1;
    }
    std::string line;
    TickFmt fmt = TickFmt::Unknown;
    // First line -> detect format
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        fmt = detect_format(line);
        if (fmt != TickFmt::Unknown) break;
    }
    if (fmt == TickFmt::Unknown) {
        std::fprintf(stderr, "ERROR: cannot detect tape format\n");
        return 1;
    }
    if (!args.quiet) {
        std::fprintf(stderr, "[XTF-BT] format detected, beginning replay\n");
    }
    // re-position to start of file (re-open)
    in.close();
    in.open(args.tape);
    if (!in) return 1;

    H1Bar cur_h1{};
    bool cur_h1_open = false;
    int64_t cur_h1_bucket = 0;
    int64_t row_count = 0, parse_skips = 0;

    auto emit_h1 = [&](const H1Bar& bar, double bid, double ask, int64_t now_ms) {
        XauTf2hBar tf{};
        tf.bar_start_ms = bar.bar_start_ms;
        tf.open  = bar.open;
        tf.high  = bar.high;
        tf.low   = bar.low;
        tf.close = bar.close;
        eng.on_h1_bar(tf, bid, ask, now_ms, on_close);
    };

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        TickRow r;
        if (!parse_row(line, fmt, r)) { ++parse_skips; continue; }
        ++row_count;
        if (args.limit > 0 && row_count > args.limit) break;

        // Tick exit management
        eng.on_tick(r.bid, r.ask, r.ts_ms, on_close);

        // H1 bar aggregation
        const int64_t bucket = h1_bucket_ms(r.ts_ms);
        const double mid = (r.bid + r.ask) * 0.5;
        if (!cur_h1_open) {
            cur_h1 = {};
            cur_h1.bar_start_ms = bucket;
            cur_h1.open = cur_h1.high = cur_h1.low = cur_h1.close = mid;
            cur_h1.n_ticks = 1;
            cur_h1_bucket = bucket;
            cur_h1_open = true;
        } else if (bucket != cur_h1_bucket) {
            emit_h1(cur_h1, r.bid, r.ask, r.ts_ms);
            cur_h1 = {};
            cur_h1.bar_start_ms = bucket;
            cur_h1.open = cur_h1.high = cur_h1.low = cur_h1.close = mid;
            cur_h1.n_ticks = 1;
            cur_h1_bucket = bucket;
            cur_h1_open = true;
        } else {
            if (mid > cur_h1.high) cur_h1.high = mid;
            if (mid < cur_h1.low)  cur_h1.low  = mid;
            cur_h1.close = mid;
            ++cur_h1.n_ticks;
        }
    }
    // Flush final bar (force-close any open positions instead of holding)
    eng.force_close(/*bid*/0.0, /*ask*/0.0, /*now_ms*/0, on_close, "END_OF_DATA");

    if (!args.quiet) {
        std::fprintf(stderr, "[XTF-BT] rows=%lld skips=%lld trades=%zu\n",
                     (long long)row_count, (long long)parse_skips, trades.size());
    }

    // Write trades.csv
    std::ofstream tout(args.trades_path);
    if (!tout) {
        std::fprintf(stderr, "ERROR: cannot open trades file: %s\n", args.trades_path.c_str());
        return 1;
    }
    tout << "entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,"
            "mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score\n";
    for (const auto& t : trades) {
        const int64_t hold = (t.exit_ts > t.entry_ts) ? (t.exit_ts - t.entry_ts) : 0;
        tout << t.entry_ts << "," << t.exit_ts << "," << t.symbol << ","
             << t.side << "," << t.engine << ","
             << t.entry << "," << t.exit << "," << t.pnl << ","
             << t.mfe << "," << t.mae << "," << t.exit_reason << ","
             << hold << ",0,0\n";
    }
    tout.close();

    // Aggregate stats
    int n = (int)trades.size();
    int wins = 0, n_tp = 0, n_sl = 0, n_lc = 0, n_be = 0, n_other = 0;
    double gross = 0.0, sum_pos = 0.0, sum_neg = 0.0;
    double worst = 0.0, best = 0.0;
    for (const auto& t : trades) {
        gross += t.pnl;
        if (t.pnl > 0) { sum_pos += t.pnl; ++wins; }
        else if (t.pnl < 0) sum_neg += t.pnl;
        if (t.pnl < worst) worst = t.pnl;
        if (t.pnl > best)  best  = t.pnl;
        if      (t.exit_reason == "TP_HIT")     ++n_tp;
        else if (t.exit_reason == "SL_HIT")     ++n_sl;
        else if (t.exit_reason == "LOSS_CUT")   ++n_lc;
        else if (t.exit_reason == "BE_CUT")     ++n_be;
        else                                    ++n_other;
    }
    const double pf = (sum_neg < 0) ? (sum_pos / -sum_neg) : 0.0;
    const double win_rate = (n > 0) ? (100.0 * wins / n) : 0.0;

    std::ofstream rep(args.report_path);
    if (!rep) {
        std::fprintf(stderr, "ERROR: cannot open report file: %s\n", args.report_path.c_str());
        return 1;
    }
    rep << "metric,value\n";
    rep << "engine_name,XauTrendFollow2hEngine\n";
    rep << "n_trades," << n << "\n";
    rep << "wins," << wins << "\n";
    rep << "win_rate_pct," << win_rate << "\n";
    rep << "gross_pnl," << gross << "\n";
    rep << "sum_pos," << sum_pos << "\n";
    rep << "sum_neg," << sum_neg << "\n";
    rep << "pf," << pf << "\n";
    rep << "n_tp_hit," << n_tp << "\n";
    rep << "n_sl_hit," << n_sl << "\n";
    rep << "n_loss_cut," << n_lc << "\n";
    rep << "n_be_cut," << n_be << "\n";
    rep << "n_other," << n_other << "\n";
    rep << "worst_trade," << worst << "\n";
    rep << "best_trade," << best << "\n";
    rep << "loss_cut_pct," << eng.LOSS_CUT_PCT << "\n";
    rep << "be_arm_pct," << eng.BE_ARM_PCT << "\n";
    rep << "be_buffer_pct," << eng.BE_BUFFER_PCT << "\n";
    rep.close();

    return 0;
}

// =============================================================================
// run_4h_engine -- fully wired path (filled in 2026-05-14 part-Z)
//
// Mirrors run_2h_engine() exactly, with the following XauTrendFollow4hEngine-
// specific substitutions:
//   - Bar type:   XauTfBar         (defined in XauTrendFollow4hEngine.hpp)
//   - Bar bucket: h4_bucket_ms()   (14400000 ms = H4)
//   - Bar call:   eng.on_h4_bar(tf, bid, ask, atr14_external, now_ms, cb)
//                 atr14_external = 0.0 -> engine computes ATR14 locally from
//                 its own bar history (see XauTrendFollow4hEngine.hpp:290-294;
//                 the external-ATR override is a production-stack convenience
//                 that pulls from g_bars_gold.h4.ind.atr14, but the harness is
//                 self-contained so we let the engine compute locally).
//   - engine_name in report: XauTrendFollow4hEngine
// All other logic (on_tick management, CSV writing, stats aggregation, S63
// CLI overrides, mode-baseline-vs-tuned defaults) is byte-equivalent to the
// 2h path.
// =============================================================================
static int run_4h_engine(const Args& args) {
    using namespace omega;
    XauTrendFollow4hEngine eng;
    eng.shadow_mode = true;
    eng.enabled     = true;
    if (!std::isnan(args.lot_override))         eng.lot        = args.lot_override;
    if (!std::isnan(args.max_spread_override))  eng.max_spread = args.max_spread_override;

    // S63 mode + overrides (mirrors run_2h_engine)
    if (args.mode == "tuned") {
        eng.LOSS_CUT_PCT  = 0.05;
        eng.BE_ARM_PCT    = 0.03;
        eng.BE_BUFFER_PCT = 0.012;
    } else {
        eng.LOSS_CUT_PCT  = 0.0;
        eng.BE_ARM_PCT    = 0.0;
        eng.BE_BUFFER_PCT = 0.0;
    }
    if (!std::isnan(args.loss_cut_override))    eng.LOSS_CUT_PCT  = args.loss_cut_override;
    if (!std::isnan(args.be_arm_override))      eng.BE_ARM_PCT    = args.be_arm_override;
    if (!std::isnan(args.be_buffer_override))   eng.BE_BUFFER_PCT = args.be_buffer_override;

    eng.init();

    std::vector<TradeOut> trades;
    auto on_close = [&](const omega::TradeRecord& tr) {
        TradeOut t;
        t.entry_ts = tr.entryTs;
        t.exit_ts  = tr.exitTs;
        t.symbol   = tr.symbol;
        t.side     = tr.side;
        t.engine   = tr.engine;
        t.entry    = tr.entryPrice;
        t.exit     = tr.exitPrice;
        t.pnl      = tr.pnl;
        t.mfe      = tr.mfe;
        t.mae      = tr.mae;
        t.exit_reason = tr.exitReason;
        trades.push_back(std::move(t));
    };

    std::ifstream in(args.tape);
    if (!in) {
        std::fprintf(stderr, "ERROR: cannot open tape: %s\n", args.tape.c_str());
        return 1;
    }
    std::string line;
    TickFmt fmt = TickFmt::Unknown;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        fmt = detect_format(line);
        if (fmt != TickFmt::Unknown) break;
    }
    if (fmt == TickFmt::Unknown) {
        std::fprintf(stderr, "ERROR: cannot detect tape format\n");
        return 1;
    }
    if (!args.quiet) {
        std::fprintf(stderr, "[XTF-BT] format detected, beginning replay (4h)\n");
    }
    in.close();
    in.open(args.tape);
    if (!in) return 1;

    // H4 bar aggregation state (mirrors H1 aggregator in run_2h_engine).
    H1Bar cur_h4{};         // same struct layout; only the bucket cadence differs
    bool cur_h4_open = false;
    int64_t cur_h4_bucket = 0;
    int64_t row_count = 0, parse_skips = 0;

    auto emit_h4 = [&](const H1Bar& bar, double bid, double ask, int64_t now_ms) {
        XauTfBar tf{};
        tf.bar_start_ms = bar.bar_start_ms;
        tf.open  = bar.open;
        tf.high  = bar.high;
        tf.low   = bar.low;
        tf.close = bar.close;
        // atr14_external = 0.0 -> engine computes ATR14 locally from bars_
        eng.on_h4_bar(tf, bid, ask, /*atr14_external=*/0.0, now_ms, on_close);
    };

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        TickRow r;
        if (!parse_row(line, fmt, r)) { ++parse_skips; continue; }
        ++row_count;
        if (args.limit > 0 && row_count > args.limit) break;

        // Tick exit management
        eng.on_tick(r.bid, r.ask, r.ts_ms, on_close);

        // H4 bar aggregation
        const int64_t bucket = h4_bucket_ms(r.ts_ms);
        const double mid = (r.bid + r.ask) * 0.5;
        if (!cur_h4_open) {
            cur_h4 = {};
            cur_h4.bar_start_ms = bucket;
            cur_h4.open = cur_h4.high = cur_h4.low = cur_h4.close = mid;
            cur_h4.n_ticks = 1;
            cur_h4_bucket = bucket;
            cur_h4_open = true;
        } else if (bucket != cur_h4_bucket) {
            emit_h4(cur_h4, r.bid, r.ask, r.ts_ms);
            cur_h4 = {};
            cur_h4.bar_start_ms = bucket;
            cur_h4.open = cur_h4.high = cur_h4.low = cur_h4.close = mid;
            cur_h4.n_ticks = 1;
            cur_h4_bucket = bucket;
            cur_h4_open = true;
        } else {
            if (mid > cur_h4.high) cur_h4.high = mid;
            if (mid < cur_h4.low)  cur_h4.low  = mid;
            cur_h4.close = mid;
            ++cur_h4.n_ticks;
        }
    }
    eng.force_close(/*bid*/0.0, /*ask*/0.0, /*now_ms*/0, on_close, "END_OF_DATA");

    if (!args.quiet) {
        std::fprintf(stderr, "[XTF-BT] rows=%lld skips=%lld trades=%zu\n",
                     (long long)row_count, (long long)parse_skips, trades.size());
    }

    // Write trades.csv (schema parity with run_2h_engine)
    std::ofstream tout(args.trades_path);
    if (!tout) {
        std::fprintf(stderr, "ERROR: cannot open trades file: %s\n", args.trades_path.c_str());
        return 1;
    }
    tout << "entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,"
            "mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score\n";
    for (const auto& t : trades) {
        const int64_t hold = (t.exit_ts > t.entry_ts) ? (t.exit_ts - t.entry_ts) : 0;
        tout << t.entry_ts << "," << t.exit_ts << "," << t.symbol << ","
             << t.side << "," << t.engine << ","
             << t.entry << "," << t.exit << "," << t.pnl << ","
             << t.mfe << "," << t.mae << "," << t.exit_reason << ","
             << hold << ",0,0\n";
    }
    tout.close();

    // Aggregate stats (same logic + schema as run_2h_engine)
    int n = (int)trades.size();
    int wins = 0, n_tp = 0, n_sl = 0, n_lc = 0, n_be = 0, n_other = 0;
    double gross = 0.0, sum_pos = 0.0, sum_neg = 0.0;
    double worst = 0.0, best = 0.0;
    for (const auto& t : trades) {
        gross += t.pnl;
        if (t.pnl > 0) { sum_pos += t.pnl; ++wins; }
        else if (t.pnl < 0) sum_neg += t.pnl;
        if (t.pnl < worst) worst = t.pnl;
        if (t.pnl > best)  best  = t.pnl;
        if      (t.exit_reason == "TP_HIT")     ++n_tp;
        else if (t.exit_reason == "SL_HIT")     ++n_sl;
        else if (t.exit_reason == "LOSS_CUT")   ++n_lc;
        else if (t.exit_reason == "BE_CUT")     ++n_be;
        else                                    ++n_other;
    }
    const double pf = (sum_neg < 0) ? (sum_pos / -sum_neg) : 0.0;
    const double win_rate = (n > 0) ? (100.0 * wins / n) : 0.0;

    std::ofstream rep(args.report_path);
    if (!rep) {
        std::fprintf(stderr, "ERROR: cannot open report file: %s\n", args.report_path.c_str());
        return 1;
    }
    rep << "metric,value\n";
    rep << "engine_name,XauTrendFollow4hEngine\n";
    rep << "n_trades," << n << "\n";
    rep << "wins," << wins << "\n";
    rep << "win_rate_pct," << win_rate << "\n";
    rep << "gross_pnl," << gross << "\n";
    rep << "sum_pos," << sum_pos << "\n";
    rep << "sum_neg," << sum_neg << "\n";
    rep << "pf," << pf << "\n";
    rep << "n_tp_hit," << n_tp << "\n";
    rep << "n_sl_hit," << n_sl << "\n";
    rep << "n_loss_cut," << n_lc << "\n";
    rep << "n_be_cut," << n_be << "\n";
    rep << "n_other," << n_other << "\n";
    rep << "worst_trade," << worst << "\n";
    rep << "best_trade," << best << "\n";
    rep << "loss_cut_pct," << eng.LOSS_CUT_PCT << "\n";
    rep << "be_arm_pct," << eng.BE_ARM_PCT << "\n";
    rep << "be_buffer_pct," << eng.BE_BUFFER_PCT << "\n";
    rep.close();

    return 0;
}

// =============================================================================
// run_d1_engine -- fully wired path (filled in 2026-05-14 part-Z)
//
// Mirrors run_2h_engine() / run_4h_engine() with the following
// XauTrendFollowD1Engine-specific substitutions:
//   - Bar type:   XauTfD1Bar       (defined in XauTrendFollowD1Engine.hpp)
//   - Bar bucket: h4_bucket_ms()   (D1 consumes H4 bars and aggregates to D1
//                                   internally via UTC date_id; see
//                                   XauTrendFollowD1Engine.hpp:103-119, 245-269)
//   - Bar call:   eng.on_h4_bar(tf, bid, ask, now_ms, cb)
//                 NOTE: D1 signature has NO atr14_external argument
//                 (see XauTrendFollowD1Engine.hpp:245-248). D1 computes
//                 daily ATR14 from its own synthesised daily bars.
//   - engine_name in report: XauTrendFollowD1Engine
//
// Small-n caveat (from XauTrendFollowD1Engine.hpp:174-179): D1 cadence
// is ~2 trades/month combined across 3 cells (n=15-31 per cell over 30
// months in the original Pass-8 deep_dive). Phase 3 walk-forward windows
// will have wide error bars; a per-window trade-count floor may want to
// be added at the WF-driver layer for D1 specifically.
// =============================================================================
static int run_d1_engine(const Args& args) {
    using namespace omega;
    XauTrendFollowD1Engine eng;
    eng.shadow_mode = true;
    eng.enabled     = true;
    if (!std::isnan(args.lot_override))         eng.lot        = args.lot_override;
    if (!std::isnan(args.max_spread_override))  eng.max_spread = args.max_spread_override;

    // S63 mode + overrides (mirrors run_2h_engine / run_4h_engine)
    if (args.mode == "tuned") {
        eng.LOSS_CUT_PCT  = 0.05;
        eng.BE_ARM_PCT    = 0.03;
        eng.BE_BUFFER_PCT = 0.012;
    } else {
        eng.LOSS_CUT_PCT  = 0.0;
        eng.BE_ARM_PCT    = 0.0;
        eng.BE_BUFFER_PCT = 0.0;
    }
    if (!std::isnan(args.loss_cut_override))    eng.LOSS_CUT_PCT  = args.loss_cut_override;
    if (!std::isnan(args.be_arm_override))      eng.BE_ARM_PCT    = args.be_arm_override;
    if (!std::isnan(args.be_buffer_override))   eng.BE_BUFFER_PCT = args.be_buffer_override;

    eng.init();

    std::vector<TradeOut> trades;
    auto on_close = [&](const omega::TradeRecord& tr) {
        TradeOut t;
        t.entry_ts = tr.entryTs;
        t.exit_ts  = tr.exitTs;
        t.symbol   = tr.symbol;
        t.side     = tr.side;
        t.engine   = tr.engine;
        t.entry    = tr.entryPrice;
        t.exit     = tr.exitPrice;
        t.pnl      = tr.pnl;
        t.mfe      = tr.mfe;
        t.mae      = tr.mae;
        t.exit_reason = tr.exitReason;
        trades.push_back(std::move(t));
    };

    std::ifstream in(args.tape);
    if (!in) {
        std::fprintf(stderr, "ERROR: cannot open tape: %s\n", args.tape.c_str());
        return 1;
    }
    std::string line;
    TickFmt fmt = TickFmt::Unknown;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        fmt = detect_format(line);
        if (fmt != TickFmt::Unknown) break;
    }
    if (fmt == TickFmt::Unknown) {
        std::fprintf(stderr, "ERROR: cannot detect tape format\n");
        return 1;
    }
    if (!args.quiet) {
        std::fprintf(stderr, "[XTF-BT] format detected, beginning replay (d1)\n");
    }
    in.close();
    in.open(args.tape);
    if (!in) return 1;

    // H4 bar aggregation state. D1 ingests H4 bars and synthesises D1 from
    // them internally (see XauTrendFollowD1Engine.hpp _finalise_day_and_evaluate).
    H1Bar cur_h4{};
    bool cur_h4_open = false;
    int64_t cur_h4_bucket = 0;
    int64_t row_count = 0, parse_skips = 0;

    auto emit_h4 = [&](const H1Bar& bar, double bid, double ask, int64_t now_ms) {
        XauTfD1Bar tf{};
        tf.bar_start_ms = bar.bar_start_ms;
        tf.open  = bar.open;
        tf.high  = bar.high;
        tf.low   = bar.low;
        tf.close = bar.close;
        // D1 signature has no atr14_external arg.
        eng.on_h4_bar(tf, bid, ask, now_ms, on_close);
    };

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        TickRow r;
        if (!parse_row(line, fmt, r)) { ++parse_skips; continue; }
        ++row_count;
        if (args.limit > 0 && row_count > args.limit) break;

        // Tick exit management
        eng.on_tick(r.bid, r.ask, r.ts_ms, on_close);

        // H4 bar aggregation
        const int64_t bucket = h4_bucket_ms(r.ts_ms);
        const double mid = (r.bid + r.ask) * 0.5;
        if (!cur_h4_open) {
            cur_h4 = {};
            cur_h4.bar_start_ms = bucket;
            cur_h4.open = cur_h4.high = cur_h4.low = cur_h4.close = mid;
            cur_h4.n_ticks = 1;
            cur_h4_bucket = bucket;
            cur_h4_open = true;
        } else if (bucket != cur_h4_bucket) {
            emit_h4(cur_h4, r.bid, r.ask, r.ts_ms);
            cur_h4 = {};
            cur_h4.bar_start_ms = bucket;
            cur_h4.open = cur_h4.high = cur_h4.low = cur_h4.close = mid;
            cur_h4.n_ticks = 1;
            cur_h4_bucket = bucket;
            cur_h4_open = true;
        } else {
            if (mid > cur_h4.high) cur_h4.high = mid;
            if (mid < cur_h4.low)  cur_h4.low  = mid;
            cur_h4.close = mid;
            ++cur_h4.n_ticks;
        }
    }
    eng.force_close(/*bid*/0.0, /*ask*/0.0, /*now_ms*/0, on_close, "END_OF_DATA");

    if (!args.quiet) {
        std::fprintf(stderr, "[XTF-BT] rows=%lld skips=%lld trades=%zu\n",
                     (long long)row_count, (long long)parse_skips, trades.size());
    }

    // Write trades.csv (schema parity with run_2h_engine)
    std::ofstream tout(args.trades_path);
    if (!tout) {
        std::fprintf(stderr, "ERROR: cannot open trades file: %s\n", args.trades_path.c_str());
        return 1;
    }
    tout << "entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,"
            "mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score\n";
    for (const auto& t : trades) {
        const int64_t hold = (t.exit_ts > t.entry_ts) ? (t.exit_ts - t.entry_ts) : 0;
        tout << t.entry_ts << "," << t.exit_ts << "," << t.symbol << ","
             << t.side << "," << t.engine << ","
             << t.entry << "," << t.exit << "," << t.pnl << ","
             << t.mfe << "," << t.mae << "," << t.exit_reason << ","
             << hold << ",0,0\n";
    }
    tout.close();

    // Aggregate stats
    int n = (int)trades.size();
    int wins = 0, n_tp = 0, n_sl = 0, n_lc = 0, n_be = 0, n_other = 0;
    double gross = 0.0, sum_pos = 0.0, sum_neg = 0.0;
    double worst = 0.0, best = 0.0;
    for (const auto& t : trades) {
        gross += t.pnl;
        if (t.pnl > 0) { sum_pos += t.pnl; ++wins; }
        else if (t.pnl < 0) sum_neg += t.pnl;
        if (t.pnl < worst) worst = t.pnl;
        if (t.pnl > best)  best  = t.pnl;
        if      (t.exit_reason == "TP_HIT")     ++n_tp;
        else if (t.exit_reason == "SL_HIT")     ++n_sl;
        else if (t.exit_reason == "LOSS_CUT")   ++n_lc;
        else if (t.exit_reason == "BE_CUT")     ++n_be;
        else                                    ++n_other;
    }
    const double pf = (sum_neg < 0) ? (sum_pos / -sum_neg) : 0.0;
    const double win_rate = (n > 0) ? (100.0 * wins / n) : 0.0;

    std::ofstream rep(args.report_path);
    if (!rep) {
        std::fprintf(stderr, "ERROR: cannot open report file: %s\n", args.report_path.c_str());
        return 1;
    }
    rep << "metric,value\n";
    rep << "engine_name,XauTrendFollowD1Engine\n";
    rep << "n_trades," << n << "\n";
    rep << "wins," << wins << "\n";
    rep << "win_rate_pct," << win_rate << "\n";
    rep << "gross_pnl," << gross << "\n";
    rep << "sum_pos," << sum_pos << "\n";
    rep << "sum_neg," << sum_neg << "\n";
    rep << "pf," << pf << "\n";
    rep << "n_tp_hit," << n_tp << "\n";
    rep << "n_sl_hit," << n_sl << "\n";
    rep << "n_loss_cut," << n_lc << "\n";
    rep << "n_be_cut," << n_be << "\n";
    rep << "n_other," << n_other << "\n";
    rep << "worst_trade," << worst << "\n";
    rep << "best_trade," << best << "\n";
    rep << "loss_cut_pct," << eng.LOSS_CUT_PCT << "\n";
    rep << "be_arm_pct," << eng.BE_ARM_PCT << "\n";
    rep << "be_buffer_pct," << eng.BE_BUFFER_PCT << "\n";
    rep.close();

    return 0;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
    Args args;
    int r = parse_args(argc, argv, args);
    if (r != 0) return r;

    if (!args.quiet) {
        std::fprintf(stderr,
            "[XTF-BT] tape=%s engine=%s mode=%s\n",
            args.tape.c_str(), args.engine_kind.c_str(), args.mode.c_str());
    }

    if      (args.engine_kind == "2h") return run_2h_engine(args);
    else if (args.engine_kind == "4h") return run_4h_engine(args);
    else if (args.engine_kind == "d1") return run_d1_engine(args);
    else {
        std::fprintf(stderr, "unknown --engine: %s (expected 2h|4h|d1)\n",
                     args.engine_kind.c_str());
        return 2;
    }
}
