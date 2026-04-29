#pragma once
#include <iomanip>
#include <iostream>
#include "SpreadRegimeGate.hpp"  // 2026-04-29 PM Option 1 (audit-fixes-18)
// =============================================================================
// GoldHybridBracketEngine.hpp  --  Compression breakout bracket for XAUUSD
// =============================================================================
//
// DESIGN:
//   Arms BOTH a long stop-order above the range high AND a short stop-order
//   below the range low simultaneously. Whichever fills first becomes the
//   live position; the other is cancelled.
//
// DOM FILTER (wired 2026-04-07):
//   At PENDING->FIRE time, DOM confirms which side has a clear path:
//     book_slope > SLOPE_CONFIRM: buy pressure → LONG order gets lot bonus (1.3x)
//     book_slope < -SLOPE_CONFIRM: sell pressure → SHORT order gets lot bonus (1.3x)
//     vacuum_ask: ask side thin → LONG path clear → LONG lot bonus
//     vacuum_bid: bid side thin → SHORT path clear → SHORT lot bonus
//   Wall gates at entry:
//     If wall_above AND wall_below: no clear path either side → skip fire
//     If wall_above only: reduce LONG lot by 50% (ceiling blocks TP)
//     If wall_below only: reduce SHORT lot by 50%
//   When l2_real=false: DOM filter bypassed (safe fallback, both sides equal lots)
//
// SIZING:
//   Standalone (no flow active): risk_dollars = $30, SL = range * 0.5 + 0.5pt
//   Alongside flow (pyramid):    risk_dollars = $10 (30% addon), same SL formula
//   DOM lot bonus applied AFTER risk sizing, before max_lot cap (0.50)
//
// S47 T4a 2026-04-27: TWO BUG/FEATURE CHANGES
//   (1) entry_ts capture bug
//       PRIOR: confirm_fill() set pos.entry_ts = std::time(nullptr), which
//       returns wall-clock time of the host. In backtest replay against
//       historical ticks (March 2024 onwards), entry_ts captured the host's
//       2026 wall-clock while exit_ts was the simulated tick time. Result:
//       Hold = exit - entry produced large negative values (e.g. -7719671s
//       observed in S46 backtest). The S46 26-month run reported
//       Hold=-7719671s and Sh=-7.96 partly because of bogus per-trade hold
//       contaminating Sharpe via the variance term in the per-engine
//       accumulator.
//       FIX: cache the simulated tick second at the top of on_tick into
//       m_last_tick_s, and have confirm_fill() read that value. Live mode
//       passes UTC ms in now_ms identically, so behaviour in production is
//       unchanged (still UTC seconds, just sourced from the tick instead of
//       std::time). All other engines use the passed-in now_ms; this brings
//       HBG into line with the codebase convention.
//
//   (2) ATR-expansion gate at FIRE time
//       PRIOR: any compression that satisfied MIN_RANGE..MAX_RANGE armed and
//       fired regardless of whether current volatility was elevated relative
//       to recent volatility. This produced fires during low-vol grinds that
//       noise-stopped immediately.
//       FIX: track last EXPANSION_HISTORY_LEN range observations. At FIRE
//       time, require current range >= EXPANSION_MULT * median(history). If
//       history shorter than EXPANSION_MIN_HISTORY entries, gate is
//       pass-through (warmup). On gate fail: return to IDLE, log
//       [HYBRID-GOLD] ATR_GATE_FAIL diagnostic, no fire.
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

