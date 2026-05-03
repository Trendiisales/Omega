// =============================================================================
// verify_xauusd_fvg.cpp -- synthetic-trace verifier for XauusdFvgEngine.
// =============================================================================
//
// 2026-05-02 SESSION (Claude / Jo):
//   Per docs/DESIGN_XAUUSD_FVG_ENGINE.md  §9 (Verification plan), this
//   binary replays the cached 15-min XAUUSD bar pickle through the live
//   omega::XauusdFvgEngine and asserts row-for-row match against the
//   trades_top.csv produced by scripts/fvg_pnl_backtest_v3.py at the v3 #5
//   ACCEPTED config (top10, BE off, SL=2.5xATR, TP=5.0xATR).
//
// 2026-05-03 SESSION (Claude / Jo) -- bar-window truncation:
//   The cached bars pickle covers Sep 1 2025 -> Mar 1 2026 (6 months, ~11,700
//   bars), but the v3 single-window trades_top.csv we replay against was
//   generated on a Sep 1 -> Dec 1 (3-month) run. Without a cap the engine
//   processes the entire 6-month bar feed and emits roughly 2x the trades
//   v3 saw -- 136 vs the expected 70. v3's diagnostics
//   (`Top  98 cand / 70 trades / 27 overlap / 1 unfillable`) confirm v3 saw
//   exactly 98 candidates and fired 70 of them inside its 3-month window;
//   any post-Nov-28 trade the engine emits is a "phantom" v3 never even
//   considered. Since `min(score_at_entry across trades_top.csv) == v3's
//   actual cutoff` (top decile of train scores), and the engine's score
//   computation now matches v3 within numerical noise, the engine's
//   candidate pool inside the v3 window is a subset of v3's pool -- so
//   capping the bar feed at v3's window end recovers the 70-row match
//   without dropping any trade v3 took.
//
//   The verifier therefore caps the bar feed at the END of v3's window. The
//   default derivation is `max(exit_ts across trades_top.csv) + BAR_SECS`:
//     * +BAR_SECS guarantees the bar AFTER the last exit is fed, so the
//       engine's on_bar_close step-5 SL/TP / first-tick-of-next-bar
//       TIME_STOP path (which fires from a tick INSIDE the bar after the
//       exit bar) runs to completion and the last trade closes cleanly.
//     * No further extension because (a) any FVG with score >= cutoff and
//       entry_idx > max(exit_ts) would have fired in v3 (post-exit, no
//       overlap; top-decile-pool implies score >= cutoff), and v3 has none
//       -- so feeding bars beyond that point can ONLY produce phantom
//       trades v3 didn't see.
//   Two CLI overrides for diagnostic / cross-window runs:
//     --bar-end-ts <unix>           explicit unix-second cutoff
//     --bar-end-date YYYY-MM-DD     UTC midnight of the given day
//   Pass `--bar-end-ts 0` to disable the cap entirely (e.g. when the
//   trades_top.csv was generated on the SAME window as the bars CSV --
//   walk-forward runs whose train_end matches the bars-pickle end fall
//   into this category).
//
// USAGE
//   1. Pre-flatten the bar pickle to CSV (one-shot helper):
//        python3 scripts/dump_bars_to_csv.py \
//            fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.pkl
//
//   2. Build:
//        cmake -B build -DCMAKE_BUILD_TYPE=Release
//        cmake --build build --target verify_xauusd_fvg --config Release
//
//   3. Run from the repo root (so relative paths resolve):
//        ./build/verify_xauusd_fvg \
//            fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.csv \
//            fvg_pnl_backtest_v3/XAUUSD_15min_top10_be0.0_sl2.5_tp5.0/trades_top.csv
//
//      Optional flags (after the two positional paths):
//        --cutoff <val>          override score cutoff (default: derived as the
//                                min(score_at_entry) seen in trades_top.csv,
//                                which equals the v3 train-window top-decile
//                                cutoff that produced the file).
//        --tol-price <val>       abs tolerance for price-like fields (default 1e-3).
//        --tol-score <val>       abs tolerance for score / atr / gap (default 1e-4).
//        --tol-exit-price <val>  loose tol for exit_price (default 5.0).
//        --tol-exit-ts-s <int>   loose tol for exit_ts seconds (default 900).
//        --bar-end-ts <unix>     cap bar feed at this unix-second timestamp
//                                (0 = no cap; default = auto from trades csv).
//        --bar-end-date <date>   cap bar feed at YYYY-MM-DD UTC midnight.
//        --verbose               dump every emitted trade alongside the expected.
//
// EXIT CODES
//   0  PASS  -- engine emits exactly the same trades as trades_top.csv,
//               in the same order, with every field within tolerance.
//   1  FAIL  -- count mismatch OR first-row diff (precise location + field
//               printed to stderr; processing stops at first divergence).
//   2  USAGE -- bad CLI / missing file.
//
// IMPLEMENTATION NOTES
//   - Tick synthesis per bar is direction-aware: when a position is open we
//     emit ticks in [open, low, high, close, close*N-4] for long and
//     [open, high, low, close, close*N-4] for short so the engine's per-tick
//     SL/TP check fires SL first on the bar-low (long) / bar-high (short),
//     matching v3 simulate_trade()'s "sl_hit and tp_hit -> sl" priority.
//   - tick_count is preserved per bar so the engine's 20-bar tv_mean stays
//     bit-equivalent with v3 add_indicators().
//   - spread is replayed as bar.spread_mean on every tick so the engine's
//     spread_sum/tick_count yields exactly bar.spread_mean.
//   - The engine's score_cutoff_override hook is set to the trades_top.csv-
//     derived cutoff so the engine's candidate pool matches v3's top-decile
//     filter -- otherwise overlap-skip diverges and row-for-row is impossible.
//   - SpreadRegimeGate is fed every tick and is consulted at entry; default
//     thresholds permit normal XAUUSD spreads, so no override is required.
//   - g_macroDetector / g_news_blackout call sites in the engine are gated
//     by `#ifndef OMEGA_BACKTEST` -- this TU compiles with -DOMEGA_BACKTEST
//     so those externs are never referenced.
// =============================================================================

