// =============================================================================
// UstecTrendFollow5mBacktest.cpp -- dedicated harness for UstecTrendFollow5mEngine
// =============================================================================
// Created 2026-05-14 (session 2026-05-14 part-T, follow-up to part-S/handoff
// SESSION_HANDOFF_2026-05-14h.md §"Recommended next-session focus" item 1).
//
// Purpose:
//   The promotion gate documented at engine_init.hpp:964-967 reads:
//
//     > g_ustec_tf_5m.enabled stays FALSE until a fresh-tape backtest sweep
//     > confirms S63 + S37 widened SL/TP profile is net-positive on USTEC.
//
//   The four existing UTF5m sweep harnesses in backtest/ all pre-date the
//   part-L S63 wiring and re-implement the engine logic standalone (separate
//   `class UstecTfEngine { ... }` per file, no `#include` of the production
//   engine). They cannot answer the promotion-gate question without porting
//   the S63 management-path into each standalone copy, and any such port
//   risks drift from the production engine over time.
//
//   This harness instantiates the actual omega::UstecTrendFollow5mEngine
//   directly, the way backtest/VWAPReversionBacktest.cpp does for the four
//   live VWAPReversionEngine instances. Logic drift is structurally
//   impossible: if the engine header changes, the harness recompiles.
//
// Scope of this harness (part-T phased decision):
//   The S34-B static-constexpr guards (PROVE_IT_SECS, PROVE_IT_MIN_FAVOURABLE_PTS,
//   MIN_SL_PTS_FLOOR, MIN_ATR_PTS) plus the cell sl_mult / tp_mult are
//   intentionally NOT exposed at the CLI. They stay pinned at the engine-
//   header defaults. The promotion gate text asks specifically whether
//   "S63 + S37 widened SL/TP profile is net-positive" -- S37 is the engine
//   header default, so it is always on, and the sweep dimension is purely
//   the S63 trio (LOSS_CUT_PCT / BE_ARM_PCT / BE_BUFFER_PCT).
//
//   If a later session needs to vary the S34-B guards (per the S70 VWR
//   precedent — promote static-constexpr → non-static member, defaults
//   preserved), that becomes a separate engine-touching commit followed
//   by a CLI-extension commit on this harness. Today's scope: S63 only.
//
// CLI:
//   ./build/UstecTrendFollow5mBacktest <ticks.csv> [options]
//     --mode <m>         baseline | tuned                  (default: tuned)
//                        baseline: LOSS_CUT_PCT=0, BE_ARM_PCT=0, BE_BUFFER_PCT=0
//                                  (S37 widened SL/TP still active -- it lives
//                                   in the static-constexpr kUstecTfCells[].)
//                        tuned:    engine-header defaults
//                                  (LOSS_CUT_PCT=0.08, BE_ARM_PCT=0.05,
//                                   BE_BUFFER_PCT=0.02)
//     --loss-cut <pct>   override LOSS_CUT_PCT  (Phase-1 sweep dimension)
//     --be-arm   <pct>   override BE_ARM_PCT    (Phase-1 sweep dimension)
//     --be-buffer<pct>   override BE_BUFFER_PCT (Phase-1 sweep dimension)
//     --lot <val>        override lot           (default: engine = 0.1)
//     --max-spread <pts> override max_spread    (default: engine = 5.0)
//     --trades <file>    trades CSV output       (default: utf5m_trades.csv)
//     --report <file>    summary report CSV      (default: utf5m_report.csv)
//     --limit <N>        max ticks to process    (default: unlimited)
//     --warmup <N>       warmup ticks pre-trade  (default: 5000)
//     --quiet            suppress engine stdout chatter
//
// Tick CSV formats supported (auto-detected from the first data row):
//   A: timestamp_ms,bid,ask
//   B: timestamp_ms,bid,ask,vol
//   C: YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol         (Dukascopy)
//   D: YYYYMMDD HHMMSSmmm,bid,ask,vol              (HistData M1 tick)
//   E: timestamp_ms,open,high,low,close,vol        (OHLCV; bid=close, ask=close)
//
//   The original fresh-tape used by the VWR USTEC.F sweep
//   (/Users/jo/Tick/NSXUSD_merged.csv) is HistData format D. Other formats
//   are kept available so the same harness can run on Dukascopy tapes or
//   pre-aggregated OHLCV without rebuilds.
//
// Output schema:
//   trades.csv columns mirror VWAPReversionBacktest exactly so the existing
//   walk-forward driver (scripts/vrev_wf_t1.py) can be reused with minimal
//   flag substitutions:
//     entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,
//     mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score
//
//   Note: the production UstecTrendFollow5mEngine does NOT populate
//   TradeRecord.spreadAtEntry (unlike VWAPReversionEngine), so this column
//   is always 0. The engine also has no confluence score, so that column
//   is also always 0. Schema parity is intentional -- the WF driver does
//   not consume these fields, but downstream pandas readers should not
//   break on column-shape changes.
//
//   report.csv columns mirror VWAPReversionBacktest report. New rows:
//     loss_cut_pct, be_arm_pct, be_buffer_pct  -- the effective S63 values
//     prove_it_secs, prove_it_min_fav_pts,
//     min_sl_pts_floor, min_atr_pts             -- engine-pinned S34-B values
//     donchian_sl_mult, donchian_tp_mult,
//     keltner_sl_mult,  keltner_tp_mult         -- engine-pinned S37 values
//
//   Exit-reason histogram in the report covers the engine's full vocabulary:
//     n_tp_hit, n_sl_hit, n_prove_it_fail, n_loss_cut, n_be_cut, n_end_of_data,
//     n_other.
//
// Build:
//   cmake --build build --target UstecTrendFollow5mBacktest --config Release
//   (Mac/Linux uses -O3 -std=c++20; OmegaTimeShim is force-included via -include
//   in the CMake target so std::chrono::steady_clock / system_clock route to
//   simulated time. Same pattern as OmegaBacktest and VWAPReversionBacktest.)
//
// AUTHORITY: NEW file, not core code. Engine header untouched per the part-T
// phased decision. CMakeLists.txt gets one add_executable block (non-core
// configuration file edit). All other source files untouched.
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

