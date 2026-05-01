#pragma once
#include <iomanip>
#include <iostream>
#include "SpreadRegimeGate.hpp"
// =============================================================================
// GoldMidScalperEngine.hpp  --  Mid-band compression bracket for XAUUSD
// =============================================================================
//
// 2026-05-01 SESSION_h DESIGN (Claude / Jo):
//   Sister engine to GoldHybridBracketEngine. Same algorithmic skeleton,
//   different parameter band. Purpose: capture the $20-40 P&L zone that
//   neither GoldHybridBracket (~$6-12 captures) nor GoldBracket
//   (~$7-24 captures, but rarely fires after S22c MAX_SL_DIST_PTS=6 cap)
//   reliably hits in current XAUUSD volatility.
//
//   Math: with SL_dist = range*0.5 + 0.5 and TP_RR = 4.0:
//     range $ 8 -> SL $4.5  -> TP $18
//     range $10 -> SL $5.5  -> TP $22
//     range $12 -> SL $6.5  -> TP $26
//     range $15 -> SL $8.0  -> TP $32
//     range $18 -> SL $9.5  -> TP $38
//     range $20 -> SL $10.5 -> TP $42
//
//   STRUCTURE_LOOKBACK = 300 (~90 sec @ 200 ticks/min) finds these wider
//   compressions that a 120-tick window misses.
//
// SAFETY:
//   - Defaults to shadow_mode = true. Live promotion requires explicit
//     authorisation after a 2-week paper run shows positive expectancy.
//   - Uses MIN_BREAK_TICKS = 5 (matches GoldBracket S22c sweep guard).
//   - Inherits all audit-validated guards from HybridGold lineage:
//       S20 trail-arm guards (MIN_TRAIL_ARM_PTS=5.0, MIN_TRAIL_ARM_SECS=15)
//       S43 mae tracker
//       S47 T4a ATR-expansion gate (EXPANSION_MULT=1.10) with ratchet fix
//       S51 1A.1.a spread_at_entry capture
//       S52 MFE_TRAIL_FRAC = 0.55 (S53 2026-05-01 raised 0.40 -> 0.55)
//       S53 2026-05-01 trail-arm bump: 3.0 -> 5.0 (see constant block below)
//       AUDIT 2026-04-29 mutex on _close path
//       audit-fixes-18 SpreadRegimeGate per-engine
//
// DOM FILTER (inherited from HybridGold):
//   At PENDING->FIRE time, DOM confirms which side has a clear path:
//     book_slope > SLOPE_CONFIRM: buy pressure -> LONG order gets lot bonus (1.3x)
//     book_slope < -SLOPE_CONFIRM: sell pressure -> SHORT order gets lot bonus (1.3x)
//     vacuum_ask: ask side thin -> LONG path clear -> LONG lot bonus
//     vacuum_bid: bid side thin -> SHORT path clear -> SHORT lot bonus
//   Wall gates at entry:
//     If wall_above AND wall_below: no clear path either side -> skip fire
//     If wall_above only: reduce LONG lot by 50% (ceiling blocks TP)
//     If wall_below only: reduce SHORT lot by 50%
//   When l2_real=false: DOM filter bypassed (safe fallback, both sides equal lots)
//
// SIZING:
//   Standalone (no flow active): risk_dollars = $30, SL = range * 0.5 + 0.5pt
//   Alongside flow (pyramid):    risk_dollars = $10 (30% addon), same SL formula
//   DOM lot bonus applied AFTER risk sizing, before max_lot cap (0.50)
//   Live lot capped to 0.01 per FIX 2026-04-22 uniformity.
//
// LOG NAMESPACE:
//   All log lines use prefix [MID-SCALPER-GOLD] / [MID-SCALPER-GOLD-DIAG].
//   tr.engine = "MidScalperGold" (distinct from HybridBracketGold).
//   tr.regime = "MID_COMPRESSION".
// =============================================================================

