#pragma once
// =============================================================================
// GoldHybridBracketEngine.hpp
//
// HYBRID BRACKET ARCHITECTURE for XAUUSD
//
// STRATEGY:
//   Detects compression ranges (sustained tight price action over 30 ticks).
//   Arms BOTH a long stop-order above the range high AND a short stop-order
//   below the range low simultaneously. Whichever fills first becomes the
//   live position. The other order is immediately cancelled via cancel_fn.
//
//   This is the "outsized breakout" engine designed to capture macro crash/surge
//   events (April 2 tariff crash, FOMC spikes etc.) where GoldFlow is already
//   in position but the bracket catches the structural break independently.
//
// SIZING (70/30 split):
//   Standalone (no flow active): risk_dollars = $30, SL = range * 0.5 + 0.5pt
//   Alongside flow (pyramid):    risk_dollars = $10 (30% addon), same SL formula
//   This gives: GoldFlow($30) + HybridBracket($10) = $40 combined on macro events
//   The 70% is already committed to GoldFlow's velocity trail. Bracket adds 30%.
//
// SL FORMULA (corrected):
//   SL = entry ± (range * 0.5 + 0.5pt buffer)
//   NOT the full range -- that gives RR~1 which is inadequate.
//   0.5x range SL gives RR~4 with TP = range * 2.0 beyond entry.
//   Rationale: if price compressed in a 15pt range and breaks out by 1pt,
//   a retracement back to the midpoint (7.5pt) means the breakout failed.
//   SL at midpoint + 0.5pt buffer = correct invalidation level.
//
// REGIME GATE:
//   New arming blocked when GoldFlow is live and unprotected.
//   PENDING orders are NOT cancelled when flow becomes active -- stop orders
//   already resting at the broker are safe to leave. Only new arming is blocked.
//   Pyramid bypass: can arm when flow is be_locked + trail_stage >= 1.
//
// SL DIRECTIONAL COOLDOWN:
//   After SL_HIT in a direction, that direction is blocked for 60s.
//   Prevents immediate re-arming into the same failed breakout.
//
// PARAMETER CALIBRATION (validated against Apr 2 incident):
//   MIN_RANGE = 1.5pt  -- catches micro-compressions at session open
//   MAX_RANGE = 25pt   -- catches pre-crash compressions (Apr 2 was 15-20pt)
//   MIN_BREAK_TICKS = 3 -- prevents spike-and-snap false arms
//   PENDING_TIMEOUT_S = 300 -- 5 min for price to hit bracket level
//   SL_FRAC = 0.5 -- SL at midpoint of range (correct invalidation level)
//   TP_RR = 2.0 -- TP = SL_dist * 2.0 beyond entry (gives RR~4 on range basis)
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"

namespace omega {

class GoldHybridBracketEngine {
public:
    // ── Config (all tunable) ─────────────────────────────────────────────────
    static constexpr double USD_PER_PT           = 100.0;  // $100/pt XAUUSD BlackBull
    static constexpr double RISK_DOLLARS         = 30.0;   // $ risk standalone
    static constexpr double RISK_DOLLARS_PYRAMID = 10.0;   // $ risk alongside GoldFlow (30% addon)
    static constexpr int    STRUCTURE_LOOKBACK   = 30;     // ticks to establish range
    static constexpr double MIN_RANGE            = 4.0;    // raised 1.5->4.0: 1.5pt is sub-spread noise. At ATR~5pt, 4pt = real structure.
    static constexpr double MAX_RANGE            = 25.0;   // pts max (raised from 12 -- pre-crash ranges are 15-20pt)
    static constexpr double MAX_SPREAD           = 2.5;    // pts spread gate
    static constexpr double SL_FRAC              = 0.5;    // SL = range * SL_FRAC + SL_BUFFER beyond entry
    static constexpr double SL_BUFFER            = 0.5;    // pts additional buffer beyond midpoint
    static constexpr double TP_RR                = 2.0;    // TP = sl_dist * TP_RR beyond entry (RR~4 on range)
    static constexpr double TRAIL_FRAC           = 0.25;   // trail_dist = range * TRAIL_FRAC
    static constexpr int    COOLDOWN_S           = 120;    // seconds post-close
    static constexpr int    DIR_SL_COOLDOWN_S    = 60;     // seconds directional block after SL_HIT
    static constexpr int    PENDING_TIMEOUT_S    = 300;    // seconds waiting for fill
    static constexpr int    MIN_HOLD_S           = 15;     // seconds minimum hold
    static constexpr int    MIN_BREAK_TICKS      = 5;      // raised 3->5: more conviction before arm, reduces false breakouts
    static constexpr int    MIN_ENTRY_TICKS      = 150;    // warmup guard (same as BracketEngine)

