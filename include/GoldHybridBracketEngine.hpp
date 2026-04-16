#pragma once
#include <iomanip>
#include <iostream>
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
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <string>
#include <deque>
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

    // ── Phase ─────────────────────────────────────────────────────────────────
    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    // ── State ─────────────────────────────────────────────────────────────────
    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = 0.01;
        double  mfe      = 0.0;
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
            if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot_long);  return; }
            if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot_short); return; }
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

            const bool is_pyramid = flow_pyramid_ok;
            const double risk     = is_pyramid ? RISK_DOLLARS_PYRAMID : RISK_DOLLARS;
            const double base_lot = std::max(0.01, std::min(0.20, risk / (sl_dist * USD_PER_PT)));  // capped 0.50->0.20

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
                if (slope_long  || vacuum_ask) lot_long  = std::min(0.20, lot_long  * DOM_LOT_BONUS);  // capped 0.50->0.20
                if (slope_short || vacuum_bid) lot_short = std::min(0.20, lot_short * DOM_LOT_BONUS);  // capped 0.50->0.20
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

    void confirm_fill(bool is_long, double fill_px, double fill_lot) noexcept {
        if (cancel_fn) {
            if (is_long  && !pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
            if (!is_long && !pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
        }
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();

        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = fill_px;
        pos.sl       = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp       = is_long ? (fill_px + tp_dist)  : (fill_px - tp_dist);
        pos.size     = fill_lot;
        pos.mfe      = 0.0;
        pos.entry_ts = static_cast<int64_t>(std::time(nullptr));
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

        // Trail: MFE-proportional -- tightens as move grows, locks in more profit
        // trail_dist = min(range * TRAIL_FRAC, mfe * 0.20)
        //   Small move (2pt): min(1.5, 0.4) = 0.4pt trail -- locks 80% of move
        //   Medium move (6pt): min(1.5, 1.2) = 1.2pt trail -- locks 80% of move
        //   Large move (15pt): min(1.5, 3.0) = 1.5pt trail -- range caps it
        // This ensures we capture ~80% of MFE rather than giving back the entire move
        if (move > 0) {
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
            const char* reason   = (std::fabs(pos.sl - pos.entry) < 0.01) ? "BE_HIT" : "TRAIL_HIT";
            if (pos.sl <= pos.entry + 0.01 && pos.sl >= pos.entry - 0.01) reason = "BE_HIT";
            else if (move > 0) reason = "TRAIL_HIT";
            else reason = "SL_HIT";
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
    std::deque<double> m_window;

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;
        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[HYBRID-GOLD] EXIT %s @ %.2f reason=%s pnl_raw=%.4f mfe=%.2f\n",                pos.is_long ? "LONG" : "SHORT", exit_px, reason, pnl, pos.mfe);
            std::cout << _buf;
            std::cout.flush();
        }

        if (reason == std::string("SL_HIT")) {
            m_sl_cooldown_dir = pos.is_long ? 1 : -1;
            m_sl_cooldown_ts  = now_s + DIR_SL_COOLDOWN_S;
        }

        omega::TradeRecord tr;
        tr.id = ++m_trade_id; tr.symbol = "XAUUSD";
        tr.side = pos.is_long ? "LONG" : "SHORT";
        tr.engine = "HybridBracketGold"; tr.regime = "COMPRESSION";
        tr.entryPrice = pos.entry; tr.exitPrice = exit_px;
        tr.tp = pos.tp; tr.sl = pos.sl;
        tr.size = pos.size;
        tr.pnl = (pos.is_long ? (exit_px - pos.entry)
                              : (pos.entry - exit_px)) * pos.size;  // raw -- tick_mult in lifecycle
        tr.net_pnl = tr.pnl;
        tr.mfe = pos.mfe * pos.size; tr.mae = 0.0;
        tr.entryTs = pos.entry_ts; tr.exitTs = now_s;
        tr.exitReason = reason; tr.spreadAtEntry = 0.0;
        tr.bracket_hi = bracket_high; tr.bracket_lo = bracket_low;

        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        if (on_close) on_close(tr);
    }
};

} // namespace omega