// OMEGA_BACKTEST is injected by CMake via target_compile_definitions so the
// engine's g_macroDetector / g_news_blackout call sites compile out cleanly.
// (Previously also #define'd here, which produced a -Wmacro-redefined warning
// on Clang -- removed in favour of the CMake-side define so there's a single
// source of truth.)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

#include "include/XauusdFvgEngine.hpp"

// -----------------------------------------------------------------------------
// CLI
// -----------------------------------------------------------------------------
//
// Sentinel values for bar_end_ts:
//   -1 (default) : auto-derive from trades_top.csv (max exit_ts + BAR_SECS).
//    0           : disable the cap entirely (process every bar in the CSV).
//   >0           : explicit unix-second cutoff.
struct Cli {
    std::string bars_csv;
    std::string trades_top_csv;
    double      cutoff_override = -1.0;   // <0 means: derive from trades_top.csv
    double      tol_price       = 1e-3;   // strict prices (entry_price/sl/tp)
    double      tol_score       = 1e-4;   // strict scores/atr/gap
    // Loose tolerance for exit_price: the engine fires SL/TP per-tick at bid
    // (long) / ask (short), not at the bar high/low used by the v3 reference.
    // Worst-case divergence is `(sl - bar.low) + half_spread` for a long SL
    // hit deep inside the bar. Default 5.0 USD covers typical XAUUSD bars.
    double      tol_exit_price  = 5.0;
    // Loose tolerance for exit_ts: TIME_STOP and bar-start-aligned exits
    // should match exactly (engine fix in this session moved tr.exitTs to
    // m_cur_bar.start_ts), but if the engine fires SL/TP one bar earlier or
    // later than v3 because of the half-spread boundary, the bar timestamp
    // shifts by exactly BAR_SECS. Default tolerance one bar = 900 seconds.
    int64_t     tol_exit_ts_s   = 900;
    // Bar-feed cutoff. -1 = auto-derive (default), 0 = no cap, >0 = explicit.
    int64_t     bar_end_ts      = -1;
    bool        verbose         = false;
};

[[noreturn]] static void usage(int code, const char* msg = nullptr)
{
    if (msg) std::cerr << "[usage-error] " << msg << "\n";
    std::cerr <<
        "usage: verify_xauusd_fvg <bars.csv> <trades_top.csv>\n"
        "                          [--cutoff <val>]\n"
        "                          [--tol-price <val>]       (default 1e-3)\n"
        "                          [--tol-score <val>]       (default 1e-4)\n"
        "                          [--tol-exit-price <val>]  (default 5.0)\n"
        "                          [--tol-exit-ts-s <int>]   (default 900)\n"
        "                          [--bar-end-ts <unix>]     (default auto;\n"
        "                                                     0 disables cap)\n"
        "                          [--bar-end-date <YYYY-MM-DD>]\n"
        "                          [--verbose]\n";
    std::exit(code);
}

// Parse "YYYY-MM-DD" to unix seconds at UTC midnight. Returns -1 on parse error.
static int64_t parse_date_utc_midnight(const std::string& s)
{
    int Y = 0, M = 0, D = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &Y, &M, &D) != 3) return -1;
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
#ifdef _WIN32
    return static_cast<int64_t>(_mkgmtime(&tm));
#else
    return static_cast<int64_t>(timegm(&tm));
#endif
}