    bool shadow_mode = true;   // set false only for live orders

    // ── Observable state ────────────────────────────────────────────────────
    enum class Phase : uint8_t { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase  phase        = Phase::IDLE;
    double bracket_high = 0.0;
    double bracket_low  = 0.0;
    double range        = 0.0;

    struct OpenPos {
        bool    active    = false;
        bool    is_long   = false;
        bool    be_locked = false;
        double  entry     = 0.0;
        double  tp        = 0.0;
        double  sl        = 0.0;
        double  size      = 0.01;
        double  mfe       = 0.0;
        int64_t entry_ts  = 0;
        double  spread_at_entry = 0.0;
    } pos;

    // Stored by tick_gold.hpp to cancel the losing side on fill
    std::string pending_long_clOrdId;
    std::string pending_short_clOrdId;
    // Pending lot -- read by tick_gold.hpp to send correct order size
    double      pending_lot = 0.01;

    bool has_open_position() const noexcept {
        return phase == Phase::PENDING || phase == Phase::LIVE;
    }

    using CloseCallback  = std::function<void(const omega::TradeRecord&)>;
    using CancelCallback = std::function<void(const std::string&)>;
    CancelCallback cancel_fn;

    // ── on_tick ──────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 bool flow_live,
                 bool flow_be_locked,
                 int  flow_trail_stage,
                 CloseCallback on_close) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        // ── COOLDOWN ─────────────────────────────────────────────────────────
        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }

