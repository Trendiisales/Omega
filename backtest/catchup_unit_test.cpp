// catchup_unit_test.cpp — negative-path unit tests for the S-2026-07-18 BOUNDED
// CATCH-UP port (FxLadderPair::catchup_replay_ + GoldTrendMimicBook::on_trend_restore).
// Template: ChimeraCrypto/tests/catchup_seed_test.cpp (arms only within bounds; stays
// flat on every excluded class; legs open only via the live confirm path).
//
// Build+run: g++ -std=c++17 -O2 -Iinclude backtest/catchup_unit_test.cpp -o /tmp/cutest && /tmp/cutest

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <unistd.h>
#include "FxUpJumpLadderCompanion.hpp"
#include "GoldTrendMimicLadder.hpp"

static int g_pass = 0, g_fail = 0;
static void chk(bool ok, const char* name) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) ++g_pass; else ++g_fail;
}
static bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// ── FX side ─────────────────────────────────────────────────────────────────
// Synthetic H1 series: flat base then a jump bar; ts anchored to a Tuesday so the
// weekend-arm block stays out of the way unless a test wants it.
static const int64_t T0 = 1752537600;   // 2025-07-15 00:00 UTC (Tuesday)

struct FxProbe {
    std::string dir;
    std::unique_ptr<omega::FxLadderPair> p;
    explicit FxProbe(omega::FxLadderPair::Config c) {
        char tmpl[] = "/tmp/cutest.XXXXXX"; dir = mkdtemp(tmpl);
        { std::ofstream f(dir + "/deploy.txt"); f << "0\n"; }
        c.deploy_path = dir + "/deploy.txt"; c.bars_path = dir + "/h1.csv";
        c.book_path = dir + "/book.txt"; c.live_path = dir + "/live.txt";
        c.closed_path = dir + "/closed.csv";
        p = std::make_unique<omega::FxLadderPair>(std::move(c));
        p->set_exec([](const std::string&, bool, double, double)->std::string{ return "t"; },
                    [](const std::string&, bool, double, double, const std::string&){},
                    [](const std::string&, double, double)->bool{ return true; },
                    [](const std::string&, const std::string&, bool, double, double, double,
                       int64_t, int64_t, const char*){});
    }
    ~FxProbe(){ std::string c = "rm -rf '" + dir + "'"; (void)!system(c.c_str()); }
    bool win_active() const { return contains(p->pair_json(), "\"active\":true"); }
    int  live_leg_lines() const {
        std::ifstream f(dir + "/live.txt"); std::string ln; int n = 0;
        while (std::getline(f, ln)) if (ln.rfind("leg ", 0) == 0) n++;
        return n;
    }
};

static omega::FxLadderPair::Config fx_cfg(int catchup, double be = 0.08, bool floor_on = true,
                                          bool short_dj = false) {
    omega::FxLadderPair::Config c;
    c.pair = "TESTPAIR"; c.live_sym = "TESTPAIR";
    c.W = 12; c.thr = 1.0; c.rt_cost_bp = 2.0;
    c.wide_gb_frac = 0.10; c.wide_arm_pct = 1.0;
    c.be_entry_pct = be; c.pend_bars = 4;
    c.be_floor_on_open = floor_on;
    c.block_weekend_arms = true; c.weekend_carry_frac = 0.0;
    c.catchup_max_age_bars = catchup;
    c.short_downjump = short_dj;
    return c;
}

// seed: `pre` flat bars at 100, a jump close (+thr+0.2% long / mirrored short), then
// `post` bars whose highs stay below/above the BE level (drift, no BE cross).
static void fx_seed(FxProbe& fp, int pre, int post, double d = 1.0, double post_fav_pct = 0.0) {
    int64_t ts = T0; const double base = 100.0;
    for (int i = 0; i < pre; ++i, ts += 3600)
        fp.p->seed_bar(ts, base + 0.02, base - 0.02, base);
    const double jump = base * (1.0 + d * 0.012);          // 1.2% > thr 1.0%
    fp.p->seed_bar(ts, d > 0 ? jump : base, d > 0 ? base : jump, jump); ts += 3600;
    for (int i = 0; i < post; ++i, ts += 3600) {
        const double fav = jump * (1.0 + d * post_fav_pct / 100.0);
        fp.p->seed_bar(ts, d > 0 ? fav : jump - 0.5, d > 0 ? jump - 0.5 : fav, jump * (1.0 - d * 0.0002));
    }
    fp.p->finalize_seed();
}