#include <cstdint>
#include <mutex>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <string>
#include <deque>
#include <vector>
#include "OmegaTradeLedger.hpp"

namespace omega {

class GoldMidScalperEngine {
public:
    // -- Parameters (tuned 2026-05-01 SESSION_h for $20-40 capture) ----------
    // Lookback window: 300 ticks ~= 90 sec at 200 ticks/min XAUUSD.
    //   Long enough to find genuine $8+ compressions that a 120-tick window
    //   misses, short enough to fire several times per session.
    static constexpr int    STRUCTURE_LOOKBACK   = 300;
    // Warmup: refuse new arming until at least this many ticks have arrived.
    //   Prevents arming on cold-start window noise.
    static constexpr int    MIN_ENTRY_TICKS      = 30;
    // Sweep guard: price must sit inside the formed bracket for this many
    //   consecutive ticks before stop orders are sent. Matches GoldBracket
    //   S22c value -- protects against the same single-tick sweep pattern.
    static constexpr int    MIN_BREAK_TICKS      = 5;
    // $20-40 capture band. Lower edge = $18 TP after RR4; upper edge = $42.
    static constexpr double MIN_RANGE            = 8.0;
    static constexpr double MAX_RANGE            = 20.0;
    static constexpr double SL_FRAC              = 0.5;
    static constexpr double SL_BUFFER            = 0.5;
    // RR=4: TP = sl_dist * 4. Drives the $18-42 capture envelope.
    //   HybridGold uses RR=2 for $6-12 scalps; the wider $20-40 band needs
    //   the asymmetric reward to compensate for lower fire frequency.
    static constexpr double TP_RR                = 4.0;
    static constexpr double TRAIL_FRAC           = 0.25;
    // Trail-arm guards inherited from HybridGold S20 (2026-04-25).
    // S53 2026-05-01 (SESSION_h trade-quality follow-up): raised 3.0 -> 5.0
    //   in lockstep with HBG (same shadow tape, same trail-too-tight pattern;
    //   MidScalper TP target $20-42 means the engine wants the trade to MFE
    //   significantly before any trail-side close is possible). If MidScalper
    //   data later shows trail still firing too early on the larger
    //   $20-40 captures, raise this to 7.0 independently.
    static constexpr double MIN_TRAIL_ARM_PTS    = 5.0;
    static constexpr int    MIN_TRAIL_ARM_SECS   = 15;
    // S52 MFE give-back: 0.40 (preserve 60% of run, survive normal noise).
    // S53 2026-05-01 (SESSION_h same audit): raised 0.40 -> 0.55 alongside HBG.
    static constexpr double MFE_TRAIL_FRAC       = 0.55;
    // S53 2026-05-01 (SESSION_h): break-even lock trigger.
    //   Move SL to entry once MFE >= BE_TRIGGER_PTS. Fills the gap between
    //   the original SL and MIN_TRAIL_ARM_PTS=5.0 -- trades that MFE 3-5pt
    //   then reverse exit at $0 instead of taking the original SL.
    static constexpr double BE_TRIGGER_PTS       = 3.0;
    // S53 2026-05-01 (SESSION_h): same-level re-arm block.
    //   Mirrors the IndexHybridBracketEngine SAME_LEVEL_BLOCK pattern.
    //   After an exit, block re-arming when the new compression's hi or lo
    //   falls within SAME_LEVEL_BLOCK_PTS of the prior exit price for the
    //   configured timeout. Loss exits block longer than wins. BE_HIT
    //   does NOT stamp -- it carries no directional signal.
    //
    //   Continuation: once price has moved beyond the block radius, re-arm
    //   is allowed -- trend continuation is captured naturally on the new
    //   structure. Threshold of 8pt is chosen relative to MIN_RANGE=8.0:
    //   a fresh structure of width 8 cannot overlap a prior exit unless
    //   one of its edges is within 8 of the exit price.
    static constexpr double SAME_LEVEL_BLOCK_PTS         = 8.0;
    static constexpr int    SAME_LEVEL_POST_SL_BLOCK_S   = 900;  // 15 min after SL
    static constexpr int    SAME_LEVEL_POST_WIN_BLOCK_S  = 600;  // 10 min after TP/TRAIL
    static constexpr double MAX_SPREAD           = 2.5;
    static constexpr double RISK_DOLLARS         = 30.0;
    static constexpr double RISK_DOLLARS_PYRAMID = 10.0;
    static constexpr double USD_PER_PT           = 100.0;
    // Pending timeout: longer compressions deserve longer wait for breakout.
    //   HybridGold uses 30s for fast scalps; mid-band needs 120s.
    static constexpr int    PENDING_TIMEOUT_S    = 120;
    // Cooldown after close: longer than HBG (60s) to avoid stacking trades
    //   on the same compression structure.
    static constexpr int    COOLDOWN_S           = 180;
    // DIR_SL_COOLDOWN_S: pre-S53 dead-code constant. m_sl_cooldown_ts was
    //   set on SL_HIT but never read anywhere. The S53 same-level block
    //   above supersedes this with a working 15-minute post-SL guard
    //   keyed on m_sl_price overlap. Constant kept for backwards reference.
    static constexpr int    DIR_SL_COOLDOWN_S    = 240;
    static constexpr double DOM_SLOPE_CONFIRM    = 0.15;
    static constexpr double DOM_LOT_BONUS        = 1.3;
    static constexpr double DOM_WALL_PENALTY     = 0.5;

