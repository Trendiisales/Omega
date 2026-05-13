// =============================================================================
// VWAPReversionBacktest.cpp -- dedicated backtest harness for VWAPReversionEngine
// =============================================================================
// Created 2026-05-13 (session 2026-05-13 part J) to fill the gap surfaced in
// the part-I priority 3 audit: no existing backtest target instantiates the
// production VWAPReversionEngine. OmegaBacktest's CrossRunner removed vrev at
// S49 X3 (gold-only harness). IndexBacktest covers IndexFlow / IndexMacroCrash
// only. IndexFlowBacktest is synthetic ticks. None of them exercise the
// S37-H-followup in-flight cut fields (LOSS_CUT_PCT / BE_ARM_PCT /
// BE_BUFFER_PCT) we want to validate.
//
// What this file does:
//   1. Reads a tick CSV for one of the four live VWAPReversionEngine symbols
//      (US500.F, USTEC.F, GER40, EURUSD).
//   2. Configures a single omega::cross::VWAPReversionEngine instance with
//      the per-symbol parameters from include/engine_init.hpp (lines 585-634).
//   3. Drives the engine tick-by-tick under simulated time via OmegaTimeShim,
//      tracking day-boundaries to feed a fresh vwap_seed each UTC midnight.
//   4. Emits a trades CSV and a per-mode summary report keyed on the metrics
//      from part-I priority 3 (Net PnL, Win rate, worst-loss percentile,
//      TP_HIT, T/O, LOSS_CUT, BE_CUT, MAE_EARLY_EXIT, SL_HIT counts).
//   5. Supports two run modes for A/B comparison:
//        --mode baseline   in-flight cut DISABLED (zero out the three new
//                          fields). Restores pre-S37-H behaviour: legacy
//                          MAE_EXIT_RATIO + MAX_HOLD_SEC timeout only.
//        --mode tuned      per-symbol values from engine_init.hpp (default).
//      CLI overrides --loss-cut / --be-arm / --be-buffer let you sweep
//      individual thresholds without rebuilding.
//
// Build:
//   cmake --build build --target VWAPReversionBacktest --config Release
//   (Mac/Linux uses -O3 -std=c++20; the OmegaTimeShim is force-included via
//    -include in the CMake target, matching OmegaBacktest's pattern.)
//
// Run:
//   ./build/VWAPReversionBacktest <ticks.csv> --symbol <SYM> [options]
//
// Tick CSV formats supported (auto-detected from the first data row, mirroring
// the OmegaBacktest parser convention documented in BACKTEST.md):
//   A: timestamp_ms,bid,ask
//   B: timestamp_ms,bid,ask,vol
//   C: YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol      (Dukascopy)
//   D: timestamp_ms,open,high,low,close,vol     (OHLCV; bid=close, ask=close)
//
// AUTHORITY: written under CLAUDE.md edit discipline -- this is a NEW file,
// not core code, and is staged in the operator's outputs folder for review.
// No git commit. Operator decides when to drop it into backtest/ and commit.
//
// -----------------------------------------------------------------------------
// 2026-05-13 part K (precision fix for EURUSD readability):
//   The PnL fields in the summary report and stderr summary were written with
//   %.4f. EURUSD trades scale to ~1e-7 per round-trip (0.01 lot * 1e-5 pip),
//   so every PnL field rendered as -0.0000 / 0.0000 in the report even when
//   the underlying values were nonzero (trades CSV uses %.5f and is fine).
//   Bumped to %.6f for the four CSV PnL/loss fields specified in part-J:
//     gross_pnl, best_trade, worst_trade, p95_worst_loss
//   Plus avg_pnl (same precision pathology -- gross_pnl/trades) and the
//   matching stderr-summary block (gross_pnl, avg_pnl, best, worst, p95)
//   so both surfaces stay consistent. Index symbols are unaffected -- their
//   PnL magnitudes already render fine at %.6f as they did at %.4f.
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
#include <limits>

// OmegaTimeShim.hpp is force-included via -include / /FI in the CMakeLists
// add_executable block, so it is the very first header in this TU and the
// engine clocks resolve to simulated time. We still include it explicitly
// for any cross-checker reading this file standalone -- the include guard
// makes the redundant include a no-op.
#include "OmegaTimeShim.hpp"

