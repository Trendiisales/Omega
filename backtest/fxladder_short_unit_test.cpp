// fxladder_short_unit_test.cpp — S-2026-07-08c synthetic unit test for the
// DOWN-JUMP SHORT mirror added to FxUpJumpLadderCompanion (cfg.short_downjump).
// Validates on hand-built H1 bars:
//   1. falling closes >= thr% under the rolling W-bar HIGH arm a short window
//   2. 5 legs enter SHORT at the trigger close; reclip on a further -1.67thr
//   3. bounce-back clips book POSITIVE pct (price below entry = short profit)
//   4. LOSS_CUT fires on an adverse RALLY at entry*(1+5thr%) with pct = -5thr%
//   5. ledger/close callbacks report SHORT (is_long=false)
//   6. auto-retirement latch: forward book <= retire_usd blocks NEW windows
//   7. long-side smoke: original up-jump mechanics still arm/clip positive
// Build:  g++ -std=c++17 -Iinclude backtest/fxladder_short_unit_test.cpp -o /tmp/fxlad_test
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

struct LedgerRow { std::string engine; bool is_long; double entry, exit; std::string reason; };
static std::vector<LedgerRow> g_ledger;
static std::vector<bool> g_close_dirs;   // orig_is_long per close_fn call

static void wire(omega::FxLadderPair& p) {
    p.set_exec(
        [](const std::string&, bool, double, double) -> std::string { return "TOK"; },
        [](const std::string&, bool orig_is_long, double, double, const std::string&) {
            g_close_dirs.push_back(orig_is_long);
        },
        [](const std::string&, double, double) { return true; },
        [](const std::string& engine, const std::string&, bool is_long,
           double entry_px, double exit_px, double, int64_t, int64_t, const char* reason) {
            g_ledger.push_back({engine, is_long, entry_px, exit_px, reason});
        });
}

static omega::FxLadderPair::Config mk_cfg(const char* pair, bool short_dj,
                                          double retire, const char* prefix) {
    omega::FxLadderPair::Config c;
    c.pair = pair; c.live_sym = pair;
    c.W = 6; c.thr = 0.5; c.rt_cost_bp = 0.0;   // cost 0 -> exact sign/level checks
    c.notional = 10000.0;
    c.short_downjump = short_dj;
    c.retire_usd = retire;
    c.file_prefix = prefix;
    return c;
}