    // ATR-expansion gate (S47 T4a, ratchet fix 2026-04-29).
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    // 2026-05-01 SESSION_h: shadow ON by default. Promote to live ONLY after
    //   a clean 2-week paper validation showing positive expectancy in the
    //   $20-40 capture zone. Live promotion via engine_init.hpp override --
    //   do NOT change this default in the header.
    bool  shadow_mode = true;

    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = 0.01;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts = 0;
        // S53 2026-05-01 (SESSION_h trade-quality): break-even lock flag.
        //   Set true once MFE has crossed BE_TRIGGER_PTS and SL has been
        //   moved to entry. One-shot -- prevents repeated BE moves.
        bool    be_locked = false;
    } pos;

    double bracket_high  = 0.0;
    double bracket_low   = 0.0;
    double range         = 0.0;
    double pending_lot   = 0.01;
    double pending_lot_long  = 0.01;
    double pending_lot_short = 0.01;

    std::string pending_long_clOrdId;
    std::string pending_short_clOrdId;
    std::function<void(const std::string&)> cancel_fn;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // -- Main tick ------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 bool flow_live,
                 bool flow_be_locked,
                 int  flow_trail_stage,
                 CloseCallback on_close,
                 // DOM data (defaulted for backward compat)
                 double book_slope  = 0.0,
                 bool   vacuum_ask  = false,
                 bool   vacuum_bid  = false,
                 bool   wall_above  = false,
                 bool   wall_below  = false,
                 bool   l2_real     = false) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        // SpreadRegimeGate: feed every tick; consult only on new-entry path.
        m_spread_gate.on_tick(now_ms, spread);
#ifndef OMEGA_BACKTEST
        m_spread_gate.set_macro_regime(g_macroDetector.regime());