#include "../include/CrossAssetEngines.hpp"
#include "../include/OmegaTradeLedger.hpp"

// =============================================================================
// Tick row and CSV parsing
// =============================================================================
struct TickRow {
    int64_t ts_ms = 0;   // unix epoch milliseconds
    double  bid   = 0.0;
    double  ask   = 0.0;
};

enum class TickFmt { Unknown, A_TBA, B_TBAV, C_DUKA, D_OHLCV };

// Detect the column layout from the first non-empty/non-header row. We commit
// to a single format for the whole file; mixed formats are an error.
static TickFmt detect_format(const std::string& line) {
    // Count commas
    int commas = 0;
    for (char c : line) if (c == ',') ++commas;

    // Dukascopy: starts with "YYYY.MM.DD" (year.month.day with dots)
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

    // Numeric-leading formats are distinguished by comma count
    if (commas == 2) return TickFmt::A_TBA;
    if (commas == 3) return TickFmt::B_TBAV;
    if (commas == 5) return TickFmt::D_OHLCV;
    return TickFmt::Unknown;
}

// Parse a single line into a TickRow given a known format. Returns false on
// malformed rows (caller skips them but does not abort).
static bool parse_row(const std::string& line, TickFmt fmt, TickRow& out) {
    if (line.empty()) return false;
    // Skip header-like lines (any letter in the first 12 chars besides Dukascopy's dots/colons)
    if (fmt != TickFmt::C_DUKA) {
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

    if (fmt == TickFmt::D_OHLCV) {
        const int64_t ts = std::strtoll(p, &e, 10);
        if (*e != ',') return false; p = e + 1;
        std::strtod(p, &e); if (*e != ',') return false; p = e + 1; // open
        std::strtod(p, &e); if (*e != ',') return false; p = e + 1; // high
        std::strtod(p, &e); if (*e != ',') return false; p = e + 1; // low
        const double close = std::strtod(p, &e);
        if (close <= 0.0) return false;
        // Synthesize a half-spread of 0.15 (matches OmegaBacktest convention)
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
        // Find the third comma (after time field) to locate bid/ask
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

    return false;
}

// =============================================================================
// Per-symbol parameter mirrors of include/engine_init.hpp lines 585-634
// =============================================================================
// Keeping these in lockstep with engine_init.hpp is the operator's
// responsibility. If engine_init.hpp changes the per-instance thresholds,
// update this function so the harness stays representative.
// =============================================================================
struct VWRParams {
    double EXTENSION_THRESH_PCT;
    double MAX_EXTENSION_PCT;
    int    MAX_HOLD_SEC;
    int    COOLDOWN_SEC;
    double LOSS_CUT_PCT;
    double BE_ARM_PCT;
    double BE_BUFFER_PCT;
};

static VWRParams params_for(const std::string& sym) {
    VWRParams p{};
    // 2026-05-13 (part L sync): values below now mirror engine_init.hpp current
    // state (US500/USTEC/EURUSD all reverted to zero on in-flight cuts; GER40
    // still has cuts pending its own harness run). If engine_init.hpp changes,
    // update this function too -- the harness header at L205-209 calls this out.
    if (sym == "US500.F") {
        p.EXTENSION_THRESH_PCT = 0.35;
        p.MAX_EXTENSION_PCT    = 1.20;
        p.MAX_HOLD_SEC         = 600;
        p.COOLDOWN_SEC         = 300;
        // part-K revert (sweep showed all wider cells worse than baseline).
        p.LOSS_CUT_PCT         = 0.0;
        p.BE_ARM_PCT           = 0.0;
        p.BE_BUFFER_PCT        = 0.0;
    } else if (sym == "USTEC.F") {
        p.EXTENSION_THRESH_PCT = 0.40;
        p.MAX_EXTENSION_PCT    = 1.20;
        p.MAX_HOLD_SEC         = 600;
        p.COOLDOWN_SEC         = 300;
        // part-L revert (same winner-amputation pattern as US500 on NSXUSD tape).
        p.LOSS_CUT_PCT         = 0.0;
        p.BE_ARM_PCT           = 0.0;
        p.BE_BUFFER_PCT        = 0.0;
    } else if (sym == "GER40") {
        p.EXTENSION_THRESH_PCT = 0.30;
        p.MAX_EXTENSION_PCT    = 1.00;
        p.MAX_HOLD_SEC         = 600;
        p.COOLDOWN_SEC         = 300;
        // engine_init.hpp still has GER40 cuts ON pending its own harness run.
        p.LOSS_CUT_PCT         = 0.08;
        p.BE_ARM_PCT           = 0.05;
        p.BE_BUFFER_PCT        = 0.02;
    } else if (sym == "EURUSD") {
        p.EXTENSION_THRESH_PCT = 0.12;
        p.MAX_EXTENSION_PCT    = 0.40;
        p.MAX_HOLD_SEC         = 600;
        p.COOLDOWN_SEC         = 120;
        // part-K revert (precision fix exposed 91% regression; sweep no help).
        p.LOSS_CUT_PCT         = 0.0;
        p.BE_ARM_PCT           = 0.0;
        p.BE_BUFFER_PCT        = 0.0;
    } else {
        // Unknown symbol -- engine defaults will be used (still valid VWR config)
        p.EXTENSION_THRESH_PCT = 0.20;
        p.MAX_EXTENSION_PCT    = 0.80;
        p.MAX_HOLD_SEC         = 900;
        p.COOLDOWN_SEC         = 180;
        p.LOSS_CUT_PCT         = 0.08;
        p.BE_ARM_PCT           = 0.05;
        p.BE_BUFFER_PCT        = 0.02;
    }
    return p;
}

// =============================================================================
// Stats accumulator
// =============================================================================
// Tracks the metrics the part-I priority 3 comparison table calls for plus a
// few diagnostic counters useful when explaining "why did distribution X look
// like Y" in the follow-up review.
// =============================================================================
struct Stats {
    int64_t trades             = 0;
    int64_t wins               = 0;
    double  gross_pnl          = 0.0;   // sum of TradeRecord.pnl (price * size)
    double  best_trade         = 0.0;
    double  worst_trade        = 0.0;
    // Exit-reason counts -- mirrors the strings emitted by CrossPosition::emit()
    // and VWAPReversionEngine's force_close() call sites.
    int64_t n_tp_hit           = 0;
    int64_t n_sl_hit           = 0;
    int64_t n_timeout          = 0;
    int64_t n_mae_early_exit   = 0;
    int64_t n_loss_cut         = 0;
    int64_t n_be_cut           = 0;
    int64_t n_other            = 0;
    // Worst-N tail tracking -- the worst 5% of losers, used to derive the
    // outlier-tail signal part-I cares about.
    std::vector<double> losses;

    void record(const omega::TradeRecord& tr) {
        ++trades;
        gross_pnl += tr.pnl;
        if (tr.pnl > 0.0) ++wins;
        if (tr.pnl > best_trade)  best_trade  = tr.pnl;
        if (tr.pnl < worst_trade) worst_trade = tr.pnl;
        if (tr.pnl < 0.0) losses.push_back(tr.pnl);

        const std::string& r = tr.exitReason;
        if      (r == "TP_HIT")         ++n_tp_hit;
        else if (r == "SL_HIT")         ++n_sl_hit;
        else if (r == "TIMEOUT")        ++n_timeout;
        else if (r == "MAE_EARLY_EXIT") ++n_mae_early_exit;
        else if (r == "LOSS_CUT")       ++n_loss_cut;
        else if (r == "BE_CUT")         ++n_be_cut;
        else                            ++n_other;
    }

    double win_rate_pct() const {
        return trades > 0 ? (100.0 * static_cast<double>(wins) / static_cast<double>(trades)) : 0.0;
    }

    // 95th-percentile worst loss (the value such that 95% of losses are smaller
    // in magnitude). This is what part-I's "outlier-loss tail" metric refers to.
    double p95_worst_loss() {
        if (losses.empty()) return 0.0;
        std::vector<double> sorted = losses;
        std::sort(sorted.begin(), sorted.end()); // most negative first
        const size_t idx = static_cast<size_t>(0.05 * static_cast<double>(sorted.size()));
        return sorted[idx];
    }
};

// =============================================================================
// Day-boundary tracking for VWAP seed
// =============================================================================
// The engine maintains its own rolling EWM VWAP internally with a 2hr
// half-life; vwap_seed is only used on the first tick of each session (the
// engine seeds ewm_vwap_ from it when ewm_vwap_ <= 0). In live operation
// main.cpp resets the EWM at session open via reset_ewm_vwap(anchor). We
// mirror that here: track UTC day-of-year and call reset_ewm_vwap with the
// first mid of each new UTC day.
// =============================================================================
static int utc_day_of_year(int64_t ts_ms) {
    const time_t t = static_cast<time_t>(ts_ms / 1000LL);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm.tm_yday;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: VWAPReversionBacktest <ticks.csv> [options]\n"
            "  --symbol <SYM>      US500.F | USTEC.F | GER40 | EURUSD (required)\n"
            "  --mode <m>          baseline | tuned                  (default: tuned)\n"
            "                      baseline: LOSS_CUT_PCT=0, BE_ARM_PCT=0, BE_BUFFER_PCT=0\n"
            "                      tuned:    per-symbol engine_init.hpp values\n"
            "  --loss-cut <pct>    override LOSS_CUT_PCT\n"
            "  --be-arm <pct>      override BE_ARM_PCT\n"
            "  --be-buffer <pct>   override BE_BUFFER_PCT\n"
            "  --trades <file>     trades CSV output     (default: vrev_trades.csv)\n"
            "  --report <file>     summary report CSV    (default: vrev_report.csv)\n"
            "  --limit <N>         max ticks to process  (default: unlimited)\n"
            "  --warmup <N>        warmup ticks pre-trade (default: 5000)\n"
            "  --quiet             suppress engine printf chatter to stdout\n");
        return 1;
    }

    const char* in_path    = argv[1];
    const char* trades_path = "vrev_trades.csv";
    const char* report_path = "vrev_report.csv";
    std::string symbol;
    std::string mode = "tuned";
    int64_t limit  = -1;
    int64_t warmup = 5000;
    bool quiet     = false;
    bool have_loss_cut_ovr   = false;
    bool have_be_arm_ovr     = false;
    bool have_be_buffer_ovr  = false;
    double loss_cut_ovr = 0.0, be_arm_ovr = 0.0, be_buffer_ovr = 0.0;
    // Entry-side overrides (part-P, 2026-05-14e): added for the VWR USTEC.F
    // retune sweep. See outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md Phase 0.
    // Pattern mirrors the S63 trio above. Applied AFTER --mode baseline zeroes
    // out the S63 fields, so a sweep can vary entry-side params under either
    // baseline or tuned S63 settings.
    bool have_ext_ovr        = false;
    bool have_max_ext_ovr    = false;
    bool have_max_hold_ovr   = false;
    bool have_cooldown_ovr   = false;
    double ext_ovr = 0.0, max_ext_ovr = 0.0;
    int    max_hold_ovr = 0, cooldown_ovr = 0;

    for (int i = 2; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--symbol")    && i + 1 < argc) symbol = argv[++i];
        else if (!std::strcmp(argv[i], "--mode")      && i + 1 < argc) mode   = argv[++i];
        else if (!std::strcmp(argv[i], "--trades")    && i + 1 < argc) trades_path = argv[++i];
        else if (!std::strcmp(argv[i], "--report")    && i + 1 < argc) report_path = argv[++i];
        else if (!std::strcmp(argv[i], "--limit")     && i + 1 < argc) limit  = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--warmup")    && i + 1 < argc) warmup = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--loss-cut")  && i + 1 < argc) { loss_cut_ovr  = std::atof(argv[++i]); have_loss_cut_ovr  = true; }
        else if (!std::strcmp(argv[i], "--be-arm")    && i + 1 < argc) { be_arm_ovr    = std::atof(argv[++i]); have_be_arm_ovr    = true; }
        else if (!std::strcmp(argv[i], "--be-buffer") && i + 1 < argc) { be_buffer_ovr = std::atof(argv[++i]); have_be_buffer_ovr = true; }
        else if (!std::strcmp(argv[i], "--ext")       && i + 1 < argc) { ext_ovr       = std::atof(argv[++i]); have_ext_ovr       = true; }
        else if (!std::strcmp(argv[i], "--max-ext")   && i + 1 < argc) { max_ext_ovr   = std::atof(argv[++i]); have_max_ext_ovr   = true; }
        else if (!std::strcmp(argv[i], "--max-hold")  && i + 1 < argc) { max_hold_ovr  = std::atoi(argv[++i]); have_max_hold_ovr  = true; }
        else if (!std::strcmp(argv[i], "--cooldown")  && i + 1 < argc) { cooldown_ovr  = std::atoi(argv[++i]); have_cooldown_ovr  = true; }
        else if (!std::strcmp(argv[i], "--quiet"))                      quiet = true;
        else {
            std::fprintf(stderr, "[ERROR] Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (symbol.empty()) {
        std::fprintf(stderr, "[ERROR] --symbol is required (US500.F | USTEC.F | GER40 | EURUSD)\n");
        return 1;
    }
    if (mode != "baseline" && mode != "tuned") {
        std::fprintf(stderr, "[ERROR] --mode must be baseline or tuned, got: %s\n", mode.c_str());
        return 1;
    }

    // Resolve params: start from per-symbol defaults, apply baseline-zeroing if
    // requested, then layer any CLI overrides last.
    VWRParams p = params_for(symbol);
    if (mode == "baseline") {
        p.LOSS_CUT_PCT  = 0.0;
        p.BE_ARM_PCT    = 0.0;
        p.BE_BUFFER_PCT = 0.0;
    }
    if (have_loss_cut_ovr)  p.LOSS_CUT_PCT  = loss_cut_ovr;
    if (have_be_arm_ovr)    p.BE_ARM_PCT    = be_arm_ovr;
    if (have_be_buffer_ovr) p.BE_BUFFER_PCT = be_buffer_ovr;
    // Entry-side overrides apply equally in baseline and tuned modes (they
    // are not part of the S63 trio that --mode baseline zeroes out).
    if (have_ext_ovr)       p.EXTENSION_THRESH_PCT = ext_ovr;
    if (have_max_ext_ovr)   p.MAX_EXTENSION_PCT    = max_ext_ovr;
    if (have_max_hold_ovr)  p.MAX_HOLD_SEC         = max_hold_ovr;
    if (have_cooldown_ovr)  p.COOLDOWN_SEC         = cooldown_ovr;

    // Optional stdout suppression -- redirect to /dev/null so the per-trade
    // engine printf chatter does not flood logs during long sweeps. The CSV
    // outputs use explicit FILE* and are unaffected.
    if (quiet) {
#if defined(_WIN32)
        std::freopen("NUL", "w", stdout);
#else
        std::freopen("/dev/null", "w", stdout);
#endif
    }

    std::fprintf(stderr,
        "================================================================\n"
        "  VWAPReversionBacktest -- %s -- mode=%s\n"
        "  Input    : %s\n"
        "  Trades   : %s\n"
        "  Report   : %s\n"
        "  Params:\n"
        "    EXTENSION_THRESH_PCT = %.4f%s\n"
        "    MAX_EXTENSION_PCT    = %.4f%s\n"
        "    MAX_HOLD_SEC         = %d%s\n"
        "    COOLDOWN_SEC         = %d%s\n"
        "    LOSS_CUT_PCT         = %.4f%s\n"
        "    BE_ARM_PCT           = %.4f%s\n"
        "    BE_BUFFER_PCT        = %.4f%s\n"
        "================================================================\n",
        symbol.c_str(), mode.c_str(), in_path, trades_path, report_path,
        p.EXTENSION_THRESH_PCT, have_ext_ovr      ? "  (cli override)" : "",
        p.MAX_EXTENSION_PCT,    have_max_ext_ovr  ? "  (cli override)" : "",
        p.MAX_HOLD_SEC,         have_max_hold_ovr ? "  (cli override)" : "",
        p.COOLDOWN_SEC,         have_cooldown_ovr ? "  (cli override)" : "",
        p.LOSS_CUT_PCT,  have_loss_cut_ovr  ? "  (cli override)" : "",
        p.BE_ARM_PCT,    have_be_arm_ovr    ? "  (cli override)" : "",
        p.BE_BUFFER_PCT, have_be_buffer_ovr ? "  (cli override)" : "");

    // ----- Engine setup ------------------------------------------------------
    omega::cross::VWAPReversionEngine eng;
    eng.enabled              = true;
    eng.EXTENSION_THRESH_PCT = p.EXTENSION_THRESH_PCT;
    eng.MAX_EXTENSION_PCT    = p.MAX_EXTENSION_PCT;
    eng.MAX_HOLD_SEC         = p.MAX_HOLD_SEC;
    eng.COOLDOWN_SEC         = p.COOLDOWN_SEC;
    eng.LOSS_CUT_PCT         = p.LOSS_CUT_PCT;
    eng.BE_ARM_PCT           = p.BE_ARM_PCT;
    eng.BE_BUFFER_PCT        = p.BE_BUFFER_PCT;
    // The other engine fields (EXTENSION_SL_RATIO, MAE_EXIT_RATIO,
    // MAE_COOLDOWN_SEC, CONSEC_FC_BLOCK_SEC, TP_FLIP_COOLDOWN_SEC,
    // MIN_SESSION_MIN, CONF_VIX_THRESH, CONF_L2_THRESH) keep their in-class
    // defaults, which match engine_init.hpp's "leave alone" behaviour for
    // those fields across all four live instances at HEAD = 0e95ecd.

    // ----- Output files ------------------------------------------------------
    std::FILE* f_trades = std::fopen(trades_path, "w");
    if (!f_trades) {
        std::fprintf(stderr, "[ERROR] Cannot open trades file: %s\n", trades_path);
        return 1;
    }
    std::fprintf(f_trades,
        "entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,"
        "mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score\n");

    // ----- Stats and close callback -----------------------------------------
    Stats stats;
    int64_t last_signal_score = 1;  // captured at signal time, attached to next close
    auto on_close = [&](const omega::TradeRecord& tr) {
        stats.record(tr);
        std::fprintf(f_trades,
            "%lld,%lld,%s,%s,%s,%.5f,%.5f,%.5f,%.5f,%.5f,%s,%lld,%.5f,%lld\n",
            static_cast<long long>(tr.entryTs),
            static_cast<long long>(tr.exitTs),
            tr.symbol.c_str(), tr.side.c_str(), tr.engine.c_str(),
            tr.entryPrice, tr.exitPrice, tr.pnl, tr.mfe, tr.mae,
            tr.exitReason.c_str(),
            static_cast<long long>(tr.exitTs - tr.entryTs),
            tr.spreadAtEntry,
            static_cast<long long>(last_signal_score));
    };

    // ----- Read and process ticks -------------------------------------------
    std::ifstream fin(in_path);
    if (!fin) {
        std::fprintf(stderr, "[ERROR] Cannot open input: %s\n", in_path);
        std::fclose(f_trades);
        return 1;
    }

    std::string line;
    TickFmt fmt = TickFmt::Unknown;
    int64_t row_count   = 0;
    int64_t parse_skips = 0;
    int     last_day    = -1;
    double  day_open_mid = 0.0;
    int64_t first_ts_ms = 0, last_ts_ms = 0;
    double  last_bid = 0.0, last_ask = 0.0;

    while (std::getline(fin, line)) {
        // Trim trailing CR (Windows line endings)
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        // Detect format on first non-empty row
        if (fmt == TickFmt::Unknown) {
            fmt = detect_format(line);
            if (fmt == TickFmt::Unknown) {
                std::fprintf(stderr, "[ERROR] Cannot detect tick format. First row:\n  %s\n",
                             line.c_str());
                std::fclose(f_trades);
                return 1;
            }
            const char* names[] = { "unknown", "ts,bid,ask", "ts,bid,ask,vol",
                                     "dukascopy", "ts,o,h,l,c,vol" };
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

        // Advance simulated clock so every cooldown / session / hold-time gate
        // in the engine reads simulated time, not wall time.
        omega::bt::set_sim_time(r.ts_ms);

        const double mid = (r.bid + r.ask) * 0.5;

        // Day-boundary VWAP reset -- mirrors the session-open behaviour wired
        // in production main.cpp. The engine's internal rolling EWM then
        // converges from this anchor over the next ~2 hours.
        const int dy = utc_day_of_year(r.ts_ms);
        if (dy != last_day) {
            last_day      = dy;
            day_open_mid  = mid;
            eng.reset_ewm_vwap(day_open_mid);
        }

        // Warmup gate -- pass ticks through but ignore any returned signals
        // until the engine has seen enough data. on_tick still maintains
        // internal state (EWM VWAP, momentum window) during warmup.
        const bool in_warmup = (row_count <= warmup);

        omega::cross::CrossSignal sig =
            eng.on_tick(symbol, r.bid, r.ask, day_open_mid, on_close, 0.0, 0.5);
        if (sig.valid && !in_warmup) {
            // The engine has already opened the position internally inside
            // on_tick(); here we just capture the confluence score for the
            // trades CSV. last_signal_score is consumed by the next on_close.
            last_signal_score = sig.confluence_score;
        } else if (sig.valid && in_warmup) {
            // During warmup, cancel any phantom entry the engine wired up.
            // VWAPReversionEngine has no inherent warmup guard -- it can
            // signal on tick 1 if conditions align.
            eng.cancel();
        }
    }

    // Force-close any still-open position at the last tick's actual bid/ask so
    // the summary metrics don't miss in-flight trades at end-of-data and the
    // settlement price reflects the true closing quote rather than the (stale)
    // day-open mid.
    if (eng.has_open_position() && last_bid > 0.0 && last_ask > 0.0) {
        eng.force_close(last_bid, last_ask, on_close, "END_OF_DATA");
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
    const double avg_pnl   = stats.trades > 0 ? stats.gross_pnl / static_cast<double>(stats.trades) : 0.0;
    const double p95_worst = stats.p95_worst_loss();

    // Note: PnL/loss fields use %.6f (was %.4f pre-part-K) so EURUSD's
    // ~1e-7-per-trade magnitudes render readably. Index symbols retain
    // visible precision at %.6f too -- the bump is monotonic.
    std::fprintf(f_report,
        "metric,value\n"
        "symbol,%s\n"
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
        "n_timeout,%lld\n"
        "n_mae_early_exit,%lld\n"
        "n_loss_cut,%lld\n"
        "n_be_cut,%lld\n"
        "n_other,%lld\n"
        "extension_thresh_pct,%.4f\n"
        "max_extension_pct,%.4f\n"
        "max_hold_sec,%d\n"
        "cooldown_sec,%d\n"
        "loss_cut_pct,%.4f\n"
        "be_arm_pct,%.4f\n"
        "be_buffer_pct,%.4f\n",
        symbol.c_str(), mode.c_str(),
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
        static_cast<long long>(stats.n_timeout),
        static_cast<long long>(stats.n_mae_early_exit),
        static_cast<long long>(stats.n_loss_cut),
        static_cast<long long>(stats.n_be_cut),
        static_cast<long long>(stats.n_other),
        p.EXTENSION_THRESH_PCT, p.MAX_EXTENSION_PCT,
        p.MAX_HOLD_SEC, p.COOLDOWN_SEC,
        p.LOSS_CUT_PCT, p.BE_ARM_PCT, p.BE_BUFFER_PCT);
    std::fclose(f_report);

    // Stderr summary -- visible even when --quiet redirected stdout.
    // PnL fields bumped to %.6f to match the report CSV (part-K precision fix).
    std::fprintf(stderr,
        "================================================================\n"
        "  Summary -- %s mode=%s\n"
        "    ticks_read   = %lld (skipped %lld)\n"
        "    duration     = %.2f days\n"
        "    trades       = %lld   (wins=%lld  win_rate=%.1f%%)\n"
        "    gross_pnl    = %.6f   (avg=%.6f)\n"
        "    best         = %.6f\n"
        "    worst        = %.6f   (p95 worst loss=%.6f)\n"
        "    Exits: TP=%lld  SL=%lld  T/O=%lld  MAE=%lld  LOSS_CUT=%lld  BE_CUT=%lld  other=%lld\n"
        "================================================================\n",
        symbol.c_str(), mode.c_str(),
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
        static_cast<long long>(stats.n_timeout),
        static_cast<long long>(stats.n_mae_early_exit),
        static_cast<long long>(stats.n_loss_cut),
        static_cast<long long>(stats.n_be_cut),
        static_cast<long long>(stats.n_other));

    return 0;
}