// OmegaTimeShim.hpp is force-included via -include / /FI in the CMakeLists
// add_executable block, so it is the very first header in this TU. The
// engine reads time only via the now_ms parameter we pass into on_5m_bar
// / on_tick / force_close -- it has no internal std::chrono / std::time
// dependence -- so the shim is technically not load-bearing here. It is
// included for consistency with the VWR / OmegaBacktest pattern and to
// future-proof against any downstream engine change that introduces such
// a dependency.
#include "OmegaTimeShim.hpp"

#include "../include/UstecTrendFollow5mEngine.hpp"
#include "../include/OmegaTradeLedger.hpp"

// =============================================================================
// Tick row + CSV parsing
// =============================================================================
// Five supported layouts. The first non-empty / non-header line of the input
// determines the format; mixed formats inside one file are an error and rows
// that fail to parse under the chosen format are counted as parse_skips but
// do not abort the run.
// =============================================================================
struct TickRow {
    int64_t ts_ms = 0;   // unix epoch milliseconds
    double  bid   = 0.0;
    double  ask   = 0.0;
};

enum class TickFmt { Unknown, A_TBA, B_TBAV, C_DUKA, D_HIST, E_OHLCV };

static TickFmt detect_format(const std::string& line) {
    // Count commas
    int commas = 0;
    for (char c : line) if (c == ',') ++commas;

    // Dukascopy: starts with "YYYY.MM.DD" (year.month.day with dots in pos 4 + 7)
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

    // HistData M1 tick: "YYYYMMDD HHMMSSmmm,bid,ask,vol" -- 18 chars before
    // the first comma, with a space at position 8 separating date from time.
    // The all-digit timestamp distinguishes this from numeric-leading
    // millisecond-epoch formats which have NO space in the first field.
    if (line.size() >= 19 && line[8] == ' ' &&
        std::isdigit(static_cast<unsigned char>(line[0])) &&
        std::isdigit(static_cast<unsigned char>(line[7])) &&
        std::isdigit(static_cast<unsigned char>(line[9])) &&
        std::isdigit(static_cast<unsigned char>(line[17]))) {
        return TickFmt::D_HIST;
    }

    // Numeric-leading formats are distinguished by comma count
    if (commas == 2) return TickFmt::A_TBA;
    if (commas == 3) return TickFmt::B_TBAV;
    if (commas == 5) return TickFmt::E_OHLCV;
    return TickFmt::Unknown;
}

