// =============================================================================
// bracket_gold_2yr_audit.cpp -- 2-year XAUUSD backtest for g_bracket_gold
//
// Built 2026-05-28 (operator directive after S37-Y re-enable). Re-validates
// the 2026-04-30 audit decision that produced g_disable_bracket_gold=true.
// That audit ran a 4-week window without session-of-day breakdown -- this
// harness fixes that gap by replaying the full 2-year XAUUSD tick tape
// through the production GoldBracketEngine and tagging every trade with
// the UTC session it fired in.
//
// FIDELITY (per HARNESS_FIDELITY_CHECKLIST.md):
//   - Uses production class omega::GoldBracketEngine. NO inline reimpl.
//   - OmegaTimeShim force-included via CMake so engine sees tape time, not
//     wall clock. Required for cooldown / hold-time / session gates.
//   - shadow_mode=true: BracketEngine.hpp:254 enables price-triggered fill
//     sim in PENDING phase (no broker round-trip).
//   - macro_regime="NEUTRAL" passed every tick (matches counterfactual).
//
// TICK CORPUS FORMAT (DukasCopy-export, 2yr_XAUUSD_tick_fresh.csv):
//   YYYYMMDD,HH:MM:SS,bid,ask
//   20240301,02:00:00,2044.265,2044.562
//
// USAGE:
//   cmake --build build --target bracket_gold_2yr_audit -j
//   ./build/bracket_gold_2yr_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv [report.txt]
//
// OUTPUT:
//   stderr: progress meter every 50k ticks
//   argv[2]: aggregate stats, per-session breakdown, per-month breakdown,
//            trade journal (CSV). Defaults to
//            /tmp/bracket_gold_2yr_audit_report.txt when omitted.
//   stdout : redirected to /dev/null so engine cout/printf doesn't pollute
//            the run -- mirrors disabled_engine_audit.cpp idiom.
// =============================================================================

// OmegaTimeShim MUST be included before any engine header so its
// OMEGA_BT_SHIM_ACTIVE macro is defined when BracketEngine.hpp is parsed.
// Also force-included via CMake target_compile_options, but explicit include
// keeps the LSP happy and removes the build-system dependency.
#include "OmegaTimeShim.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>

#include "OmegaTradeLedger.hpp"
#include "BracketEngine.hpp"

// ---------------------------------------------------------------------------
// Tick parser for DukasCopy format
// ---------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

// Parse "YYYYMMDD,HH:MM:SS,bid,ask" -> Tick with UTC epoch ms.
// Returns false on malformed row.
static bool parse_duka_row(const char* line, Tick& out) {
    int Y, M, D, h, m, s;
    double bid, ask;
    // "%4d%2d%2d,%d:%d:%d,%lf,%lf" -- DukasCopy uses no separator between
    // year/month/day in column 1 ("20240301").
    if (std::sscanf(line, "%4d%2d%2d,%d:%d:%d,%lf,%lf",
                    &Y, &M, &D, &h, &m, &s, &bid, &ask) != 8) {
        return false;
    }
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    // timegm: input is UTC. (DukasCopy timestamps are UTC.)
    const time_t epoch_s = timegm(&tm);
    if (epoch_s <= 0) return false;
    out.ts_ms = static_cast<int64_t>(epoch_s) * 1000LL;
    out.bid   = bid;
    out.ask   = ask;
    return true;
}

// ---------------------------------------------------------------------------
// Session tagger (UTC). Mirrors the BracketEngine session gate (CLAUDE.md
// references [BRACKET-XAUUSD] "session/risk gate closed" -- the engine itself
// internally restricts arming to London/NY-open compression-break windows).
// ---------------------------------------------------------------------------
//   Asia       : 22:00 - 07:00 UTC  (Sydney->Tokyo->Singapore tape)
//   London     : 07:00 - 12:00 UTC  (London open + morning session)
//   NY         : 12:00 - 17:00 UTC  (NY open + London/NY overlap)
//   Late_NY    : 17:00 - 22:00 UTC  (NY afternoon, Asia rollover)
// ---------------------------------------------------------------------------
static const char* utc_session_of_ts_ms(int64_t ts_ms) {
    const time_t t_s = static_cast<time_t>(ts_ms / 1000LL);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t_s);
#else
    gmtime_r(&t_s, &tm);
#endif
    const int hr = tm.tm_hour;
    if (hr >= 22 || hr < 7)  return "Asia";
    if (hr >= 7  && hr < 12) return "London";
    if (hr >= 12 && hr < 17) return "NY";
    return "Late_NY";
}

static std::string utc_month_of_ts_ms(int64_t ts_ms) {
    const time_t t_s = static_cast<time_t>(ts_ms / 1000LL);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t_s);
#else
    gmtime_r(&t_s, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
    return buf;
}