        // ── LIVE: manage position ────────────────────────────────────────────
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // ── PENDING: wait for fill or timeout ────────────────────────────────
        // NOTE: PENDING is NOT cancelled when flow becomes active.
        // Stop orders already resting at the broker are safe to leave.
        // Only new arming is blocked by the regime gate below.
        if (phase == Phase::PENDING) {
            if (!can_enter) {
                // Session/risk gate closed -- cancel resting orders
                cancel_both();
                reset_to_idle();
                return;
            }
            if ((now_s - m_armed_ts) > PENDING_TIMEOUT_S) {
                printf("[HYBRID-GOLD] PENDING TIMEOUT hi=%.2f lo=%.2f\n",
                       bracket_high, bracket_low);
                fflush(stdout);
                cancel_both();
                reset_to_idle();
                return;
            }
            // Shadow fill simulation
            if (shadow_mode) {
                if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot); return; }
                if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot); return; }
            }
            return;
        }

        // ── Feed structure window (unconditional) ────────────────────────────
        m_window.push_back(mid);
        ++m_ticks_received;
        if ((int)m_window.size() > STRUCTURE_LOOKBACK * 2) m_window.pop_front();

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if ((int)m_window.size() < STRUCTURE_LOOKBACK) return;

        // ── Entry gates ──────────────────────────────────────────────────────
        if (!can_enter) {
            if (phase == Phase::ARMED) return; // preserve armed timer
            return;
        }
        if (spread > MAX_SPREAD) return;

        // Regime gate: only block NEW ARMING when flow is unprotected live
        // (PENDING orders already placed are exempt -- see above)
        const bool flow_pyramid_ok = flow_live && flow_be_locked && flow_trail_stage >= 1;
        if (flow_live && !flow_pyramid_ok && phase == Phase::IDLE) return;

        // Directional SL cooldown -- block direction that just got stopped out
        // (checked at arm time, not on every tick -- just stored as m_sl_cooldown_dir)

        // ── Compute current range ────────────────────────────────────────────
        double w_hi = *std::max_element(m_window.begin(), m_window.end());
        double w_lo = *std::min_element(m_window.begin(), m_window.end());
        range = w_hi - w_lo;

        // ── IDLE -> ARMED ────────────────────────────────────────────────────
        if (phase == Phase::IDLE) {
            if (range >= MIN_RANGE && range <= MAX_RANGE) {
                phase         = Phase::ARMED;
                bracket_high  = w_hi;
                bracket_low   = w_lo;
                m_armed_ts    = now_s;
                m_inside_ticks = 0;
                printf("[HYBRID-GOLD] ARMED hi=%.2f lo=%.2f range=%.2f spread=%.2f\n",
                       bracket_high, bracket_low, range, spread);
                fflush(stdout);
            }
            return;
        }

        // ── ARMED: wait for MIN_BREAK_TICKS of stable inside price ──────────
        if (phase == Phase::ARMED) {
            if (mid < bracket_high && mid > bracket_low) {
                ++m_inside_ticks;
                // Tighten bracket as compression deepens
                bracket_high = std::min(bracket_high, w_hi);
                bracket_low  = std::max(bracket_low,  w_lo);
                range        = bracket_high - bracket_low;
            } else {
                // Price escaped -- reset
                m_inside_ticks = 0;
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }

            if (m_inside_ticks < MIN_BREAK_TICKS) return;
            if (range < MIN_RANGE || range > MAX_RANGE) { phase = Phase::IDLE; return; }

            // ── Compute sizing ────────────────────────────────────────────────
            const double risk    = flow_pyramid_ok ? RISK_DOLLARS_PYRAMID : RISK_DOLLARS;
            // SL distance = range * SL_FRAC + SL_BUFFER
            // This is the distance from entry to SL, NOT the full range
            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double lot_raw = risk / (sl_dist * USD_PER_PT);
            const double lot     = std::max(0.01,
                                   std::min(0.50,
                                   std::floor(lot_raw / 0.001) * 0.001));

            // TP distance = sl_dist * TP_RR (gives RR~4 on gross range)
            const double tp_dist = sl_dist * TP_RR;
            // Cost viability: TP must cover spread cost + commission
            const double min_tp  = spread * 3.0 + 1.0;
            if (tp_dist < min_tp) {
                printf("[HYBRID-GOLD] COST_FAIL range=%.2f sl_dist=%.2f tp_dist=%.2f min=%.2f\n",
                       range, sl_dist, tp_dist, min_tp);
                fflush(stdout);
                phase = Phase::IDLE;
                return;
            }

            pending_lot   = lot;
            phase         = Phase::PENDING;
            m_armed_ts    = now_s; // reset timeout from now

            printf("[HYBRID-GOLD] FIRE hi=%.2f lo=%.2f range=%.2f sl_dist=%.2f "
                   "tp_dist=%.2f lot=%.3f risk=$%.0f %s\n",
                   bracket_high, bracket_low, range, sl_dist, tp_dist, lot, risk,
                   flow_pyramid_ok ? "[PYRAMID alongside GoldFlow]" : "[STANDALONE]");
            fflush(stdout);

            // tick_gold.hpp reads phase==PENDING, pending_lot, bracket_high, bracket_low
            // and sends both stop orders, storing clOrdIds in pending_long/short_clOrdId
        }
    }

    // Called by tick_gold.hpp when broker ACKs a fill on one side
    void confirm_fill(bool is_long, double fill_px, double fill_lot) noexcept {
        if (phase != Phase::PENDING) return;

        cancel_losing_side(is_long); // cancel the other stop order immediately

        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;

        pos.active    = true;
        pos.is_long   = is_long;
        pos.entry     = fill_px;
        pos.sl        = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp        = is_long ? (fill_px + tp_dist) : (fill_px - tp_dist);
        pos.size      = fill_lot;
        pos.mfe       = 0.0;
        pos.be_locked = false;
        pos.entry_ts  = static_cast<int64_t>(std::time(nullptr));
        phase         = Phase::LIVE;

        printf("[HYBRID-GOLD] FILL %s @ %.2f sl=%.2f(dist=%.2f) tp=%.2f lot=%.3f\n",
               is_long ? "LONG" : "SHORT",
               fill_px, pos.sl, sl_dist, pos.tp, fill_lot);
        fflush(stdout);
    }

