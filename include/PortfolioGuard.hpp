#pragma once
// =============================================================================
// PortfolioGuard.hpp -- portfolio-level safety + sizing helpers (S48)
// =============================================================================
//
// Provides four overlays the trend zoo lacked:
//   1. Concurrency cap         -- max N simultaneous open positions across all
//                                 gold engines. Prevents 11 longs stacking.
//   2. Vol-scaled lot sizing   -- lot = risk_dollars / (atr * $/pt). Consistent
//                                 dollar-risk regardless of vol regime.
//   3. HTF bias size scalar    -- multiplier 0.5-1.5x on lot based on HTF bias
//                                 vs trade direction. Soft tilt, NOT hard gate
//                                 (S44 lesson: hard HTF gate destroyed -38% edge).
//   4. Kill-file circuit       -- if C:/Omega/KILL_SWITCH.lock exists, refuse
//                                 all new entries. Operator manually deletes
//                                 file to resume after reviewing the breach.
//
// All four are designed to be NO-OP defaults when guards disabled. Existing
// engines pass through unchanged unless they opt-in.
//
// Threading: all state is atomic. State is process-local (resets on restart) —
// kill-file is the only persistent layer.
//
// HISTORY:
//   2026-05-27 S48 (post-S47 scalp purge): operator-directive bundle of
//   improvements identified from the day's audit. Tested via the standalone
//   test_portfolio_guard.cpp harness before shipping.
// =============================================================================

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>

namespace omega { namespace pg {

// ---- Config (tunable) -------------------------------------------------------

struct Config {
    // Concurrency cap: max simultaneously-open positions across all gold
    // engines. 0 = disabled. Default 4 (sized to roughly 1/3 of the 11-engine
    // zoo so multiple engines can fire on a trend without all of them stacking).
    int max_concurrent_positions = 4;

    // Vol-scaled lot sizing
    bool   vol_scale_enabled   = false;   // opt-in per engine
    double risk_dollars        = 50.0;    // target dollar risk per trade
    double atr_floor_pts       = 0.5;     // minimum ATR for divide-by-zero safety
    double lot_min             = 0.01;
    double lot_max             = 0.10;

    // HTF size scalar (soft tilt — never zero, never hard-block)
    bool   htf_scalar_enabled  = false;   // opt-in
    double htf_align_scalar    = 1.50;    // longs when HTF=BULL, shorts when HTF=BEAR
    double htf_oppose_scalar   = 0.50;    // longs when HTF=BEAR, shorts when HTF=BULL
    double htf_neutral_scalar  = 1.00;

    // Kill-file circuit
    bool        kill_file_enabled = true;
    const char* kill_file_path    = "C:/Omega/KILL_SWITCH.lock";
    int         kill_file_recheck_sec = 30;
};

static Config g_pg_cfg;  // mutate from engine_init.hpp

// ---- Runtime state (atomics — safe across threads) --------------------------

struct State {
    std::atomic<int> concurrent_positions{0};
    std::atomic<bool> kill_switch_active{false};
    std::atomic<int64_t> last_kill_check_ms{0};

