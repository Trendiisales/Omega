#pragma once
// ==============================================================================
// BracketEngine — CRTP true-bracket breakout engine
//
// TRUE BRACKET behaviour:
//   Once a structural range is locked, BOTH sides are armed simultaneously.
//   The engine emits TWO pending signals (long above high, short below low).
//   main.cpp sends BOTH stop orders to the broker.
//   Whichever fills first → that becomes the live position.
//   The other order is cancelled via the stored clOrdId.
//
// State machine:
//   IDLE     → not enough data / range too small
//   ARMED    → bracket locked, MIN_STRUCTURE_MS timer running
//   PENDING  → both orders sent, waiting for first fill via confirm_fill()
//   LIVE     → one side filled, other cancelled, managing position
//   COOLDOWN → post-close cooldown
//
// Used by: GoldBracketEngine (GOLD.F), SilverBracketEngine (XAGUSD)
// ==============================================================================

#include <deque>
#include <string>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
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

// Both-sides signal — main.cpp sends two stop orders when this is non-empty
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
    // ── Config ────────────────────────────────────────────────────────────────
    int    STRUCTURE_LOOKBACK  = 30;
    double BUFFER              = 0.3;
    double RR                  = 1.5;
    int    COOLDOWN_MS         = 120000;
    double MIN_RANGE           = 0.0;
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
    // PENDING_TIMEOUT_SEC: how long to wait for price to hit a bracket level.
    // LIVE mode: 60s is fine — broker holds the stop orders, we just wait for fill ACK.
    // SHADOW mode: price must cross the level within this window for fill simulation.
    // Gold often consolidates 2-5 min after compressing — 60s was expiring before break.
    // Default 300s (5 min). Set per-engine in main.cpp after configure().
    int    PENDING_TIMEOUT_SEC = 300;
    const char* symbol         = "???";
    bool   shadow_mode         = false;  // set by main.cpp — enables price-triggered fill sim in PENDING

    // ── Observable state ──────────────────────────────────────────────────────
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

    // ── configure() ──────────────────────────────────────────────────────────
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

    BracketBothSignals get_signals() noexcept {
        BracketBothSignals out = pending_both;
        pending_both = BracketBothSignals{};
        return out;
    }

    // ── on_tick() ─────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask, long long /*ts_ms*/,
                 bool can_enter,
                 const char* macro_regime,
                 CloseCallback on_close,
                 double vwap = 0.0,
                 double l2_imbalance = 0.5) noexcept
    {
        // Store latest L2 imbalance — used in arm_both_sides() for direction bias logging
        m_l2_imbalance = l2_imbalance;
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid        = (bid + ask) * 0.5;
        const double spread     = ask - bid;
        const double spread_pct = (mid > 0.0) ? (spread / mid * 100.0) : 999.0;
        const int64_t now       = nowSec();

        // ── COOLDOWN ──────────────────────────────────────────────────────────
        if (phase == BracketPhase::COOLDOWN) {
            if (now - m_cooldown_start >= static_cast<int64_t>(COOLDOWN_MS / 1000))
                phase = BracketPhase::IDLE;
            else return;
        }

        // ── LIVE: manage open position ────────────────────────────────────────
        if (phase == BracketPhase::LIVE) {
            if (!pos.active) return;
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if ( move > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            // Breakout failure: price re-crosses midpoint of bracket
            if (FAILURE_WINDOW_MS > 0 &&
                (now - pos.entry_ts) < static_cast<int64_t>(FAILURE_WINDOW_MS / 1000)) {
                const double bracket_mid = (m_locked_hi + m_locked_lo) * 0.5;
                if ( pos.is_long && bid < bracket_mid) {
                    closePos(bid, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
                if (!pos.is_long && ask > bracket_mid) {
                    closePos(ask, "BREAKOUT_FAIL", macro_regime, on_close); return;
                }
            }

            if ((now - pos.entry_ts) < static_cast<int64_t>(MIN_HOLD_MS / 1000)) return;

            // ── Stepped trailing stop — rides multi-hour trends ──────────────
            // Instead of a fixed TP that exits at 1R, we use a stepped trail:
            //   Step 1 (40% of TP dist):  SL → breakeven. Position is free.
            //   Step 2 (100% = 1R):       SL → entry + 50% of TP dist. Lock half.
            //   Step 3 (200% = 2R):       SL → entry + 100% of TP dist. Lock full 1R.
            //   Step 4 (300%+ = 3R+):     Trail SL at MFE - trail_dist (25% of initial range).
            //                              This is the "ride the cascade" zone.
            // There is NO fixed TP — position runs until trail stop is hit.
            // On a $6 range / RR=3.0 initial setup:
            //   trail_dist = 6 * 0.25 = $1.50 trail
            //   At 1R ($18 in): SL → $9 locked
            //   At 2R ($36 in): SL → $18 locked
            //   At 3R ($54 in): SL trails $1.50 behind MFE
            //   At $100 move:   SL at ~$98.50 behind entry — position stays open all day
            {
                const double initial_range = std::fabs(m_locked_hi - m_locked_lo);
                const double trail_dist    = std::max(initial_range * 0.25, spread * 2.0);
                const double tp_dist       = std::fabs(pos.tp - pos.entry); // initial target dist
                const double trail_move    = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);

                if (trail_move > 0.0) {
                    if (trail_move > pos.mfe) pos.mfe = trail_move;  // track max favourable

                    // Step 1: BE lock at 40% of initial target
                    if (trail_move >= tp_dist * 0.40 && !pos.sl_locked_to_be) {
                        if ( pos.is_long && pos.entry > pos.sl) {
                            pos.sl = pos.entry; pos.sl_locked_to_be = true;
                            std::cout << "[BRACKET-" << symbol << "] TRAIL-STEP1 SL->BE move=" << trail_move << "\n";
                        }
                        if (!pos.is_long && pos.entry < pos.sl) {
                            pos.sl = pos.entry; pos.sl_locked_to_be = true;
                            std::cout << "[BRACKET-" << symbol << "] TRAIL-STEP1 SL->BE move=" << move << "\n";
                        }
                    }
                    // Step 2: Lock 50% of initial TP at 1R
                    if (trail_move >= tp_dist && pos.sl_locked_to_be) {
                        const double lock2 = pos.is_long
                            ? pos.entry + tp_dist * 0.50
                            : pos.entry - tp_dist * 0.50;
                        if ((pos.is_long && lock2 > pos.sl) || (!pos.is_long && lock2 < pos.sl)) {
                            pos.sl = lock2;
                            std::cout << "[BRACKET-" << symbol << "] TRAIL-STEP2 lock_half move=" << trail_move << "\n";
                        }
                    }
                    // Step 3: Lock full 1R at 2R
                    if (trail_move >= tp_dist * 2.0 && pos.sl_locked_to_be) {
                        const double lock3 = pos.is_long
                            ? pos.entry + tp_dist
                            : pos.entry - tp_dist;
                        if ((pos.is_long && lock3 > pos.sl) || (!pos.is_long && lock3 < pos.sl)) {
                            pos.sl = lock3;
                            std::cout << "[BRACKET-" << symbol << "] TRAIL-STEP3 lock_1R move=" << trail_move << "\n";
                        }
                    }
                    // Step 4: Free-running trail at MFE - trail_dist (3R+)
                    if (trail_move >= tp_dist * 3.0 && pos.sl_locked_to_be) {
                        const double trail_sl = pos.is_long
                            ? (pos.entry + pos.mfe - trail_dist)
                            : (pos.entry - pos.mfe + trail_dist);
                        if ((pos.is_long && trail_sl > pos.sl) || (!pos.is_long && trail_sl < pos.sl)) {
                            pos.sl = trail_sl;
                            std::cout << "[BRACKET-" << symbol << "] TRAIL-STEP4 mfe_trail=" << pos.mfe
                                      << " sl=" << pos.sl << "\n";
                        }
                    }
                }
            }

            // No fixed TP — trail stop exits the position
            // SL check handles all exits: initial SL, BE, stepped locks, and trail
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

        // ── PENDING: both orders out, waiting for first fill ──────────────────
        if (phase == BracketPhase::PENDING) {
            if (!can_enter) {
                std::cout << "[BRACKET-" << symbol << "] PENDING CANCELLED — session/risk gate closed\n";
                std::cout.flush();
                cancel_both_broker_orders();
                reset();
                return;
            }
            if ((now - m_armed_ts) > static_cast<int64_t>(PENDING_TIMEOUT_SEC)) {
                std::cout << "[BRACKET-" << symbol << "] PENDING TIMEOUT after " << PENDING_TIMEOUT_SEC
                          << "s — price never hit bracket hi=" << m_locked_hi
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

        // ── Entry gate ────────────────────────────────────────────────────────
        // While ARMED: preserve the timer — do NOT reset m_armed_ts.
        // A can_enter=false blip means we can't arm new structure, but
        // structure that already qualified should not lose its timer.
        if (!can_enter) {
            if (phase == BracketPhase::ARMED) {
                // Timer preserved — do not touch m_armed_ts
                std::cout << "[BRACKET-" << symbol << "] ARMED HOLD — can_enter=false blip, timer preserved\n";
                std::cout.flush();
                return;
            }
            phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // ── Maintain structure window ─────────────────────────────────────────
        m_window.push_back(mid);
        if (static_cast<int>(m_window.size()) > STRUCTURE_LOOKBACK * 2)
            m_window.pop_front();
        if (static_cast<int>(m_window.size()) < STRUCTURE_LOOKBACK) return;

        // ── ATR ───────────────────────────────────────────────────────────────
        if (ATR_PERIOD > 0) {
            m_atr_window.push_back(spread);
            if (static_cast<int>(m_atr_window.size()) > ATR_PERIOD * 3)
                m_atr_window.pop_front();
            if (static_cast<int>(m_atr_window.size()) >= ATR_PERIOD) {
                double s = 0.0;
                const int n = static_cast<int>(m_atr_window.size());
                for (int i = n - ATR_PERIOD; i < n; ++i) s += m_atr_window[i];
                atr = s / ATR_PERIOD;
            }
        }
        const double eff_min_range = (ATR_RANGE_K > 0.0 && atr > 0.0)
                                     ? std::max(atr * ATR_RANGE_K, MIN_RANGE)
                                     : MIN_RANGE;

        // ── Structural range ──────────────────────────────────────────────────
        const int    wsz    = static_cast<int>(m_window.size());
        const auto   wbegin = m_window.begin() + (wsz - STRUCTURE_LOOKBACK);
        const auto   wend   = m_window.end() - 1;
        const double shi    = *std::max_element(wbegin, wend);
        const double slo    = *std::min_element(wbegin, wend);
        const double range  = shi - slo;

        if (range < eff_min_range) {
            // While ARMED: hold state — don't reset timer on transient range collapse.
            // Rolling window shrinks naturally as trend pushes through it.
            if (phase != BracketPhase::ARMED) {
                phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            }
            return;
        }

        // shouldTrade() gate — only resets to IDLE from IDLE, not from ARMED.
        // If we are already ARMED (structure was valid), a single-tick shouldTrade
        // failure (e.g. transient spread spike) must not destroy the timer.
        if (!static_cast<Derived*>(this)->shouldTrade(bid, ask, spread_pct, vwap)) {
            if (phase != BracketPhase::ARMED) {
                phase = BracketPhase::IDLE; bracket_high = 0.0; bracket_low = 0.0;
            }
            return;
        }

        const double buf = spread * 0.5;
        bracket_high = shi + buf;
        bracket_low  = slo - buf;

        // ── IDLE → ARMED ──────────────────────────────────────────────────────
        if (phase == BracketPhase::IDLE) {
            phase      = BracketPhase::ARMED;
            m_armed_ts = nowSec();
            std::cout << "[BRACKET-" << symbol << "] ARMED"
                      << " hi=" << bracket_high << " lo=" << bracket_low
                      << " range=" << range << "\n";
            std::cout.flush();
            return;
        }

        // ── ARMED → PENDING: fire both orders once structure has held ─────────
        if (phase == BracketPhase::ARMED) {
            if (MIN_STRUCTURE_MS > 0 &&
                (nowSec() - m_armed_ts) < static_cast<int64_t>(MIN_STRUCTURE_MS / 1000))
                return;
            arm_both_sides(spread, macro_regime);
        }
    }

    // ── confirm_fill() ────────────────────────────────────────────────────────
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
        // One side rejected — cancel the other side on the broker then reset
        std::cout << "[BRACKET-" << symbol << "] REJECTED — cancelling other leg and resetting\n";
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
    // ── cancel_both_broker_orders() ───────────────────────────────────────────
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
    int64_t m_cooldown_start = 0;
    int64_t m_armed_ts       = 0;
    int     m_trade_id       = 0;
    double  m_l2_imbalance   = 0.5;  // last L2 bid/(bid+ask) imbalance — >0.65 bid-heavy, <0.35 ask-heavy

    // Locked at the moment both orders are sent — never change after PENDING
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

        // ── Max spread gate ──────────────────────────────────────────────────
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

        // ── Hard range floor ─────────────────────────────────────────────────
        // dist is the SL distance for both legs. Must meet absolute minimum.
        if (dist < MIN_RANGE) {
            std::cout << "[BRACKET-" << symbol << "] BLOCKED: range_too_small"
                      << " dist=" << dist << " min=" << MIN_RANGE << "\n";
            std::cout.flush();
            phase = BracketPhase::IDLE;
            bracket_high = 0.0; bracket_low = 0.0;
            return;
        }

        // ── Spread viability check (mandatory, runs before any other calc) ───
        // A trade is only viable if the SL distance covers:
        //   - entry spread (paid on open)
        //   - exit spread (paid on close)
        //   - slippage on both sides
        // If dist <= round_trip_cost the trade has negative expectancy before
        // it even moves — spread alone will push it to SL.
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
        if (macro_regime) strncpy_s(pos.regime, macro_regime, 31);

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

        // L2 imbalance direction note — informational, does not gate the trade.
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
                                        : (pos.entry - exit_px)) * pos.size;
        tr.mfe           = pos.mfe;
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
        phase = BracketPhase::IDLE;
    }
};

// ==============================================================================
class GoldBracketEngine final : public BracketEngineBase<GoldBracketEngine>
{
public:
    explicit GoldBracketEngine() noexcept { symbol = "GOLD.F"; ENTRY_SIZE = 0.01; }
    bool shouldTrade(double, double, double, double) const noexcept { return true; }
};

class SilverBracketEngine final : public BracketEngineBase<SilverBracketEngine>
{
public:
    explicit SilverBracketEngine() noexcept { symbol = "XAGUSD"; ENTRY_SIZE = 0.01; }
    bool shouldTrade(double, double, double, double) const noexcept { return true; }
};

class BracketEngine final : public BracketEngineBase<BracketEngine>
{
public:
    explicit BracketEngine() noexcept = default;
    bool shouldTrade(double, double, double, double) const noexcept { return true; }
};

} // namespace omega