class GoldHybridBracketEngine {
public:
    // ── Parameters ────────────────────────────────────────────────────────────
    static constexpr int    STRUCTURE_LOOKBACK   = 20;
    static constexpr int    MIN_ENTRY_TICKS      = 15;
    static constexpr int    MIN_BREAK_TICKS      = 3;
    static constexpr double MIN_RANGE            = 6.0;
    static constexpr double MAX_RANGE            = 25.0;
    static constexpr double SL_FRAC              = 0.5;
    static constexpr double SL_BUFFER            = 0.5;
    static constexpr double TP_RR                = 2.0;
    static constexpr double TRAIL_FRAC           = 0.25;
    // S20 2026-04-25: trail-arm guards
    //   2026-04-24 15:44:43 saw a TRAIL exit after 3 seconds of hold (+$0.04
    //   gross, -$0.02 net). The MFE-proportional trail activated on a sub-pt
    //   MFE and was immediately hit by bid-ask noise on the next tick.
    //   MIN_TRAIL_ARM_PTS: position must have MFE >= this before trail recomputes
    //   MIN_TRAIL_ARM_SECS: position must have been open >= this before trail recomputes
    //   Both must be satisfied. Set either to 0 to disable that guard.
    static constexpr double MIN_TRAIL_ARM_PTS    = 1.5;
    static constexpr int    MIN_TRAIL_ARM_SECS   = 15;
    static constexpr double MAX_SPREAD           = 2.5;
    static constexpr double RISK_DOLLARS         = 30.0;
    static constexpr double RISK_DOLLARS_PYRAMID = 10.0;
    static constexpr double USD_PER_PT           = 100.0;
    static constexpr int    PENDING_TIMEOUT_S    = 30;
    static constexpr int    COOLDOWN_S           = 60;
    static constexpr int    DIR_SL_COOLDOWN_S    = 120;
    static constexpr double DOM_SLOPE_CONFIRM    = 0.15; // |book_slope| for lot bonus
    static constexpr double DOM_LOT_BONUS        = 1.3;  // lot multiplier when DOM confirms
    static constexpr double DOM_WALL_PENALTY     = 0.5;  // lot reduction when wall blocks TP

    // S47 T4a: ATR-expansion gate (range-of-range proxy).
    //   At FIRE time, require current compression range >= EXPANSION_MULT *
    //   median of last EXPANSION_HISTORY_LEN observed ranges. While history
    //   has fewer than EXPANSION_MIN_HISTORY entries the gate is bypassed
    //   (warmup pass-through).
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;

    // ── Phase ─────────────────────────────────────────────────────────────────
    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    bool  shadow_mode = true;  // default true = log only, no live orders (Class C added 2026-04-21)

