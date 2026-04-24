#pragma once
// ==============================================================================
// BracketEngine -- CRTP true-bracket breakout engine
//
// TRUE BRACKET behaviour:
//   Once a structural range is locked, BOTH sides are armed simultaneously.
//   The engine emits TWO pending signals (long above high, short below low).
//   main.cpp sends BOTH stop orders to the broker.
//   Whichever fills first ? that becomes the live position.
//   The other order is cancelled via the stored clOrdId.
//
// State machine:
//   IDLE     ? not enough data / range too small
//   ARMED    ? bracket locked, MIN_STRUCTURE_MS timer running
//   PENDING  ? both orders sent, waiting for first fill via confirm_fill()
//   LIVE     ? one side filled, other cancelled, managing position
//   COOLDOWN ? post-close cooldown
//
// Used by: GoldBracketEngine (XAUUSD)
// ==============================================================================

#include <deque>
#include <string>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include "OmegaTradeLedger.hpp"

namespace omega {

enum class BracketPhase : uint8_t {
    IDLE     = 0,
    ARMED    = 1,
    PENDING  = 2,
    LIVE     = 3,
    COOLDOWN = 4,
};

struct BracketSignal {
    bool        valid   = false;
    bool        is_long = true;
    double      entry   = 0.0;
    double      tp      = 0.0;
    double      sl      = 0.0;
    const char* reason  = "";
};

// Both-sides signal -- main.cpp sends two stop orders when this is non-empty
struct BracketBothSignals {
    bool   valid         = false;
    double long_entry    = 0.0;
    double long_tp       = 0.0;
    double long_sl       = 0.0;
    double short_entry   = 0.0;
    double short_tp      = 0.0;
    double short_sl      = 0.0;
    double size          = 0.01;
};

// ==============================================================================
template<typename Derived>
class BracketEngineBase
{
public:
    // ?? Config ????????????????????????????????????????????????????????????????
    int    STRUCTURE_LOOKBACK  = 30;
    // Cold-start entry gate -- ticks received before arming is allowed.
    // BracketEngine has no seed() but m_window fills from tick 1 and
    // STRUCTURE_LOOKBACK=30 means it can arm in ~3s on a fast feed.
    // At ~5-10 ticks/s: 150 ticks ? 15-30s of real market data.
    int    MIN_ENTRY_TICKS    = 150;
    double BUFFER              = 0.3;
    double RR                  = 1.5;
    int    COOLDOWN_MS         = 120000;
    double MIN_RANGE           = 0.0;
    double MAX_RANGE           = 0.0;  // if >0, blocks arm when range EXCEEDS this (prevents bracketing trending moves)
    int    MIN_HOLD_MS         = 15000;
    int    FAILURE_WINDOW_MS   = 5000;
    double VWAP_MIN_DIST       = 0.0;
    int    MIN_STRUCTURE_MS    = 0;
    int    ATR_PERIOD          = 0;
    double ATR_RANGE_K         = 0.0;
    double SLIPPAGE_BUFFER     = 0.0;
    double EDGE_MULTIPLIER     = 1.5;
    double MAX_SPREAD          = 0.0;  // if >0, blocks arm_both_sides when spread exceeds this value
    double ENTRY_SIZE          = 0.01;
    double SL_PCT              = 0.0;
    // ?? Continuous trail parameters (S19 2026-04-24) ??????????????????????????
    // Replaces the stepped trail cascade (0.40/1.0/1.5/2.0 × tp_dist milestones)
    // that left a dead zone between BE lock and 1R where SL sat pinned at entry.
    //
    // Mechanics:
    //   1. Once MFE reaches TRAIL_ACTIVATION_PTS in our favour, trail arms.
    //   2. SL ratchets to (entry + MFE - TRAIL_DISTANCE_PTS) for LONG,
    //      or (entry - MFE + TRAIL_DISTANCE_PTS) for SHORT.
    //   3. SL only moves in our favour (one-way ratchet). Any SL check at
    //      line 419 (unchanged) fires the exit on reversal.
    //
    // XAUUSD defaults: 3.0 / 2.0 -- activation just above typical spread+noise,
    // trail tight enough to capture 10-20pt intraday moves, loose enough to
    // not get clipped by normal tick wiggle. Override per-symbol via configure.
    double TRAIL_ACTIVATION_PTS = 3.0;
    double TRAIL_DISTANCE_PTS   = 2.0;
    // PENDING_TIMEOUT_SEC: how long to wait for price to hit a bracket level.
    // LIVE mode: 60s is fine -- broker holds the stop orders, we just wait for fill ACK.
    // SHADOW mode: price must cross the level within this window for fill simulation.
    // Gold often consolidates 2-5 min after compressing -- 60s was expiring before break.
    // Default 300s (5 min). Set per-engine in main.cpp after configure().
    int    PENDING_TIMEOUT_SEC = 300;
    // MAX_HOLD_SEC (S20 2026-04-25): absolute cap on how long a filled position
    // can remain open. Motivation: 2026-04-24 session held XAUUSD_BRACKET LONG
    // 234 minutes (-$18.74) while gold dropped 18pt. No existing exit path
    // caps hold time -- trail only tightens SL, regime_flip needs a confirmed
    // drift reversal, and BREAKOUT_FAIL only fires inside FAILURE_WINDOW_MS.
    // This gate exits at market when the position has been open for
    // MAX_HOLD_SEC seconds. 0 = disabled (prior behaviour). Set per-engine.
    // XAUUSD suggested default: 3600 (60 min) — tighten to 1800 after
    // review if trail isn't capturing enough profit.
    int    MAX_HOLD_SEC        = 0;
    // MIN_BREAK_TICKS: consecutive ticks price must stay INSIDE the bracket before
    // arm_both_sides() fires. Guards against liquidity sweeps at London open and
    // other spike-and-snap patterns where price blows through a bracket level in
    // a single tick then reverses. Default 0 = disabled. Set to 3 for gold.
    // How it works: once structure has held (MIN_STRUCTURE_MS passed), each tick
    // checks whether mid is inside [bracket_low, bracket_high]. If yes, counter
    // increments. If price spikes outside (sweep), counter resets to 0. Only when
    // counter reaches MIN_BREAK_TICKS does arm_both_sides() fire.
    int    MIN_BREAK_TICKS     = 0;
    // ?? Whipsaw guards (default ON) ??????????????????????????????????????????
    // Two independent mechanisms, both default ON, both disable cleanly:
    //
    // 1) Same-bracket lockout (WHIPSAW_OVERLAP_K):
    //    After a LOSING close (SL_HIT, BREAKOUT_FAIL, or any negative-pnl exit),
    //    the stopped bracket's [hi, lo] is remembered. Future IDLE->ARMED attempts
    //    are blocked while the new structural range overlaps the stopped range by
    //    >= WHIPSAW_OVERLAP_K (fraction of the NEW range). Expires once price
    //    establishes a non-overlapping range OR WHIPSAW_LOCKOUT_MAX_MS elapses.
    //    Set WHIPSAW_OVERLAP_K=0.0 to disable.
    //
    // 2) Whipsaw counter (WHIPSAW_COOLDOWN_MULT):
    //    Consecutive losses on overlapping brackets increment a counter. When it
    //    reaches 2, the NEXT cooldown is multiplied by WHIPSAW_COOLDOWN_MULT.
    //    A winning close or a non-overlapping loss resets the counter.
    //    Set WHIPSAW_COOLDOWN_MULT=1.0 to disable.
    //
    // TP wins never trigger either mechanism and always reset the counter.
    double WHIPSAW_OVERLAP_K        = 0.5;      // 0.0 disables lockout
    double WHIPSAW_COOLDOWN_MULT    = 2.0;      // 1.0 disables cooldown extension
    int    WHIPSAW_LOCKOUT_MAX_MS   = 3600000;  // 60 min safety ceiling (raised from 15min 2026-04-21 after 4777.50/4789.34 death spiral: 07:08 and 07:48 SLs armed against SAME zone once 15min ceiling expired)
    // ?? Consecutive-SL kill (pattern-lift from EMACrossEngine) ???????????????
    // Independent of WHIPSAW_OVERLAP_K -- counts raw consecutive SL exits regardless
    // of range overlap. When CONSEC_SL_KILL_THRESHOLD SL closes fire in a row, all
    // IDLE->ARMED attempts are blocked for CONSEC_SL_KILL_DURATION_MS. Any non-SL
    // close (TP, BE, TRAIL, BREAKOUT_FAIL, T/O, manual) resets the counter to 0.
    // Set CONSEC_SL_KILL_THRESHOLD=0 to disable.
    int    CONSEC_SL_KILL_THRESHOLD   = 3;        // 0 disables
    int    CONSEC_SL_KILL_DURATION_MS = 1800000;  // 30 min block
    // ?? Regime-flip exit (Session 13, 2026-04-23) ??????????????????????????????
    // Early-exit a LIVE position when ewm_drift flips against the position direction
    // with confirmed magnitude. Independent of SL / trail / BREAKOUT_FAIL branches.
    //
    // Trigger (per on_tick call, drift arg passed by caller, default 0.0):
    //   LONG:  drift <= -REGIME_FLIP_MIN_DRIFT
    //   SHORT: drift >= +REGIME_FLIP_MIN_DRIFT
    // On match, increment m_regime_flip_ticks. If it reaches REGIME_FLIP_CONFIRM_TICKS,
    // close at bid (long) / ask (short) with reason "REGIME_FLIP_EXIT". If the match
    // condition breaks on any tick, counter resets to 0 (debounce).
    //
    // Design rationale: gold bracket stopped out repeatedly on 2026-04-23 after the
    // regime had already flipped 30+ seconds earlier -- engine held to initial SL
    // despite unambiguous drift reversal. DXYDivergence engine caught the same signal
    // and profited on the flip. This branch gives bracket the same awareness.
    //
    // Default 0.0 / 0 disables. Set REGIME_FLIP_MIN_DRIFT > 0 AND
    // REGIME_FLIP_CONFIRM_TICKS > 0 to activate. Only fires after MIN_HOLD_MS so
    // it never pre-empts a legitimate SL / BREAKOUT_FAIL exit.
    // Exit reason "REGIME_FLIP_EXIT" does NOT increment consec-SL counter (only
    // SL_HIT does, see closePos m_consec_sl logic). A flip exit is a managed close
    // not a protective stop, so whipsaw/CONSEC_SL machinery stays pristine.
    double REGIME_FLIP_MIN_DRIFT      = 0.0;   // 0.0 disables
    int    REGIME_FLIP_CONFIRM_TICKS  = 0;     // 0 disables
    const char* symbol         = "???";
    bool   shadow_mode         = false;  // set by main.cpp -- enables price-triggered fill sim in PENDING