static bool parse_row(const std::string& line, TickFmt fmt, TickRow& out) {
    if (line.empty()) return false;
    // Skip header-like lines for the all-numeric layouts (any letter in the
    // first 12 chars besides Dukascopy's dots / HistData's space).
    if (fmt != TickFmt::C_DUKA && fmt != TickFmt::D_HIST) {
        for (size_t i = 0; i < std::min<size_t>(12, line.size()); ++i) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (std::isalpha(c)) return false;
        }
    }

    const char* p = line.c_str();
    char* e = nullptr;

    if (fmt == TickFmt::A_TBA || fmt == TickFmt::B_TBAV) {
        const int64_t ts = std::strtoll(p, &e, 10);
        if (*e != ',') return false; p = e + 1;
        const double bid = std::strtod(p, &e);
        if (*e != ',') return false; p = e + 1;
        const double ask = std::strtod(p, &e);
        if (bid <= 0.0 || ask <= 0.0 || bid > ask) return false;
        out.ts_ms = ts;
        out.bid   = bid;
        out.ask   = ask;
        return true;
    }

    if (fmt == TickFmt::E_OHLCV) {
        const int64_t ts = std::strtoll(p, &e, 10);
        if (*e != ',') return false; p = e + 1;
        std::strtod(p, &e); if (*e != ',') return false; p = e + 1; // open
        std::strtod(p, &e); if (*e != ',') return false; p = e + 1; // high
        std::strtod(p, &e); if (*e != ',') return false; p = e + 1; // low
        const double close = std::strtod(p, &e);
        if (close <= 0.0) return false;
        // Synthesize a half-spread of 0.075 (matches OmegaBacktest / VWR convention).
        const double half = 0.075;
        out.ts_ms = ts;
        out.bid   = close - half;
        out.ask   = close + half;
        return true;
    }

    if (fmt == TickFmt::C_DUKA) {
        // YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol
        int Y, M, D, h, m, s, ms;
        if (std::sscanf(p, "%4d.%2d.%2d,%2d:%2d:%2d.%3d", &Y, &M, &D, &h, &m, &s, &ms) != 7)
            return false;
        // Find the second comma (after time field) to locate bid/ask.
        int seen = 0;
        const char* q = p;
        while (*q && seen < 2) { if (*q == ',') ++seen; ++q; }
        if (seen < 2) return false;
        const double bid = std::strtod(q, &e);
        if (*e != ',') return false;
        const double ask = std::strtod(e + 1, nullptr);
        if (bid <= 0.0 || ask <= 0.0 || bid > ask) return false;

        std::tm tm{};
        tm.tm_year = Y - 1900;
        tm.tm_mon  = M - 1;
        tm.tm_mday = D;
        tm.tm_hour = h;
        tm.tm_min  = m;
        tm.tm_sec  = s;
#if defined(__APPLE__) || defined(__linux__)
        const int64_t t = static_cast<int64_t>(timegm(&tm));
#else
        const int64_t t = static_cast<int64_t>(_mkgmtime(&tm));
#endif
        out.ts_ms = t * 1000LL + ms;
        out.bid   = bid;
        out.ask   = ask;
        return true;
    }

    if (fmt == TickFmt::D_HIST) {
        // HistData M1 tick: YYYYMMDD HHMMSSmmm,bid,ask,vol
        // Positions:
        //   0..3   = YYYY
        //   4..5   = MM
        //   6..7   = DD
        //   8      = ' '
        //   9..10  = HH
        //   11..12 = MM (minutes)
        //   13..14 = SS
        //   15..17 = mmm (milliseconds, 3 chars)
        //   18     = ','
        if (line.size() < 19) return false;
        if (line[8] != ' ') return false;
        const auto d = [&](size_t i) -> int {
            return static_cast<int>(static_cast<unsigned char>(line[i]) - '0');
        };
        const int Y  = d(0)*1000 + d(1)*100 + d(2)*10 + d(3);
        const int M  = d(4)*10 + d(5);
        const int D  = d(6)*10 + d(7);
        const int hh = d(9)*10 + d(10);
        const int mm = d(11)*10 + d(12);
        const int ss = d(13)*10 + d(14);
        const int ms = d(15)*100 + d(16)*10 + d(17);
        std::tm tm{};
        tm.tm_year = Y - 1900;
        tm.tm_mon  = M - 1;
        tm.tm_mday = D;
        tm.tm_hour = hh;
        tm.tm_min  = mm;
        tm.tm_sec  = ss;
#if defined(__APPLE__) || defined(__linux__)
        const int64_t t = static_cast<int64_t>(timegm(&tm));
#else
        const int64_t t = static_cast<int64_t>(_mkgmtime(&tm));
#endif
        const int64_t ts_ms = t * 1000LL + ms;

        // Parse bid,ask after position 18 (the first comma)
        const char* q = p + 18;
        if (*q != ',') return false;
        const double bid = std::strtod(q + 1, &e);
        if (*e != ',') return false;
        const double ask = std::strtod(e + 1, nullptr);
        if (bid <= 0.0 || ask <= 0.0 || bid > ask) return false;

        out.ts_ms = ts_ms;
        out.bid   = bid;
        out.ask   = ask;
        return true;
    }

    return false;
}