// ---------------------------------------------------------------------------
// Aggregates
// ---------------------------------------------------------------------------
struct Stats {
    int    n = 0, wins = 0, losses = 0;
    double gross = 0, gw = 0, gl = 0;
    double mfe_sum = 0, mae_sum = 0;
    double max_dd = 0;
    double running_eq = 0, peak_eq = 0;

    void add(double pnl) {
        ++n;
        gross += pnl;
        running_eq += pnl;
        if (running_eq > peak_eq) peak_eq = running_eq;
        const double dd = peak_eq - running_eq;
        if (dd > max_dd) max_dd = dd;
        if (pnl > 0) { ++wins;   gw += pnl;  }
        else         { ++losses; gl += -pnl; }
    }

    double wr() const { return n ? 100.0 * wins / n : 0.0; }
    double pf() const { return gl > 0 ? gw / gl : 0.0; }
    double avg() const { return n ? gross / n : 0.0; }
};

static void print_block(FILE* rpt, const char* tag, const Stats& s) {
    std::fprintf(rpt, "  %-12s n=%-5d  WR=%6.2f%%  PF=%6.3f  gross=%+10.2f  avg=%+7.3f  MaxDD=%8.2f\n",
                 tag, s.n, s.wr(), s.pf(), s.gross, s.avg(), s.max_dd);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <xau_tick_csv> [report_file]\n", argv[0]);
        return 1;
    }
    const std::string path        = argv[1];
    const std::string report_path = (argc >= 3) ? std::string(argv[2])
                                                : std::string("/tmp/bracket_gold_2yr_audit_report.txt");

    // Report file -- explicit FILE* so shell stdout redirects can't lose it.
    // Same idiom as backtest/disabled_engine_audit.cpp -- prior version wrote
    // via std::printf and any caller redirecting stdout (`>` or `> /dev/null`)
    // silently dropped the entire report.
    FILE* RPT = std::fopen(report_path.c_str(), "w");
    if (!RPT) { std::fprintf(stderr, "ERROR: cannot open %s\n", report_path.c_str()); return 3; }
    std::fprintf(stderr, "[BT] report -> %s\n", report_path.c_str());

    // Engines (and OmegaCostGuard) write diagnostics via std::cout / printf().
    // Over 154M ticks that's tens of GB of noise -- redirect FD 1 to /dev/null.
    // RPT is a separate FILE* so the report still lands.
    std::freopen("/dev/null", "w", stdout);

    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { std::fprintf(stderr, "ERROR: cannot open %s\n", path.c_str()); std::fclose(RPT); return 2; }

    // Build production engine with production-realistic gates so the audit
    // reflects what live will do (post-S37-Z 2026-05-29 fix). Without these
    // the engine defaults all gates to 0/off and the audit is unrealistically
    // permissive vs production.
    omega::GoldBracketEngine eng;
    eng.symbol           = "XAUUSD";
    eng.shadow_mode      = true;
    eng.ENTRY_SIZE       = 0.01;
    eng.MIN_RANGE        = 2.5;     // matches engine_init.hpp:988
    // 2026-06-12 re-opt A/B: PCT_MODE env => %-of-price gates (auto-tracks price);
    //   else the legacy absolute 19.0pt (calibrated to ~$4700, drifts as gold moves).
    if (std::getenv("PCT_MODE")) {
        eng.MAX_RANGE_PCT   = 0.40;  eng.MAX_RANGE       = 0.0;
        eng.MAX_SL_DIST_PCT = 0.40;  eng.MAX_SL_DIST_PTS = 0.0;
    } else {
        eng.MAX_RANGE        = 19.0;    // matches engine_init.hpp (S37-Z 2026-05-29 raise 12->19)
        eng.MAX_SL_DIST_PTS  = 19.0;    // matches engine_init.hpp (S37-Z 2026-05-29 raise 6->19)
    }
    eng.MIN_BREAK_TICKS  = 5;       // matches engine_init.hpp:972
    eng.MAX_SPREAD       = 2.5;     // typical live BlackBull XAU max

    // Trade collector with per-trade enrichment for session/month tagging.
    struct EnrichedTrade {
        omega::TradeRecord rec;
        std::string        session;
        std::string        month;
    };
    std::vector<EnrichedTrade> trades;
    int64_t latest_ts_ms = 0;

    auto cb = [&](const omega::TradeRecord& t) {
        EnrichedTrade et;
        et.rec     = t;
        et.session = utc_session_of_ts_ms(latest_ts_ms);
        et.month   = utc_month_of_ts_ms(latest_ts_ms);
        trades.push_back(std::move(et));
    };

    // Pump ticks.
    char line[1024];
    int64_t lines = 0, parsed = 0, t0_ms = 0, t1_ms = 0;
    const int64_t PROGRESS_EVERY = 1'000'000;

    while (std::fgets(line, sizeof(line), f)) {
        ++lines;
        Tick t;
        if (!parse_duka_row(line, t)) continue;
        ++parsed;
        if (t0_ms == 0) t0_ms = t.ts_ms;
        t1_ms = t.ts_ms;
        latest_ts_ms = t.ts_ms;

        // Drive simulated time so the engine's cooldown / session / hold-time
        // gates resolve against tape time. OmegaTimeShim.hpp is force-included
        // via CMake; set_sim_time is the only required call per tick.
        omega::bt::set_sim_time(t.ts_ms);

        eng.on_tick(t.bid, t.ask, t.ts_ms,
                    /*can_enter*/ true,
                    /*macro_regime*/ "NEUTRAL",
                    cb);

        if (lines % PROGRESS_EVERY == 0) {
            std::fprintf(stderr, "\r[BT] %lldM lines  %zu trades  phase=%d  ",
                         (long long)(lines / 1'000'000), trades.size(), (int)eng.phase);
        }
    }
    std::fclose(f);

    const double hours = (t1_ms > t0_ms) ? (t1_ms - t0_ms) / 3600000.0 : 0.0;
    std::fprintf(stderr, "\n[BT] done. %lld lines, %lld parsed, %zu trades over %.1fh (%.1fd)\n",
                 (long long)lines, (long long)parsed, trades.size(), hours, hours / 24.0);

    // -----------------------------------------------------------------------
    // Aggregation
    // -----------------------------------------------------------------------
    Stats overall;
    std::map<std::string, Stats> by_session;
    std::map<std::string, Stats> by_month;
    std::unordered_map<std::string, Stats> by_reason;

    for (const auto& et : trades) {
        const double pnl = et.rec.pnl;
        overall.add(pnl);
        by_session[et.session].add(pnl);
        by_month  [et.month  ].add(pnl);
        by_reason [et.rec.exitReason].add(pnl);
    }

    // -----------------------------------------------------------------------
    // Report  -- written to RPT (argv[2]) so shell stdout redirects can't
    // lose it. Trade-journal CSV is appended to the same file inside the
    // ===TRADE-JOURNAL-{BEGIN,END}=== guards so a downstream tool can sed
    // it out.
    // -----------------------------------------------------------------------
    std::fprintf(RPT, "\n");
    std::fprintf(RPT, "======================================================================\n");
    std::fprintf(RPT, "  bracket_gold_2yr_audit  --  %s\n", path.c_str());
    std::fprintf(RPT, "  corpus: %lld lines parsed, %.1f days simulated\n",
                 (long long)parsed, hours / 24.0);
    std::fprintf(RPT, "  gates: MIN_RANGE=%.2f  MAX_RANGE=%.2f  MAX_SL_DIST_PTS=%.2f  MIN_BREAK_TICKS=%d  MAX_SPREAD=%.2f\n",
                 eng.MIN_RANGE, eng.MAX_RANGE, eng.MAX_SL_DIST_PTS,
                 eng.MIN_BREAK_TICKS, eng.MAX_SPREAD);
    std::fprintf(RPT, "======================================================================\n");
    std::fprintf(RPT, "\nOVERALL\n");
    print_block(RPT, "ALL", overall);

    std::fprintf(RPT, "\nBY SESSION (UTC)\n");
    print_block(RPT, "Asia",    by_session["Asia"]);
    print_block(RPT, "London",  by_session["London"]);
    print_block(RPT, "NY",      by_session["NY"]);
    print_block(RPT, "Late_NY", by_session["Late_NY"]);

    std::fprintf(RPT, "\nBY EXIT REASON\n");
    for (const auto& kv : by_reason) print_block(RPT, kv.first.c_str(), kv.second);

    std::fprintf(RPT, "\nBY MONTH\n");
    for (const auto& kv : by_month) print_block(RPT, kv.first.c_str(), kv.second);

    // -----------------------------------------------------------------------
    // Trade journal CSV
    // -----------------------------------------------------------------------
    std::fprintf(RPT, "\n===TRADE-JOURNAL-BEGIN===\n");
    std::fprintf(RPT, "idx,session,month,side,entry,exit,pnl,exit_reason,hold_sec\n");
    for (size_t i = 0; i < trades.size(); ++i) {
        const auto& et = trades[i];
        std::fprintf(RPT, "%zu,%s,%s,%s,%.3f,%.3f,%+.3f,%s,%lld\n",
                     i + 1, et.session.c_str(), et.month.c_str(),
                     et.rec.side.c_str(), et.rec.entryPrice, et.rec.exitPrice,
                     et.rec.pnl, et.rec.exitReason.c_str(),
                     (long long)(et.rec.exitTs - et.rec.entryTs));
    }
    std::fprintf(RPT, "===TRADE-JOURNAL-END===\n");
    std::fclose(RPT);
    std::fprintf(stderr, "[BT] report written to %s\n", report_path.c_str());
    return 0;
}