static void fx_tests() {
    std::printf("FX FxLadderPair catchup_replay_:\n");
    { FxProbe fp(fx_cfg(0));  fx_seed(fp, 20, 1); chk(!fp.win_active(), "default-off (catchup=0) stays flat"); }
    { FxProbe fp(fx_cfg(24)); fx_seed(fp, 20, 1);
      chk(fp.win_active(), "eligible jump 1 bar old -> window re-opened");
      chk(fp.live_leg_lines() == 5, "5 PENDING base-batch legs re-spawned"); }
    { FxProbe fp(fx_cfg(24)); fx_seed(fp, 20, 1, 1.0, 0.10);   // post bar high +0.10% > be 0.08
      chk(!fp.win_active(), "BE crossed during outage -> SKIP (no backfill)"); }
    { FxProbe fp(fx_cfg(2));  fx_seed(fp, 20, 3); chk(!fp.win_active(), "age 3 > bound 2 -> flat"); }
    { FxProbe fp(fx_cfg(24, 0.0)); fx_seed(fp, 20, 1); chk(!fp.win_active(), "be_entry_pct=0 -> ineligible"); }
    { FxProbe fp(fx_cfg(24, 0.08, false)); fx_seed(fp, 20, 1); chk(!fp.win_active(), "be_floor_on_open=false -> ineligible"); }
    { FxProbe fp(fx_cfg(24)); fx_seed(fp, 4, 1); chk(!fp.win_active(), "history shorter than W -> flat"); }
    {   // window expired: jump then FULL retrace (no re-trigger), 14 bars > W=12
        FxProbe fp(fx_cfg(24));
        int64_t ts = T0; for (int i = 0; i < 20; ++i, ts += 3600) fp.p->seed_bar(ts, 100.02, 99.98, 100.0);
        fp.p->seed_bar(ts, 101.2, 100.0, 101.2); ts += 3600;
        for (int i = 0; i < 14; ++i, ts += 3600) fp.p->seed_bar(ts, 100.02, 99.98, 100.0);
        fp.p->finalize_seed();
        chk(!fp.win_active(), "window expired (age>=W 12) -> flat"); }
    {   // pend-expired: age 5 (>= pend 4) inside bound -> LEGLESS window restore
        FxProbe fp(fx_cfg(24)); fx_seed(fp, 20, 5);
        chk(fp.win_active() && fp.live_leg_lines() == 0, "age>=pend_bars -> legless window restored"); }
    {   // in-flight persisted state untouched: pre-write a live.txt with an active window
        omega::FxLadderPair::Config c = fx_cfg(24);
        char tmpl[] = "/tmp/cutest.XXXXXX"; std::string dir = tmpl; dir = mkdtemp(&dir[0]);
        { std::ofstream f(dir + "/deploy.txt"); f << "0\n"; }
        { std::ofstream f(dir + "/live.txt"); f << "win 1 3 1 100.5\n"; }
        c.deploy_path = dir + "/deploy.txt"; c.bars_path = dir + "/h1.csv";
        c.book_path = dir + "/book.txt"; c.live_path = dir + "/live.txt"; c.closed_path = dir + "/closed.csv";
        omega::FxLadderPair p(std::move(c));
        p.set_exec([](const std::string&, bool, double, double)->std::string{ return "t"; },
                   [](const std::string&, bool, double, double, const std::string&){},
                   [](const std::string&, double, double)->bool{ return true; },
                   [](const std::string&, const std::string&, bool, double, double, double,
                      int64_t, int64_t, const char*){});
        int64_t ts = T0; for (int i = 0; i < 20; ++i, ts += 3600) p.seed_bar(ts, 100.02, 99.98, 100.0);
        p.seed_bar(ts, 101.2, 100.0, 101.2);
        p.finalize_seed();
        // persisted win age=3 must survive; catch-up must not restamp (age stays 3, not 0/1)
        chk(contains(p.pair_json(), "\"active\":true") && contains(p.pair_json(), "\"age\":3"),
            "in-flight persisted window untouched (no restamp)");
        std::string cmd = "rm -rf '" + dir + "'"; (void)!system(cmd.c_str());
    }
    {   // weekend-arm block honoured in the replay: trigger close inside the weekend
        // T0 anchored Tuesday; place the jump so ts+3600 is Saturday 02:00 UTC
        FxProbe fp(fx_cfg(24));
        int64_t sat = T0 + 4*86400 + 1*3600;   // Sat 01:00 close -> +3600 in weekend
        int64_t ts = sat - 13*3600;
        for (int i = 0; i < 12; ++i, ts += 3600) fp.p->seed_bar(ts, 100.02, 99.98, 100.0);
        fp.p->seed_bar(sat, 101.2, 100.0, 101.2);
        fp.p->seed_bar(sat + 3600, 101.2, 101.0, 101.1);
        fp.p->finalize_seed();
        chk(!fp.win_active(), "weekend-arm-blocked trigger never becomes a window");
    }
    { FxProbe fp(fx_cfg(24, 0.08, true, true)); fx_seed(fp, 20, 1, -1.0);
      chk(fp.win_active() && fp.live_leg_lines() == 5, "short down-jump mirror recovers"); }
    {   // recovered leg opens ONLY via the live BE-ENTRY path
        FxProbe fp(fx_cfg(24)); fx_seed(fp, 20, 1);
        // live bar crossing BE: high clears trig*1.0008 (trig=101.2)
        fp.p->on_h1_bar(T0 + 23*3600, 101.35, 101.15, 101.30, 101.18);
        chk(contains(fp.p->pair_json(), "\"upnl_pct\""), "recovered pending leg BE-ENTERED on live cross");
    }
}

