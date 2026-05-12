#pragma once
// =============================================================================
//  engine_protections.hpp -- Standard downside-protection bundle for omega
//                            intraday engines (1m / 5m / 10m / 15m / 30m).
// =============================================================================
//
//  PROVENANCE
//    2026-05-12 S35 -- Initial version. Built in response to the S33/S34
//    intraday-XAU research conclusion that scalping per se does not produce
//    edge, and the resulting need to standardise downside protection across
//    the next generation of XAU intraday engines (HANDOFF_S35.md Phase 4,
//    user instruction "yes do all these and backtest fully").
//
//  PROVIDES
//    A single ProtectedEngineGuards struct that bundles ten protections.
//    Engines hold one of these as a member and call its hooks at the
//    standard points in their lifecycle. The bundle adds zero dependencies
//    on the protected core stack (order_exec / OmegaTradeLedger /
//    trade_lifecycle / omega_main / RiskMonitor / IndexFlowEngine /
//    microscalper_crtp_sweep) and is a header-only opt-in.
//
//  TEN PROTECTIONS
//    (1)  Hard SL multiplier  -- exposed via ProtectionConfig::sl_atr_mult
//                                 (the engine still owns the sl_px assignment
//                                 at fire-entry; the value lives here so all
//                                 engines pick the same default).
//    (2)  Time stop           -- close after N bars held
//                                 (cfg.max_bars_held; 0 disables).
//    (3)  Break-even shift    -- when MFE >= be_trigger_atr * ATR_at_entry,
//                                 move SL to entry +/- be_cost_buffer_pts.
//    (4)  Trailing stop       -- after BE armed, trail SL = mid -/+ trail_mult
//                                 * ATR_at_entry. Only tightens; never loosens.
//    (5)  Daily loss cap      -- disable engine for the rest of the UTC day
//                                 after cumulative net_pnl reaches
//                                 -cfg.daily_loss_limit.
//    (6)  Consec-loss kill    -- after N losses in a row, trip the engine's
//                                 killswitch (shadow_mode for the rest of the
//                                 session). Engine reads st.killswitch_tripped.
//    (7)  Volatility regime   -- ATR floor (no entry when tape is dead) and
//                                 ATR ceiling (no entry when tape is panic).
//    (8)  Spread cap          -- standard guard, included here for uniformity.
//    (9)  Session-window      -- block entries in [start, end) UTC hours;
//                                 wraps midnight if start > end (e.g. 22 -> 8).
//    (10) shadow_mode and enabled remain engine-owned (they gate the engine
//         itself, not individual entries).
//
//  USAGE INSIDE AN ENGINE
//    struct MyEngine {
//        omega::ProtectedEngineGuards guards;
//
//        void init() noexcept {
//            guards.cfg.sl_atr_mult         = 2.0;
//            guards.cfg.max_bars_held       = 4;
//            guards.cfg.be_trigger_atr      = 1.0;
//            guards.cfg.be_cost_buffer_pts  = 0.10;
//            guards.cfg.trail_after_be      = true;
//            guards.cfg.trail_atr_mult      = 0.75;
//            guards.cfg.daily_loss_limit    = 5.0;
//            guards.cfg.max_consec_losses   = 5;
//            guards.cfg.min_atr_floor       = 0.20;
//            guards.cfg.max_atr_ceil        = 30.0;
//            guards.cfg.max_spread          = 1.0;
//            guards.cfg.block_hour_start    = 22;
//            guards.cfg.block_hour_end      = 8;
//            guards.reset_all();
//        }
//
//        void on_30m_bar(const Bar& bar, double bid, double ask,
//                        double atr14, int64_t now_ms, OnCloseFn on_close)
//        {
//            const int64_t now_unix_s = now_ms / 1000;
//            guards.roll_day(now_unix_s);
//            if (pos.active) {
//                guards.on_bar_held();
//                if (guards.time_stop_fired()) {
//                    double exit_px = pos.is_long ? bid : ask;
//                    _close(exit_px, "TIME_STOP", now_ms, on_close);
//                    return;
//                }
//            }
//            if (pos.active) return;
//            if (const char* why = guards.check_entry_ok(bid, ask, atr14,
//                                                       now_unix_s)) {
//                omega::log_entry_block("MyEngine", why);
//                return;
//            }
//            // ... regular signal evaluation + _fire_entry()
//        }
//
//        void on_tick(double bid, double ask, int64_t now_ms,
//                     OnCloseFn on_close)
//        {
//            if (!pos.active) return;
//            const double mid = (bid + ask) * 0.5;
//            guards.update_mfe_mae(pos.is_long, pos.entry_px, mid);
//            pos.sl_px = guards.update_sl(pos.is_long, pos.entry_px,
//                                          pos.sl_px, mid, pos.atr_at_entry);
//            // ... standard SL/TP hit check using pos.sl_px
//        }
//
//        void _close(double exit_px, const char* reason,
//                    int64_t now_ms, OnCloseFn on_close)
//        {
//            const double pts_move = pos.is_long
//                ? (exit_px - pos.entry_px)
//                : (pos.entry_px - exit_px);
//            const double pnl_usd  = pts_move * lot
//                                    * tick_value_multiplier(symbol);
//            const bool was_armed_before = guards.st.killswitch_tripped;
//            guards.on_close(pnl_usd);
//            if (!was_armed_before && guards.st.killswitch_tripped) {
//                omega::log_killswitch("MyEngine",
//                                       guards.st.consec_losses,
//                                       guards.st.daily_pnl_usd);
//            }
//            guards.reset_per_trade();
//            // ... write TradeRecord using guards.st.mfe_pts / mae_pts
//        }
//    };
//
//  PROTECTED-CODE INVARIANT
//    This header touches NO protected file. Engines opt in by adding a
//    ProtectedEngineGuards member and calling its hooks. The protected
//    ledger / order_exec / trade_lifecycle / RiskMonitor stack is untouched.
//    No new globals, no new external dependencies, no new locks.
//
//  CONCURRENCY
//    A ProtectedEngineGuards instance is owned by exactly one engine and
//    is touched only from that engine's on_bar / on_tick / _close path,
//    which the rest of the codebase already guarantees is single-threaded
//    per engine. No internal mutex needed.
// =============================================================================

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace omega {

// ----------------------------------------------------------------------------
//  ProtectionConfig
//    Knobs settable once per engine in init(). Any field at its disabled-value
//    (0 for most numeric, -1 for hours) turns the corresponding protection
//    off, so an engine can opt in selectively.
// ----------------------------------------------------------------------------
struct ProtectionConfig {
    // (1) Hard SL multiplier. Engine reads this when computing sl_px at entry.
    //     Default 2.0 matches XauThreeBar30m and the S33 Pass-8 sweep optimum.
    double sl_atr_mult         = 2.0;

    // (2) Time stop. Close a trade that has been open >= max_bars_held bars
    //     without hitting TP. 0 disables. Suggested by timeframe:
    //       1m -> 30 bars  (30 min cap)
    //       5m -> 12 bars  ( 1 h  cap)
    //      10m ->  9 bars  (90 min cap)
    //      15m ->  8 bars  ( 2 h  cap)
    //      30m ->  4 bars  ( 2 h  cap)
    int    max_bars_held       = 0;

    // (3) Break-even shift. Arm BE when MFE in points >= be_trigger_atr
    //     * ATR_at_entry. New SL is moved to entry +/- be_cost_buffer_pts.
    //     be_cost_buffer_pts > 0 means BE locks in a tiny win covering
    //     commission/slippage; = 0 means exact breakeven. 0 in be_trigger_atr
    //     disables.
    double be_trigger_atr      = 0.0;
    double be_cost_buffer_pts  = 0.05;

    // (4) Trailing stop after BE armed. Trail SL = mid -/+ trail_atr_mult
    //     * ATR_at_entry. Engine still only TIGHTENS SL, never loosens
    //     (handled internally). trail_after_be=false disables.
    bool   trail_after_be      = false;
    double trail_atr_mult      = 0.0;

    // (5) Daily loss cap in USD (positive number). When cumulative
    //     daily_pnl_usd <= -daily_loss_limit, the engine refuses entries
    //     until the next UTC day rollover. 0.0 disables.
    double daily_loss_limit    = 0.0;

    // (6) Consecutive-loss kill switch. After N losing trades in a row,
    //     trip killswitch_tripped (engine should set shadow_mode=true and
    //     emit [ENGINE-AUTO-SHADOW]). Streak resets on any winning trade.
    //     0 disables.
    int    max_consec_losses   = 0;

    // (7) Volatility regime gates (raw price points). 0.0 disables each.
    double min_atr_floor       = 0.0;
    double max_atr_ceil        = 0.0;

    // (8) Max spread (raw price points). 0.0 disables.
    double max_spread          = 0.0;

    // (9) Session-window block (UTC hours). [start, end). -1 disables.
    //     start <= end : blocked hours are [start, end).
    //     start  > end : wraps midnight -- blocked is [start, 24) U [0, end).
    int    block_hour_start    = -1;
    int    block_hour_end      = -1;
};

// ----------------------------------------------------------------------------
//  ProtectionState
//    Runtime values. Read/written by the bundle's hooks. Engines normally
//    do not write these directly, but may read them (e.g. to surface
//    diagnostic info in their own logs).
// ----------------------------------------------------------------------------
struct ProtectionState {
    // Per-trade (cleared by reset_per_trade())
    bool    be_armed           = false;
    double  mfe_pts            = 0.0;
    double  mae_pts            = 0.0;
    int     bars_held          = 0;

    // Per-day (cleared by roll_day() on UTC date change)
    int64_t day_utc_idx        = -1;
    double  daily_pnl_usd      = 0.0;
    bool    daily_capped       = false;

    // Per-session (cleared by reset_session())
    int     consec_losses      = 0;
    bool    killswitch_tripped = false;
};

// ----------------------------------------------------------------------------
//  ProtectedEngineGuards
//    The bundle. One per engine. Methods are inline + noexcept; the whole
//    struct is move/copy-default.
// ----------------------------------------------------------------------------
struct ProtectedEngineGuards {
    ProtectionConfig cfg{};
    ProtectionState  st{};

    // Full reset (idempotent). Call from engine init() and on hard restart.
    void reset_all() noexcept {
        st = ProtectionState{};
    }

    // Clear per-trade state. Engine calls this after _close() finishes
    // writing the TradeRecord (so the next entry starts fresh).
    void reset_per_trade() noexcept {
        st.be_armed  = false;
        st.mfe_pts   = 0.0;
        st.mae_pts   = 0.0;
        st.bars_held = 0;
    }

    // Reset per-session counters (operator review / start of trading day
    // outside the UTC rollover model). Useful for tests and forced restarts.
    void reset_session() noexcept {
        st.consec_losses      = 0;
        st.killswitch_tripped = false;
    }

    // Engine increments bars_held when a new bar closes while the position
    // is still open. Called from on_<TF>_bar before the time-stop check.
    void on_bar_held() noexcept {
        ++st.bars_held;
    }

    // Roll the UTC-day index. Call at the start of each bar/tick. On day
    // change, clears daily_pnl_usd and daily_capped.
    void roll_day(int64_t now_unix_s) noexcept {
        constexpr int64_t SECS_PER_DAY = 86400;
        const int64_t today = now_unix_s / SECS_PER_DAY;
        if (st.day_utc_idx == -1) {
            st.day_utc_idx = today;
            return;
        }
        if (today != st.day_utc_idx) {
            st.day_utc_idx   = today;
            st.daily_pnl_usd = 0.0;
            st.daily_capped  = false;
        }
    }

    // Returns nullptr if a fresh entry is allowed, or a short uppercase
    // reason string when a guard fires. Engine logs the reason via
    // log_entry_block() and skips the entry.
    const char* check_entry_ok(double bid, double ask, double atr,
                               int64_t now_unix_s) const noexcept {
        // (8) Spread cap
        if (cfg.max_spread > 0.0 && (ask - bid) > cfg.max_spread)
            return "SPREAD_CAP";

        // (7) ATR regime
        if (cfg.min_atr_floor > 0.0 && atr < cfg.min_atr_floor)
            return "ATR_BELOW_FLOOR";
        if (cfg.max_atr_ceil  > 0.0 && atr > cfg.max_atr_ceil)
            return "ATR_ABOVE_CEIL";

        // (5) Daily cap
        if (st.daily_capped)
            return "DAILY_LOSS_CAP";

        // (6) Killswitch
        if (st.killswitch_tripped)
            return "KILLSWITCH";

        // (9) Session-window block
        if (cfg.block_hour_start >= 0 && cfg.block_hour_end >= 0) {
            const int hour = static_cast<int>((now_unix_s / 3600) % 24);
            bool blocked;
            if (cfg.block_hour_start <= cfg.block_hour_end) {
                blocked = (hour >= cfg.block_hour_start
                        && hour <  cfg.block_hour_end);
            } else {
                // Wraps midnight: e.g. 22 -> 8 blocks [22,24) U [0,8)
                blocked = (hour >= cfg.block_hour_start)
                       || (hour <  cfg.block_hour_end);
            }
            if (blocked) return "SESSION_BLOCK";
        }

        return nullptr;
    }

    // Update MFE/MAE in raw price points. Engine calls every tick while
    // a position is open. mid is the (bid+ask)/2 reference price.
    void update_mfe_mae(bool is_long, double entry_px, double mid) noexcept {
        if (mid <= 0.0 || entry_px <= 0.0) return;
        const double favourable = is_long ? (mid - entry_px)
                                          : (entry_px - mid);
        if (favourable > st.mfe_pts) st.mfe_pts = favourable;
        if (favourable < st.mae_pts) st.mae_pts = favourable;
    }

    // Returns the (possibly tightened) SL price. Engine assigns this to
    // pos.sl_px every tick. Pass atr_at_entry = the ATR snapshot taken at
    // fire-entry time (so trail distance does not drift with live ATR).
    //
    // Logic:
    //   - If !be_armed and MFE >= be_trigger_atr * atr_at_entry, set
    //     be_armed=true and propose BE_SL = entry +/- be_cost_buffer_pts.
    //   - If be_armed and trail_after_be, propose trail_SL = mid -/+
    //     trail_atr_mult * atr_at_entry.
    //   - Only accept a proposal that TIGHTENS the SL (long: new > current;
    //     short: new < current). Never loosen.
    double update_sl(bool is_long, double entry_px, double current_sl_px,
                     double mid, double atr_at_entry) noexcept {
        if (atr_at_entry <= 0.0 || mid <= 0.0 || entry_px <= 0.0)
            return current_sl_px;

        // (3) Break-even arm
        if (!st.be_armed && cfg.be_trigger_atr > 0.0) {
            const double favourable = is_long ? (mid - entry_px)
                                              : (entry_px - mid);
            if (favourable >= cfg.be_trigger_atr * atr_at_entry) {
                st.be_armed = true;
                const double be_sl = is_long
                    ? (entry_px + cfg.be_cost_buffer_pts)
                    : (entry_px - cfg.be_cost_buffer_pts);
                if (is_long  && be_sl > current_sl_px) current_sl_px = be_sl;
                if (!is_long && be_sl < current_sl_px) current_sl_px = be_sl;
            }
        }

        // (4) Trailing stop after BE
        if (st.be_armed && cfg.trail_after_be && cfg.trail_atr_mult > 0.0) {
            const double trail_dist = cfg.trail_atr_mult * atr_at_entry;
            const double trail_sl   = is_long ? (mid - trail_dist)
                                              : (mid + trail_dist);
            if (is_long  && trail_sl > current_sl_px) current_sl_px = trail_sl;
            if (!is_long && trail_sl < current_sl_px) current_sl_px = trail_sl;
        }

        return current_sl_px;
    }

    // (2) Time-stop check. Call after on_bar_held() at each bar close while
    // a position is open. Returns true if the engine should immediately
    // close the trade with reason "TIME_STOP".
    bool time_stop_fired() const noexcept {
        if (cfg.max_bars_held <= 0) return false;
        return st.bars_held >= cfg.max_bars_held;
    }

    // Update bookkeeping after a trade closes. Engine passes the USD net pnl
    // (raw_pts * lot * tick_value_multiplier(symbol)). Updates:
    //   - daily_pnl_usd  += net_pnl_usd
    //   - daily_capped    = (daily_pnl_usd <= -daily_loss_limit)
    //   - consec_losses   += 1 on loss, = 0 on win, unchanged on flat
    //   - killswitch_tripped = (consec_losses >= max_consec_losses)
    // Does NOT clear per-trade state; engine calls reset_per_trade()
    // separately after writing the TradeRecord.
    void on_close(double net_pnl_usd) noexcept {
        st.daily_pnl_usd += net_pnl_usd;
        if (net_pnl_usd < 0.0) {
            ++st.consec_losses;
            if (cfg.max_consec_losses > 0
                && st.consec_losses >= cfg.max_consec_losses) {
                st.killswitch_tripped = true;
            }
        } else if (net_pnl_usd > 0.0) {
            st.consec_losses = 0;
        }
        if (cfg.daily_loss_limit > 0.0
            && st.daily_pnl_usd <= -cfg.daily_loss_limit) {
            st.daily_capped = true;
        }
    }

    // Snapshot the runtime state into a caller-provided buffer for
    // periodic logging. Truncates safely if buf is small.
    void dump_state(char* buf, std::size_t bufsz,
                    const char* engine_name) const noexcept {
        if (!buf || bufsz == 0) return;
        std::snprintf(buf, bufsz,
            "[GUARDS] %s be_armed=%d bars_held=%d mfe=%.4f mae=%.4f "
            "day_pnl=%.2f day_cap=%d consec_loss=%d killsw=%d",
            engine_name,
            static_cast<int>(st.be_armed), st.bars_held,
            st.mfe_pts, st.mae_pts,
            st.daily_pnl_usd, static_cast<int>(st.daily_capped),
            st.consec_losses, static_cast<int>(st.killswitch_tripped));
    }
};

// ----------------------------------------------------------------------------
//  Free helpers for uniform engine logging
// ----------------------------------------------------------------------------

// Emitted when check_entry_ok() returns a reason.
inline void log_entry_block(const char* engine_name,
                            const char* reason) noexcept {
    std::printf("[GUARD-BLOCK] engine=%s reason=%s\n",
                engine_name ? engine_name : "?",
                reason      ? reason      : "?");
    std::fflush(stdout);
}

// Emitted on the trade where consec_losses crosses max_consec_losses. The
// engine should also flip its own shadow_mode = true at this point.
inline void log_killswitch(const char* engine_name,
                           int consec_losses,
                           double daily_pnl_usd) noexcept {
    std::fprintf(stderr,
        "\033[1;31m[ENGINE-AUTO-SHADOW] %s killswitch tripped: "
        "consec_losses=%d day_pnl=%.2f. Auto-shadowing engine for this "
        "session.\033[0m\n",
        engine_name ? engine_name : "?",
        consec_losses, daily_pnl_usd);
    std::fflush(stderr);
}

// Emitted by the engine when a time-stop closes a trade. Engine usually
// just sets exit_reason="TIME_STOP" inside _close(), but a separate marker
// in stdout helps log-greppers correlate.
inline void log_time_stop(const char* engine_name,
                          int bars_held,
                          double mfe_pts,
                          double mae_pts) noexcept {
    std::printf("[GUARD-TIME-STOP] engine=%s bars_held=%d mfe=%.4f mae=%.4f\n",
                engine_name ? engine_name : "?",
                bars_held, mfe_pts, mae_pts);
    std::fflush(stdout);
}

// Emitted by the engine when daily_loss_limit triggers and disables
// further entries for the UTC day.
inline void log_daily_cap(const char* engine_name,
                          double daily_pnl_usd,
                          double daily_loss_limit) noexcept {
    std::fprintf(stderr,
        "\033[1;33m[GUARD-DAILY-CAP] %s day_pnl=%.2f limit=%.2f -- "
        "engine paused until UTC day rollover.\033[0m\n",
        engine_name ? engine_name : "?",
        daily_pnl_usd, daily_loss_limit);
    std::fflush(stderr);
}

} // namespace omega