    // ?? Observable state ??????????????????????????????????????????????????????
    BracketPhase phase        = BracketPhase::IDLE;
    double       bracket_high = 0.0;
    double       bracket_low  = 0.0;
    int          signal_count = 0;
    double       atr          = 0.0;

    struct OpenPos {
        bool    active          = false;
        bool    is_long         = true;
        bool    sl_locked_to_be = false;
        double  entry           = 0.0;
        double  tp              = 0.0;
        double  sl              = 0.0;
        double  size            = 0.01;
        double  mfe             = 0.0;
        double  mae             = 0.0;
        int64_t entry_ts        = 0;
        double  spread_at_entry = 0.0;
        char    regime[32]      = {};
    } pos;

    BracketBothSignals pending_both;

    // Stored by main.cpp so the losing-side order can be cancelled on fill
    std::string pending_long_clOrdId;
    std::string pending_short_clOrdId;

    using CloseCallback = std::function<void(const TradeRecord&)>;
    using CancelCallback = std::function<void(const std::string&)>; // called with clOrdId to cancel

    bool shouldTrade(double, double, double, double) const noexcept { return true; }
    void onSignal(const BracketBothSignals&) const noexcept {}

    // Set by main.cpp so engine can cancel broker orders directly on timeout/reject
    CancelCallback cancel_order_fn;

    // ?? configure() ??????????????????????????????????????????????????????????
    void configure(double buffer,
                   int    lookback,
                   double rr,
                   int    cooldown_ms,
                   double min_range,
                   double /*confirm_move_unused*/,
                   int    /*confirm_timeout_unused*/,
                   int    min_hold_ms,
                   double vwap_min_dist     = 0.0,
                   int    min_structure_ms  = 0,
                   int    failure_window_ms = 5000,
                   int    atr_period        = 0,
                   double /*atr_confirm_k*/ = 0.0,
                   double atr_range_k       = 0.0,
                   double slippage_buffer   = 0.0,
                   double edge_multiplier   = 1.5)
    {
        BUFFER             = buffer;
        STRUCTURE_LOOKBACK = lookback;
        RR                 = rr;
        COOLDOWN_MS        = cooldown_ms;
        MIN_RANGE          = min_range;
        MIN_HOLD_MS        = min_hold_ms;
        VWAP_MIN_DIST      = vwap_min_dist;
        MIN_STRUCTURE_MS   = min_structure_ms;
        FAILURE_WINDOW_MS  = failure_window_ms;
        ATR_PERIOD         = atr_period;
        ATR_RANGE_K        = atr_range_k;
        SLIPPAGE_BUFFER    = slippage_buffer;
        EDGE_MULTIPLIER    = edge_multiplier;
    }

    bool has_open_position() const noexcept {
        return phase == BracketPhase::PENDING || phase == BracketPhase::LIVE;
    }

    // current_range(): locked structure range for health watchdog
    // Returns 0.0 when not yet armed (IDLE phase or window still filling)
    double current_range() const noexcept {
        if (m_locked_hi <= 0.0 || m_locked_lo <= 0.0) return 0.0;
        return m_locked_hi - m_locked_lo;
    }

    BracketBothSignals get_signals() noexcept {
        BracketBothSignals out = pending_both;
        pending_both = BracketBothSignals{};
        return out;
    }

