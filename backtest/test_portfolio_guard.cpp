// =============================================================================
// test_portfolio_guard.cpp -- unit test for PortfolioGuard helpers.
//
// Exercises:
//   1. Concurrency cap honours max_concurrent_positions
//   2. Vol-scaled lot returns expected values + clamps
//   3. HTF scalar tilts up/down correctly, neutral when disabled
//   4. Kill-file detection toggles state correctly
//
// Build:
//   clang++ -O2 -std=c++17 -I include backtest/test_portfolio_guard.cpp -o backtest/test_portfolio_guard
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <fstream>
#include <string>

#include "PortfolioGuard.hpp"

using namespace omega::pg;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s)\n", msg, #cond); ++g_failures; } \
    else         { std::fprintf(stderr, "OK  : %s\n", msg); } \
} while(0)

static void reset_state() {
    g_pg_state.concurrent_positions.store(0);
    g_pg_state.kill_switch_active.store(false);
    g_pg_state.last_kill_check_ms.store(0);
    g_pg_state.n_blocked_concurrency.store(0);
    g_pg_state.n_blocked_kill_file.store(0);
    g_pg_state.n_size_scaled_up.store(0);
    g_pg_state.n_size_scaled_down.store(0);
}

int main() {
    std::fprintf(stderr, "=== PortfolioGuard unit tests ===\n\n");

    // ---- 1. Concurrency cap ------------------------------------------------
    reset_state();
    g_pg_cfg.max_concurrent_positions = 3;
    g_pg_cfg.kill_file_enabled = false;
    g_pg_cfg.vol_scale_enabled = false;
    g_pg_cfg.htf_scalar_enabled = false;

    CHECK(can_open_new_position(), "concurrency: cap=3, current=0 -> allow");
    register_position_open();
    register_position_open();
    register_position_open();
    CHECK(!can_open_new_position(), "concurrency: cap=3, current=3 -> block");
    CHECK(g_pg_state.n_blocked_concurrency.load() == 1, "concurrency: blocked counter incremented");
    register_position_close();
    CHECK(can_open_new_position(), "concurrency: cap=3, current=2 -> allow after close");

    // Underflow protection
    reset_state();
    register_position_close();  // close with zero open shouldn't go negative
    CHECK(g_pg_state.concurrent_positions.load() == 0, "concurrency: no negative count");

    // Disabled cap (0)
    reset_state();
    g_pg_cfg.max_concurrent_positions = 0;
    for (int i = 0; i < 100; ++i) register_position_open();
    CHECK(can_open_new_position(), "concurrency: cap=0 -> always allow");

    // ---- 2. Vol-scaled lot --------------------------------------------------
    reset_state();
    g_pg_cfg.max_concurrent_positions = 4;
    g_pg_cfg.vol_scale_enabled = true;
    g_pg_cfg.risk_dollars = 50.0;
    g_pg_cfg.atr_floor_pts = 0.5;
    g_pg_cfg.lot_min = 0.01;
    g_pg_cfg.lot_max = 0.10;

    // Gold: ATR=$3, $100/pt at 1.0 lot. risk $50 / (3 * 100) = 0.167 -> clamped to 0.10
    double lot = vol_scaled_lot(3.0, 100.0);
    CHECK(std::abs(lot - 0.10) < 1e-9, "vol_scale: ATR=3 gold -> clamp to lot_max");

    // ATR=$10 = high vol -> $50 / (10*100) = 0.05 (within range)
    lot = vol_scaled_lot(10.0, 100.0);
    CHECK(std::abs(lot - 0.05) < 1e-9, "vol_scale: ATR=10 -> 0.05 lot");

    // ATR=$50 (extreme) -> $50 / (50*100) = 0.01 -> at lot_min
    lot = vol_scaled_lot(50.0, 100.0);
    CHECK(std::abs(lot - 0.01) < 1e-9, "vol_scale: ATR=50 -> clamp to lot_min");

    // ATR floor: ATR=0.1 should clamp to atr_floor=0.5, lot = $50 / (0.5*100) = 1.0 -> clamp to lot_max 0.10
    lot = vol_scaled_lot(0.1, 100.0);
    CHECK(std::abs(lot - 0.10) < 1e-9, "vol_scale: atr_floor enforced");

    // Disabled
    g_pg_cfg.vol_scale_enabled = false;
    lot = vol_scaled_lot(3.0, 100.0);
    CHECK(lot == 0.0, "vol_scale: disabled returns 0 (caller falls back)");

    // ---- 3. HTF scalar ------------------------------------------------------
    reset_state();
    g_pg_cfg.htf_scalar_enabled = true;
    g_pg_cfg.htf_align_scalar = 1.5;
    g_pg_cfg.htf_oppose_scalar = 0.5;
    g_pg_cfg.htf_neutral_scalar = 1.0;

    CHECK(std::abs(htf_size_scalar(+1, true)  - 1.5) < 1e-9, "htf: BULL+LONG  -> 1.5 (align up)");
    CHECK(std::abs(htf_size_scalar(-1, false) - 1.5) < 1e-9, "htf: BEAR+SHORT -> 1.5 (align up)");
    CHECK(std::abs(htf_size_scalar(+1, false) - 0.5) < 1e-9, "htf: BULL+SHORT -> 0.5 (oppose)");
    CHECK(std::abs(htf_size_scalar(-1, true)  - 0.5) < 1e-9, "htf: BEAR+LONG  -> 0.5 (oppose)");
    CHECK(std::abs(htf_size_scalar(0,  true)  - 1.0) < 1e-9, "htf: NEUTRAL    -> 1.0");

    // Disabled
    g_pg_cfg.htf_scalar_enabled = false;
    CHECK(std::abs(htf_size_scalar(+1, false) - 1.0) < 1e-9, "htf: disabled -> always 1.0");

    // ---- 4. Kill-file -------------------------------------------------------
    reset_state();
    g_pg_cfg.htf_scalar_enabled = false;
    g_pg_cfg.kill_file_enabled = true;
    g_pg_cfg.kill_file_path = "/tmp/test_omega_kill_switch.lock";
    g_pg_cfg.kill_file_recheck_sec = 0;  // recheck every tick for test
    g_pg_cfg.max_concurrent_positions = 100;  // not the gate under test

    // No file -> not active
    std::remove(g_pg_cfg.kill_file_path);
    refresh_kill_switch(1000);
    CHECK(!g_pg_state.kill_switch_active.load(), "kill: no file -> inactive");
    CHECK(can_open_new_position(), "kill: no file -> allow open");

    // File present -> active
    { std::ofstream f(g_pg_cfg.kill_file_path); f << "DD_BREACH\n"; }
    refresh_kill_switch(2000);
    CHECK(g_pg_state.kill_switch_active.load(), "kill: file exists -> active");
    CHECK(!can_open_new_position(), "kill: file exists -> block open");
    CHECK(g_pg_state.n_blocked_kill_file.load() == 1, "kill: blocked counter incremented");

    // File deleted -> inactive again
    std::remove(g_pg_cfg.kill_file_path);
    refresh_kill_switch(3000);
    CHECK(!g_pg_state.kill_switch_active.load(), "kill: file removed -> inactive");
    CHECK(can_open_new_position(), "kill: file removed -> allow again");

    // ---- Summary ------------------------------------------------------------
    std::fprintf(stderr, "\n=== %s: %d failures ===\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