// =============================================================================
// BarBuilder -- aggregate ticks into UTC-anchored 5m bars
// =============================================================================
// Pattern mirrors backtest/ustec_trend_follow_5m_entry_sweep.cpp BarBuilder:
//   - bar_start_ms = floor(ts_ms / 5min) * 5min  (UTC, no DST shenanigans)
//   - on every tick: update high/low/close of the current bar; if the next
//     tick crosses into a new 5m window, emit the completed bar and start
//     a fresh one anchored at the new window.
//
// Returns true the first time on_tick observes a window crossing, populating
// `out` with the just-completed bar. Caller passes `out` to
// eng.on_5m_bar(out, bid, ask, 0.0, ts_ms, on_close).
// =============================================================================
struct BarBuilder {
    static constexpr int64_t kBarMs = 5LL * 60LL * 1000LL;
    int64_t           cur_start_ms = 0;
    omega::UstecTfBar cur;
    bool              has_bar      = false;

    // Update the in-progress bar with this tick. If this tick is the first tick
    // of a new 5m window, emit the just-completed bar via `out` and return
    // true; otherwise return false.
    bool on_tick(int64_t ts_ms, double bid, double ask, omega::UstecTfBar& out) noexcept {
        const double mid = (bid + ask) * 0.5;
        const int64_t bs = (ts_ms / kBarMs) * kBarMs;
        if (!has_bar) {
            cur_start_ms     = bs;
            cur.bar_start_ms = bs;
            cur.open  = mid;
            cur.high  = mid;
            cur.low   = mid;
            cur.close = mid;
            has_bar   = true;
            return false;
        }
        if (bs != cur_start_ms) {
            out          = cur;
            cur_start_ms = bs;
            cur.bar_start_ms = bs;
            cur.open  = mid;
            cur.high  = mid;
            cur.low   = mid;
            cur.close = mid;
            return true;
        }
        if (mid > cur.high) cur.high = mid;
        if (mid < cur.low)  cur.low  = mid;
        cur.close = mid;
        return false;
    }
};

// =============================================================================
// Stats accumulator
// =============================================================================
// Tracks the same metrics VWAPReversionBacktest's Stats does, with the exit-
// reason vocabulary matching UstecTrendFollow5mEngine: TP_HIT, SL_HIT,
// PROVE_IT_FAIL, LOSS_CUT, BE_CUT, FORCE_CLOSE / END_OF_DATA, plus a residual
// `n_other` bucket for anything unexpected.
// =============================================================================
struct Stats {
    int64_t trades             = 0;
    int64_t wins               = 0;
    double  gross_pnl          = 0.0;
    double  best_trade         = 0.0;
    double  worst_trade        = 0.0;
    int64_t n_tp_hit           = 0;
    int64_t n_sl_hit           = 0;
    int64_t n_prove_it_fail    = 0;
    int64_t n_loss_cut         = 0;
    int64_t n_be_cut           = 0;
    int64_t n_end_of_data      = 0;
    int64_t n_other            = 0;
    std::vector<double> losses;

    void record(const omega::TradeRecord& tr) {
        ++trades;
        gross_pnl += tr.pnl;
        if (tr.pnl > 0.0) ++wins;
        if (tr.pnl > best_trade)  best_trade  = tr.pnl;
        if (tr.pnl < worst_trade) worst_trade = tr.pnl;
        if (tr.pnl < 0.0) losses.push_back(tr.pnl);

        const std::string& r = tr.exitReason;
        if      (r == "TP_HIT")        ++n_tp_hit;
        else if (r == "SL_HIT")        ++n_sl_hit;
        else if (r == "PROVE_IT_FAIL") ++n_prove_it_fail;
        else if (r == "LOSS_CUT")      ++n_loss_cut;
        else if (r == "BE_CUT")        ++n_be_cut;
        else if (r == "END_OF_DATA" ||
                 r == "FORCE_CLOSE")   ++n_end_of_data;
        else                           ++n_other;
    }

    double win_rate_pct() const {
        return trades > 0
            ? (100.0 * static_cast<double>(wins) / static_cast<double>(trades))
            : 0.0;
    }