    // ?? on_tick() ?????????????????????????????????????????????????????????????
    // drift: signed EWM momentum proxy from upstream (e.g. g_gold_stack.ewm_drift()).
    //   > 0 bullish pressure, < 0 bearish pressure. Default 0.0 preserves behaviour
    //   for every caller that pre-dates the regime-flip exit (Session 13).
    //   Only read when REGIME_FLIP_MIN_DRIFT > 0 AND REGIME_FLIP_CONFIRM_TICKS > 0.
    void on_tick(double bid, double ask, long long /*ts_ms*/,
                 bool can_enter,
                 const char* macro_regime,
                 CloseCallback on_close,
                 double vwap = 0.0,
                 double l2_imbalance = 0.5,
                 double drift = 0.0) noexcept
    {
        // Store latest L2 imbalance -- used in arm_both_sides() for direction bias logging
        m_l2_imbalance = l2_imbalance;
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid        = (bid + ask) * 0.5;
        const double spread     = ask - bid;
        const double spread_pct = (mid > 0.0) ? (spread / mid * 100.0) : 999.0;
        const int64_t now       = nowSec();

        // ?? COOLDOWN ??????????????????????????????????????????????????????????
        // m_cooldown_ms_override, when non-zero, takes precedence for this single
        // cooldown cycle -- used by the whipsaw-counter mechanism to extend the
        // cooldown after consecutive overlapping losses. Cleared on IDLE entry.
        if (phase == BracketPhase::COOLDOWN) {
            const int64_t effective_cd_ms = (m_cooldown_ms_override > 0)
                ? static_cast<int64_t>(m_cooldown_ms_override)
                : static_cast<int64_t>(COOLDOWN_MS);
            if (now - m_cooldown_start >= effective_cd_ms / 1000) {
                phase = BracketPhase::IDLE;
                m_cooldown_ms_override = 0;  // single-use: clear after cooldown expires
            }
            else return;
        }

        // ?? LIVE: manage open position ????????????????????????????????????????
        if (phase == BracketPhase::LIVE) {
            if (!pos.active) return;
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if ( move > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            // Breakout failure: price re-crosses midpoint of bracket
            // FAILURE_WINDOW_MS divided by 1000 uses integer truncation -- values < 1000ms
            // truncate to 0, disabling the window. Use ceiling division to preserve small values.
            if (FAILURE_WINDOW_MS > 0 &&
                (now - pos.entry_ts) < static_cast<int64_t>((FAILURE_WINDOW_MS + 999) / 1000)) {
                const double bracket_mid = (m_locked_hi + m_locked_lo) * 0.5;
                if ( pos.is_long && bid < bracket_mid) {
                    closePos(bid, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
                if (!pos.is_long && ask > bracket_mid) {
                    closePos(ask, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
            }

            if ((now - pos.entry_ts) < static_cast<int64_t>(MIN_HOLD_MS / 1000)) return;

            // ?? Max-hold timeout exit (S20 2026-04-25) ????????????????????????
            // Exit at market when position has been open >= MAX_HOLD_SEC. Guards
            // against positions ground to SL over hours while the trail never
            // activates. MAX_HOLD_SEC = 0 disables this gate (prior behaviour).
            // Uses "TIMEOUT" reason so post-close analytics can isolate these
            // exits from trail/SL/TP/REGIME_FLIP paths.
            if (MAX_HOLD_SEC > 0 &&
                (now - pos.entry_ts) >= static_cast<int64_t>(MAX_HOLD_SEC)) {
                const double exit_px = pos.is_long ? bid : ask;
                std::cout << "[BRACKET-" << symbol << "] MAX_HOLD_TIMEOUT"
                          << " side=" << (pos.is_long ? "LONG" : "SHORT")
                          << " hold_s=" << (now - pos.entry_ts)
                          << " cap_s=" << MAX_HOLD_SEC
                          << " exit_px=" << exit_px
                          << " entry=" << pos.entry
                          << " sl=" << pos.sl
                          << " mfe=" << pos.mfe << "\n";
                std::cout.flush();
                closePos(exit_px, "MAX_HOLD_TIMEOUT", macro_regime, on_close);
                return;
            }

            // ?? Regime-flip exit (Session 13, 2026-04-23) ?????????????????????
            // Early-close when ewm_drift has flipped convincingly against the
            // position for REGIME_FLIP_CONFIRM_TICKS consecutive ticks. Debounced
            // by resetting the counter on any tick where the flip condition is
            // NOT met. Disabled entirely when either threshold is non-positive.
            // See config block at top of file for rationale.
            if (REGIME_FLIP_MIN_DRIFT > 0.0 && REGIME_FLIP_CONFIRM_TICKS > 0) {
                const bool flipped_against =
                    ( pos.is_long && drift <= -REGIME_FLIP_MIN_DRIFT) ||
                    (!pos.is_long && drift >= +REGIME_FLIP_MIN_DRIFT);
                if (flipped_against) {
                    ++m_regime_flip_ticks;
                    if (m_regime_flip_ticks >= REGIME_FLIP_CONFIRM_TICKS) {
                        const double exit_px = pos.is_long ? bid : ask;
                        std::cout << "[BRACKET-" << symbol << "] REGIME_FLIP_EXIT"
                                  << " side=" << (pos.is_long ? "LONG" : "SHORT")
                                  << " drift=" << drift
                                  << " min_drift=" << REGIME_FLIP_MIN_DRIFT
                                  << " confirm_ticks=" << m_regime_flip_ticks
                                  << " exit_px=" << exit_px << "\n";
                        std::cout.flush();
                        closePos(exit_px, "REGIME_FLIP_EXIT", macro_regime, on_close);
                        return;
                    }
                } else {
                    m_regime_flip_ticks = 0;  // debounce: any non-flipped tick resets
                }
            }

            // ?? Continuous MFE trailing stop (S19 2026-04-24) ?????????????????
            // Replaces the stepped cascade (0.40/1.0/1.5/2.0 × tp_dist milestones)
            // that left a dead zone between BE and 1R where SL sat pinned at
            // entry. On a 10pt bracket with RR=1.5 (tp_dist=15pt) the old logic
            // needed MFE>=30pt before any profit-locking trail engaged; typical
            // XAUUSD trades never reached it and scratched at BE.
            //
            // Mechanics:
            //   1. Once MFE >= TRAIL_ACTIVATION_PTS, trail arms.
            //   2. SL ratchets to (entry + MFE - TRAIL_DISTANCE_PTS) for LONG,
            //      (entry - MFE + TRAIL_DISTANCE_PTS) for SHORT.
            //   3. SL is one-way ratchet -- only moves in our favour.
            //   4. SL exit check at line ~419 (unchanged) fires exit on reversal.
            //
            // Params (TRAIL_ACTIVATION_PTS=3.0, TRAIL_DISTANCE_PTS=2.0 on XAUUSD)
            // defined at top of struct, tunable per-symbol via configure.
            //
            // Exit labelling (unchanged at line ~419):
            //   sl_locked_to_be && pos.sl > pos.entry + 0.01 -> TRAIL_HIT
            //   sl_locked_to_be && pos.sl ≈ pos.entry        -> BE_HIT
            //   !sl_locked_to_be                             -> SL_HIT
            // REGIME_FLIP_EXIT branch above this block is unchanged (safety rail).
            {
                const double trail_move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
                if (trail_move > 0.0) {
                    if (trail_move > pos.mfe) pos.mfe = trail_move;  // one-way MFE ratchet

                    if (pos.mfe >= TRAIL_ACTIVATION_PTS) {
                        const double new_sl = pos.is_long
                            ? (pos.entry + pos.mfe - TRAIL_DISTANCE_PTS)
                            : (pos.entry - pos.mfe + TRAIL_DISTANCE_PTS);

                        // Only ever move SL in our favour (ratchet).
                        const bool ratchets = pos.is_long
                            ? (new_sl > pos.sl)
                            : (new_sl < pos.sl);

                        if (ratchets) {
                            const bool crossed_be = pos.is_long
                                ? (new_sl >= pos.entry)
                                : (new_sl <= pos.entry);
                            const bool was_locked = pos.sl_locked_to_be;
                            pos.sl = new_sl;
                            if (crossed_be) pos.sl_locked_to_be = true;

                            // Log transitions of note: first BE crossing + every new profit-lock high.
                            if (!was_locked && pos.sl_locked_to_be) {
                                std::cout << "[BRACKET-" << symbol
                                          << "] TRAIL-ARMED SL->BE+ move=" << trail_move
                                          << " mfe=" << pos.mfe
                                          << " sl=" << pos.sl << "\n";
                            } else if (pos.sl_locked_to_be) {
                                std::cout << "[BRACKET-" << symbol
                                          << "] TRAIL mfe=" << pos.mfe
                                          << " sl=" << pos.sl
                                          << " locked=" << (pos.is_long ? pos.sl - pos.entry
                                                                        : pos.entry - pos.sl) << "\n";
                            }
                        }
                    }
                }
            }

            // No fixed TP -- continuous trail exits the position on reversal.
            // SL check below handles all exits: initial SL, BE crossing, and trail.
            if ( pos.is_long && bid <= pos.sl) {
                const char* r = pos.sl_locked_to_be
                    ? (pos.sl > pos.entry + 0.01 ? "TRAIL_HIT" : "BE_HIT") : "SL_HIT";
                closePos(pos.sl, r, macro_regime, on_close); return;
            }
            if (!pos.is_long && ask >= pos.sl) {
                const char* r = pos.sl_locked_to_be
                    ? (pos.sl < pos.entry - 0.01 ? "TRAIL_HIT" : "BE_HIT") : "SL_HIT";
                closePos(pos.sl, r, macro_regime, on_close); return;
            }
            return;
        }

        // ?? PENDING: both orders out, waiting for first fill ??????????????????
        if (phase == BracketPhase::PENDING) {
            if (!can_enter) {
                std::cout << "[BRACKET-" << symbol << "] PENDING CANCELLED -- session/risk gate closed\n";
                std::cout.flush();
                cancel_both_broker_orders();
                reset();
                return;
            }
            if ((now - m_armed_ts) > static_cast<int64_t>(PENDING_TIMEOUT_SEC)) {
                std::cout << "[BRACKET-" << symbol << "] PENDING TIMEOUT after " << PENDING_TIMEOUT_SEC
                          << "s -- price never hit bracket hi=" << m_locked_hi
                          << " lo=" << m_locked_lo
                          << " last_mid=" << std::fixed << std::setprecision(4) << ((bid+ask)*0.5)
                          << " dist_to_hi=" << (m_locked_hi - (bid+ask)*0.5)
                          << " dist_to_lo=" << ((bid+ask)*0.5 - m_locked_lo) << "\n";
                std::cout.flush();
                cancel_both_broker_orders();
                reset();
                return;
            }
            if (shadow_mode) {
                if (ask >= m_locked_hi) {
                    std::cout << "[BRACKET-" << symbol << "] SHADOW FILL LONG @ " << m_locked_hi << "\n";
                    std::cout.flush();
                    confirm_fill(true, m_locked_hi, ENTRY_SIZE);
                    return;
                }
                if (bid <= m_locked_lo) {
                    std::cout << "[BRACKET-" << symbol << "] SHADOW FILL SHORT @ " << m_locked_lo << "\n";
                    std::cout.flush();
                    confirm_fill(false, m_locked_lo, ENTRY_SIZE);
                    return;
                }
            }
            return;
        }

        // ?? Structure window -- always fed, regardless of can_enter ???????????
        // FIX 2026-04-03: previously window only received ticks when can_enter=1.
        // asia_ok flickers every 10-30s, dropping can_enter to 0 repeatedly and
        // starving the window. STRUCTURE_LOOKBACK=20 was never reached consecutively
        // so bracket NEVER armed during entire Asia sessions (confirmed Apr 1+2 logs).
        // Fix: feed window on EVERY tick. can_enter still gates IDLE->ARMED and
        // ARMED hold. Window is price data -- no entry permission semantics.
        // Sanity check: never push invalid prices into window.
        // A zero or negative mid causes slo=~0 which makes bracket_low=-0.11
        // and range=~4722 producing TP targets of 7500+ points.
        if (mid > 100.0) {  // gold is always > $100, anything below = bad tick
            m_window.push_back(mid);
        }
        ++m_ticks_received;

        // Update recent tight-window range (last 60 ticks) for MAX_RANGE check.
        // Separate from the full STRUCTURE_LOOKBACK window -- see m_recent_range comment.
        {
            static constexpr int RECENT_WINDOW = 60;
            if ((int)m_window.size() >= RECENT_WINDOW) {
                const auto rb = m_window.end() - RECENT_WINDOW;
                const auto re = m_window.end();
                m_recent_range = *std::max_element(rb, re) - *std::min_element(rb, re);
            } else {
                m_recent_range = 0.0;
            }
        }

        // ?? Entry gate ????????????????????????????????????????????????????????
        // While ARMED: preserve the timer -- do NOT reset m_armed_ts.
        // A can_enter=false blip means we can't arm new structure, but
        // structure that already qualified should not lose its timer.
        if (!can_enter) {
            if (phase == BracketPhase::ARMED) {
                // Timer preserved -- do not touch m_armed_ts
                return;
            }
            // IDLE: window fed above, but transition to ARMED blocked.
            return;
        }

        if (static_cast<int>(m_window.size()) > STRUCTURE_LOOKBACK * 2)
            m_window.pop_front();
        if (static_cast<int>(m_window.size()) < STRUCTURE_LOOKBACK) return;

        // ?? Cold-start entry gate ????????????????????????????????????????????????
        // m_window fills from tick 1; STRUCTURE_LOOKBACK=30 means the engine
        // can arm within 3s of startup on a fast feed. Block IDLE?ARMED transition
        // until MIN_ENTRY_TICKS real ticks have been received.
        // ARMED/PENDING/LIVE/COOLDOWN phases pass through -- only fresh arming blocked.
        const bool cold_start_blocked = (m_ticks_received < MIN_ENTRY_TICKS
                                         && phase == BracketPhase::IDLE);

        // ?? Volatility-scaled minimum range ???????????????????????????????????
        // ATR_PERIOD > 0: compute true price-range volatility (hi-lo of mid over
        // ATR_PERIOD ticks from m_window). This is the actual market noise floor.
        // eff_min_range = max(recent_volatility * ATR_RANGE_K, MIN_RANGE).
        //
        // Why this matters: with a fixed MIN_RANGE, a $6 bracket qualifies on a
        // quiet Asian session (noise $3-5) but also during London open (noise $12-20)
        // where a $6 bracket SL sits well inside normal noise and gets swept.
        // Scaling to recent volatility raises the floor automatically when the
        // market is noisy -- a bracket must be LARGER than noise to be valid.
        //
        // ATR_RANGE_K tuning guide (set per-symbol in main.cpp):
        //   Gold:   ATR_PERIOD=20, ATR_RANGE_K=1.5  ? eff_min = max($6, noise*1.5)
        //           London noise ~$8-12 ? eff_min rises to $12-18 automatically
        //   Silver: ATR_PERIOD=20, ATR_RANGE_K=1.5
        //   FX:     ATR_PERIOD=20, ATR_RANGE_K=1.8  (tighter price, higher multiplier)
        //   Indices: leave disabled (ATR_PERIOD=0) -- noise floor more stable
        if (ATR_PERIOD > 0 && static_cast<int>(m_window.size()) >= ATR_PERIOD) {
            const int    n    = static_cast<int>(m_window.size());
            const auto   vbeg = m_window.begin() + (n - ATR_PERIOD);
            const double vhi  = *std::max_element(vbeg, m_window.end());
            const double vlo  = *std::min_element(vbeg, m_window.end());
            atr = vhi - vlo;  // true price-range volatility over ATR_PERIOD ticks
        }
        const double eff_min_range = (ATR_RANGE_K > 0.0 && atr > 0.0)
                                     ? std::max(atr * ATR_RANGE_K, MIN_RANGE)
                                     : MIN_RANGE;

        // ?? Structural range ??????????????????????????????????????????????????
        const auto   wbegin = m_window.end() - STRUCTURE_LOOKBACK;
        const auto   wend   = m_window.end();   // inclusive of current tick
        const double shi    = *std::max_element(wbegin, wend);
        const double slo    = *std::min_element(wbegin, wend);
        const double range  = shi - slo;

        // Guard: if slo is invalid (zero, negative, or implausibly small),
        // reset to IDLE. This catches any window corruption from bad ticks
        // that slipped through before the push_back guard was added.
        if (slo < 100.0 || shi < 100.0) {
            if (phase != BracketPhase::ARMED) {
                phase = BracketPhase::IDLE;
                bracket_high = 0.0; bracket_low = 0.0;
                // Purge bad values from window
                while (!m_window.empty() && m_window.front() < 100.0)
                    m_window.pop_front();
            }
            static int64_t s_corrupt_log = 0;
            const int64_t now_corrupt = static_cast<int64_t>(std::time(nullptr));
            if (now_corrupt - s_corrupt_log >= 10) {
                s_corrupt_log = now_corrupt;
                printf("[BRACKET-%s] CORRUPT window slo=%.2f shi=%.2f -- purging bad ticks\n",
                       symbol, slo, shi);
                fflush(stdout);
            }
            return;
        }

        if (range < eff_min_range) {
            // While ARMED: hold state -- don't reset timer on transient range collapse.
            // Rolling window shrinks naturally as trend pushes through it.
            if (phase != BracketPhase::ARMED) {
                phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            }
            return;
        }

        // shouldTrade() gate -- only resets to IDLE from IDLE, not from ARMED.
        // If we are already ARMED (structure was valid), a single-tick shouldTrade
        // failure (e.g. transient spread spike) must not destroy the timer.
        if (cold_start_blocked) return;

        if (!static_cast<Derived*>(this)->shouldTrade(bid, ask, spread_pct, vwap)) {
            if (phase != BracketPhase::ARMED) {
                phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            }
            return;
        }

        const double buf = spread * 0.5;
        bracket_high = shi + buf;
        bracket_low  = slo - buf;

        // ?? Consecutive-SL kill (pattern-lift from EMACrossEngine) ???????????
        // Only consulted on IDLE->ARMED attempts. Blocks all arming until
        // m_sl_kill_until_s expires. Cleared by any non-SL close via closePos().
        // Independent of WHIPSAW_OVERLAP_K -- fires on raw consecutive SLs regardless
        // of whether the stopped brackets overlap each other.
        if (phase == BracketPhase::IDLE
            && CONSEC_SL_KILL_THRESHOLD > 0
            && m_sl_kill_until_s > 0
            && nowSec() < m_sl_kill_until_s)
        {
            static int64_t s_sl_kill_log = 0;
            const int64_t now_sk = nowSec();
            if (now_sk - s_sl_kill_log >= 30) {
                s_sl_kill_log = now_sk;
                std::cout << "[BRACKET-" << symbol << "] SL-KILL BLOCK"
                          << " consec_sl=" << m_consec_sl
                          << " thresh=" << CONSEC_SL_KILL_THRESHOLD
                          << " remaining_s=" << (m_sl_kill_until_s - now_sk) << "\n";
                std::cout.flush();
            }
            return;
        }
        // Clear kill window once elapsed (lazy clear on next IDLE check)
        if (m_sl_kill_until_s > 0 && nowSec() >= m_sl_kill_until_s) {
            std::cout << "[BRACKET-" << symbol << "] SL-KILL EXPIRED -- arming re-enabled\n";
            std::cout.flush();
            m_sl_kill_until_s = 0;
            m_consec_sl = 0;
        }

        // ?? Same-bracket lockout (whipsaw guard) ??????????????????????????????
        // Only consulted on IDLE->ARMED attempts. ARMED/PENDING/LIVE phases pass
        // through untouched -- we only block NEW arming into the zone we just
        // lost money in.
        //
        // Release conditions (checked in order):
        //   1) Age: (now - m_last_stop_ts) * 1000 >= WHIPSAW_LOCKOUT_MAX_MS
        //   2) Range has moved away: overlap(new_range, stopped_range) < OVERLAP_K
        // Either condition clears the lockout.
        //
        // Overlap math: overlap = max(0, min(hi_a,hi_b) - max(lo_a,lo_b))
        // Normalise by the NEW range (not the stopped range) so that a tight
        // new compression inside the old wide range still counts as "same zone".
        if (phase == BracketPhase::IDLE
            && WHIPSAW_OVERLAP_K > 0.0
            && m_last_stop_hi > 0.0 && m_last_stop_lo > 0.0)
        {
            const int64_t age_ms = (nowSec() - m_last_stop_ts) * 1000;
            if (age_ms >= static_cast<int64_t>(WHIPSAW_LOCKOUT_MAX_MS)) {
                // Safety ceiling -- release lockout unconditionally
                std::cout << "[BRACKET-" << symbol << "] WHIPSAW-LOCKOUT RELEASED (age)"
                          << " age_ms=" << age_ms << " max=" << WHIPSAW_LOCKOUT_MAX_MS << "\n";
                std::cout.flush();
                m_last_stop_hi = 0.0;
                m_last_stop_lo = 0.0;
                m_whipsaw_count = 0;
            } else {
                const double ov_lo  = std::max(bracket_low,  m_last_stop_lo);
                const double ov_hi  = std::min(bracket_high, m_last_stop_hi);
                const double ov     = (ov_hi > ov_lo) ? (ov_hi - ov_lo) : 0.0;
                const double new_rng = bracket_high - bracket_low;
                const double ov_frac = (new_rng > 0.0) ? (ov / new_rng) : 0.0;
                if (ov_frac >= WHIPSAW_OVERLAP_K) {
                    static int64_t s_ws_log = 0;
                    const int64_t now_ws = nowSec();
                    if (now_ws - s_ws_log >= 30) {
                        s_ws_log = now_ws;
                        std::cout << "[BRACKET-" << symbol << "] WHIPSAW-LOCKOUT BLOCK"
                                  << " new=[" << bracket_low << "," << bracket_high << "]"
                                  << " stopped=[" << m_last_stop_lo << "," << m_last_stop_hi << "]"
                                  << " overlap_frac=" << ov_frac
                                  << " thresh=" << WHIPSAW_OVERLAP_K
                                  << " count=" << m_whipsaw_count << "\n";
                        std::cout.flush();
                    }
                    // Hold IDLE -- do NOT arm. Lockout persists until price
                    // establishes a non-overlapping range or ages out.
                    return;
                }
                // Overlap below threshold -- market has moved. Clear lockout.
                std::cout << "[BRACKET-" << symbol << "] WHIPSAW-LOCKOUT RELEASED (range moved)"
                          << " overlap_frac=" << ov_frac
                          << " thresh=" << WHIPSAW_OVERLAP_K << "\n";
                std::cout.flush();
                m_last_stop_hi = 0.0;
                m_last_stop_lo = 0.0;
                // Do NOT reset m_whipsaw_count here -- it resets on winning close
                // or on the natural COOLDOWN->IDLE transition path, not just because
                // the market drifted. Counter only matters for the NEXT stopout.
            }
        }

        // ?? IDLE ? ARMED ??????????????????????????????????????????????????????
        if (phase == BracketPhase::IDLE) {
            phase      = BracketPhase::ARMED;
            m_armed_ts = nowSec();
            std::cout << "[BRACKET-" << symbol << "] ARMED"
                      << " hi=" << bracket_high << " lo=" << bracket_low
                      << " range=" << range << "\n";
            std::cout.flush();
            return;
        }

        // ?? ARMED ? PENDING: fire both orders once structure has held ?????????
        if (phase == BracketPhase::ARMED) {
            if (MIN_STRUCTURE_MS > 0 &&
                (nowSec() - m_armed_ts) < static_cast<int64_t>(MIN_STRUCTURE_MS / 1000))
                return;

            // ?? Sweep gate (MIN_BREAK_TICKS) ?????????????????????????????????
            // Only fires arm_both_sides() when price has been inside the bracket
            // for MIN_BREAK_TICKS consecutive ticks. Guards against London open
            // liquidity sweeps where price spikes through a bracket level in 1
            // tick then snaps back -- causing a fill at the worst possible price.
            // Each tick inside increments counter; any tick outside resets it.
            // When MIN_BREAK_TICKS=0 (default) this block is skipped entirely.
            if (MIN_BREAK_TICKS > 0) {
                if (mid >= bracket_low && mid <= bracket_high) {
                    ++m_inside_ticks;
                } else {
                    if (m_inside_ticks > 0) {
                        std::cout << "[BRACKET-" << symbol << "] SWEEP-RESET"
                                  << " mid=" << mid
                                  << " bracket=[" << bracket_low << "," << bracket_high << "]"
                                  << " inside_ticks=" << m_inside_ticks << " (reset to 0)\n";
                        std::cout.flush();
                    }
                    m_inside_ticks = 0;
                }
                if (m_inside_ticks < MIN_BREAK_TICKS) {
                    return;
                }
                // Threshold reached -- reset counter and fall through to arm
                m_inside_ticks = 0;
                std::cout << "[BRACKET-" << symbol << "] SWEEP-CONFIRMED"
                          << " inside_ticks>=" << MIN_BREAK_TICKS
                          << " mid=" << mid << " -- arming\n";
                std::cout.flush();
            }

            arm_both_sides(spread, macro_regime);
        }
    }

    // ?? confirm_fill() ????????????????????????????????????????????????????????
    // Called by main.cpp when one side fills.
    // is_long_filled: true = long (buy stop) filled, false = short (sell stop) filled.
    void confirm_fill(bool is_long_filled, double actual_price, double actual_size) noexcept {
        if (phase != BracketPhase::PENDING) return;
        pos.active   = true;
        pos.is_long  = is_long_filled;
        pos.entry    = actual_price;
        pos.size     = actual_size;
        pos.entry_ts = nowSec();
        pos.sl       = is_long_filled ? m_locked_long_sl  : m_locked_short_sl;
        pos.tp       = is_long_filled ? m_locked_long_tp  : m_locked_short_tp;
        phase        = BracketPhase::LIVE;
        m_regime_flip_ticks = 0;  // fresh position -- start counter clean
        // Engine-enforced: cancel the other leg immediately on fill
        if (cancel_order_fn) {
            const std::string& other_id = is_long_filled
                ? pending_short_clOrdId : pending_long_clOrdId;
            if (!other_id.empty()) {
                cancel_order_fn(other_id);
                std::cout << "[BRACKET-" << symbol << "] OCO CANCEL other_leg=" << other_id << "\n";
                std::cout.flush();
            }
        }
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();
        std::cout << "[BRACKET-" << symbol << "] FILL CONFIRMED"
                  << " side=" << (is_long_filled ? "LONG" : "SHORT")
                  << " px=" << actual_price << " size=" << actual_size
                  << " sl=" << pos.sl << " tp=" << pos.tp << "\n";
        std::cout.flush();
    }

    void on_reject() noexcept {
        // One side rejected -- cancel the other side on the broker then reset
        std::cout << "[BRACKET-" << symbol << "] REJECTED -- cancelling other leg and resetting\n";
        std::cout.flush();
        cancel_both_broker_orders();
        reset();
    }

    void forceClose(double bid, double ask, const char* reason,
                    double /*latency_ms*/, const char* macro_regime,
                    CloseCallback on_close) noexcept
    {
        if (phase == BracketPhase::LIVE && pos.active)
            closePos((bid + ask) * 0.5, reason, macro_regime, on_close);
        else if (phase == BracketPhase::PENDING) {
            cancel_both_broker_orders();
            reset();
        }
    }

protected:
    // ?? cancel_both_broker_orders() ???????????????????????????????????????????
    // Fires cancel_order_fn for both pending legs if they exist.
    // Called on: timeout, session gate close, reject, forceClose while PENDING.
    void cancel_both_broker_orders() noexcept {
        if (cancel_order_fn) {
            if (!pending_long_clOrdId.empty())  cancel_order_fn(pending_long_clOrdId);
            if (!pending_short_clOrdId.empty()) cancel_order_fn(pending_short_clOrdId);
        }
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();
    }
    std::deque<double> m_window;
    std::deque<double> m_atr_window;
    int64_t m_cooldown_start  = 0;
    int64_t m_armed_ts        = 0;
    int     m_trade_id        = 0;
    int     m_ticks_received  = 0;  // raw tick count since construction -- cold-start gate
    double  m_l2_imbalance    = 0.5;
    int     m_inside_ticks    = 0;  // consecutive ticks mid was inside bracket -- for MIN_BREAK_TICKS gate
    // Recent tight-window range (last 60 ticks) -- used for MAX_RANGE check only.
    // The structural range (STRUCTURE_LOOKBACK=600) correctly identifies bracket levels
    // but must not be used for MAX_RANGE: after a 33pt completed swing, 600-tick range=33pt
    // exceeds MAX_RANGE=12pt and blocks arming during the subsequent consolidation.
    // Using the recent 60-tick range for MAX_RANGE asks "is the market currently trending?"
    // not "did the market ever move this much in the last 10 minutes?"
    double  m_recent_range    = 0.0;

    // ?? Whipsaw guard state ??????????????????????????????????????????????????
    // Set on any LOSING close (closePos with pnl < 0). Cleared on:
    //   - TP win / any winning close (counter reset to 0)
    //   - Lockout age exceeds WHIPSAW_LOCKOUT_MAX_MS (safety release)
    //   - New structural range that overlaps the stopped range by < OVERLAP_K
    // A value of m_last_stop_hi == 0.0 means "no active lockout".
    double  m_last_stop_hi       = 0.0;
    double  m_last_stop_lo       = 0.0;
    int64_t m_last_stop_ts       = 0;
    int     m_whipsaw_count      = 0;
    bool    m_next_cooldown_ext  = false;  // set true when counter>=2 at close time
    int     m_cooldown_ms_override = 0;    // non-zero overrides COOLDOWN_MS for this cycle
    // ?? Consecutive-SL kill state (pattern-lift from EMACrossEngine) ????????????
    // m_consec_sl counts SL closes in a row. Reset to 0 on any non-SL close.
    // When m_consec_sl hits CONSEC_SL_KILL_THRESHOLD, m_sl_kill_until_s is set to
    // (now + CONSEC_SL_KILL_DURATION_MS/1000) and all IDLE->ARMED attempts are
    // blocked until that time. Value 0 means no active kill.
    int     m_consec_sl          = 0;
    int64_t m_sl_kill_until_s    = 0;
    // ?? Regime-flip exit state (Session 13, 2026-04-23) ?????????????????????????
    // Counter of consecutive ticks where drift has been flipped against the position
    // direction by at least REGIME_FLIP_MIN_DRIFT. Reset to 0 on any non-flipped tick
    // (debounce), in reset() (IDLE transition), and in confirm_fill() (new position).
    int     m_regime_flip_ticks  = 0;

    // Locked at the moment both orders are sent -- never change after PENDING
    double m_locked_hi        = 0.0;
    double m_locked_lo        = 0.0;
    double m_locked_long_sl   = 0.0;
    double m_locked_long_tp   = 0.0;
    double m_locked_short_sl  = 0.0;
    double m_locked_short_tp  = 0.0;

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void arm_both_sides(double spread, const char* macro_regime) noexcept {
        const double dist = bracket_high - bracket_low;

        // ?? Max spread gate ??????????????????????????????????????????????????
        // Block entry if current spread exceeds configured maximum.
        // Prevents firing into wide-spread conditions where round-trip cost
        // eats the entire expected edge.
        if (MAX_SPREAD > 0.0 && spread > MAX_SPREAD) {
            std::cout << "[BRACKET-" << symbol << "] BLOCKED: spread_too_wide"
                      << " spread=" << spread << " max=" << MAX_SPREAD << "\n";
            std::cout.flush();
            phase = BracketPhase::IDLE;
            bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // ?? Hard range floor -- checked on RAW structural range, not spread-padded dist ??
        // dist = (shi + spread*0.5) - (slo - spread*0.5) = raw_range + spread.
        // Checking dist < MIN_RANGE allows a $0.18 structure range + $0.12 spread
        // to pass MIN_RANGE=$0.30 -- the SL is then $0.30 wide but the STRUCTURE
        // that justified the bracket was only $0.18. Any normal tick sweeps it.
        // Fix: derive raw_range from dist and spread, check that separately.
        const double raw_range = dist - spread; // dist = raw_range + spread (buf = spread*0.5 each side)
        if (raw_range < MIN_RANGE) {
            std::cout << "[BRACKET-" << symbol << "] BLOCKED: raw_range_too_small"
                      << " raw_range=" << raw_range << " dist=" << dist
                      << " spread=" << spread << " min=" << MIN_RANGE << "\n";
            std::cout.flush();
            phase = BracketPhase::IDLE;
            bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // ?? Hard range ceiling -- prevents bracketing ONGOING trending moves ????
        // Uses m_recent_range (last 60 ticks), NOT the full structural range (raw_range).
        //
        // Root cause fixed (2026-04-07):
        //   After a 33pt completed swing, 600-tick structural range = 33pt.
        //   raw_range=33 > MAX_RANGE=12 -> BLOCKED the entire consolidation phase.
        //   Bracket never armed despite clear tight compression at the swing low.
        //
        // Fix: check m_recent_range (last 60 ticks = ~12s of actual compression).
        //   Recent=1.5pt < MAX_RANGE=12 -> PASS (market compressing NOW).
        //   Recent=25pt  > MAX_RANGE=12 -> BLOCK (market still trending NOW).
        //   MAX_RANGE=0 disables the cap entirely.
        if (MAX_RANGE > 0.0 && m_recent_range > MAX_RANGE) {
            std::cout << "[BRACKET-" << symbol << "] BLOCKED: recent_range_too_large"
                      << " recent_range=" << m_recent_range
                      << " raw_range=" << raw_range
                      << " max=" << MAX_RANGE << "\n";
            std::cout.flush();
            phase = BracketPhase::IDLE;
            bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // ?? Spread viability check (mandatory, runs before any other calc) ???
        // A trade is only viable if the SL distance covers:
        //   - entry spread (paid on open)
        //   - exit spread (paid on close)
        //   - slippage on both sides
        // If dist <= round_trip_cost the trade has negative expectancy before
        // it even moves -- spread alone will push it to SL.
        const double round_trip_cost = (spread * 2.0) + (SLIPPAGE_BUFFER * 2.0);
        if (dist <= round_trip_cost) {
            std::cout << "[BRACKET-" << symbol << "] BLOCKED: spread_not_covered"
                      << " dist=" << dist
                      << " round_trip_cost=" << round_trip_cost
                      << " (spread=" << spread
                      << " slip=" << SLIPPAGE_BUFFER << ")\n";
            std::cout.flush();
            phase = BracketPhase::IDLE;
            bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        const double long_entry  = bracket_high;
        const double long_sl     = bracket_low;
        const double long_tp     = bracket_high + dist * RR;
        const double short_entry = bracket_low;
        const double short_sl    = bracket_high;
        const double short_tp    = bracket_low - dist * RR;
        const double tp_dist     = dist * RR;
        const double cost        = spread + SLIPPAGE_BUFFER;

        if (EDGE_MULTIPLIER > 0.0 && cost > 0.0 && tp_dist < cost * EDGE_MULTIPLIER) {
            std::cout << "[BRACKET-" << symbol << "] BLOCKED: no_edge"
                      << " tp_dist=" << tp_dist << " cost=" << cost
                      << " need>=" << cost * EDGE_MULTIPLIER << "\n";
            std::cout.flush();
            phase = BracketPhase::IDLE;
            bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // Lock all levels
        m_locked_hi       = bracket_high;
        m_locked_lo       = bracket_low;
        m_locked_long_sl  = long_sl;
        m_locked_long_tp  = long_tp;
        m_locked_short_sl = short_sl;
        m_locked_short_tp = short_tp;

        pos             = OpenPos{};
        pos.active      = false;
        pos.size        = ENTRY_SIZE;
        pos.spread_at_entry = spread;
#ifdef _WIN32
        if (macro_regime) strncpy_s(pos.regime, macro_regime, 31);
#else
        if (macro_regime) { strncpy(pos.regime, macro_regime, 31); pos.regime[31] = '\0'; }
#endif

        pending_both.valid       = true;
        pending_both.long_entry  = long_entry;
        pending_both.long_tp     = long_tp;
        pending_both.long_sl     = long_sl;
        pending_both.short_entry = short_entry;
        pending_both.short_tp    = short_tp;
        pending_both.short_sl    = short_sl;
        pending_both.size        = ENTRY_SIZE;

        m_armed_ts = nowSec();
        phase      = BracketPhase::PENDING;
        ++signal_count;
        ++m_trade_id;

        // L2 imbalance direction note -- informational, does not gate the trade.
        // >0.65 = bid-heavy (bullish pressure, long leg favoured)
        // <0.35 = ask-heavy (bearish pressure, short leg favoured)
        // 0.35-0.65 = balanced (both legs equally likely)
        const char* l2_bias = (m_l2_imbalance > 0.65) ? "BID_HEAVY->LONG_FAVOURED"
                            : (m_l2_imbalance < 0.35) ? "ASK_HEAVY->SHORT_FAVOURED"
                            :                            "BALANCED";

        std::cout << "[BRACKET-" << symbol << "] BOTH ARMED"
                  << " LONG@" << long_entry << " tp=" << long_tp << " sl=" << long_sl
                  << " | SHORT@" << short_entry << " tp=" << short_tp << " sl=" << short_sl
                  << " range=" << dist
                  << " l2_imb=" << m_l2_imbalance << " (" << l2_bias << ")\n";
        std::cout.flush();

        static_cast<Derived*>(this)->onSignal(pending_both);
    }

    void closePos(double exit_px, const char* reason,
                  const char* macro_regime, CloseCallback& on_close) noexcept
    {
        if (!pos.active) return;
        TradeRecord tr;
        tr.id            = m_trade_id;
        tr.symbol        = symbol;
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = pos.tp;
        tr.sl            = pos.sl;
        tr.size          = pos.size;
        tr.pnl           = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;  // raw pts*lots -- handle_closed_trade applies tick_mult
        tr.net_pnl       = tr.pnl;
        tr.mfe           = pos.mfe * pos.size;
        tr.mae           = pos.mae;
        tr.entryTs       = pos.entry_ts;
        tr.exitTs        = nowSec();
        tr.exitReason    = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.latencyMs     = 0.0;
        tr.engine        = std::string(symbol ? symbol : "???") + "_BRACKET";
        tr.regime        = (macro_regime && *macro_regime) ? macro_regime : pos.regime;
        tr.bracket_hi    = m_locked_hi;   // upper boundary locked at arm time
        tr.bracket_lo    = m_locked_lo;   // lower boundary locked at arm time

        // ?? Whipsaw guard bookkeeping ?????????????????????????????????????????
        // Classify this exit:
        //   LOSS    : raw pnl (pts) is negative beyond a half-spread dead zone
        //   WIN     : raw pnl is positive beyond a half-spread dead zone
        //   NEUTRAL : |pnl| within dead zone (BE_HIT, tiny scratch) -- no change
        //
        // On LOSS:
        //   - Record stopped-bracket [hi, lo] and timestamp for same-bracket lockout
        //   - If the NEW stopped bracket overlaps the PREVIOUS stopped bracket by
        //     >= WHIPSAW_OVERLAP_K, this is a chased chop -> increment counter.
        //     Non-overlapping loss = different structure, reset counter to 1.
        //   - If counter >= 2 AND WHIPSAW_COOLDOWN_MULT > 1.0: set cooldown override
        //     so the NEXT cooldown is extended.
        //
        // On WIN:
        //   - Clear lockout (hi/lo = 0) and reset counter to 0.
        //
        // NEUTRAL leaves state untouched.
        const double raw_pnl_pts = pos.is_long ? (exit_px - pos.entry)
                                               : (pos.entry - exit_px);
        const double dead_zone   = pos.spread_at_entry * 0.5;
        const bool   is_loss     = (raw_pnl_pts < -dead_zone);
        const bool   is_win      = (raw_pnl_pts >  dead_zone);

        if (is_loss) {
            // Overlap with PREVIOUS stopped bracket (if any)?
            bool overlaps_prev = false;
            if (m_last_stop_hi > 0.0 && m_last_stop_lo > 0.0
                && WHIPSAW_OVERLAP_K > 0.0)
            {
                const double ov_lo  = std::max(m_locked_lo, m_last_stop_lo);
                const double ov_hi  = std::min(m_locked_hi, m_last_stop_hi);
                const double ov     = (ov_hi > ov_lo) ? (ov_hi - ov_lo) : 0.0;
                const double rng    = m_locked_hi - m_locked_lo;
                const double ov_frac = (rng > 0.0) ? (ov / rng) : 0.0;
                overlaps_prev = (ov_frac >= WHIPSAW_OVERLAP_K);
            }
            if (overlaps_prev) {
                ++m_whipsaw_count;
            } else {
                m_whipsaw_count = 1;  // first loss or loss on new zone
            }
            // Record THIS bracket as the stopped bracket for lockout
            m_last_stop_hi = m_locked_hi;
            m_last_stop_lo = m_locked_lo;
            m_last_stop_ts = nowSec();
            // Extend next cooldown if counter crossed threshold
            if (m_whipsaw_count >= 2 && WHIPSAW_COOLDOWN_MULT > 1.0) {
                m_cooldown_ms_override = static_cast<int>(
                    static_cast<double>(COOLDOWN_MS) * WHIPSAW_COOLDOWN_MULT);
            } else {
                m_cooldown_ms_override = 0;
            }
            std::cout << "[BRACKET-" << symbol << "] WHIPSAW-STATE loss"
                      << " reason=" << (reason ? reason : "?")
                      << " pnl_pts=" << raw_pnl_pts
                      << " count=" << m_whipsaw_count
                      << " overlaps_prev=" << (overlaps_prev ? 1 : 0)
                      << " next_cd_ms=" << (m_cooldown_ms_override > 0 ? m_cooldown_ms_override : COOLDOWN_MS)
                      << "\n";
            std::cout.flush();
        } else if (is_win) {
            // Winning close clears lockout entirely
            if (m_last_stop_hi > 0.0 || m_whipsaw_count > 0) {
                std::cout << "[BRACKET-" << symbol << "] WHIPSAW-STATE cleared_on_win"
                          << " pnl_pts=" << raw_pnl_pts << "\n";
                std::cout.flush();
            }
            m_last_stop_hi = 0.0;
            m_last_stop_lo = 0.0;
            m_whipsaw_count = 0;
            m_cooldown_ms_override = 0;
        }
        // NEUTRAL: no change

        // ?? Consecutive-SL counter (independent of whipsaw state) ??????????????
        // Counts raw SL closes. Any non-SL close (TP, BE, TRAIL, BREAKOUT_FAIL,
        // T/O, manual) resets the counter. Reason string is checked exactly --
        // SL_HIT is the canonical SL close label in this engine.
        if (CONSEC_SL_KILL_THRESHOLD > 0 && reason != nullptr) {
            const bool is_sl = (std::string(reason) == "SL_HIT");
            if (is_sl) {
                ++m_consec_sl;
                if (m_consec_sl >= CONSEC_SL_KILL_THRESHOLD) {
                    m_sl_kill_until_s = nowSec()
                        + static_cast<int64_t>(CONSEC_SL_KILL_DURATION_MS / 1000);
                    std::cout << "[BRACKET-" << symbol << "] SL-KILL TRIGGERED"
                              << " consec_sl=" << m_consec_sl
                              << " thresh=" << CONSEC_SL_KILL_THRESHOLD
                              << " block_duration_s=" << (CONSEC_SL_KILL_DURATION_MS / 1000)
                              << "\n";
                    std::cout.flush();
                }
            } else {
                if (m_consec_sl > 0) {
                    std::cout << "[BRACKET-" << symbol << "] SL-KILL-COUNTER reset"
                              << " prev_consec_sl=" << m_consec_sl
                              << " reason=" << reason << "\n";
                    std::cout.flush();
                }
                m_consec_sl = 0;
                m_sl_kill_until_s = 0;
            }
        }

        pos              = OpenPos{};
        pending_both     = BracketBothSignals{};
        m_cooldown_start = nowSec();
        phase            = BracketPhase::COOLDOWN;
        if (on_close) on_close(tr);
    }

    void reset() noexcept {
        pos = OpenPos{};
        pending_both = BracketBothSignals{};
        bracket_high = 0.0; bracket_low = 0.0;
        m_locked_hi = m_locked_lo = 0.0;
        m_locked_long_sl = m_locked_long_tp = 0.0;
        m_locked_short_sl = m_locked_short_tp = 0.0;
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();
        m_armed_ts     = 0;
        m_l2_imbalance = 0.5;
        m_inside_ticks = 0;
        m_regime_flip_ticks = 0;
        phase = BracketPhase::IDLE;
    }
};

// ==============================================================================
class GoldBracketEngine final : public BracketEngineBase<GoldBracketEngine>
{
public:
    explicit GoldBracketEngine() noexcept { symbol = "XAUUSD"; ENTRY_SIZE = 0.01; }
    bool shouldTrade(double, double, double, double) const noexcept { return true; }
};

class BracketEngine final : public BracketEngineBase<BracketEngine>
{
public:
    explicit BracketEngine() noexcept = default;
    bool shouldTrade(double, double, double, double) const noexcept { return true; }
};

} // namespace omega