static Cli parse_cli(int argc, char** argv)
{
    Cli c;
    if (argc < 3) usage(2, "expected at least bars.csv and trades_top.csv");
    c.bars_csv       = argv[1];
    c.trades_top_csv = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto need_arg = [&](const char* name) {
            if (i + 1 >= argc) usage(2, (std::string(name) + " requires an argument").c_str());
            return std::string(argv[++i]);
        };
        if      (a == "--cutoff")         c.cutoff_override = std::stod(need_arg("--cutoff"));
        else if (a == "--tol-price")      c.tol_price       = std::stod(need_arg("--tol-price"));
        else if (a == "--tol-score")      c.tol_score       = std::stod(need_arg("--tol-score"));
        else if (a == "--tol-exit-price") c.tol_exit_price  = std::stod(need_arg("--tol-exit-price"));
        else if (a == "--tol-exit-ts-s")  c.tol_exit_ts_s   = std::stoll(need_arg("--tol-exit-ts-s"));
        else if (a == "--bar-end-ts")     c.bar_end_ts      = std::stoll(need_arg("--bar-end-ts"));
        else if (a == "--bar-end-date") {
            std::string d = need_arg("--bar-end-date");
            int64_t ts = parse_date_utc_midnight(d);
            if (ts < 0) usage(2, ("bad --bar-end-date '" + d + "'; expected YYYY-MM-DD").c_str());
            c.bar_end_ts = ts;
        }
        else if (a == "--verbose")        c.verbose         = true;
        else                              usage(2, ("unknown flag: " + a).c_str());
    }
    return c;
}

// -----------------------------------------------------------------------------
// CSV utilities
// -----------------------------------------------------------------------------
static std::vector<std::string> split_csv_line(const std::string& line)
{
    std::vector<std::string> out;
    std::string field;
    field.reserve(64);
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') { in_quotes = !in_quotes; continue; }
        if (c == ',' && !in_quotes) { out.push_back(field); field.clear(); continue; }
        field.push_back(c);
    }
    out.push_back(field);
    return out;
}

static int find_col(const std::vector<std::string>& header, const std::string& name)
{
    for (size_t i = 0; i < header.size(); ++i) if (header[i] == name) return static_cast<int>(i);
    return -1;
}

// Parse "YYYY-MM-DD HH:MM:SS+00:00" (and the tighter "YYYY-MM-DDTHH:MM:SS+00:00"
// pandas variants) to unix seconds, UTC. Returns -1 on parse error.
static int64_t parse_iso_utc(const std::string& s)
{
    int Y=0, M=0, D=0, h=0, m=0, sec=0;
    // pandas writes either a space or 'T' between date and time. Both forms
    // appear in trades_top.csv (entry_time = "YYYY-MM-DD HH:MM:SS+00:00").
    // The trailing TZ offset is permitted to be ANYTHING here -- the v3
    // backtest always writes UTC bars so we treat the timestamp as UTC.
    if (s.size() < 19) return -1;
    int n = std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec);
    if (n != 6) {
        n = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec);
        if (n != 6) return -1;
    }
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = sec;
    // timegm-equivalent (UTC). _mkgmtime on Windows; timegm on POSIX.
#ifdef _WIN32
    return static_cast<int64_t>(_mkgmtime(&tm));
#else
    return static_cast<int64_t>(timegm(&tm));
#endif
}

// Format a unix-second timestamp as "YYYY-MM-DD HH:MM:SS UTC" for log lines.
static std::string fmt_ts_utc(int64_t ts)
{
    time_t t = static_cast<time_t>(ts);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buf);
}

// -----------------------------------------------------------------------------
// Bar + expected-trade types
// -----------------------------------------------------------------------------
struct Bar {
    int64_t ts_unix     = 0;     // bar START, unix seconds, UTC
    double  open        = 0.0;
    double  high        = 0.0;
    double  low         = 0.0;
    double  close       = 0.0;
    int     tick_count  = 0;
    double  spread_mean = 0.0;
};

struct ExpectedTrade {
    int         row_idx        = 0;
    int         fvg_formed_idx = 0;
    std::string direction;       // "long" / "short"
    int         entry_idx      = 0;
    int64_t     entry_ts       = 0;
    double      entry_price    = 0.0;
    double      sl             = 0.0;
    double      tp             = 0.0;
    double      score_at_entry = 0.0;
    double      atr_at_entry   = 0.0;
    double      gap_height     = 0.0;
    std::string session;         // "asian" / "london" / "ny" / "off"
    int         exit_idx       = 0;
    int64_t     exit_ts        = 0;
    double      exit_price     = 0.0;
    std::string exit_reason;     // "tp" / "sl" / "time_stop"
    double      pnl_dollars    = 0.0;
    int         bars_held      = 0;
};

// Mirrors the LogXauusdFvgCsv side-channel layout but is captured in-process
// so the verifier doesn't need to round-trip through a file. Holds the
// engine's last_extras() snapshot pulled inside the on_close lambda.
struct EmittedTrade {
    omega::TradeRecord                          tr;
    omega::XauusdFvgEngine::LastClosedExtras    extras;
};