// ── GOLD side ───────────────────────────────────────────────────────────────
struct GmProbe {
    std::string dir;
    std::unique_ptr<omega::GoldTrendMimicBook> b;
    std::vector<std::string> clips;
    explicit GmProbe(omega::GoldTrendMimicBook::Config c) {
        char tmpl[] = "/tmp/cutest.XXXXXX"; dir = mkdtemp(tmpl);
        c.state_path = dir + "/state.txt"; c.closed_path = dir + "/closed.csv";
        b = std::make_unique<omega::GoldTrendMimicBook>(std::move(c));
        b->set_exec([](const std::string&, bool, double, double)->std::string{ return "t"; },
                    [](const std::string&, bool, double, double, const std::string&){},
                    [](const std::string&, double, double)->bool{ return true; },
                    [this](const std::string& eng, const std::string&, bool, double, double, double,
                           int64_t, int64_t, const char* reason, double, double){
                        clips.push_back(eng + ":" + reason); });
    }
    ~GmProbe(){ std::string c = "rm -rf '" + dir + "'"; (void)!system(c.c_str()); }
    int open_count() const {
        const std::string j = b->json();
        const size_t p = j.find("\"open\":");
        return p == std::string::npos ? -1 : std::atoi(j.c_str() + p + 7);
    }
};

static omega::GoldTrendMimicBook::Config gm_cfg(int64_t maxage, double be = 0.15, bool floored = true) {
    omega::GoldTrendMimicBook::Config c;
    c.trigger_tag = "TestBook"; c.live_sym = "XAUUSD";
    c.legs = {{"T", 0.08}, {"W", 0.20}};
    c.arm_pct = 0.25; c.lc_pct = 1.5; c.cap_bars = 12; c.rt_cost_bp = 15.0;
    c.be_entry_pct = be; c.no_prebe_loss = floored; c.pend_bars = 6;
    c.catchup_max_age_secs = maxage; c.catchup_bar_secs = 14400;
    return c;
}