    // ── State ─────────────────────────────────────────────────────────────────
    // S43 2026-04-26: added mae field + tracker. Pre-S43 every HBG trade in
    //   gold_combined_20260426_051234.csv had mae=0.00 because the writer
    //   was hardcoded to tr.mae=0.0 at close time. Now tracks pos.mae as the
    //   maximum adverse excursion in price-points (matches GoldEngineStack
    //   convention) and writes tr.mae = pos.mae * pos.size at close.
    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = 0.01;
        double  mfe      = 0.0;
        double  mae      = 0.0;   // S43: max adverse excursion (price points, <= 0)
        // S51 1A.1.a 2026-04-28: full bid-ask spread at fill time (price units).
        //   Pre-S51 tr.spreadAtEntry was hardcoded to 0.0 -> apply_realistic_costs()
        //   computed slippage_entry = slippage_exit = 0 for every HBG trade,
        //   silently treating live and shadow trades as zero-spread fills.
        //   Symmetric with HBI fix in same patch. Captured in confirm_fill()
        //   at the moment of fill.
        double  spread_at_entry = 0.0;
        int64_t entry_ts = 0;
    } pos;

    double bracket_high  = 0.0;
    double bracket_low   = 0.0;
    double range         = 0.0;
    double pending_lot   = 0.01;
    double pending_lot_long  = 0.01; // DOM-adjusted lots for each side
    double pending_lot_short = 0.01;

    std::string pending_long_clOrdId;
    std::string pending_short_clOrdId;
    std::function<void(const std::string&)> cancel_fn;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Main tick ─────────────────────────────────────────────────────────────
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

        // 2026-04-29 PM Option 1 (audit-fixes-18): feed spread-regime gate
        //   on EVERY tick (always -- including LIVE/PENDING/COOLDOWN ticks)
        //   so the 1h rolling window stays fresh.  The can_fire() check
        //   below only gates the new-entry path; existing position
        //   management (manage()/confirm_fill()) is unaffected.
        m_spread_gate.on_tick(now_ms, spread);
        // S44 (2026-04-29 LATE): feed macro regime into the gate so RISK_OFF
        // widens the spread threshold by 10% (more permissive on news days)
        // and RISK_ON tightens by 5% (stricter when calm).  g_macroDetector
        // is the single source of truth, updated by VIX/DXY/ES/NQ ticks in
        // on_tick.hpp.  set_macro_regime is cheap (string compare + double
        // assign) so calling per-tick is fine.
        m_spread_gate.set_macro_regime(g_macroDetector.regime());

        // S47 T4a: cache simulated tick second so confirm_fill() can stamp
        //   pos.entry_ts using the tick clock rather than std::time(nullptr).
        m_last_tick_s = now_s;

        ++m_ticks_received;
        m_window.push_back(mid);
        if ((int)m_window.size() > STRUCTURE_LOOKBACK * 2) m_window.pop_front();

        // Warmup diag
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
                // converted from printf
                char _buf[512];
                snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD-DIAG] ticks=%d phase=%d window=%d/%d range=%.2f spread=%.2f\n",                    m_ticks_received, (int)phase, (int)m_window.size(), window_needed,                    live_range, spread);
                std::cout << _buf;
                std::cout.flush();
            }
        }

        // ── COOLDOWN ─────────────────────────────────────────────────────────
        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }

        // ── LIVE: manage position ─────────────────────────────────────────────
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // ── PENDING: wait for fill ACK from tick_gold ─────────────────────────
        if (phase == Phase::PENDING) {
            // S51 1A.1.a: pass real spread to populate pos.spread_at_entry.
            if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot_long,  spread); return; }
            if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot_short, spread); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD] PENDING TIMEOUT after %ds -- resetting\n", PENDING_TIMEOUT_S);
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

        // 2026-04-29 PM Option 1 (audit-fixes-18): regime-aware sit-out.
        //   When 1h median spread > 0.5pt (e.g. 2026-Q1 across XAUUSD),
        //   block new entries.  LIVE/PENDING management above already
        //   returned, so this only short-circuits the IDLE/ARMED path.
        if (!m_spread_gate.can_fire()) return;

        const bool flow_pyramid_ok = flow_live && flow_be_locked && flow_trail_stage >= 1;
        if (flow_live && !flow_pyramid_ok && phase == Phase::IDLE) return;

        double w_hi = *std::max_element(m_window.begin(), m_window.end());
        double w_lo = *std::min_element(m_window.begin(), m_window.end());
        range = w_hi - w_lo;

        // ── IDLE -> ARMED ─────────────────────────────────────────────────────
        if (phase == Phase::IDLE) {
            if (range >= MIN_RANGE && range <= MAX_RANGE) {
                phase        = Phase::ARMED;
                bracket_high = w_hi;
                bracket_low  = w_lo;
                m_inside_ticks = 0;
                m_armed_ts   = now_s;
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD] ARMED hi=%.2f lo=%.2f range=%.2f spread=%.2f\n",                        bracket_high, bracket_low, range, spread);
                    std::cout << _buf;
                    std::cout.flush();
                }
            }
            return;
        }

        // ── ARMED ─────────────────────────────────────────────────────────────
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
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD] COST_FAIL range=%.2f sl_dist=%.2f tp_dist=%.2f min=%.2f\n",                        range, sl_dist, tp_dist, min_tp);
                    std::cout << _buf;
                    std::cout.flush();
                }
                phase = Phase::IDLE;
                return;
            }

            // ── S47 T4a: ATR-expansion gate (FIX 2026-04-29 ratchet) ─────────
            //   Require current range to exceed EXPANSION_MULT * median of
            //   last EXPANSION_HISTORY_LEN qualifying compressions before
            //   firing. Bypass while history shorter than EXPANSION_MIN_HISTORY.
            //
            //   2026-04-29 ratchet fix: pre-fix this push only ran on the
            //   gate-passing path. With EXPANSION_MULT > 1.0 the recorded
            //   ranges therefore drifted monotonically upward (only above-
            //   median fires got into history), and the median ratcheted up
            //   until no compression could pass -- observed in the 26-month
            //   BT as 43 fires total, then full lockout from 2026-02 onward.
            //
            //   Now we push EVERY qualifying compression range (one that
            //   reached this point -- passed MIN_RANGE/MAX_RANGE,
            //   MIN_BREAK_TICKS and COST_FAIL gates) into the history before
            //   evaluating the expansion threshold. The gate now means
            //   "current range >= 1.10 * median of recent qualifying
            //   compressions" instead of "current range >= 1.10 * median of
            //   recent fires." Non-fired qualifying ranges enter the history
            //   and pull the median back toward the population mean, breaking
            //   the ratchet.
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
                        char _buf[512];
                        snprintf(_buf, sizeof(_buf),
                            "[HYBRID-GOLD] ATR_GATE_FAIL range=%.2f median=%.2f mult=%.2f threshold=%.2f hist=%d\n",
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
            // Gate passed (or warmup). The qualifying-range push above already
            // recorded this attempt for future median calculations.

            const bool is_pyramid = flow_pyramid_ok;
            const double risk     = is_pyramid ? RISK_DOLLARS_PYRAMID : RISK_DOLLARS;
            const double base_lot = std::max(0.01, std::min(0.01, risk / (sl_dist * USD_PER_PT)));  // FIX 2026-04-22 uniformity: capped to 0.01

            // ── DOM lot sizing ────────────────────────────────────────────────
            double lot_long  = base_lot;
            double lot_short = base_lot;

            if (l2_real) {
                // Both sides walled = no clear path, skip
                if (wall_above && wall_below) {
                    {
                        // converted from printf
                        char _buf[512];
                        snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD] DOM_BLOCK both walls present -- skipping fire\n");
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    phase = Phase::IDLE;
                    return;
                }
                // DOM bonus: clear path on one side gets larger lot
                const bool slope_long  = (book_slope >  DOM_SLOPE_CONFIRM);
                const bool slope_short = (book_slope < -DOM_SLOPE_CONFIRM);
                if (slope_long  || vacuum_ask) lot_long  = std::min(0.01, lot_long  * DOM_LOT_BONUS);  // FIX 2026-04-22 uniformity
                if (slope_short || vacuum_bid) lot_short = std::min(0.01, lot_short * DOM_LOT_BONUS);  // FIX 2026-04-22 uniformity
                // Wall penalty: ceiling blocks TP on that side
                if (wall_above) lot_long  = std::max(0.01, lot_long  * DOM_WALL_PENALTY);
                if (wall_below) lot_short = std::max(0.01, lot_short * DOM_WALL_PENALTY);
            }

            pending_lot       = base_lot; // kept for backward compat
            pending_lot_long  = lot_long;
            pending_lot_short = lot_short;
            phase             = Phase::PENDING;
            m_armed_ts        = now_s;
            m_pending_blocked_since = 0;

            {
                // converted from printf
                char _buf[512];
                snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD] FIRE hi=%.2f lo=%.2f range=%.2f sl=%.2f tp=%.2f "                    "lot_base=%.3f lot_L=%.3f lot_S=%.3f slope=%.2f vac_a=%d vac_b=%d "                    "wall_a=%d wall_b=%d %s\n",                    bracket_high, bracket_low, range, sl_dist, tp_dist,                    base_lot, lot_long, lot_short,                    book_slope, (int)vacuum_ask, (int)vacuum_bid,                    (int)wall_above, (int)wall_below,                    is_pyramid ? "[PYRAMID]" : "[STANDALONE]");
                std::cout << _buf;
                std::cout.flush();
            }
        }
    }

    // S51 1A.1.a 2026-04-28: spread_at_fill default-arg added (symmetric with HBI).
    //   External callers (order_exec.hpp:445) use the 3-arg form and continue to
    //   compile unchanged -- live FIX fills will retain spread_at_entry=0.0 until
    //   the FIX execution-report path is updated to pass real spread. Internal
    //   shadow-mode call sites in on_tick() now pass (ask - bid).
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
        pos.mae             = 0.0;   // S43: reset adverse-excursion tracker
        pos.spread_at_entry = spread_at_fill;  // S51: stash for tr.spreadAtEntry at close
        // S47 T4a 2026-04-27: was std::time(nullptr) -- broke backtest hold
        //   computation by capturing host wall-clock instead of simulated tick
        //   clock. m_last_tick_s is set at the top of on_tick() from now_s
        //   (= now_ms / 1000), which in live mode is UTC seconds and in
        //   backtest is the simulated tick second. Live behaviour unchanged
        //   (still UTC seconds); backtest now produces correct positive Hold.
        pos.entry_ts = m_last_tick_s;
        phase        = Phase::LIVE;

        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD] FILL %s @ %.2f sl=%.2f(dist=%.2f) tp=%.2f lot=%.3f\n",                is_long ? "LONG" : "SHORT", fill_px, pos.sl, sl_dist, pos.tp, fill_lot);
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
        // S43 2026-04-26: track max adverse excursion (negative or zero).
        //   pos.mae stays <= 0; tracks worst against-position move in price points.
        //   Mirrors GoldEngineStack.hpp:4377 convention.
        if (move < pos.mae) pos.mae = move;

        // Trail: MFE-proportional -- tightens as move grows, locks in more profit
        // trail_dist = min(range * TRAIL_FRAC, mfe * 0.20)
        //   Small move (2pt): min(1.5, 0.4) = 0.4pt trail -- locks 80% of move
        //   Medium move (6pt): min(1.5, 1.2) = 1.2pt trail -- locks 80% of move
        //   Large move (15pt): min(1.5, 3.0) = 1.5pt trail -- range caps it
        // This ensures we capture ~80% of MFE rather than giving back the entire move
        //
        // S20 2026-04-25 arm guards:
        //   Require MFE >= MIN_TRAIL_ARM_PTS AND hold >= MIN_TRAIL_ARM_SECS before
        //   moving SL. Prevents the 3-second TRAIL seen on 2026-04-24 15:44:43
        //   where a sub-pt MFE on tick 1 armed the trail and bid-ask noise hit it
        //   on tick 2. Either guard set to 0 disables that check.
        const int64_t held_s     = now_s - pos.entry_ts;
        const bool    arm_mfe_ok = (MIN_TRAIL_ARM_PTS  <= 0.0) || (pos.mfe >= MIN_TRAIL_ARM_PTS);
        const bool    arm_hold_ok = (MIN_TRAIL_ARM_SECS <= 0 ) || (held_s  >= MIN_TRAIL_ARM_SECS);
        if (move > 0 && arm_mfe_ok && arm_hold_ok) {
            const double mfe_trail = pos.mfe * 0.20;
            const double range_trail = range * TRAIL_FRAC;
            const double trail_dist = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
            const double trail_sl = pos.is_long ? (pos.entry + pos.mfe - trail_dist)
                                                : (pos.entry - pos.mfe + trail_dist);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        // TP
        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) { _close(pos.is_long ? pos.tp : pos.tp, "TP_HIT", now_s, on_close); return; }

        // SL
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            // S43 2026-04-26: TRAIL_HIT/SL_HIT classifier was buggy.
            //   PRIOR: TRAIL_HIT was emitted whenever instantaneous `move > 0` at
            //   SL-fire time, regardless of whether the trail had actually moved
            //   the SL past entry. This contaminated diagnostics: 28/28 HBG
            //   "TRAIL_HIT" trades in gold_combined_20260426_051234.csv had
            //   median hold 1s, max 4s -- they were flicker-stops on the
            //   original 3.5pt SL being mislabelled as trail-hits because price
            //   had ticked one count favourable before reversing.
            //   FIX: gate the TRAIL_HIT label on whether pos.sl has actually
            //   moved strictly past pos.entry in the position-favourable
            //   direction (i.e. trail engaged and SL is in profit). Mirrors
            //   the S20 GoldEngineStack relabel pattern at
            //   GoldEngineStack.hpp:4440-4443.
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
    int64_t m_pending_blocked_since = 0;
    int     m_trade_id        = 0;
    // S47 T4a: simulated/live tick second cached at top of on_tick() so
    //   confirm_fill() can stamp pos.entry_ts using the tick clock instead
    //   of std::time(nullptr) (which broke backtest hold computation).
    int64_t m_last_tick_s     = 0;
    std::deque<double> m_window;

    // 2026-04-29 PM Option 1 (audit-fixes-18): per-engine 1h rolling spread
    //   gate.  Updated every tick from on_tick(); consulted only on the
    //   new-entry path (after MAX_SPREAD check).  See SpreadRegimeGate.hpp.
    omega::SpreadRegimeGate m_spread_gate;
    // S47 T4a: rolling history of ranges that passed the FIRE gate, used by
    //   the ATR-expansion gate to require current range >= EXPANSION_MULT *
    //   median(history) before firing.
    std::deque<double> m_range_history;

    // AUDIT 2026-04-29: mutex around the whole _close() path. Two close paths
    //   exist (manage->TP/SL hit, force_close()), each callable from a
    //   different thread. Without serialisation the trade-record write
    //   could see partial state, producing the Apr-7 -$3,008.38 record where
    //   tr.pnl came out exactly 100x the expected value. Holding the lock
    //   for the entire emit path also makes the [HYBRID-GOLD] EXIT log line
    //   atomic vs. concurrent log writes from other engines.
    mutable std::mutex m_close_mtx;

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        // AUDIT 2026-04-29: serialise the whole close path. See m_close_mtx
        //   comment above for context (Apr-7 -$3,008.38 phantom).
        std::lock_guard<std::mutex> _lk(m_close_mtx);

        // Re-check pos.active under the lock so a concurrent close that won
        //   the race silently bails here instead of double-emitting tr.
        if (!pos.active) return;

        // Snapshot live state into locals BEFORE building tr. This defeats
        //   any race where pos.* could mutate (e.g. via reset to LivePos{}
        //   on another thread) between the printf and the trade record
        //   construction.
        const bool   is_long_  = pos.is_long;
        const double entry_    = pos.entry;
        const double sl_       = pos.sl;
        const double tp_       = pos.tp;
        const double size_     = pos.size;
        const double mfe_      = pos.mfe;
        const double mae_      = pos.mae;
        const double spread_at_entry_ = pos.spread_at_entry;
        const int64_t entry_ts_ = pos.entry_ts;

        // Single source-of-truth pnl computation from snapshot.
        const double pnl = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;

        // Sanity check against the same -$3,008 race: if pnl magnitude is
        //   wildly larger than max_lot * 200pt move (~$1000 even at 0.50
        //   gold lots), something has corrupted the state -- log and emit
        //   a recomputed value rather than the suspicious one.
        const double sane_max = std::max(1.0, size_) * 200.0;  // pts; * 100 in lifecycle
        double pnl_to_emit = pnl;
        if (std::fabs(pnl) > sane_max) {
            const double recomputed = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;
            std::ostringstream warn;
            warn << "[HYBRID-GOLD][SANITY] anomalous pnl=" << pnl
                 << " (size=" << size_ << " entry=" << entry_ << " exit=" << exit_px
                 << "). Recomputed=" << recomputed
                 << ". Emitting recomputed value.\n";
            std::cout << warn.str();
            std::cout.flush();
            pnl_to_emit = recomputed;
        }

        // Atomic log line (single ostringstream -> single cout write).
        {
            std::ostringstream os;
            os << "[HYBRID-GOLD] EXIT " << (is_long_ ? "LONG" : "SHORT")
               << " @ " << std::fixed << std::setprecision(2) << exit_px
               << " reason=" << reason
               << " pnl_raw=" << std::setprecision(4) << pnl_to_emit
               << " mfe="    << std::setprecision(2) << mfe_
               << " mae="    << mae_
               << "\n";
            std::cout << os.str();
            std::cout.flush();
        }

        if (reason == std::string("SL_HIT")) {
            m_sl_cooldown_dir = is_long_ ? 1 : -1;
            m_sl_cooldown_ts  = now_s + DIR_SL_COOLDOWN_S;
        }

        omega::TradeRecord tr;
        tr.id           = ++m_trade_id;
        tr.symbol       = "XAUUSD";
        tr.side         = is_long_ ? "LONG" : "SHORT";
        tr.engine       = "HybridBracketGold";
        tr.regime       = "COMPRESSION";
        tr.entryPrice   = entry_;
        tr.exitPrice    = exit_px;
        tr.tp           = tp_;
        tr.sl           = sl_;
        tr.size         = size_;
        tr.pnl          = pnl_to_emit;             // sanity-checked, raw pts*size
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

        // Reset state AFTER tr is fully built but BEFORE callback so that any
        //   re-entrant on_close handler that queries this engine sees a
        //   clean post-close state.
        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        if (on_close) on_close(tr);
    }
};

} // namespace omega

