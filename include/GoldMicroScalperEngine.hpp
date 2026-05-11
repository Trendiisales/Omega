#pragma once
// =============================================================================
// GoldMicroScalperEngine.hpp -- DISABLED STUB
// =============================================================================
//
// 2026-05-11 S33d (operator-authorised disable, this session):
//
//   The GoldMicroScalperEngine has been DISABLED. The original 952-line
//   implementation has been moved to:
//
//       disabled_engines/GoldMicroScalperEngine.hpp.disabled_2026-05-11
//
//   ...and replaced with this no-op stub. The stub keeps the rest of the
//   build compiling -- tick_gold.hpp's dispatch site, engine_init.hpp's
//   shadow_pin/auto-pin registrations, the open-positions source, the
//   engine heartbeat, and the API listing all still link. But every
//   method on g_gold_microscalper is now a no-op:
//
//       has_open_position()  -> always returns false
//       on_tick(...)         -> returns immediately
//       force_close(...)     -> returns immediately
//       on_fire_hook         -> never invoked (engine never fires)
//       shadow_mode          -> pinned true (cannot be set to live)
//
// Why disabled:
//
//   The S33 Option A geometry (z=2.0, W=200, TP=35, SL=12, Asia-only)
//   was tested 2026-05-11 against the full 31-day XAU L2 capture with
//   realistic $0.06/RT cost using backtest/s33_revised_backtest. Result:
//
//       8,435 trades, 7.09% WR, gross -$731, costs -$506, NET -$1,237
//       over 28 calendar days at 0.01 lot. Every single trading day in
//       the red. Sharpe -111.7.
//
//   The geometry needs WR >= 25.5% just to break even on points before
//   costs (SL/(SL+TP)=12/47). Actual is 7%. Gap is structural, not
//   parameter-tunable.
//
//   The wider 500-cell L2 sweep (6 signal families x 4 sessions x 4
//   symbols) found ZERO positive cells with n>=30 trades at realistic
//   cost. The conventional short-horizon microstructure thesis does
//   not have an edge on this broker's L2 feed.
//
//   The multi-timeframe sweep (multi_tf_sweep.cpp, 5m/10m/15m/1h/4h
//   bars across L2 31 days + Duka 623 days) DID find cross-validated
//   positive cells, but they are trend-following at 10m-1h bar
//   timeframes (MA crossover, Donchian breakout, momentum). Those are
//   a different strategy class than this engine implements and warrant
//   a new engine class -- NOT a re-tune of this one.
//
// How to re-enable (if a future session is authorised to re-enable
// the literal micro-scalp logic):
//
//       1. Move disabled_engines/GoldMicroScalperEngine.hpp.disabled_*
//          back to include/GoldMicroScalperEngine.hpp.
//       2. Confirm engine_init.hpp pin still sets shadow_mode=true.
//       3. Operator sign-off required (rule 4 of S33 §8 invariants).
//
// What is intentionally NOT changed by this disable:
//     - VPS deploy. After git pull + rebuild, the engine will simply
//       not fire. The dispatch in tick_gold.hpp still runs but every
//       method call no-ops. No risk of accidental LIVE fills.
//     - mode=SHADOW config gate (still in place via order_exec).
//     - KILL_MICROSCALPER sentinel on VPS (leave it; cheap belt+braces).
//     - RiskMonitor, OmegaTradeLedger, trade_lifecycle, omega_main
//       (all protected files, all untouched).
//     - engine_init.hpp registrations of "MicroScalperGold" in heartbeat,
//       open-positions, API engines list. These all still resolve to
//       this stub's no-op methods and report a permanently-quiet engine.
// =============================================================================

#include <cstdint>
#include <functional>
#include <string>

class GoldMicroScalperEngine {
public:
    // Pinned-true: this stub cannot be flipped live. Setters appearing in
    // engine_init.hpp (e.g. `g_gold_microscalper.shadow_mode = false;`) still
    // compile, but a subsequent on_tick() / has_open_position() / force_close()
    // is a no-op so the value has no effect.
    bool shadow_mode = true;

    // Same shape as the original on_fire_hook. Never invoked by the stub.
    std::function<void(int64_t now_s)> on_fire_hook;

    // Same shape as the original `pos` substruct. All fields default to
    // their "no position" values. Other modules read these (especially
    // engine_init.hpp's open-positions source); reading from this stub
    // always reports "no open position".
    struct Pos {
        bool        active             = false;
        bool        is_long            = false;
        double      entry              = 0.0;
        double      tp                 = 0.0;
        double      sl                 = 0.0;
        double      size               = 0.0;
        double      mfe                = 0.0;
        double      mae                = 0.0;
        double      spread_at_entry    = 0.0;
        int64_t     entry_ts           = 0;
        bool        be_locked          = false;
        bool        l2_real_at_entry   = false;
        double      z_at_entry         = 0.0;
        std::string broker_position_id;
        std::string entry_clOrdId;
        std::string close_clOrdId;
    } pos;

    GoldMicroScalperEngine() noexcept = default;

    // Always reports "no open position". The dispatch in tick_gold.hpp
    // gates on this and short-circuits the manage-and-entry block.
    bool has_open_position() const noexcept { return false; }

    // Signature kept identical to the original engine so the call site in
    // tick_gold.hpp continues to compile. All parameters are intentionally
    // silenced with (void)x; the body is a hard return.
    template <typename CloseFn>
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 CloseFn on_close,
                 double l2_imbalance,
                 double slope,
                 double vacuum_ask,
                 double vacuum_bid,
                 bool l2_real) noexcept {
        (void)bid; (void)ask; (void)now_ms; (void)can_enter; (void)on_close;
        (void)l2_imbalance; (void)slope; (void)vacuum_ask; (void)vacuum_bid;
        (void)l2_real;
        // DISABLED: see header comment block. No entry, no manage, no exit.
        return;
    }

    // Signature parity for any external force_close caller.
    template <typename CloseFn>
    void force_close(double bid, double ask, int64_t now_ms,
                     CloseFn on_close,
                     const char* reason) noexcept {
        (void)bid; (void)ask; (void)now_ms; (void)on_close; (void)reason;
        return;
    }
};
