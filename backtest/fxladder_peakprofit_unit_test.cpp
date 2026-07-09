// fxladder_peakprofit_unit_test.cpp — S-2026-07-09 unit test for the WIDE-runner
// PEAK-PROFIT TRAIL retune added to FxUpJumpLadderCompanion (wide_arm_pct + wide_gb_frac).
// Validates on hand-built H1 bars that the WIDE (ti=1) leg:
//   1. gb10 + arm+1%: peaks +5% then turns down -> EXITS at +4.5% (keeps 90% of the peak
//      gain), NOT breakeven-via-window-flush and NOT the old +2.5% (50% giveback).
//   2. never reaches the engage arm (+1% MFE) -> rides unarmed, books via WINDOW_EXIT at the
//      close (behaves as before — no early peak-profit exit).
//   3. LEGACY (wide_* unset): peaks +5% -> exits +2.5% (the original 2.7*thr / g50), proving
//      the retune is opt-in and backward-compatible.
// Build:  g++ -std=c++17 -Iinclude backtest/fxladder_peakprofit_unit_test.cpp -o /tmp/fxlad_pp
#include "../include/FxUpJumpLadderCompanion.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) std::printf("  PASS: %s\n", msg); \
    else { std::printf("  FAIL: %s\n", msg); ++g_fail; } \
} while (0)

struct LedgerRow { std::string engine; double entry, exit; std::string reason; };
static std::vector<LedgerRow> g_ledger;

static void wire(omega::FxLadderPair& p) {
    p.set_exec(
        [](const std::string&, bool, double, double) -> std::string { return "TOK"; },
        [](const std::string&, bool, double, double, const std::string&) {},
        [](const std::string&, double, double) { return true; },
        [](const std::string& engine, const std::string&, bool,
           double entry_px, double exit_px, double, int64_t, int64_t, const char* reason) {
            g_ledger.push_back({engine, entry_px, exit_px, reason});
        });
}

// find the WIDE (ti=1, "..Lad_W") ledger row; returns true + fills out if present.
static bool wide_row(LedgerRow& out) {
    for (const auto& r : g_ledger)
        if (r.engine.size() >= 5 && r.engine.substr(r.engine.size() - 5) == "Lad_W") { out = r; return true; }
    return false;
}

static omega::FxLadderPair::Config mk_cfg(const char* prefix, double warm, double wgb) {
    omega::FxLadderPair::Config c;
    c.pair = "TEST"; c.live_sym = "TEST";
    c.W = 8; c.thr = 1.0; c.rt_cost_bp = 0.0;   // cost 0 -> exact level checks
    c.notional = 10000.0;
    c.wide_arm_pct = warm; c.wide_gb_frac = wgb;
    c.file_prefix = prefix;
    return c;
}

// Drive: seed flat @99, trigger @100 (+1.01% over the 99 lows -> WIDE entry=100),
// then bars that take the WIDE leg to a +5% peak and back down.
// Returns after the reversal bar. arm_h/peak_h/rev_l are the intrabar highs/low we feed.
static void drive_peak_then_reverse(omega::FxLadderPair& p, int64_t& ts, int64_t H) {
    for (int i = 0; i < 10; ++i) { p.seed_bar(ts, 99, 99, 99); ts += H; }
    p.finalize_seed();
    for (int i = 0; i < 2; ++i) { p.on_h1_bar(ts, 99, 99, 99); ts += H; }   // flat live, no trigger
    p.on_h1_bar(ts, 100, 99, 100); ts += H;   // TRIGGER close=100 -> base legs enter @100
    p.on_h1_bar(ts, 101, 100, 101); ts += H;  // arm the WIDE leg (+1% MFE = px 101)
    p.on_h1_bar(ts, 105, 101, 105); ts += H;  // peak to +5% (px 105)
    p.on_h1_bar(ts, 105, 104, 104); ts += H;  // low 104 pierces the trail -> WIDE exits
}