    // For diagnostics:
    std::atomic<int64_t> n_blocked_concurrency{0};
    std::atomic<int64_t> n_blocked_kill_file{0};
    std::atomic<int64_t> n_size_scaled_up{0};
    std::atomic<int64_t> n_size_scaled_down{0};
};

static State g_pg_state;

// ---- Helpers ----------------------------------------------------------------

inline int64_t _pg_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Refresh kill-switch state by stat'ing the kill file. Rate-limited to once
// per `kill_file_recheck_sec` to avoid hammering the filesystem on every tick.
inline void refresh_kill_switch(int64_t now_ms) noexcept {
    if (!g_pg_cfg.kill_file_enabled) return;
    const int64_t last = g_pg_state.last_kill_check_ms.load(std::memory_order_relaxed);
    if (now_ms - last < g_pg_cfg.kill_file_recheck_sec * 1000) return;
    g_pg_state.last_kill_check_ms.store(now_ms, std::memory_order_relaxed);
    std::ifstream f(g_pg_cfg.kill_file_path);
    const bool active = f.good();
    const bool was = g_pg_state.kill_switch_active.exchange(active);
    if (active != was) {
        std::printf("[PG] kill_switch %s (file=%s)\n",
                    active ? "ENGAGED" : "CLEARED", g_pg_cfg.kill_file_path);
        std::fflush(stdout);
    }
}

// Returns true if a new position may open right now. Side-effect free.
inline bool can_open_new_position() noexcept {
    if (g_pg_state.kill_switch_active.load(std::memory_order_relaxed)) {
        g_pg_state.n_blocked_kill_file.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (g_pg_cfg.max_concurrent_positions > 0
        && g_pg_state.concurrent_positions.load(std::memory_order_relaxed)
           >= g_pg_cfg.max_concurrent_positions) {
        g_pg_state.n_blocked_concurrency.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

inline void register_position_open() noexcept {
    g_pg_state.concurrent_positions.fetch_add(1, std::memory_order_relaxed);
}

inline void register_position_close() noexcept {
    int prev = g_pg_state.concurrent_positions.fetch_sub(1, std::memory_order_relaxed);
    if (prev <= 0) {  // never go negative
        g_pg_state.concurrent_positions.store(0, std::memory_order_relaxed);
    }
}

// Vol-scaled lot: target a fixed dollar risk per trade.
// risk_dollars / (atr_pts * usd_per_pt) — clamped to [lot_min, lot_max].
inline double vol_scaled_lot(double atr_pts, double usd_per_pt) noexcept {
    if (!g_pg_cfg.vol_scale_enabled) return 0.0;  // 0 = signal to caller to use its own
    const double atr = std::max(atr_pts, g_pg_cfg.atr_floor_pts);
    const double raw = g_pg_cfg.risk_dollars / (atr * usd_per_pt);
    if (raw < g_pg_cfg.lot_min) return g_pg_cfg.lot_min;
    if (raw > g_pg_cfg.lot_max) return g_pg_cfg.lot_max;
    return raw;
}

// HTF size scalar — pass +1 for BULL bias, -1 for BEAR, 0 for NEUTRAL or off.
// Returns multiplier that callers apply to their own lot.
inline double htf_size_scalar(int htf_bias, bool trade_is_long) noexcept {
    if (!g_pg_cfg.htf_scalar_enabled || htf_bias == 0) {
        return g_pg_cfg.htf_neutral_scalar;
    }
    const bool aligned = (htf_bias > 0 && trade_is_long)
                       || (htf_bias < 0 && !trade_is_long);
    if (aligned) {
        g_pg_state.n_size_scaled_up.fetch_add(1, std::memory_order_relaxed);
        return g_pg_cfg.htf_align_scalar;
    } else {
        g_pg_state.n_size_scaled_down.fetch_add(1, std::memory_order_relaxed);
        return g_pg_cfg.htf_oppose_scalar;
    }
}

// Diagnostic print — call from quote_loop stats block every 60s or so.
inline void print_stats() noexcept {
    std::printf("[PG-STATS] concurrent=%d/%d  blocked_conc=%lld  blocked_kill=%lld"
                "  scaled_up=%lld  scaled_down=%lld  kill_active=%d\n",
                g_pg_state.concurrent_positions.load(),
                g_pg_cfg.max_concurrent_positions,
                (long long)g_pg_state.n_blocked_concurrency.load(),
                (long long)g_pg_state.n_blocked_kill_file.load(),
                (long long)g_pg_state.n_size_scaled_up.load(),
                (long long)g_pg_state.n_size_scaled_down.load(),
                (int)g_pg_state.kill_switch_active.load());
    std::fflush(stdout);
}

}} // namespace omega::pg
