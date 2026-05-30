// =============================================================================
// goldstack_subs_audit.cpp -- 2yr XAUUSD audit of 10 GoldStack sub-engines
//                             after the 2026-05-29 ABS-PT threshold doublings
//                             (S37-Z task#18).
//
// WHY THIS EXISTS
//   include/GoldEngineStack.hpp ships ~17 `static constexpr double`
//   thresholds (IMPULSE_MIN, VWAP_DEV_MIN, NET_MIN, MIN_RANGE, MIN_SPIKE,
//   etc.) that were calibrated against $2400-era XAU. Gold is now $4700+
//   and those thresholds had silently disabled half the sub-engine stack
//   (BLOCKED with sl_dist_too_wide / impulse_below_min / spread_too_wide
//   in shadow logs). Edits on 2026-05-29 doubled each threshold; this
//   harness re-runs the 2yr tape through every patched engine so the
//   "doubled = right ballpark" assumption can be verified empirically
//   before the change is pushed.
//
// SCOPE
//   10 sub-engines listed in goldstack_subs_audit_drivers.hpp. DXY engine
//   deferred (no DXY tick corpus on disk).
//
// DESIGN
//   * Pure C++. NO PYTHON. CRTP driver tuple with single fold-expression
//     dispatch in the inner loop -- mirrors backtest/disabled_engine_audit.
//   * GoldSnapshot synthesised by gsa::SnapshotBuilder using tape time
//     (NOT std::time). This avoids the wall-clock leakage that breaks
//     session-window gates in the 21 std::time(nullptr) sites inside
//     GoldEngineStack.hpp.
//   * Time-override hard-link of `time()` at file scope -- intercepts
//     every libc time(nullptr) call in the engine TU and routes it to
//     omega::bt::g_sim_now_ms. Standard faketime trick.
//
// USAGE
//   cmake --build build --target goldstack_subs_audit -j
//   ./build/goldstack_subs_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv \
//                                /tmp/goldstack_subs_audit.txt
// =============================================================================

// OmegaTimeShim must precede engine headers so OMEGA_BT_SHIM_ACTIVE is
// defined when GoldEngineStack.hpp parses its nowSec macros, and so that
// omega::bt::set_sim_time / g_sim_now_ms are available.
#include "OmegaTimeShim.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <tuple>
#include <chrono>
#include <iostream>

#include "goldstack_subs_audit_drivers.hpp"

// ---------------------------------------------------------------------------
// Time-override -- link-level interception of libc time(). Every
// std::time(nullptr) call in the engine stack (21 sites in
// GoldEngineStack.hpp + transitively in EngineBase helpers) resolves to
// `using ::time` in std, which the linker binds to this symbol because
// user objects are searched before libc on macOS/Linux. Without this
// override, session-window gates (SessionMomentum, IntradaySeasonality,
// AsianRange, DonchianBreakout) leak wall clock and the audit only
// fires when the test happens to run during real London/NY hours.
//
// CALIBRATION RULE (operator directive 2026-05-29): every audit harness
// that uses abs-pt thresholds must surface "calibration era vs current
// price" up front -- printed at boot below.
// ---------------------------------------------------------------------------
// NOTE: libc declares time() without noexcept; the user override must match.
extern "C" time_t time(time_t* t) {
    const time_t v = static_cast<time_t>(omega::bt::g_sim_now_ms / 1000LL);
    if (t) *t = v;
    return v;
}

namespace gsa = omega::gsa;

// ---------------------------------------------------------------------------
// Tick parser -- DukasCopy YYYYMMDD,HH:MM:SS,bid,ask (same as the other audits)
// ---------------------------------------------------------------------------
struct Tick { int64_t ts_ms; double bid; double ask; };

static bool parse_duka_xau(const char* line, Tick& out) {
    int Y, M, D, h, m, s; double bid, ask;
    if (std::sscanf(line, "%4d%2d%2d,%d:%d:%d,%lf,%lf",
                    &Y, &M, &D, &h, &m, &s, &bid, &ask) != 8) return false;
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h;        tm.tm_min  = m;    tm.tm_sec  = s;
    const time_t epoch_s = timegm(&tm);
    if (epoch_s <= 0) return false;
    out.ts_ms = static_cast<int64_t>(epoch_s) * 1000LL;
    out.bid = bid; out.ask = ask;
    return true;
}