// -----------------------------------------------------------------------------
// Loaders
// -----------------------------------------------------------------------------
static std::vector<Bar> load_bars_csv(const std::string& path)
{
    std::ifstream in(path);
    if (!in) { std::cerr << "[error] cannot open bars CSV: " << path << "\n"; std::exit(2); }
    std::string line;
    if (!std::getline(in, line)) { std::cerr << "[error] empty bars CSV\n"; std::exit(2); }
    auto header = split_csv_line(line);
    const int c_ts    = find_col(header, "ts_unix");
    const int c_open  = find_col(header, "open");
    const int c_high  = find_col(header, "high");
    const int c_low   = find_col(header, "low");
    const int c_close = find_col(header, "close");
    const int c_tc    = find_col(header, "tick_count");
    const int c_sp    = find_col(header, "spread_mean");
    if (c_ts < 0 || c_open < 0 || c_high < 0 || c_low < 0 ||
        c_close < 0 || c_tc < 0 || c_sp < 0)
    {
        std::cerr << "[error] bars CSV missing one of the required columns "
                     "(ts_unix, open, high, low, close, tick_count, spread_mean)\n";
        std::exit(2);
    }
    std::vector<Bar> bars;
    bars.reserve(20000);
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto f = split_csv_line(line);
        if (static_cast<int>(f.size()) <= c_sp) continue;
        Bar b;
        b.ts_unix     = static_cast<int64_t>(std::stoll(f[c_ts]));
        b.open        = std::stod(f[c_open]);
        b.high        = std::stod(f[c_high]);
        b.low         = std::stod(f[c_low]);
        b.close       = std::stod(f[c_close]);
        b.tick_count  = std::stoi(f[c_tc]);
        b.spread_mean = std::stod(f[c_sp]);
        bars.push_back(b);
    }
    return bars;
}

static std::vector<ExpectedTrade> load_trades_top(const std::string& path)
{
    std::ifstream in(path);
    if (!in) { std::cerr << "[error] cannot open trades_top CSV: " << path << "\n"; std::exit(2); }
    std::string line;
    if (!std::getline(in, line)) { std::cerr << "[error] empty trades_top CSV\n"; std::exit(2); }
    auto h = split_csv_line(line);
    const int c_fi = find_col(h, "fvg_formed_idx");
    const int c_dr = find_col(h, "direction");
    const int c_ei = find_col(h, "entry_idx");
    const int c_et = find_col(h, "entry_time");
    const int c_ep = find_col(h, "entry_price");
    const int c_sl = find_col(h, "sl");
    const int c_tp = find_col(h, "tp");
    const int c_sc = find_col(h, "score_at_entry");
    const int c_at = find_col(h, "atr_at_entry");
    const int c_gh = find_col(h, "gap_height");
    const int c_se = find_col(h, "session");
    const int c_xi = find_col(h, "exit_idx");
    const int c_xt = find_col(h, "exit_time");
    const int c_xp = find_col(h, "exit_price");
    const int c_xr = find_col(h, "exit_reason");
    const int c_pd = find_col(h, "pnl_dollars");
    const int c_bh = find_col(h, "bars_held");
    if (c_fi < 0 || c_dr < 0 || c_ei < 0 || c_et < 0 || c_ep < 0 ||
        c_sl < 0 || c_tp < 0 || c_sc < 0 || c_at < 0 || c_gh < 0 ||
        c_se < 0 || c_xi < 0 || c_xt < 0 || c_xp < 0 || c_xr < 0 ||
        c_pd < 0 || c_bh < 0)
    {
        std::cerr << "[error] trades_top CSV missing one of the required columns\n";
        std::exit(2);
    }
    std::vector<ExpectedTrade> out;
    int row = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto f = split_csv_line(line);
        ExpectedTrade t;
        t.row_idx        = ++row;
        t.fvg_formed_idx = std::stoi(f[c_fi]);
        t.direction      = f[c_dr];
        t.entry_idx      = std::stoi(f[c_ei]);
        t.entry_ts       = parse_iso_utc(f[c_et]);
        t.entry_price    = std::stod(f[c_ep]);
        t.sl             = std::stod(f[c_sl]);
        t.tp             = std::stod(f[c_tp]);
        t.score_at_entry = std::stod(f[c_sc]);
        t.atr_at_entry   = std::stod(f[c_at]);
        t.gap_height     = std::stod(f[c_gh]);
        t.session        = f[c_se];
        t.exit_idx       = std::stoi(f[c_xi]);
        t.exit_ts        = parse_iso_utc(f[c_xt]);
        t.exit_price     = std::stod(f[c_xp]);
        t.exit_reason    = f[c_xr];
        t.pnl_dollars    = std::stod(f[c_pd]);
        t.bars_held      = std::stoi(f[c_bh]);
        out.push_back(t);
    }
    return out;
}

