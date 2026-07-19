// =============================================================================
// catastrophic_guard_selftest.cpp — sandbox FIRES-ON-BREACH proof for the LIVE
// CatastrophicGuard (include/CatastrophicGuard.hpp), the universal in-flight
// catastrophe net wired to the KILL-ALL flatten path in S-2026-07-20k.
//
// WHY (S-2026-07-20i audit item 5 / standing rule): the guard's ARMED boot line
// was verified in production, but the BREACH branch (loss > catastrophe_x *
// per_trade_usd -> flatten hook fires) had never executed anywhere. A rare-event
// protection must prove its trigger path in a sandbox, not just its arming.
// This harness compiles the ACTUAL shipped header and drives check() through
// every branch:
//   A) SHADOW breach     -> counted, NO flatten call (engine SL must run)
//   B) LIVE breach       -> UNIVERSAL flatten invoked with the right snapshot
//   C) per-engine hook   -> takes precedence over universal
//   D) inside noise band -> untouched (loss < threshold books nothing)
//   E) unknown pnl (0)   -> skipped, never flattened
//   F) NO-CLOSER path    -> universal returning false still counted (loud log)
//
// Usage: catastrophic_guard_selftest    (exit 0 = all branches proven)
// Compiled+run by protection_selftest.py check [10]; NOT part of Omega.exe.
// =============================================================================
#include "../include/CatastrophicGuard.hpp"
#include <cstdio>
#include <string>
#include <vector>

// Normally defined once in include/globals.hpp (main.cpp's TU); this harness
// is its own binary so it owns the definition.
omega::OpenPositionRegistry g_open_positions;

using omega::CatastrophicGuard;
using omega::PositionSnapshot;

static std::vector<PositionSnapshot> g_positions;   // what the source serves

static PositionSnapshot pos(double unr) {
    PositionSnapshot p;
    p.symbol = "TESTSYM"; p.engine = "SelfTest"; p.side = "LONG";
    p.size = 1.0; p.entry = 100.0; p.current = 90.0; p.unrealized_pnl = unr;
    return p;
}

int main() {
    int fails = 0;
    int64_t now = 1000;
    g_open_positions.register_source("selftest", []{ return g_positions; });

    auto expect = [&](const char* name, bool ok) {
        std::printf("[%s] %s\n", name, ok ? "ok" : "FAIL");
        if (!ok) fails++;
    };

    // guard threshold: 3 x $50 = $150
    // ── A) SHADOW breach: counted, flatten hooks must NOT fire
    {
        CatastrophicGuard g;
        g.per_trade_usd = 50.0; g.catastrophe_x = 3.0; g.live = false;
        int uni = 0;
        g.universal_flatten = [&](const PositionSnapshot&){ uni++; return true; };
        g_positions = { pos(-200.0) };
        const int hit = g.check(now += 60);
        expect("A shadow breach counted, no flatten", hit == 1 && uni == 0);
    }

    // ── B) LIVE breach: universal flatten fires with the breached snapshot
    {
        CatastrophicGuard g;
        g.per_trade_usd = 50.0; g.catastrophe_x = 3.0; g.live = true;
        int uni = 0; std::string seen;
        g.universal_flatten = [&](const PositionSnapshot& p){ uni++; seen = p.symbol + "|" + p.engine; return true; };
        g_positions = { pos(-200.0) };
        const int hit = g.check(now += 60);
        expect("B live breach -> universal flatten", hit == 1 && uni == 1 && seen == "TESTSYM|SelfTest");
    }

    // ── C) per-engine hook takes precedence over universal
    {
        CatastrophicGuard g;
        g.per_trade_usd = 50.0; g.catastrophe_x = 3.0; g.live = true;
        int uni = 0, eng = 0;
        g.universal_flatten = [&](const PositionSnapshot&){ uni++; return true; };
        g.register_flatten("TESTSYM|SelfTest", [&]{ eng++; });
        g_positions = { pos(-200.0) };
        const int hit = g.check(now += 60);
        expect("C per-engine precedence", hit == 1 && eng == 1 && uni == 0);
    }

    // ── D) inside noise band: loss above -threshold books nothing
    {
        CatastrophicGuard g;
        g.per_trade_usd = 50.0; g.catastrophe_x = 3.0; g.live = true;
        int uni = 0;
        g.universal_flatten = [&](const PositionSnapshot&){ uni++; return true; };
        g_positions = { pos(-100.0) };
        const int hit = g.check(now += 60);
        expect("D noise band untouched", hit == 0 && uni == 0);
    }

    // ── E) unknown pnl (0) skipped
    {
        CatastrophicGuard g;
        g.per_trade_usd = 50.0; g.catastrophe_x = 3.0; g.live = true;
        int uni = 0;
        g.universal_flatten = [&](const PositionSnapshot&){ uni++; return true; };
        g_positions = { pos(0.0) };
        const int hit = g.check(now += 60);
        expect("E unknown pnl skipped", hit == 0 && uni == 0);
    }

    // ── F) NO-CLOSER: universal returns false -> still counted + attempted
    {
        CatastrophicGuard g;
        g.per_trade_usd = 50.0; g.catastrophe_x = 3.0; g.live = true;
        int uni = 0;
        g.universal_flatten = [&](const PositionSnapshot&){ uni++; return false; };
        g_positions = { pos(-200.0) };
        const int hit = g.check(now += 60);
        expect("F no-closer still counted", hit == 1 && uni == 1);
    }

    std::printf("CATGUARD-SELFTEST %s (%d fail)\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