    // 95th-percentile worst loss: the value such that 95% of losses are
    // smaller in magnitude. Most-negative-first sort + index 5% in.
    double p95_worst_loss() {
        if (losses.empty()) return 0.0;
        std::vector<double> sorted = losses;
        std::sort(sorted.begin(), sorted.end());
        const size_t idx = static_cast<size_t>(0.05 * static_cast<double>(sorted.size()));
        return sorted[idx];
    }
};

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: UstecTrendFollow5mBacktest <ticks.csv> [options]\n"
            "  --mode <m>          baseline | tuned   (default: tuned)\n"
            "                      baseline: LOSS_CUT_PCT=0, BE_ARM_PCT=0, BE_BUFFER_PCT=0\n"
            "                      tuned:    engine header defaults (0.08 / 0.05 / 0.02)\n"
            "  --loss-cut <pct>    override LOSS_CUT_PCT\n"
            "  --be-arm   <pct>    override BE_ARM_PCT\n"
            "  --be-buffer<pct>    override BE_BUFFER_PCT\n"
            "  --lot <val>         override lot          (default: 0.1)\n"
            "  --max-spread <pts>  override max_spread   (default: 5.0)\n"
            "  --trades <file>     trades CSV output     (default: utf5m_trades.csv)\n"
            "  --report <file>     summary report CSV    (default: utf5m_report.csv)\n"
            "  --limit <N>         max ticks to process  (default: unlimited)\n"
            "  --warmup <N>        warmup ticks pre-trade(default: 5000)\n"
            "  --quiet             suppress engine stdout chatter\n");
        return 1;
    }

    const char* in_path     = argv[1];
    const char* trades_path = "utf5m_trades.csv";
    const char* report_path = "utf5m_report.csv";
    std::string mode    = "tuned";
    int64_t     limit   = -1;
    int64_t     warmup  = 5000;
    bool        quiet   = false;

    bool   have_loss_cut_ovr   = false;
    bool   have_be_arm_ovr     = false;
    bool   have_be_buffer_ovr  = false;
    bool   have_lot_ovr        = false;
    bool   have_max_spread_ovr = false;
    double loss_cut_ovr   = 0.0;
    double be_arm_ovr     = 0.0;
    double be_buffer_ovr  = 0.0;
    double lot_ovr        = 0.0;
    double max_spread_ovr = 0.0;

    for (int i = 2; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--mode")        && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--trades")      && i + 1 < argc) trades_path = argv[++i];
        else if (!std::strcmp(argv[i], "--report")      && i + 1 < argc) report_path = argv[++i];
        else if (!std::strcmp(argv[i], "--limit")       && i + 1 < argc) limit  = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--warmup")      && i + 1 < argc) warmup = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--loss-cut")    && i + 1 < argc) { loss_cut_ovr   = std::atof(argv[++i]); have_loss_cut_ovr   = true; }
        else if (!std::strcmp(argv[i], "--be-arm")      && i + 1 < argc) { be_arm_ovr     = std::atof(argv[++i]); have_be_arm_ovr     = true; }
        else if (!std::strcmp(argv[i], "--be-buffer")   && i + 1 < argc) { be_buffer_ovr  = std::atof(argv[++i]); have_be_buffer_ovr  = true; }
        else if (!std::strcmp(argv[i], "--lot")         && i + 1 < argc) { lot_ovr        = std::atof(argv[++i]); have_lot_ovr        = true; }
        else if (!std::strcmp(argv[i], "--max-spread")  && i + 1 < argc) { max_spread_ovr = std::atof(argv[++i]); have_max_spread_ovr = true; }
        else if (!std::strcmp(argv[i], "--quiet"))                       quiet = true;
        else {
            std::fprintf(stderr, "[ERROR] Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (mode != "baseline" && mode != "tuned") {
        std::fprintf(stderr, "[ERROR] --mode must be baseline or tuned, got: %s\n", mode.c_str());
        return 1;
    }

    // Optional stdout suppression -- redirects engine printf chatter to /dev/null
    // during long sweeps. trades.csv / report.csv use explicit FILE* and are
    // unaffected. stderr remains live for progress / summary.
    if (quiet) {
#if defined(_WIN32)
        std::freopen("NUL", "w", stdout);
#else
        std::freopen("/dev/null", "w", stdout);
#endif
    }

    // ----- Engine setup ------------------------------------------------------
    // The S34-B guards (PROVE_IT_SECS, PROVE_IT_MIN_FAVOURABLE_PTS,
    // MIN_SL_PTS_FLOOR, MIN_ATR_PTS) and the cell sl_mult / tp_mult
    // (kUstecTfCells[]) are static constexpr in the engine header per
    // the part-T phased decision -- the harness does not override them.
    // The S37-widened SL/TP is therefore always on, matching the
    // promotion-gate text.
    omega::UstecTrendFollow5mEngine eng;
    eng.enabled     = true;
    eng.shadow_mode = false;       // harness is not a live broker; tr.shadow flag is purely cosmetic here
    if (have_lot_ovr)        eng.lot        = lot_ovr;
    if (have_max_spread_ovr) eng.max_spread = max_spread_ovr;
    // No on_fire_hook bound -- RiskMonitor is a live-only surveillance path,
    // not relevant under the harness. The engine treats unbound as no-op.

    // S63 trio: start from engine header defaults; apply baseline-zeroing if
    // --mode baseline; then layer any CLI overrides last.
    if (mode == "baseline") {
        eng.LOSS_CUT_PCT  = 0.0;
        eng.BE_ARM_PCT    = 0.0;
        eng.BE_BUFFER_PCT = 0.0;
    }
    // mode == "tuned" leaves the engine's header defaults in place
    // (LOSS_CUT_PCT=0.08, BE_ARM_PCT=0.05, BE_BUFFER_PCT=0.02 as of part-L).
    if (have_loss_cut_ovr)  eng.LOSS_CUT_PCT  = loss_cut_ovr;
    if (have_be_arm_ovr)    eng.BE_ARM_PCT    = be_arm_ovr;
    if (have_be_buffer_ovr) eng.BE_BUFFER_PCT = be_buffer_ovr;

    eng.init();

    // ----- Startup banner ----------------------------------------------------
    std::fprintf(stderr,
        "================================================================\n"
        "  UstecTrendFollow5mBacktest -- USTEC.F -- mode=%s\n"
        "  Input    : %s\n"
        "  Trades   : %s\n"
        "  Report   : %s\n"
        "  Engine instance config:\n"
        "    lot                   = %.4f%s\n"
        "    max_spread            = %.2f%s\n"
        "    LOSS_CUT_PCT          = %.4f%s\n"
        "    BE_ARM_PCT            = %.4f%s\n"
        "    BE_BUFFER_PCT         = %.4f%s\n"
        "  Engine header pins (not overridable by this harness):\n"
        "    PROVE_IT_SECS         = %.1f\n"
        "    PROVE_IT_MIN_FAV_PTS  = %.2f\n"
        "    MIN_SL_PTS_FLOOR      = %.2f\n"
        "    MIN_ATR_PTS           = %.2f\n"
        "    Donchian cell sl/tp   = %.2f / %.2f\n"
        "    Keltner  cell sl/tp   = %.2f / %.2f\n"
        "================================================================\n",
        mode.c_str(), in_path, trades_path, report_path,
        eng.lot,           have_lot_ovr        ? "  (cli override)" : "",
        eng.max_spread,    have_max_spread_ovr ? "  (cli override)" : "",
        eng.LOSS_CUT_PCT,  have_loss_cut_ovr   ? "  (cli override)" : "",
        eng.BE_ARM_PCT,    have_be_arm_ovr     ? "  (cli override)" : "",
        eng.BE_BUFFER_PCT, have_be_buffer_ovr  ? "  (cli override)" : "",
        omega::UstecTrendFollow5mEngine::PROVE_IT_SECS,
        omega::UstecTrendFollow5mEngine::PROVE_IT_MIN_FAVOURABLE_PTS,
        omega::UstecTrendFollow5mEngine::MIN_SL_PTS_FLOOR,
        omega::UstecTrendFollow5mEngine::MIN_ATR_PTS,
        omega::kUstecTfCells[0].sl_mult, omega::kUstecTfCells[0].tp_mult,
        omega::kUstecTfCells[1].sl_mult, omega::kUstecTfCells[1].tp_mult);

    // ----- Output files ------------------------------------------------------
    std::FILE* f_trades = std::fopen(trades_path, "w");
    if (!f_trades) {
        std::fprintf(stderr, "[ERROR] Cannot open trades file: %s\n", trades_path);
        return 1;
    }
    std::fprintf(f_trades,
        "entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,"
        "mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score\n");

    // ----- Stats + close callback -------------------------------------------
    Stats stats;
    auto on_close = [&](const omega::TradeRecord& tr) {
        stats.record(tr);
        // UTF5m TradeRecord.spreadAtEntry is always 0 (the engine does not
        // populate it). We still emit the column for schema parity with VWR's
        // trades.csv. Same for confluence_score: emitted as 0.
        std::fprintf(f_trades,
            "%lld,%lld,%s,%s,%s,%.5f,%.5f,%.5f,%.5f,%.5f,%s,%lld,%.5f,%d\n",
            static_cast<long long>(tr.entryTs),
            static_cast<long long>(tr.exitTs),
            tr.symbol.c_str(), tr.side.c_str(), tr.engine.c_str(),
            tr.entryPrice, tr.exitPrice, tr.pnl, tr.mfe, tr.mae,
            tr.exitReason.c_str(),
            static_cast<long long>(tr.exitTs - tr.entryTs),
            tr.spreadAtEntry,
            0);
    };

    // ----- Read and process ticks -------------------------------------------
    std::ifstream fin(in_path);
    if (!fin) {
        std::fprintf(stderr, "[ERROR] Cannot open input: %s\n", in_path);
        std::fclose(f_trades);
        return 1;
    }

    std::string line;
    TickFmt     fmt          = TickFmt::Unknown;
    int64_t     row_count    = 0;
    int64_t     parse_skips  = 0;
    int64_t     first_ts_ms  = 0;
    int64_t     last_ts_ms   = 0;
    double      last_bid     = 0.0;
    double      last_ask     = 0.0;

    BarBuilder        bb;
    omega::UstecTfBar completed{};

    while (std::getline(fin, line)) {
        // Trim trailing CR (Windows line endings)
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        if (fmt == TickFmt::Unknown) {
            fmt = detect_format(line);
            if (fmt == TickFmt::Unknown) {
                std::fprintf(stderr, "[ERROR] Cannot detect tick format. First row:\n  %s\n",
                             line.c_str());
                std::fclose(f_trades);
                return 1;
            }
            const char* names[] = { "unknown", "ts,bid,ask", "ts,bid,ask,vol",
                                    "dukascopy", "histdata", "ts,o,h,l,c,vol" };
            std::fprintf(stderr, "[INFO] Detected tick format: %s\n",
                         names[static_cast<int>(fmt)]);
        }

        TickRow r{};
        if (!parse_row(line, fmt, r)) { ++parse_skips; continue; }

        ++row_count;
        if (limit > 0 && row_count > limit) break;
        if (first_ts_ms == 0) first_ts_ms = r.ts_ms;
        last_ts_ms = r.ts_ms;
        last_bid   = r.bid;
        last_ask   = r.ask;

        // Advance simulated clock. The UTF5m engine reads time only via
        // `now_ms` parameters we pass below, so the shim is technically not
        // load-bearing here -- kept for consistency with the VWR / OmegaBacktest
        // pattern and to future-proof against any downstream engine change.
        omega::bt::set_sim_time(r.ts_ms);

        // Warmup is naturally handled by the engine's own internal gates:
        //   - on_5m_bar early-returns if bars_.size() < 22
        //   - on_5m_bar early-returns if atr14_ <= 0.0
        // No external warmup gate is needed (and toggling `eng.enabled` off
        // during warmup would silence on_5m_bar's bar-history accumulation,
        // breaking the natural warmup). The --warmup CLI flag is accepted
        // for parity with the VWR harness (so a shared WF driver can pass
        // it unconditionally) but is a no-op here; recorded in the report
        // for traceability.

        // 5m-bar dispatch: BarBuilder emits the completed prior bar when the
        // current tick is the first of a new 5m window. Pass that completed
        // bar plus the current tick's bid/ask + ts_ms into the engine. The
        // engine evaluates Donchian/Keltner signals using the bar history
        // and may fire entries inside on_5m_bar; entries fire at the
        // current tick's bid/ask.
        if (bb.on_tick(r.ts_ms, r.bid, r.ask, completed)) {
            // atr14_external=0.0 → engine computes ATR internally from its
            // own bars_ history (the same internal ATR the production fire
            // path uses; matches the part-L promotion-gate intent).
            eng.on_5m_bar(completed, r.bid, r.ask, 0.0, r.ts_ms, on_close);
        }

        // Tick-side management: SL/TP/PROVE_IT/LOSS_CUT/BE_CUT checks for
        // any open position run here on every tick, not just at bar close.
        eng.on_tick(r.bid, r.ask, r.ts_ms, on_close);
    }

    // Force-close any still-open positions at the last tick's actual bid/ask
    // so the summary metrics don't miss in-flight trades at end-of-data and
    // the settlement price reflects the true closing quote. The engine's
    // force_close iterates over both cells and is a no-op if no cell is open.
    if (eng.has_open_position() && last_bid > 0.0 && last_ask > 0.0) {
        eng.force_close(last_bid, last_ask, last_ts_ms, on_close, "END_OF_DATA");
    }

    std::fclose(f_trades);

    // ----- Summary report ---------------------------------------------------
    std::FILE* f_report = std::fopen(report_path, "w");
    if (!f_report) {
        std::fprintf(stderr, "[ERROR] Cannot open report file: %s\n", report_path);
        return 1;
    }

    const double duration_days =
        last_ts_ms > first_ts_ms
            ? static_cast<double>(last_ts_ms - first_ts_ms) / (1000.0 * 86400.0)
            : 0.0;
    const double avg_pnl   = stats.trades > 0
        ? stats.gross_pnl / static_cast<double>(stats.trades) : 0.0;
    const double p95_worst = stats.p95_worst_loss();

    // PnL fields use %.6f for parity with VWR's part-K precision fix; USTEC.F's
    // ~$20/pt scale renders fine at %.6f too.
    std::fprintf(f_report,
        "metric,value\n"
        "symbol,USTEC.F\n"
        "mode,%s\n"
        "ticks_read,%lld\n"
        "parse_skips,%lld\n"
        "duration_days,%.2f\n"
        "warmup_ticks,%lld\n"
        "trades,%lld\n"
        "wins,%lld\n"
        "win_rate_pct,%.2f\n"
        "gross_pnl,%.6f\n"
        "avg_pnl,%.6f\n"
        "best_trade,%.6f\n"
        "worst_trade,%.6f\n"
        "p95_worst_loss,%.6f\n"
        "n_tp_hit,%lld\n"
        "n_sl_hit,%lld\n"
        "n_prove_it_fail,%lld\n"
        "n_loss_cut,%lld\n"
        "n_be_cut,%lld\n"
        "n_end_of_data,%lld\n"
        "n_other,%lld\n"
        "lot,%.4f\n"
        "max_spread,%.2f\n"
        "loss_cut_pct,%.4f\n"
        "be_arm_pct,%.4f\n"
        "be_buffer_pct,%.4f\n"
        "prove_it_secs,%.1f\n"
        "prove_it_min_fav_pts,%.2f\n"
        "min_sl_pts_floor,%.2f\n"
        "min_atr_pts,%.2f\n"
        "donchian_sl_mult,%.2f\n"
        "donchian_tp_mult,%.2f\n"
        "keltner_sl_mult,%.2f\n"
        "keltner_tp_mult,%.2f\n",
        mode.c_str(),
        static_cast<long long>(row_count),
        static_cast<long long>(parse_skips),
        duration_days,
        static_cast<long long>(warmup),
        static_cast<long long>(stats.trades),
        static_cast<long long>(stats.wins),
        stats.win_rate_pct(),
        stats.gross_pnl, avg_pnl,
        stats.best_trade, stats.worst_trade, p95_worst,
        static_cast<long long>(stats.n_tp_hit),
        static_cast<long long>(stats.n_sl_hit),
        static_cast<long long>(stats.n_prove_it_fail),
        static_cast<long long>(stats.n_loss_cut),
        static_cast<long long>(stats.n_be_cut),
        static_cast<long long>(stats.n_end_of_data),
        static_cast<long long>(stats.n_other),
        eng.lot, eng.max_spread,
        eng.LOSS_CUT_PCT, eng.BE_ARM_PCT, eng.BE_BUFFER_PCT,
        omega::UstecTrendFollow5mEngine::PROVE_IT_SECS,
        omega::UstecTrendFollow5mEngine::PROVE_IT_MIN_FAVOURABLE_PTS,
        omega::UstecTrendFollow5mEngine::MIN_SL_PTS_FLOOR,
        omega::UstecTrendFollow5mEngine::MIN_ATR_PTS,
        omega::kUstecTfCells[0].sl_mult, omega::kUstecTfCells[0].tp_mult,
        omega::kUstecTfCells[1].sl_mult, omega::kUstecTfCells[1].tp_mult);
    std::fclose(f_report);

    // Stderr summary -- visible even when --quiet redirected stdout.
    std::fprintf(stderr,
        "================================================================\n"
        "  Summary -- USTEC.F mode=%s\n"
        "    ticks_read   = %lld (skipped %lld)\n"
        "    duration     = %.2f days\n"
        "    trades       = %lld   (wins=%lld  win_rate=%.1f%%)\n"
        "    gross_pnl    = %.6f   (avg=%.6f)\n"
        "    best         = %.6f\n"
        "    worst        = %.6f   (p95 worst loss=%.6f)\n"
        "    Exits: TP=%lld  SL=%lld  PI=%lld  LC=%lld  BE=%lld  EOD=%lld  other=%lld\n"
        "================================================================\n",
        mode.c_str(),
        static_cast<long long>(row_count),
        static_cast<long long>(parse_skips),
        duration_days,
        static_cast<long long>(stats.trades),
        static_cast<long long>(stats.wins),
        stats.win_rate_pct(),
        stats.gross_pnl, avg_pnl,
        stats.best_trade, stats.worst_trade, p95_worst,
        static_cast<long long>(stats.n_tp_hit),
        static_cast<long long>(stats.n_sl_hit),
        static_cast<long long>(stats.n_prove_it_fail),
        static_cast<long long>(stats.n_loss_cut),
        static_cast<long long>(stats.n_be_cut),
        static_cast<long long>(stats.n_end_of_data),
        static_cast<long long>(stats.n_other));

    return 0;
}
