// position_persist_test.cpp — round-trip correctness test for open-position
// persistence (S-2026-06-03). Proves: a Portfolio's open positions serialize to
// disk and restore into a FRESH Portfolio with identical side/entry/sl/tp/ts.
// This is the functional validation for the persist/resume infra (not an edge
// backtest — persistence changes restart behavior, not signals).
//
// build: g++ -std=c++17 -O2 -Iinclude backtest/position_persist_test.cpp -o backtest/position_persist_test

#include "SurvivorPortfolio.hpp"
#include <cstdio>
#include <vector>

// Local mirror of engine_init's Survivor source + restorer, bound to a given
// portfolio (so the test doesn't need the global engine graph).
static void wire(omega::OpenPositionRegistry& reg, omega::survivor::Portfolio& p) {
    reg.register_source("Survivor", [&p]() {
        std::vector<omega::PositionSnapshot> out;
        for (const auto& c : p.cells) {
            if (!c.st.pos_active) continue;
            omega::PositionSnapshot ps;
            ps.symbol = c.cfg.symbol;
            ps.side   = (c.st.pos_side > 0) ? "LONG" : "SHORT";
            ps.size   = c.cfg.lot;
            ps.entry  = c.st.pos_entry;
            ps.sl     = c.st.pos_sl;
            ps.tp     = c.st.pos_tp;
            ps.entry_ts = c.st.pos_entry_ts;
            ps.engine = c.cfg.tag;
            out.push_back(ps);
        }
        return out;
    });
    reg.register_restorer([&p](const omega::PositionSnapshot& ps) { return p.adopt(ps); });
}

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

int main() {
    const std::string path = "/tmp/omega_pos_persist_test.dat";

    // ---- Portfolio A: open 3 positions across 2 symbols + 1 same-symbol pair ----
    omega::OpenPositionRegistry regA;
    omega::survivor::Portfolio pA; pA.init_default_cells();
    wire(regA, pA);

    auto open_in = [&](const char* tag, int side, double entry, double sl, double tp, long long ts) {
        for (auto& c : pA.cells) if (std::string(c.cfg.tag) == tag) {
            c.st.pos_active = true; c.st.pos_side = side;
            c.st.pos_entry = entry; c.st.pos_sl = sl; c.st.pos_tp = tp; c.st.pos_entry_ts = ts;
            return;
        }
        std::printf("  [WARN] tag %s not found in default cells\n", tag);
    };
    open_in("XAU_4h_DonchN20",  -1, 4484.51, 4562.38, 4328.77, 1780000000);
    open_in("XAU_4h_DonchN100", -1, 4484.51, 4562.38, 4328.77, 1780000000);
    open_in("USTEC_4h_ZMR",     +1, 16229.11, 16100.0, 16480.0, 1780001234);

    int saved = regA.save(path);
    std::printf("saved %d positions -> %s\n", saved, path.c_str());
    check(saved == 3, "3 open positions serialized");

    // ---- Portfolio B (fresh, simulates a restart): restore ----
    omega::OpenPositionRegistry regB;
    omega::survivor::Portfolio pB; pB.init_default_cells();
    wire(regB, pB);

    // sanity: B starts flat
    int b_open_before = 0; for (auto& c : pB.cells) if (c.st.pos_active) b_open_before++;
    check(b_open_before == 0, "fresh portfolio starts flat (positions are RAM-only)");

    auto rr = regB.restore(path);
    std::printf("restore: adopted %d / seen %d\n", rr.first, rr.second);
    check(rr.first == 3 && rr.second == 3, "all 3 positions adopted on restore");

    // ---- verify field-level fidelity ----
    auto findB = [&](const char* tag) -> omega::survivor::Cell* {
        for (auto& c : pB.cells) if (std::string(c.cfg.tag) == tag) return &c;
        return nullptr;
    };
    auto* d20  = findB("XAU_4h_DonchN20");
    auto* d100 = findB("XAU_4h_DonchN100");
    auto* zmr  = findB("USTEC_4h_ZMR");
    check(d20 && d20->st.pos_active && d20->st.pos_side == -1 &&
          d20->st.pos_entry == 4484.51 && d20->st.pos_sl == 4562.38 &&
          d20->st.pos_tp == 4328.77 && d20->st.pos_entry_ts == 1780000000,
          "DonchN20 SHORT restored entry/sl/tp/ts exact");
    check(d100 && d100->st.pos_active && d100->st.pos_side == -1 &&
          d100->st.pos_entry == 4484.51,
          "DonchN100 (same-symbol sibling) restored independently");
    check(zmr && zmr->st.pos_active && zmr->st.pos_side == +1 &&
          zmr->st.pos_entry == 16229.11 && zmr->st.pos_entry_ts == 1780001234,
          "USTEC_4h_ZMR LONG restored entry/ts exact");

    // ---- idempotency: restoring again must NOT double-adopt ----
    auto rr2 = regB.restore(path);
    check(rr2.first == 3, "re-restore is idempotent (already-open cells not doubled)");

    // ---- absent file = clean no-op ----
    auto rr3 = regB.restore("/tmp/omega_pos_persist_test_NOEXIST.dat");
    check(rr3.first == 0 && rr3.second == 0, "absent persist file = clean {0,0}");

    std::printf("\n%s (%d failures)\n", fails == 0 ? "ALL PASS" : "FAILURES", fails);
    return fails == 0 ? 0 : 1;
}