int main() {
    std::system("rm -f /tmp/fxlt_*");
    const int64_t H = 3600;
    int64_t ts = 1000000 * H;   // arbitrary hourly grid

    // ── SHORT-SIDE MECHANICS ────────────────────────────────────────────────
    {
        g_ledger.clear(); g_close_dirs.clear();
        omega::FxLadderPair p(mk_cfg("USDCAD", true, 0.0, "/tmp/fxlt_s_"));
        wire(p);
        for (int i = 0; i < 10; ++i) { p.seed_bar(ts, 100, 100, 100); ts += H; }
        p.finalize_seed();

        // flat live bars (fwd now true; history rolls, no trigger)
        for (int i = 0; i < 3; ++i) { p.on_h1_bar(ts, 100, 100, 100); ts += H; }
        CHECK(p.total_clips() == 0, "no clips before any window");

        // TRIGGER: close 99.4 = 0.6% under the rolling 6-bar high (100) >= thr 0.5%
        p.on_h1_bar(ts, 100.0, 99.3, 99.4); ts += H;
        CHECK(p.total_clips() == 0, "trigger bar books nothing (legs just entered)");

        // deeper: stacked+tight legs arm at the low 98.3; close 98.4 <= 98.57 reclip level
        // -> a 6th LADDER leg enters at 98.4
        p.on_h1_bar(ts, 99.4, 98.3, 98.4); ts += H;
        CHECK(p.total_clips() == 0, "armed legs hold through the extension bar");

        // BOUNCE-BACK: high 99.0 pierces the mirrored trails ->
        //   TIGHT  stop = trough 98.3 + 0.333 (abs)  = 98.633 -> pct = -(98.633/99.4-1)*100 = +0.772
        //   STACKx3 g50 stop = 99.4 + 0.5*(98.3-99.4) = 98.85 -> pct = +0.553
        p.on_h1_bar(ts, 99.0, 98.35, 99.0); ts += H;
        CHECK(p.total_clips() == 4, "bounce-back clips TIGHT + 3 STACKED (4 clips)");
        {
            bool signs_ok = true, all_short = true, trail_ok = true;
            for (const auto& r : g_ledger) {
                if (r.is_long) all_short = false;
                if (r.reason != "TRAIL_STOP") trail_ok = false;
                const double pct = -1.0 * (r.exit / r.entry - 1.0) * 100.0;
                if (pct <= 0) signs_ok = false;
            }
            CHECK(all_short, "all ledger rows SHORT (is_long=false)");
            CHECK(trail_ok, "bounce-back clips are TRAIL_STOP");
            CHECK(signs_ok, "short bounce-back clips book POSITIVE pnl (exit below entry)");
            const double tight_fill = g_ledger.front().exit;
            const double expect = 98.3 + 99.4 * (0.67 * 0.5 / 100.0);   // trough + abs trail (0.67*thr of entry)
            CHECK(std::fabs(tight_fill - expect) < 1e-9, "TIGHT trail fill at trough+abs (98.63299)");
        }
        const double book_after_trails = p.book_pct();
        CHECK(book_after_trails > 0.0, "book positive after profitable short clips");

        // ADVERSE RALLY: LADDER leg (entry 98.4) LOSS_CUT at 98.4*1.025=100.86 -> pct -2.5%
        size_t nled = g_ledger.size();
        p.on_h1_bar(ts, 101.0, 99.0, 100.5); ts += H;
        CHECK(g_ledger.size() == nled + 1 && g_ledger.back().reason == "LOSS_CUT",
              "adverse rally fires LOSS_CUT on the unarmed LADDER leg");
        {
            const auto& r = g_ledger.back();
            const double pct = -1.0 * (r.exit / r.entry - 1.0) * 100.0;
            CHECK(std::fabs(pct - (-2.5)) < 1e-9, "LOSS_CUT pct = -5*thr = -2.50% (mirrored)");
            CHECK(std::fabs(r.exit - 98.4 * 1.025) < 1e-9, "LOSS_CUT fill at entry*(1+5thr%)");
        }
        // second rally bar cuts the remaining WIDE leg (cut 99.4*1.025 = 101.885)
        p.on_h1_bar(ts, 102.2, 100.4, 102.0); ts += H;
        CHECK(g_ledger.back().reason == "LOSS_CUT" && std::fabs(g_ledger.back().exit - 99.4 * 1.025) < 1e-9,
              "WIDE leg LOSS_CUT at its own mirrored level");
        CHECK(p.total_clips() == 6, "all 6 short legs accounted (4 trail + 2 LC)");
        {
            bool closes_short = true;
            for (bool d : g_close_dirs) if (d) closes_short = false;
            CHECK(!g_close_dirs.empty() && closes_short, "close_fn called with orig_is_long=false");
        }
        // JSON carries the direction
        CHECK(p.pair_json().find("\"dir\":\"short\"") != std::string::npos, "pair_json dir=short");
    }

    // ── AUTO-RETIREMENT LATCH ───────────────────────────────────────────────
    {
        g_ledger.clear(); g_close_dirs.clear();
        omega::FxLadderPair p(mk_cfg("USDCAD", true, -100.0, "/tmp/fxlt_r_"));
        wire(p);
        ts += 100 * H;
        for (int i = 0; i < 10; ++i) { p.seed_bar(ts, 100, 100, 100); ts += H; }
        p.finalize_seed();
        for (int i = 0; i < 3; ++i) { p.on_h1_bar(ts, 100, 100, 100); ts += H; }
        // window 1: trigger then rally everything into LOSS_CUT (5 legs x -2.5% x $10k = -$1250)
        p.on_h1_bar(ts, 100.0, 99.3, 99.4); ts += H;
        p.on_h1_bar(ts, 103.0, 99.4, 102.9); ts += H;   // all 5 legs LC (cut 101.885)
        CHECK(p.total_clips() == 5 && p.book_usd() <= -100.0,
              "window-1 loss-cuts drive the book under retire_usd");
        // let the window expire (W=6 bars total incl. the two above)
        for (int i = 0; i < 6; ++i) { p.on_h1_bar(ts, 102.9, 102.9, 102.9); ts += H; }
        // window-2 trigger attempt: 0.6% under the rolling high -> must be BLOCKED
        const int before = p.total_clips();
        p.on_h1_bar(ts, 102.9, 102.2, 102.28); ts += H;      // 102.28 = 0.60% under 102.9
        p.on_h1_bar(ts, 102.2, 101.0, 101.1); ts += H;       // would arm/clip if legs existed
        p.on_h1_bar(ts, 102.3, 101.2, 102.25); ts += H;      // bounce would trail-clip
        CHECK(p.total_clips() == before, "retired book arms NO new windows (no new clips)");
        CHECK(p.pair_json().find("\"retired\":true") != std::string::npos, "pair_json retired=true");
    }

    // ── LONG-SIDE SMOKE (original mechanics unchanged) ──────────────────────
    {
        g_ledger.clear(); g_close_dirs.clear();
        omega::FxLadderPair p(mk_cfg("EURUSD", false, 0.0, "/tmp/fxlt_l_"));
        wire(p);
        ts += 100 * H;
        for (int i = 0; i < 10; ++i) { p.seed_bar(ts, 100, 100, 100); ts += H; }
        p.finalize_seed();
        for (int i = 0; i < 3; ++i) { p.on_h1_bar(ts, 100, 100, 100); ts += H; }
        p.on_h1_bar(ts, 100.7, 100.0, 100.6); ts += H;       // +0.6% over 6-bar min low -> window
        p.on_h1_bar(ts, 101.7, 100.6, 101.6); ts += H;       // legs arm at the high
        p.on_h1_bar(ts, 101.7, 100.9, 101.0); ts += H;       // giveback -> trail clips
        bool any_long_profit = false, all_long = true;
        for (const auto& r : g_ledger) {
            if (!r.is_long) all_long = false;
            if ((r.exit / r.entry - 1.0) > 0) any_long_profit = true;
        }
        CHECK(p.total_clips() > 0, "long side still triggers and clips");
        CHECK(all_long && any_long_profit, "long clips report LONG with positive raw move");
        CHECK(p.pair_json().find("\"dir\":\"long\"") != std::string::npos, "pair_json dir=long");
    }

    std::printf(g_fail ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