int main() {
    std::system("rm -f /tmp/fxpp_*");
    const int64_t H = 3600;

    // ── 1. gb10 + arm+1%: peak +5% -> exit +4.5% (keep 90%) ──
    {
        g_ledger.clear();
        int64_t ts = 2000000 * H;
        omega::FxLadderPair p(mk_cfg("/tmp/fxpp_a_", /*warm*/1.0, /*wgb*/0.10));
        wire(p);
        drive_peak_then_reverse(p, ts, H);
        LedgerRow w;
        bool got = wide_row(w);
        CHECK(got, "gb10: WIDE leg booked a clip");
        CHECK(got && std::fabs(w.entry - 100.0) < 1e-6, "gb10: WIDE entry = 100 (trigger close)");
        CHECK(got && std::fabs(w.exit - 104.5) < 1e-6, "gb10: WIDE exits at 104.5 (+4.5% = keep 90% of the +5% peak)");
        CHECK(got && w.reason == "TRAIL_STOP", "gb10: WIDE exit reason = TRAIL_STOP (peak-profit turn, not window flush)");
    }

    // ── 2. never hits the +1% engage arm -> rides unarmed, WINDOW_EXIT at close (as before) ──
    {
        g_ledger.clear();
        int64_t ts = 3000000 * H;
        omega::FxLadderPair p(mk_cfg("/tmp/fxpp_b_", /*warm*/1.0, /*wgb*/0.10));
        wire(p);
        for (int i = 0; i < 10; ++i) { p.seed_bar(ts, 99, 99, 99); ts += H; }
        p.finalize_seed();
        for (int i = 0; i < 2; ++i) { p.on_h1_bar(ts, 99, 99, 99); ts += H; }
        p.on_h1_bar(ts, 100, 99, 100); ts += H;   // TRIGGER, WIDE entry=100
        // stay UNDER +1% MFE (max px 100.5) for the whole window so WIDE never arms, then flush.
        for (int i = 0; i < 9; ++i) { p.on_h1_bar(ts, 100.5, 99.8, 100.0); ts += H; }
        LedgerRow w;
        bool got = wide_row(w);
        CHECK(got && w.reason == "WINDOW_EXIT", "no-arm: WIDE books via WINDOW_EXIT (unarmed ride, as before)");
        CHECK(got && std::fabs(w.exit - 100.0) < 0.11, "no-arm: WIDE flush exit ~= entry (no early peak-profit exit)");
    }

    // ── 3. LEGACY (wide_* unset): peak +5% -> exit +2.5% (old 2.7*thr / g50), backward-compatible ──
    {
        g_ledger.clear();
        int64_t ts = 4000000 * H;
        // legacy: warm<0, wgb<=0 -> WIDE arms at 2.7*thr = 2.7% (px 102.7), g50 giveback 0.5
        omega::FxLadderPair p(mk_cfg("/tmp/fxpp_c_", /*warm*/-1.0, /*wgb*/0.0));
        wire(p);
        for (int i = 0; i < 10; ++i) { p.seed_bar(ts, 99, 99, 99); ts += H; }
        p.finalize_seed();
        for (int i = 0; i < 2; ++i) { p.on_h1_bar(ts, 99, 99, 99); ts += H; }
        p.on_h1_bar(ts, 100, 99, 100); ts += H;    // TRIGGER, WIDE entry=100
        p.on_h1_bar(ts, 103, 100, 103); ts += H;   // arm the legacy WIDE (2.7% MFE = px 102.7); peak 103
        p.on_h1_bar(ts, 105, 103, 105); ts += H;   // peak to +5%
        p.on_h1_bar(ts, 105, 102, 102); ts += H;   // low 102 pierces the legacy g50 stop (102.5)
        LedgerRow w;
        bool got = wide_row(w);
        // legacy g50: stop = entry + 0.5*(peak-entry) = 100 + 0.5*5 = 102.5 -> exits +2.5% (50% giveback).
        CHECK(got, "legacy: WIDE leg booked a clip");
        CHECK(got && std::fabs(w.exit - 102.5) < 1e-6,
              "legacy: WIDE exits at 102.5 (+2.5% = old 50% giveback), NOT 104.5 -> retune is opt-in");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