#endif

        m_last_tick_s = now_s;

        ++m_ticks_received;
        m_window.push_back(mid);
        if ((int)m_window.size() > STRUCTURE_LOOKBACK * 2) m_window.pop_front();

        // Warmup diag every 300 ticks + first 30
        if (m_ticks_received % 300 == 0 || m_ticks_received <= 30) {
            double live_range = 0.0;
            const int window_needed = STRUCTURE_LOOKBACK;
            const bool window_ok = ((int)m_window.size() >= window_needed);
            if (window_ok) {
                auto it_hi = std::max_element(m_window.begin(), m_window.end());
                auto it_lo = std::min_element(m_window.begin(), m_window.end());
                live_range = *it_hi - *it_lo;
            }
            {
                char _buf[512];
                snprintf(_buf, sizeof(_buf),
                    "[MID-SCALPER-GOLD-DIAG] ticks=%d phase=%d window=%d/%d range=%.2f spread=%.2f\n",
                    m_ticks_received, (int)phase, (int)m_window.size(), window_needed,
                    live_range, spread);
                std::cout << _buf;
                std::cout.flush();
            }
        }

        // -- COOLDOWN ---------------------------------------------------------
        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }

        // -- LIVE: manage position --------------------------------------------
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // -- PENDING: wait for fill -------------------------------------------
        if (phase == Phase::PENDING) {
            if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot_long,  spread); return; }
            if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot_short, spread); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[MID-SCALPER-GOLD] PENDING TIMEOUT after %ds -- resetting\n",
                        PENDING_TIMEOUT_S);
                    std::cout << _buf;
                    std::cout.flush();
                }
                if (cancel_fn) {
                    if (!pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
                    if (!pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
                }
                pending_long_clOrdId.clear();
                pending_short_clOrdId.clear();
                phase = Phase::IDLE;
            }
            return;
        }

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if ((int)m_window.size() < STRUCTURE_LOOKBACK) return;

        if (!can_enter) {
            if (phase == Phase::ARMED) return;
            return;
        }
        if (spread > MAX_SPREAD) return;

        if (!m_spread_gate.can_fire()) return;

        const bool flow_pyramid_ok = flow_live && flow_be_locked && flow_trail_stage >= 1;
        if (flow_live && !flow_pyramid_ok && phase == Phase::IDLE) return;

        double w_hi = *std::max_element(m_window.begin(), m_window.end());
        double w_lo = *std::min_element(m_window.begin(), m_window.end());
        range = w_hi - w_lo;

        // -- IDLE -> ARMED ---------------------------------------------------
        if (phase == Phase::IDLE) {
            // S53 2026-05-01 (SESSION_h): same-level re-arm block.
            //   Block re-arming when the new compression's hi or lo overlaps
            //   a recent exit price within SAME_LEVEL_BLOCK_PTS=8pt and the
            //   relevant cooldown is still active. Continuation captured
            //   naturally once price moves past the block radius.
            if (m_sl_price > 0.0 && now_s < m_sl_cooldown_ts) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            if (m_win_exit_price > 0.0 && now_s < m_win_exit_block_ts) {
                if (std::fabs(w_hi - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            if (range >= MIN_RANGE && range <= MAX_RANGE) {
                phase        = Phase::ARMED;
                bracket_high = w_hi;
                bracket_low  = w_lo;
                m_inside_ticks = 0;
                m_armed_ts   = now_s;
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[MID-SCALPER-GOLD] ARMED hi=%.2f lo=%.2f range=%.2f spread=%.2f\n",
                        bracket_high, bracket_low, range, spread);
                    std::cout << _buf;
                    std::cout.flush();
                }
            }
            return;
        }

        // -- ARMED -----------------------------------------------------------
        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range        = bracket_high - bracket_low;
            if (range > MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < MIN_RANGE || range > MAX_RANGE) { phase = Phase::IDLE; return; }

            if (mid >= bracket_low && mid <= bracket_high) {
                ++m_inside_ticks;
            } else {
                m_inside_ticks = 0;
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * TP_RR;
            const double min_tp  = spread * 2.0 + 0.12;
            if (tp_dist < min_tp) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[MID-SCALPER-GOLD] COST_FAIL range=%.2f sl_dist=%.2f tp_dist=%.2f min=%.2f\n",
                        range, sl_dist, tp_dist, min_tp);
                    std::cout << _buf;
                    std::cout.flush();
                }
                phase = Phase::IDLE;
                return;
            }

            // -- ATR-expansion gate (S47 T4a + 2026-04-29 ratchet fix) -------
            m_range_history.push_back(range);
            if ((int)m_range_history.size() > EXPANSION_HISTORY_LEN)
                m_range_history.pop_front();

            if ((int)m_range_history.size() >= EXPANSION_MIN_HISTORY) {
                std::vector<double> sorted(m_range_history.begin(),
                                           m_range_history.end());
                std::sort(sorted.begin(), sorted.end());
                const size_t n = sorted.size();
                const double median = (n % 2 == 1)
                    ? sorted[n / 2]
                    : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
                const double threshold = median * EXPANSION_MULT;
                if (range < threshold) {
                    {
                        char _buf[256];
                        snprintf(_buf, sizeof(_buf),
                            "[MID-SCALPER-GOLD] ATR_GATE_FAIL range=%.2f median=%.2f mult=%.2f threshold=%.2f hist=%d\n",
                            range, median, EXPANSION_MULT, threshold,
                            (int)m_range_history.size());
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    phase = Phase::IDLE;
                    bracket_high = bracket_low = 0.0;
                    return;
                }
            }

            const bool is_pyramid = flow_pyramid_ok;
            const double risk     = is_pyramid ? RISK_DOLLARS_PYRAMID : RISK_DOLLARS;
            // Lot uniformity (FIX 2026-04-22): cap to 0.01 regardless of risk math.
            const double base_lot = std::max(0.01, std::min(0.01, risk / (sl_dist * USD_PER_PT)));

            // -- DOM lot sizing (inherited from HybridGold) ------------------
            double lot_long  = base_lot;
            double lot_short = base_lot;

            if (l2_real) {
                if (wall_above && wall_below) {
                    {
                        char _buf[256];
                        snprintf(_buf, sizeof(_buf),
                            "[MID-SCALPER-GOLD] DOM_BLOCK both walls present -- skipping fire\n");
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    phase = Phase::IDLE;
                    return;
                }
                const bool slope_long  = (book_slope >  DOM_SLOPE_CONFIRM);
                const bool slope_short = (book_slope < -DOM_SLOPE_CONFIRM);
                if (slope_long  || vacuum_ask) lot_long  = std::min(0.01, lot_long  * DOM_LOT_BONUS);
                if (slope_short || vacuum_bid) lot_short = std::min(0.01, lot_short * DOM_LOT_BONUS);
                if (wall_above) lot_long  = std::max(0.01, lot_long  * DOM_WALL_PENALTY);
                if (wall_below) lot_short = std::max(0.01, lot_short * DOM_WALL_PENALTY);
            }

            pending_lot       = base_lot;
            pending_lot_long  = lot_long;
            pending_lot_short = lot_short;
            phase             = Phase::PENDING;
            m_armed_ts        = now_s;
            m_pending_blocked_since = 0;

            {
                char _buf[512];
                snprintf(_buf, sizeof(_buf),
                    "[MID-SCALPER-GOLD] FIRE hi=%.2f lo=%.2f range=%.2f sl=%.2f tp=%.2f "
                    "lot_base=%.3f lot_L=%.3f lot_S=%.3f slope=%.2f vac_a=%d vac_b=%d "
                    "wall_a=%d wall_b=%d %s\n",
                    bracket_high, bracket_low, range, sl_dist, tp_dist,
                    base_lot, lot_long, lot_short,
                    book_slope, (int)vacuum_ask, (int)vacuum_bid,
                    (int)wall_above, (int)wall_below,
                    is_pyramid ? "[PYRAMID]" : "[STANDALONE]");
                std::cout << _buf;
                std::cout.flush();
            }
        }
    }

    void confirm_fill(bool is_long, double fill_px, double fill_lot,
                      double spread_at_fill = 0.0) noexcept {
        if (cancel_fn) {
            if (is_long  && !pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
            if (!is_long && !pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
        }
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();

        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = fill_px;
        pos.sl              = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp              = is_long ? (fill_px + tp_dist)  : (fill_px - tp_dist);
        pos.size            = fill_lot;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.spread_at_entry = spread_at_fill;
        pos.entry_ts = m_last_tick_s;
        phase        = Phase::LIVE;

        {
            char _buf[256];
            snprintf(_buf, sizeof(_buf),
                "[MID-SCALPER-GOLD] FILL %s @ %.2f sl=%.2f(dist=%.2f) tp=%.2f lot=%.3f\n",
                is_long ? "LONG" : "SHORT", fill_px, pos.sl, sl_dist, pos.tp, fill_lot);
            std::cout << _buf;
            std::cout.flush();
        }
    }

    void manage(double bid, double ask, double mid,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        // Trail with S20 arm guards + S52 give-back fraction.
        const int64_t held_s     = now_s - pos.entry_ts;

        // S53 2026-05-01 (SESSION_h trade-quality): break-even lock.
        //   Move SL to entry once MFE crosses BE_TRIGGER_PTS=3.0. Fills
        //   the gap between original SL and MIN_TRAIL_ARM_PTS=5.0 so
        //   trades that MFE 3-5 then reverse exit at $0 instead of SL.
        //   One-shot via pos.be_locked. No hold-time guard: $3 MFE on
        //   XAUUSD is ~10x bid-ask noise, not gameable by tick fluctuation.
        if (move > 0 && !pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            if (pos.is_long  && pos.entry > pos.sl) pos.sl = pos.entry;
            if (!pos.is_long && pos.entry < pos.sl) pos.sl = pos.entry;
            pos.be_locked = true;
        }

        const bool    arm_mfe_ok = (MIN_TRAIL_ARM_PTS  <= 0.0) || (pos.mfe >= MIN_TRAIL_ARM_PTS);
        const bool    arm_hold_ok = (MIN_TRAIL_ARM_SECS <= 0 ) || (held_s  >= MIN_TRAIL_ARM_SECS);
        if (move > 0 && arm_mfe_ok && arm_hold_ok) {
            const double mfe_trail = pos.mfe * MFE_TRAIL_FRAC;
            const double range_trail = range * TRAIL_FRAC;
            const double trail_dist = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
            const double trail_sl = pos.is_long ? (pos.entry + pos.mfe - trail_dist)
                                                : (pos.entry - pos.mfe + trail_dist);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        // TP
        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) { _close(pos.tp, "TP_HIT", now_s, on_close); return; }

        // SL with S43 TRAIL_HIT/SL_HIT/BE_HIT classifier
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const bool sl_at_be        = (pos.sl <= pos.entry + 0.01)
                                      && (pos.sl >= pos.entry - 0.01);
            const bool trail_in_profit = pos.is_long
                ? (pos.sl > pos.entry + 0.01)
                : (pos.sl < pos.entry - 0.01);
            const char* reason;
            if      (sl_at_be)        reason = "BE_HIT";
            else if (trail_in_profit) reason = "TRAIL_HIT";
            else                      reason = "SL_HIT";
            _close(exit_px, reason, now_s, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos.active) return;
        _close(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

private:
    int     m_ticks_received = 0;
    int     m_inside_ticks   = 0;
    int64_t m_armed_ts       = 0;
    int64_t m_cooldown_start = 0;
    int     m_sl_cooldown_dir = 0;
    int64_t m_sl_cooldown_ts  = 0;
    // S53 2026-05-01 (SESSION_h): same-level re-arm block state.
    //   m_sl_price       entry price at last SL_HIT (loss-side block)
    //   m_win_exit_price exit price at last TRAIL_HIT/TP_HIT (win-side block)
    //   m_win_exit_block_ts time when win-side block expires (now_s + 600s)
    //   The post-SL block reuses the existing m_sl_cooldown_ts above.
    double  m_sl_price          = 0.0;
    double  m_win_exit_price    = 0.0;
    int64_t m_win_exit_block_ts = 0;
    int64_t m_pending_blocked_since = 0;
    int     m_trade_id        = 0;
    int64_t m_last_tick_s     = 0;
    std::deque<double> m_window;

    omega::SpreadRegimeGate m_spread_gate;
    std::deque<double> m_range_history;

    // AUDIT 2026-04-29: serialise the whole _close path. Inherited from
    //   HybridGold to avoid the Apr-7 -$3,008.38 phantom-pnl race.
    mutable std::mutex m_close_mtx;

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        std::lock_guard<std::mutex> _lk(m_close_mtx);

        if (!pos.active) return;

        const bool   is_long_  = pos.is_long;
        const double entry_    = pos.entry;
        const double sl_       = pos.sl;
        const double tp_       = pos.tp;
        const double size_     = pos.size;
        const double mfe_      = pos.mfe;
        const double mae_      = pos.mae;
        const double spread_at_entry_ = pos.spread_at_entry;
        const int64_t entry_ts_ = pos.entry_ts;

        const double pnl = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;

        const double sane_max = std::max(1.0, size_) * 200.0;
        double pnl_to_emit = pnl;
        if (std::fabs(pnl) > sane_max) {
            const double recomputed = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;
            std::ostringstream warn;
            warn << "[MID-SCALPER-GOLD][SANITY] anomalous pnl=" << pnl
                 << " (size=" << size_ << " entry=" << entry_ << " exit=" << exit_px
                 << "). Recomputed=" << recomputed
                 << ". Emitting recomputed value.\n";
            std::cout << warn.str();
            std::cout.flush();
            pnl_to_emit = recomputed;
        }

        {
            std::ostringstream os;
            os << "[MID-SCALPER-GOLD] EXIT " << (is_long_ ? "LONG" : "SHORT")
               << " @ " << std::fixed << std::setprecision(2) << exit_px
               << " reason=" << reason
               << " pnl_raw=" << std::setprecision(4) << pnl_to_emit
               << " mfe="    << std::setprecision(2) << mfe_
               << " mae="    << mae_
               << "\n";
            std::cout << os.str();
            std::cout.flush();
        }

        // S53 2026-05-01 (SESSION_h): same-level re-arm block stamps.
        //   SL_HIT -> 15-min block at entry price (rejected level).
        //   TRAIL_HIT or TP_HIT -> 10-min block at exit price (exhaustion).
        //   BE_HIT -> no stamp.
        // Continuation: re-arm allowed once price moves >SAME_LEVEL_BLOCK_PTS
        // away from the stamped level.
        if (reason == std::string("SL_HIT")) {
            m_sl_cooldown_dir = is_long_ ? 1 : -1;
            m_sl_cooldown_ts  = now_s + SAME_LEVEL_POST_SL_BLOCK_S;
            m_sl_price        = entry_;
        }
        if (reason == std::string("TRAIL_HIT") || reason == std::string("TP_HIT")) {
            m_win_exit_price    = exit_px;
            m_win_exit_block_ts = now_s + SAME_LEVEL_POST_WIN_BLOCK_S;
        }

        omega::TradeRecord tr;
        tr.id           = ++m_trade_id;
        tr.symbol       = "XAUUSD";
        tr.side         = is_long_ ? "LONG" : "SHORT";
        tr.engine       = "MidScalperGold";
        tr.regime       = "MID_COMPRESSION";
        tr.entryPrice   = entry_;
        tr.exitPrice    = exit_px;
        tr.tp           = tp_;
        tr.sl           = sl_;
        tr.size         = size_;
        tr.pnl          = pnl_to_emit;
        tr.net_pnl      = tr.pnl;
        tr.mfe          = mfe_ * size_;
        tr.mae          = mae_ * size_;
        tr.entryTs      = entry_ts_;
        tr.exitTs       = now_s;
        tr.exitReason   = reason;
        tr.spreadAtEntry = spread_at_entry_;
        tr.bracket_hi   = bracket_high;
        tr.bracket_lo   = bracket_low;
        tr.shadow       = shadow_mode;

        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        if (on_close) on_close(tr);
    }
};

} // namespace omega