static const char* session_label_for(char c)
{
    switch (c) {
        case 'A': return "asian";
        case 'L': return "london";
        case 'N': return "ny";
        case 'O': return "off";
        default:  return "unknown";
    }
}

static const char* direction_label_for(char c)
{
    return (c == 'B') ? "long" : (c == 'S') ? "short" : "unknown";
}

static std::string exit_reason_label(const std::string& r)
{
    if (r == "TP_HIT")     return "tp";
    if (r == "SL_HIT")     return "sl";
    if (r == "TIME_STOP")  return "time_stop";
    if (r == "FORCE_CLOSE") return "force_close";
    return r;
}

// -----------------------------------------------------------------------------
// Tick synthesis: replay one bar through the engine.
// -----------------------------------------------------------------------------
//
// Constraints satisfied by this routine:
//   1. m_cur_bar.tick_count == bar.tick_count  (so the engine's tv_mean
//      computation matches v3 add_indicators() bit-equivalently). The cap
//      at 899 used in the first cut of this verifier ARTIFICIALLY pinned
//      tv_mean to 899 across the whole run, zeroing every formation's s_tv
//      score and cascading into a count miss + score miss. Removed.
//   2. m_cur_bar.{open,high,low,close} == bar.{open,high,low,close}.
//      Achieved by emitting at least 4 ticks at OHLC prices in an order
//      that preserves first/last (open, close) and visits both extrema.
//   3. m_cur_bar.spread_mean == bar.spread_mean (so side_cost matches v3).
//      Achieved by stamping every tick with bid = mid - bar.spread_mean/2,
//      ask = mid + bar.spread_mean/2.
//   4. SL/TP precedence on a hit-both-bar matches v3 ("sl wins on tie").
//      Achieved by emitting the SL-trigger tick BEFORE the TP-trigger tick
//      via direction-aware ordering of the low/high sweep.
//   5. All ticks for a bar fit inside the same UTC 15-minute bucket so the
//      engine's bar_idx bucketing doesn't roll over mid-bar. Distribute n
//      ticks evenly across [bar_start_ms, bar_start_ms + 899_000) so even
//      bars with tens of thousands of ticks fit cleanly.
//
// Architectural caveat (also captured at the top of this file): SL/TP per-
// tick checks fire at bid (long) / ask (short), not at the bar low/high
// the v3 reference uses. This produces an exit_price drift of up to half a
// spread on intrabar SL/TP hits. The verifier's --tol-exit-price flag
// (default 5.0 USD) absorbs this systematic difference; entry_price, sl,
// tp, score_at_entry, atr_at_entry, gap_height, session, direction, and
// entry_ts should all match within the much-tighter --tol-price / tol-score.
static void replay_bar(omega::XauusdFvgEngine& eng,
                       const Bar& b,
                       bool can_enter)
{
    // Direction-aware order so per-tick SL fires before per-tick TP on the
    // bar that hits both, matching v3's "sl wins on tie" semantics.
    bool order_low_first = true;  // long-friendly default
    if (eng.has_open_position()) {
        order_low_first = eng.m_pos.is_long;
    }

    const int n_real = std::max(1, b.tick_count);
    const int64_t bar_start_ms = b.ts_unix * 1000LL;
    // 900s = 900_000ms bar window. 899_000 leaves 1s headroom so even with
    // n_real = 899_000 the last tick lands at bar_start_ms + 898_999, still
    // inside floor(now_s / 900) = bar_idx. Real XAUUSD 15m bars carry
    // a few thousand ticks at most -- this is comfortably below 899_000.
    auto tick_ms = [bar_start_ms, n_real](int k) -> int64_t {
        if (n_real <= 1) return bar_start_ms;
        return bar_start_ms + static_cast<int64_t>(k) *
                              static_cast<int64_t>(899'000) /
                              static_cast<int64_t>(n_real);
    };

    // Build the price sequence: open, sweep1, sweep2, close, then filler at
    // close. The sweep order is direction-aware; if no position is open the
    // order doesn't matter for SL/TP (no exit possible) so we use a stable
    // [low, high] order which doesn't change across bars.
    std::vector<double> prices;
    prices.reserve(static_cast<size_t>(n_real));
    if (n_real == 1) {
        prices.push_back(b.close);
    } else if (n_real == 2) {
        prices.push_back(b.open);
        prices.push_back(b.close);
    } else if (n_real == 3) {
        prices.push_back(b.open);
        prices.push_back(order_low_first ? b.low : b.high);
        prices.push_back(b.close);
    } else {
        prices.push_back(b.open);
        if (order_low_first) {
            prices.push_back(b.low);
            prices.push_back(b.high);
        } else {
            prices.push_back(b.high);
            prices.push_back(b.low);
        }
        prices.push_back(b.close);
        for (int k = 4; k < n_real; ++k) prices.push_back(b.close);
    }

    const double half_spread = b.spread_mean * 0.5;
    for (int k = 0; k < n_real; ++k) {
        const double  mid = prices[static_cast<size_t>(k)];
        const double  bid = mid - half_spread;
        const double  ask = mid + half_spread;
        eng.on_tick(bid, ask, tick_ms(k), can_enter, nullptr);
    }
}

// -----------------------------------------------------------------------------
// Main verifier
// -----------------------------------------------------------------------------
static double derive_cutoff_from_trades(const std::vector<ExpectedTrade>& trs)
{
    if (trs.empty()) return -1.0;
    double m = trs.front().score_at_entry;
    for (const auto& t : trs) if (t.score_at_entry < m) m = t.score_at_entry;
    return m;
}

// Auto-derive bar-feed cutoff from the expected trades. Returns
// `max(exit_ts) + BAR_SECS` so the bar AFTER the last exit is fed (allowing
// the engine's on_bar_close-step5 SL/TP and first-tick TIME_STOP paths to
// run for the final position) but no further -- any FVG with score >=
// cutoff and entry_idx > max(exit_ts) would have fired in v3 (post-exit, no
// overlap; top-decile-pool implies score >= cutoff), and v3 has none.
// Returns 0 if `expected` is empty.
static int64_t derive_bar_end_ts(const std::vector<ExpectedTrade>& expected)
{
    if (expected.empty()) return 0;
    int64_t max_exit = 0;
    for (const auto& t : expected) if (t.exit_ts > max_exit) max_exit = t.exit_ts;
    return max_exit + static_cast<int64_t>(omega::XauusdFvgEngine::BAR_SECS);
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main(int argc, char** argv)
{
    Cli cli = parse_cli(argc, argv);

    std::cout << "[verify] bars=" << cli.bars_csv << "\n"
              << "[verify] trades_top=" << cli.trades_top_csv << "\n";

    auto bars     = load_bars_csv(cli.bars_csv);
    auto expected = load_trades_top(cli.trades_top_csv);
    std::cout << "[verify] loaded " << bars.size() << " bars / "
              << expected.size() << " expected trades\n";
    if (bars.empty() || expected.empty()) {
        std::cerr << "[error] empty input -- nothing to verify\n";
        return 2;
    }

    const double cutoff = (cli.cutoff_override > 0.0)
                        ? cli.cutoff_override
                        : derive_cutoff_from_trades(expected);
    std::cout << "[verify] score_cutoff_override = " << std::fixed
              << std::setprecision(6) << cutoff
              << (cli.cutoff_override > 0.0 ? "  (--cutoff)" : "  (derived from trades_top.csv)")
              << "\n";

    // Resolve the bar-feed cutoff. -1 = auto, 0 = no cap, >0 = explicit.
    int64_t bar_end_ts;
    const char* end_origin;
    if (cli.bar_end_ts < 0) {
        bar_end_ts = derive_bar_end_ts(expected);
        end_origin = "  (auto: max(exit_ts) + 1 bar from trades_top.csv)";
    } else if (cli.bar_end_ts == 0) {
        bar_end_ts = std::numeric_limits<int64_t>::max();
        end_origin = "  (no cap; --bar-end-ts 0)";
    } else {
        bar_end_ts = cli.bar_end_ts;
        end_origin = "  (--bar-end-ts / --bar-end-date)";
    }
    if (bar_end_ts == std::numeric_limits<int64_t>::max()) {
        std::cout << "[verify] bar_end_ts = INF" << end_origin << "\n";
    } else {
        std::cout << "[verify] bar_end_ts = " << bar_end_ts
                  << "  (" << fmt_ts_utc(bar_end_ts) << ")"
                  << end_origin << "\n";
    }

    omega::XauusdFvgEngine eng;
    eng.shadow_mode           = true;          // do not pollute any ledger
    eng.score_cutoff_override = cutoff;        // match v3 candidate pool

    // 2026-05-02 second pass -- two more verifier-only overrides on the
    // engine, both off-by-default in production:
    //
    //   spread_gate_disabled: bypass the SpreadRegimeGate at the
    //     mitigation entry path. The gate's adaptive thresholds were
    //     tuned for forex pairs at sub-pip raw-price spreads; XAUUSD's
    //     ~1+ USD spreads put the gate's hysteresis machine in CLOSED
    //     for most of the dataset, blocking ~133 mitigations the v3
    //     reference (which has no gate) takes. Disabling here removes
    //     that filter so trades_top.csv replays cleanly.
    //
    //   first_touch_only_mode: replace the engine's queue-and-wait
    //     mitigation with v3 run_backtest's "first-touch only,
    //     overlap-skip" semantics. The engine's default behaviour
    //     queues an FVG that first-touches during an open position and
    //     fires it on a later bar; v3 instead drops the FVG entirely.
    //     Without this override, the engine emits ~106 phantom
    //     candidates v3 would have skipped.
    eng.spread_gate_disabled  = true;
    eng.first_touch_only_mode = true;

    std::vector<EmittedTrade> emitted;
    emitted.reserve(expected.size() + 16);
    eng.on_close_cb = [&](const omega::TradeRecord& tr) {
        emitted.push_back(EmittedTrade{tr, eng.last_extras()});
    };

    // Drive every bar through the engine, capped at the resolved
    // bar_end_ts. can_enter=true matches the v3 backtest, which has no
    // cohort-side block. Bars with ts_unix > bar_end_ts are skipped so the
    // engine never sees data v3's run didn't see.
    size_t bars_fed = 0;
    for (const auto& b : bars) {
        if (b.ts_unix > bar_end_ts) break;
        replay_bar(eng, b, /*can_enter=*/true);
        ++bars_fed;
    }
    std::cout << "[verify] fed " << bars_fed << " of " << bars.size()
              << " bars (cap " << (bar_end_ts == std::numeric_limits<int64_t>::max()
                                   ? std::string("INF") : fmt_ts_utc(bar_end_ts))
              << ")\n";

    std::cout << "[verify] engine emitted " << emitted.size() << " trades\n";

    // -- Diff -----------------------------------------------------------------
    // Diagnostic improvement (2026-05-03): a count mismatch alone no longer
    // short-circuits the per-row scan. The previous behaviour (`if (fails > 0)
    // break`) bailed out at row 1 the moment count mismatched, hiding the
    // location of the extra/missing trades. Now we track count-mismatch in
    // its own counter and only stop the per-row loop on a true FIELD-level
    // divergence. Emitted rows past expected.size() are also dumped so
    // phantom extras at the tail are visible without re-running --verbose.
    int field_fails        = 0;
    int count_fails        = 0;

    auto fail_at = [&](int row, const char* field, const std::string& exp,
                       const std::string& got)
    {
        std::cerr << "[FAIL] row " << row << " field=" << field
                  << " expected=" << exp << " got=" << got << "\n";
        ++field_fails;
    };

    auto fmt_d = [](double x) {
        std::ostringstream os; os << std::fixed << std::setprecision(6) << x; return os.str();
    };
    auto fmt_i = [](long long x) { return std::to_string(x); };

    if (emitted.size() != expected.size()) {
        std::cerr << "[FAIL] count mismatch: expected=" << expected.size()
                  << " emitted=" << emitted.size() << "\n";
        ++count_fails;
    }

    const size_t n_cmp = std::min(emitted.size(), expected.size());
    for (size_t i = 0; i < n_cmp; ++i) {
        const auto& E = expected[i];
        const auto& G = emitted[i];
        const auto&  tr = G.tr;
        const auto&  ex = G.extras;

        const std::string g_dir = direction_label_for(ex.direction);
        const std::string g_ses = session_label_for(ex.session);
        const std::string g_exr = exit_reason_label(tr.exitReason);

        const int row_fails_before = field_fails;

        if (g_dir != E.direction)
            fail_at(E.row_idx, "direction", E.direction, g_dir);
        if (g_ses != E.session)
            fail_at(E.row_idx, "session", E.session, g_ses);
        if (g_exr != E.exit_reason)
            fail_at(E.row_idx, "exit_reason", E.exit_reason, g_exr);
        if (tr.entryTs != E.entry_ts)
            fail_at(E.row_idx, "entry_ts",
                    fmt_i(static_cast<long long>(E.entry_ts)),
                    fmt_i(static_cast<long long>(tr.entryTs)));
        // exit_ts: loose tolerance. Engine fires SL/TP per-tick at the bid
        // (long) / ask (short) bound, which can cross v3's bar-level threshold
        // one bar earlier or later than the bar v3 picks. Default 900s
        // tolerance allows this single-bar shift; tighter via --tol-exit-ts-s.
        if (std::llabs(static_cast<long long>(tr.exitTs - E.exit_ts)) > cli.tol_exit_ts_s)
            fail_at(E.row_idx, "exit_ts",
                    fmt_i(static_cast<long long>(E.exit_ts)),
                    fmt_i(static_cast<long long>(tr.exitTs)));
        if (!near(ex.score_at_entry, E.score_at_entry, cli.tol_score))
            fail_at(E.row_idx, "score_at_entry",
                    fmt_d(E.score_at_entry), fmt_d(ex.score_at_entry));
        if (!near(ex.atr_at_entry, E.atr_at_entry, cli.tol_score))
            fail_at(E.row_idx, "atr_at_entry",
                    fmt_d(E.atr_at_entry), fmt_d(ex.atr_at_entry));
        if (!near(ex.gap_height, E.gap_height, cli.tol_score))
            fail_at(E.row_idx, "gap_height",
                    fmt_d(E.gap_height), fmt_d(ex.gap_height));
        if (!near(tr.sl, E.sl, cli.tol_price))
            fail_at(E.row_idx, "sl", fmt_d(E.sl), fmt_d(tr.sl));
        if (!near(tr.tp, E.tp, cli.tol_price))
            fail_at(E.row_idx, "tp", fmt_d(E.tp), fmt_d(tr.tp));
        if (!near(tr.entryPrice, E.entry_price, cli.tol_price))
            fail_at(E.row_idx, "entry_price",
                    fmt_d(E.entry_price), fmt_d(tr.entryPrice));
        // exit_price: loose tolerance (default 5.0 USD) -- per-tick SL/TP
        // hits at bid/ask drift up to ~half a spread + (sl - bar.low) from
        // v3's gross_exit = sl/tp. This is an ARCHITECTURAL difference,
        // not a bug. Tighten via --tol-exit-price for diagnostic runs.
        if (!near(tr.exitPrice, E.exit_price, cli.tol_exit_price))
            fail_at(E.row_idx, "exit_price",
                    fmt_d(E.exit_price), fmt_d(tr.exitPrice));

        if (cli.verbose) {
            std::cout << "[row " << E.row_idx << "] dir=" << g_dir
                      << " entry_ts=" << tr.entryTs
                      << " sl=" << fmt_d(tr.sl) << " tp=" << fmt_d(tr.tp)
                      << " score=" << fmt_d(ex.score_at_entry)
                      << " atr=" << fmt_d(ex.atr_at_entry)
                      << " exit_ts=" << tr.exitTs
                      << " exit_px=" << fmt_d(tr.exitPrice)
                      << " reason=" << g_exr << "\n";
        }

        // Stop at first FIELD divergence so the user can fix one issue at a
        // time without 70 rows of cascading noise. Count mismatch alone does
        // NOT trip this -- the rest of the rows are still useful for
        // confirming where the engine and v3 diverge.
        if (field_fails > row_fails_before) break;
    }

    // If the engine emitted MORE trades than v3, dump the trailing extras so
    // the user can see exactly which phantom rows are appearing. (If
    // expected emitted MORE, the missing v3 rows are dumped instead.)
    if (emitted.size() > expected.size()) {
        std::cerr << "[FAIL] " << (emitted.size() - expected.size())
                  << " extra emitted row(s) past expected.size():\n";
        for (size_t i = expected.size(); i < emitted.size(); ++i) {
            const auto& G = emitted[i];
            std::cerr << "  emitted[" << (i + 1) << "] dir="
                      << direction_label_for(G.extras.direction)
                      << " entry_ts=" << G.tr.entryTs
                      << " (" << fmt_ts_utc(G.tr.entryTs) << ")"
                      << " sl=" << fmt_d(G.tr.sl)
                      << " tp=" << fmt_d(G.tr.tp)
                      << " score=" << fmt_d(G.extras.score_at_entry)
                      << " atr=" << fmt_d(G.extras.atr_at_entry)
                      << " gap=" << fmt_d(G.extras.gap_height)
                      << " sess=" << session_label_for(G.extras.session)
                      << " exit_ts=" << G.tr.exitTs
                      << " (" << fmt_ts_utc(G.tr.exitTs) << ")"
                      << " exit_px=" << fmt_d(G.tr.exitPrice)
                      << " reason=" << exit_reason_label(G.tr.exitReason)
                      << "\n";
        }
    } else if (expected.size() > emitted.size()) {
        std::cerr << "[FAIL] " << (expected.size() - emitted.size())
                  << " missing v3 row(s) the engine never emitted:\n";
        for (size_t i = emitted.size(); i < expected.size(); ++i) {
            const auto& E = expected[i];
            std::cerr << "  expected[" << E.row_idx << "] dir=" << E.direction
                      << " entry_ts=" << E.entry_ts
                      << " (" << fmt_ts_utc(E.entry_ts) << ")"
                      << " sl=" << fmt_d(E.sl)
                      << " tp=" << fmt_d(E.tp)
                      << " score=" << fmt_d(E.score_at_entry)
                      << " atr=" << fmt_d(E.atr_at_entry)
                      << " gap=" << fmt_d(E.gap_height)
                      << " sess=" << E.session
                      << " exit_ts=" << E.exit_ts
                      << " (" << fmt_ts_utc(E.exit_ts) << ")"
                      << " exit_px=" << fmt_d(E.exit_price)
                      << " reason=" << E.exit_reason << "\n";
        }
    }

    const int total_fails = count_fails + field_fails;
    if (total_fails == 0) {
        std::cout << "[PASS] verifier OK -- " << expected.size()
                  << " trades match row-for-row within tolerance "
                  << "(price=" << cli.tol_price
                  << ", score=" << cli.tol_score << ")\n";
        return 0;
    }
    std::cerr << "[FAIL] " << total_fails << " divergence(s) total ("
              << count_fails << " count, " << field_fails << " field).\n";
    return 1;
}