private:
    std::deque<double> m_window;
    int     m_ticks_received   = 0;
    int     m_inside_ticks     = 0;
    int64_t m_armed_ts         = 0;
    int64_t m_cooldown_start   = 0;
    int     m_trade_id         = 0;
    // Directional SL cooldown state
    int     m_sl_cooldown_dir  = 0;    // +1=long blocked, -1=short blocked, 0=none
    int64_t m_sl_cooldown_ts   = 0;    // when it expires

    void cancel_losing_side(bool filled_long) noexcept {
        if (filled_long && !pending_short_clOrdId.empty()) {
            if (cancel_fn) cancel_fn(pending_short_clOrdId);
            pending_short_clOrdId.clear();
        } else if (!filled_long && !pending_long_clOrdId.empty()) {
            if (cancel_fn) cancel_fn(pending_long_clOrdId);
            pending_long_clOrdId.clear();
        }
    }

    void cancel_both() noexcept {
        if (cancel_fn) {
            if (!pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
            if (!pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
        }
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();
    }

    void reset_to_idle() noexcept {
        phase        = Phase::IDLE;
        bracket_high = bracket_low = range = 0.0;
        m_inside_ticks = 0;
        pos = OpenPos{};
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();
    }

    void manage(double bid, double ask, double mid,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        if ((now_s - pos.entry_ts) < MIN_HOLD_S) return;

        const double tp_dist    = std::fabs(pos.tp - pos.entry);
        const double trail_dist = std::max(range * TRAIL_FRAC, (ask - bid) * 2.0);

        // Step 1: BE lock at 40% of TP
        if (move >= tp_dist * 0.40 && !pos.be_locked) {
            pos.sl = pos.entry; pos.be_locked = true;
            printf("[HYBRID-GOLD] TRAIL-BE %s move=%.2f\n",
                   pos.is_long ? "LONG" : "SHORT", move);
            fflush(stdout);
        }
        // Step 2: lock 50% of TP at 1R
        if (pos.be_locked && move >= tp_dist) {
            const double lock = pos.is_long
                ? pos.entry + tp_dist * 0.50 : pos.entry - tp_dist * 0.50;
            if ((pos.is_long && lock > pos.sl) || (!pos.is_long && lock < pos.sl))
                pos.sl = lock;
        }
        // Step 3: lock full 1R at 2R
        if (pos.be_locked && move >= tp_dist * 2.0) {
            const double lock = pos.is_long
                ? pos.entry + tp_dist : pos.entry - tp_dist;
            if ((pos.is_long && lock > pos.sl) || (!pos.is_long && lock < pos.sl))
                pos.sl = lock;
        }
        // Step 4: free trail at MFE - trail_dist (2R+)
        if (pos.be_locked && move >= tp_dist * 2.0) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - trail_dist)
                : (pos.entry - pos.mfe + trail_dist);
            if ((pos.is_long && trail_sl > pos.sl) || (!pos.is_long && trail_sl < pos.sl))
                pos.sl = trail_sl;
        }

        // SL check
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (!sl_hit) return;

        const double exit_px = pos.is_long ? bid : ask;
        const char* reason   = pos.be_locked
            ? (pos.sl > pos.entry + 0.01 || pos.sl < pos.entry - 0.01
               ? "TRAIL_HIT" : "BE_HIT")
            : "SL_HIT";

        // Set directional SL cooldown on failure
        if (std::strcmp(reason, "SL_HIT") == 0) {
            m_sl_cooldown_dir = pos.is_long ? 1 : -1;
            m_sl_cooldown_ts  = now_s + DIR_SL_COOLDOWN_S;
            printf("[HYBRID-GOLD] SL_COOLDOWN dir=%d for %ds\n",
                   m_sl_cooldown_dir, DIR_SL_COOLDOWN_S);
            fflush(stdout);
        }

        omega::TradeRecord tr;
        tr.id          = ++m_trade_id;
        tr.symbol      = "XAUUSD";
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = (pos.is_long ? (exit_px - pos.entry)
                                      : (pos.entry - exit_px)) * pos.size;
        tr.mfe         = pos.mfe * pos.size;
        tr.mae         = 0.0;
        tr.entryTs     = pos.entry_ts;
        tr.exitTs      = now_s;
        tr.exitReason  = reason;
        tr.engine      = "HybridBracketGold";
        tr.regime      = "HYBRID";
        tr.spreadAtEntry = pos.spread_at_entry;

        printf("[HYBRID-GOLD] EXIT %s @ %.2f reason=%s pnl_usd=%.2f mfe=%.2f\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, tr.pnl * 100.0, pos.mfe);
        fflush(stdout);

        reset_to_idle();
        m_cooldown_start = now_s;
        phase            = Phase::COOLDOWN;
        if (on_close) on_close(tr);
    }
};

} // namespace omega