// ---------------------------------------------------------------------------
// Silence engine cout / printf -- 154M ticks * 10 engines blowing up the
// stdout buffer otherwise. RPT is a separate FILE* so the report still lands.
// Mirrors disabled_engine_audit.cpp.
// ---------------------------------------------------------------------------
struct CoutSilencer {
    std::streambuf* prev;
    CoutSilencer() : prev(std::cout.rdbuf(nullptr)) {}
    ~CoutSilencer() { std::cout.rdbuf(prev); }
};

// ---------------------------------------------------------------------------
// Report helpers
// ---------------------------------------------------------------------------
static void print_block(FILE* rpt, const char* label, const gsa::Stats& s) {
    std::fprintf(rpt,
        "  %-15s n=%-6d  WR=%6.2f%%  PF=%6.3f  gross=%+10.3f  MaxDD=%9.3f\n",
        label, s.n_trades, s.wr(), s.pf(), s.gross, s.max_dd);
}

template <typename Driver>
static void emit_per_engine(FILE* rpt, const Driver& d) {
    const gsa::Stats& s = d.stats;
    std::fprintf(rpt, "\n============================================================\n");
    std::fprintf(rpt, "  ENGINE: %s\n", d.name());
    std::fprintf(rpt, "============================================================\n");
    print_block(rpt, "ALL", s);
    std::fprintf(rpt, "  BY SESSION (UTC)\n");
    std::fprintf(rpt, "    Asia    n=%-6d gross=%+10.3f\n", s.n_asia,   s.g_asia);
    std::fprintf(rpt, "    London  n=%-6d gross=%+10.3f\n", s.n_london, s.g_london);
    std::fprintf(rpt, "    NY      n=%-6d gross=%+10.3f\n", s.n_ny,     s.g_ny);
    std::fprintf(rpt, "    Late_NY n=%-6d gross=%+10.3f\n", s.n_late,   s.g_late);
    // SESSION x HALF walk-forward: an edge is promotable only if a session
    // slice is positive in BOTH halves (train 2024-03..2025-03, test
    // 2025-04..2026-04). Flag *** when both halves net positive AND n>=30 each.
    std::fprintf(rpt, "  SESSION x HALF (WF: H1=train H2=test)\n");
    static const char* SN[4] = {"Asia   ", "London ", "NY     ", "Late_NY"};
    for (int si = 0; si < 4; ++si) {
        const int    n1 = s.n_sh[si][0],  n2 = s.n_sh[si][1];
        const double g1 = s.gw_sh[si][0] - s.gl_sh[si][0];
        const double g2 = s.gw_sh[si][1] - s.gl_sh[si][1];
        const double p1 = s.gl_sh[si][0] > 0 ? s.gw_sh[si][0] / s.gl_sh[si][0] : 0.0;
        const double p2 = s.gl_sh[si][1] > 0 ? s.gw_sh[si][1] / s.gl_sh[si][1] : 0.0;
        const bool wf_ok = (g1 > 0 && g2 > 0 && n1 >= 30 && n2 >= 30);
        std::fprintf(rpt,
            "    %s  H1 n=%-4d g=%+9.3f PF=%-5.3f | H2 n=%-4d g=%+9.3f PF=%-5.3f %s\n",
            SN[si], n1, g1, p1, n2, g2, p2, wf_ok ? "*** WF-POSITIVE" : "");
    }
    if (!s.g_by_month.empty()) {
        std::fprintf(rpt, "  BY MONTH\n");
        for (const auto& kv : s.g_by_month) {
            auto it_n = s.n_by_month.find(kv.first);
            const int nm = (it_n != s.n_by_month.end()) ? it_n->second : 0;
            std::fprintf(rpt, "    %s n=%-5d gross=%+10.3f\n",
                         kv.first.c_str(), nm, kv.second);
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <xau_tick_csv> [report_file]\n", argv[0]);
        return 1;
    }
    const std::string path        = argv[1];
    const std::string report_path = (argc >= 3) ? std::string(argv[2])
                                                : std::string("/tmp/goldstack_subs_audit.txt");

    FILE* RPT = std::fopen(report_path.c_str(), "w");
    if (!RPT) { std::fprintf(stderr, "ERROR: cannot open %s\n", report_path.c_str()); return 3; }
    std::fprintf(stderr, "[GSA] report -> %s\n", report_path.c_str());

    CoutSilencer cout_kill;
    std::freopen("/dev/null", "w", stdout);

    // Compile-time driver tuple. To add an engine: include its driver in
    // goldstack_subs_audit_drivers.hpp and append the type here.
    using Drivers = std::tuple<
        gsa::SessionMomentumDriver,
        gsa::IntradaySeasonalityDriver,
        gsa::AsianRangeDriver,
        gsa::DynamicRangeDriver,
        gsa::NR3TickDriver,
        gsa::TwoBarReversalDriver,
        gsa::DonchianBreakoutDriver,
        gsa::VWAPSnapbackDriver,
        gsa::LiquiditySweepProDriver,
        gsa::VWAPStretchReversionDriver
    >;
    Drivers drivers;
    gsa::SnapshotBuilder builder;

    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { std::fprintf(stderr, "ERROR: cannot open %s\n", path.c_str()); std::fclose(RPT); return 2; }

    // Calibration banner -- operator's CALIBRATION rule per the 2026-05-29
    // root-cause discipline. Engines audited here had their abs-pt thresholds
    // doubled from $2400-era values to track the $4700 price level.
    std::fprintf(RPT, "============================================================\n");
    std::fprintf(RPT, "  goldstack_subs_audit  --  %s\n", path.c_str());
    std::fprintf(RPT, "  THRESHOLDS: $2400-era originals (S37-Z task#18 2x doubling\n");
    std::fprintf(RPT, "              was REVERTED 03e6ec11). Harness fidelity-fixed S39:\n");
    std::fprintf(RPT, "              real sweep_size + 256-tick price stddev.\n");
    std::fprintf(RPT, "============================================================\n");

    char line[1024];
    int64_t lines = 0, parsed = 0, t0 = 0, t1 = 0;
    constexpr int64_t PROGRESS_EVERY = 5'000'000;
    const auto wall_t0 = std::chrono::steady_clock::now();

    while (std::fgets(line, sizeof(line), f)) {
        ++lines;
        Tick t;
        if (!parse_duka_xau(line, t)) continue;
        ++parsed;
        if (t0 == 0) t0 = t.ts_ms;
        t1 = t.ts_ms;

        // Drive sim clock for OmegaBtClock + the time() override above.
        omega::bt::set_sim_time(t.ts_ms);

        // Synthesize one snapshot per tick, then fold-dispatch to every driver.
        builder.update(t.bid, t.ask, t.ts_ms);
        const auto& snap = builder.snap;
        std::apply([&](auto&... d){ (d.feed(snap, t.bid, t.ask, t.ts_ms), ...); }, drivers);

        if (lines % PROGRESS_EVERY == 0) {
            const auto wall_now = std::chrono::steady_clock::now();
            const double wall_s = std::chrono::duration<double>(wall_now - wall_t0).count();
            // NB: report progress to stderr only (fd 2 is untouched -- only
            // stdout was reopened to /dev/null).
            std::fprintf(stderr,
                "\r[GSA] %lldM lines  %.1fs wall  rate=%.1fM/s  ",
                (long long)(lines / 1'000'000), wall_s, (lines / 1e6) / wall_s);
        }
    }
    std::fclose(f);

    const double hours = (t1 > t0) ? (t1 - t0) / 3600000.0 : 0.0;
    const auto wall_t1 = std::chrono::steady_clock::now();
    const double wall_total_s = std::chrono::duration<double>(wall_t1 - wall_t0).count();
    std::fprintf(stderr, "\n[GSA] done. %lld lines, %lld parsed, %.1fd simulated, %.1fs wall\n",
                 (long long)lines, (long long)parsed, hours / 24.0, wall_total_s);

    std::fprintf(RPT, "  corpus: %lld lines, %.1fd simulated, %.1fs wall\n",
                 (long long)parsed, hours / 24.0, wall_total_s);
    std::fprintf(RPT, "============================================================\n");

    std::apply([&](auto const&... d){ (emit_per_engine(RPT, d), ...); }, drivers);

    std::fclose(RPT);
    std::fprintf(stderr, "[GSA] report written to %s\n", report_path.c_str());
    return 0;
}