static void gm_tests() {
    std::printf("GOLD GoldTrendMimicBook on_trend_restore:\n");
    const int64_t ts = T0; const double px = 2000.0;
    { GmProbe g(gm_cfg(0));      g.b->on_trend_restore(1, px, ts, ts + 14400);
      chk(g.open_count() == 0, "default-off (maxage=0): restore spawns nothing"); }
    { GmProbe g(gm_cfg(86400));  g.b->on_trend_restore(1, px, ts, ts + 14400);
      chk(g.open_count() == 2, "eligible restore spawns the leg set PENDING"); }
    { GmProbe g(gm_cfg(86400));  g.b->on_trend_restore(1, px, ts, ts + 2*86400);
      chk(g.open_count() == 0, "age > bound: nothing"); }
    { GmProbe g(gm_cfg(7*86400)); g.b->on_trend_restore(1, px, ts, ts + 6*14400);
      chk(g.open_count() == 0, "elapsed bars >= pend_bars: nothing"); }
    { GmProbe g(gm_cfg(86400, 0.0)); g.b->on_trend_restore(1, px, ts, ts + 14400);
      chk(g.open_count() == 0, "be_entry_pct=0: ineligible"); }
    { GmProbe g(gm_cfg(86400, 0.15, false)); g.b->on_trend_restore(1, px, ts, ts + 14400);
      chk(g.open_count() == 0, "no_prebe_loss=false: ineligible"); }
    { GmProbe g(gm_cfg(86400));
      g.b->on_trend_open(1, px, ts);                       // live fire seen -> watermark
      const int n0 = g.open_count();
      g.b->on_trend_restore(1, px, ts, ts + 14400);        // restart re-fire of the SAME trigger
      chk(g.open_count() == n0, "watermark dedup: restore of a seen trigger spawns nothing"); }
    { GmProbe g(gm_cfg(86400));
      g.b->on_trend_open(1, px, ts - 10*14400);            // older trigger's legs still pending
      const int n0 = g.open_count();
      g.b->on_trend_restore(1, px, ts, ts + 14400);
      chk(g.open_count() == n0, "in-flight book: restore spawns nothing on top"); }
    {   // gap-through cancel: first managed bar entirely ABOVE the BE level
        GmProbe g(gm_cfg(86400)); g.b->on_trend_restore(1, px, ts, ts + 14400);
        g.b->on_h1_bar(2015.0, 2010.0, 2012.0, ts + 2*14400, true, true);   // low 2010 > lvl 2003
        chk(g.open_count() == 0 && g.clips.empty(), "cu gap-through: legs CANCELLED, no book"); }
    {   // honest cross: first managed bar trades THROUGH the level -> legs enter at level
        GmProbe g(gm_cfg(86400)); g.b->on_trend_restore(1, px, ts, ts + 14400);
        g.b->on_h1_bar(2005.0, 1999.0, 2004.0, ts + 2*14400, true, true);   // low 1999 < lvl 2003 < high
        chk(g.open_count() == 2, "cu leg enters at the traded BE level"); }
    {   // one-shot: first bar below level, SECOND bar gaps above -> certified level-fill applies
        GmProbe g(gm_cfg(86400)); g.b->on_trend_restore(1, px, ts, ts + 14400);
        g.b->on_h1_bar(2001.0, 1998.0, 2000.0, ts + 2*14400, true, true);   // no cross, guard consumed
        g.b->on_h1_bar(2015.0, 2010.0, 2012.0, ts + 3*14400, true, true);   // gap bar over level
        chk(g.open_count() == 2, "guard is one-shot: later gap bar keeps level-fill convention"); }
    {   // legacy .open row without the cu field still parses (no seam leg-drop)
        char tmpl[] = "/tmp/cutest.XXXXXX"; std::string dir = tmpl; dir = mkdtemp(&dir[0]);
        { std::ofstream f(dir + "/state.txt.open");
          f << "leg 1 0 2 2003 0.5 0 1 " << (long long)ts << " 0 2000 0 -\n"; }
        omega::GoldTrendMimicBook::Config c = gm_cfg(86400);
        c.state_path = dir + "/state.txt"; c.closed_path = dir + "/closed.csv";
        omega::GoldTrendMimicBook b(std::move(c));
        chk(contains(b.json(), "\"open\":1"), "legacy old-format .open row loads (cu defaults 0)");
        std::string cmd = "rm -rf '" + dir + "'"; (void)!system(cmd.c_str());
    }
}

int main() {
    fx_tests();
    gm_tests();
    std::printf("CATCHUP UNIT TESTS: %d pass / %d fail => %s\n", g_pass, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
