// stallbook_bearcold_test.cpp -- synthetic unit test for StallBook cold_loss_bear
// (S-2026-07-08 loss-bound wire: tighter LOSS_CUT on gold legs while gold price-bear).
//
// Scenarios (arm$30/trail$15/retrig$15, cold_loss_omega=-50, cold_loss_bear=-35):
//   1. gold_bear=1  : upnl -40 -> LOSS_CUT_CLIP at -40 (bear cut -35 crossed)
//   2. gold_bear=0  : upnl -40 -> NO clip; -55 -> LOSS_CUT_CLIP (baseline -50)
//   3. gold_bear=-1 : unknown regime -> baseline (-40 no clip)   [fail-safe]
//   4. cold_loss_bear=0 (disabled), gold_bear=1 -> baseline (-40 no clip)
//   5. non-gold sym (US500), gold_bear=1, bear=-35 -> baseline (-40 no clip)
//   6. trail path untouched: +40 arm then +20 -> REVERSAL_CLIP +20 in bear
//
// Run from a scratch cwd (writes stall/<name>/ state dirs). Exit 0 = all pass.
#include "StallCompanion.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using omega::StallBook;
using omega::StallLiveRow;

static StallLiveRow row(const char* sym, double entry, double upnl) {
    StallLiveRow r;
    r.book = "OMEGA"; r.eng = "XauTrendFollow4h"; r.sym = sym; r.side = "LONG";
    r.entry = entry; r.current = entry + upnl; r.upnl = upnl;
    return r;
}

// last closed row: (reason, realized_pnl); n_closed = data rows in the csv
static bool last_close(const std::string& dir, std::string& reason, double& pnl, int& n) {
    std::ifstream f(dir + "/companion_closed.csv");
    if (!f.is_open()) { n = 0; return false; }
    std::string ln, last; n = 0; bool first = true;
    while (std::getline(f, ln)) { if (first) { first = false; continue; } if (!ln.empty()) { last = ln; ++n; } }
    if (last.empty()) return false;
    std::vector<std::string> c; std::stringstream ss(last); std::string t;
    while (std::getline(ss, t, ',')) c.push_back(t);
    if (c.size() < 10) return false;
    reason = c[2]; pnl = std::atof(c[7].c_str());
    return true;
}

static StallBook mk(const char* name, double bear_cold) {
    StallBook::Config c;
    c.name = name; c.include = {"XauTrendFollow4h"};
    c.arm_usd = 30; c.trail_usd = 15; c.retrig_usd = 15; c.stall_bars = 9999; c.tf_sec = 3600;
    c.cold_loss_omega = -50.0; c.cold_loss_bear = bear_cold;
    c.dir = std::string("stall/") + name;
    return StallBook(std::move(c));
}

int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++fails; } \
                              else std::printf("ok:   %s\n", msg); } while (0)

int main() {
    int64_t t = 1800000000;  // arbitrary epoch base

    { // 1. bear cut fires at -35 (booked at observed upnl -40)
        auto b = mk("t1_bear", -35.0);
        b.step({row("XAUUSD", 4000, -10)}, 1, 1, t);
        std::string rs; double p; int n;
        CHECK(!last_close(b.config().dir, rs, p, n) && n == 0, "t1: -10 in bear -> no clip");
        b.step({row("XAUUSD", 4000, -40)}, 1, 1, t + 60);
        bool ok = last_close(b.config().dir, rs, p, n);
        CHECK(ok && n == 1 && rs == "LOSS_CUT_CLIP" && p == -40.0, "t1: -40 in bear -> LOSS_CUT_CLIP @ -40");
    }
    { // 2. non-bear: baseline -50 still governs
        auto b = mk("t2_nonbear", -35.0);
        b.step({row("XAUUSD", 4000, -40)}, 1, 0, t);
        std::string rs; double p; int n;
        CHECK(!last_close(b.config().dir, rs, p, n) && n == 0, "t2: -40 non-bear -> no clip");
        b.step({row("XAUUSD", 4000, -55)}, 1, 0, t + 60);
        bool ok = last_close(b.config().dir, rs, p, n);
        CHECK(ok && n == 1 && rs == "LOSS_CUT_CLIP" && p == -55.0, "t2: -55 non-bear -> LOSS_CUT_CLIP @ -55");
    }
    { // 3. unknown regime (-1) = fail-safe baseline
        auto b = mk("t3_unknown", -35.0);
        b.step({row("XAUUSD", 4000, -40)}, 1, -1, t);
        std::string rs; double p; int n;
        CHECK(!last_close(b.config().dir, rs, p, n) && n == 0, "t3: -40 unknown-regime -> no clip (fail-safe)");
    }
    { // 4. cold_loss_bear=0 disabled
        auto b = mk("t4_disabled", 0.0);
        b.step({row("XAUUSD", 4000, -40)}, 1, 1, t);
        std::string rs; double p; int n;
        CHECK(!last_close(b.config().dir, rs, p, n) && n == 0, "t4: bear-cold disabled -> -40 no clip");
    }
    { // 5. non-gold leg unaffected by the gold bear cut
        auto b = mk("t5_nongold", -35.0);
        StallBook::Config c2;  // include filter must admit the index sym
        auto b2 = [&]{ StallBook::Config c;
            c.name = "t5_nongold2"; c.include = {"US500"};
            c.arm_usd = 30; c.trail_usd = 15; c.retrig_usd = 15; c.stall_bars = 9999; c.tf_sec = 3600;
            c.cold_loss_omega = -50.0; c.cold_loss_bear = -35.0; c.dir = "stall/t5_nongold2";
            return StallBook(std::move(c)); }();
        b2.step({row("US500", 6000, -40)}, 1, 1, t);
        std::string rs; double p; int n;
        CHECK(!last_close("stall/t5_nongold2", rs, p, n) && n == 0, "t5: -40 non-gold in bear -> no clip");
        b2.step({row("US500", 6000, -55)}, 1, 1, t + 60);
        bool ok = last_close("stall/t5_nongold2", rs, p, n);
        CHECK(ok && n == 1 && rs == "LOSS_CUT_CLIP" && p == -55.0, "t5: -55 non-gold -> baseline LOSS_CUT_CLIP");
        (void)b; (void)c2;
    }
    { // 6. trail path untouched in bear: arm at +40, giveback to +20 -> REVERSAL_CLIP +20
        auto b = mk("t6_trail", -35.0);
        b.step({row("XAUUSD", 4000, +40)}, 1, 1, t);
        b.step({row("XAUUSD", 4000, +20)}, 1, 1, t + 60);
        std::string rs; double p; int n;
        bool ok = last_close(b.config().dir, rs, p, n);
        CHECK(ok && n == 1 && rs == "REVERSAL_CLIP" && p == 20.0, "t6: trail exit +20 unaffected by bear-cold");
    }

    std::printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
