#pragma once
// =============================================================================
// RSIReversalEngine.hpp  --  Tick-level RSI reversal entries for XAUUSD
// =============================================================================
//
// WHAT THIS SOLVES:
//   Bar RSI (M1) only updates every 60 seconds. A 5pt move in 2 minutes
//   produces bar RSI=50 at bar close even though the move is already done.
//   This engine computes its OWN tick-level RSI(14) from the mid price
//   directly -- updates on EVERY tick, no bar dependency, no bars_ready gate.
//
// ENTRY:
//   Tick RSI < RSI_OVERSOLD  (default 42) -> LONG on first RSI tick up
//   Tick RSI > RSI_OVERBOUGHT (default 58) -> SHORT on first RSI tick down
//   All sessions (Asia + London + NY). Spread gate protects thin sessions.
//
// DOM FILTER (wired 2026-04-07):
//   LONG:  block if wall_below (support just got eaten = reversal may fail)
//          block if l2_imbalance < L2_LONG_MIN (ask-heavy book opposes bounce)
//          vacuum_bid confirms = reduce cooldown 60->30s on next re-entry
//   SHORT: block if wall_above (resistance block = short may stall)
//          block if l2_imbalance > L2_SHORT_MAX (bid-heavy book opposes fade)
//          vacuum_ask confirms = reduce cooldown 60->30s on next re-entry
//   When l2_real=false: DOM filter bypassed entirely (safe fallback)
//
// PROTECTION:
//   SL = 0.6x ATR from entry
//   BE lock at 0.4x ATR profit
//   Trail = 0.4x ATR behind MFE
//   RSI exit: LONG exits when tick RSI recovers to 52, SHORT exits at 48
//   Max hold 10 minutes, cooldown 60s (30s with vacuum confirm)
//
// v2.1: update_indicators() exposed as public method.
//   tick_gold.hpp calls it unconditionally so tick_rsi() is always current.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <deque>
#include "OmegaTradeLedger.hpp"

namespace omega {

class RSIReversalEngine {
public:
    // ── Parameters ────────────────────────────────────────────────────────────
    // RSI_OVERSOLD/OVERBOUGHT no longer used for entry (pure direction change)
    // Kept for legacy compat -- entry is now RSI turn + RSI side of 50
    double RSI_OVERSOLD       = 50.0;  // entry: RSI must be below 50 for LONG
    double RSI_OVERBOUGHT     = 50.0;  // entry: RSI must be above 50 for SHORT
    double RSI_MIN_MOVE       = 3.0;   // min RSI pts moved before reversal valid
    double PRICE_CONFIRM_PTS  = 1.0;   // price must move >1pt since RSI extreme before entry
    double L2_EXIT_THRESHOLD  = 0.50;  // L2 imbalance crosses 0.5 = DOM flipped
    double L2_EXIT_MIN_PROFIT = 0.50;  // min pts profit before L2 flip exit fires
    double RSI_EXIT_LONG      = 55.0;  // exit LONG when tick RSI reaches 55
    double RSI_EXIT_SHORT     = 45.0;  // exit SHORT when tick RSI reaches 45
    int    RSI_PERIOD         = 14;
    double SL_ATR_MULT        = 0.6;
    double TRAIL_ATR_MULT     = 0.40;
    double BE_ATR_MULT        = 0.40;
    double MAX_SPREAD_PTS     = 2.5;
    double MIN_ATR_PTS        = 1.0;
    int    COOLDOWN_S         = 60;
    int    COOLDOWN_S_VACUUM  = 30;   // reduced cooldown when vacuum confirms direction
    int    MAX_HOLD_S         = 600;
    int    MIN_HOLD_S         = 8;
    double L2_LONG_MIN        = 0.40; // min imbalance for LONG (not ask-heavy)
    double L2_SHORT_MAX       = 0.60; // max imbalance for SHORT (not bid-heavy)
    bool   enabled            = true;
    bool   shadow_mode        = true;

    // ── State ─────────────────────────────────────────────────────────────────
    struct Position {
        bool    active    = false;
        bool    is_long   = false;
        bool    be_locked = false;
        double  entry     = 0.0;
        double  sl        = 0.0;
        double  atr       = 0.0;
        double  size      = 0.01;
        double  mfe       = 0.0;
        int64_t entry_ts  = 0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Indicator update -- MUST be called every tick unconditionally ──────────
    void update_indicators(double bid, double ask) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        _update_tick_rsi(mid);
        _update_tick_atr(mid, spread);
    }

    // ── Main tick ─────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask,
                 int session_slot, int64_t now_ms,
                 // DOM data
                 double l2_imbalance  = 0.5,
                 bool   wall_above    = false,
                 bool   wall_below    = false,
                 bool   vacuum_ask    = false,
                 bool   vacuum_bid    = false,
                 bool   l2_real       = false,
                 CloseCallback on_close = nullptr) noexcept
    {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;
        if (l2_imbalance > 0.0) m_last_l2 = l2_imbalance;  // track L2 every tick

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        _update_tick_rsi(mid);
        _update_tick_atr(mid, spread);

        if (pos.active) {
            _manage(bid, ask, mid, now_s, on_close);
            return;
        }

        if (now_s < m_cooldown_until) return;

        if (spread > MAX_SPREAD_PTS)        return;
        if (m_tick_atr < MIN_ATR_PTS)       return;
        if (m_rsi_count < RSI_PERIOD + 2)   return;

        // Entry: pure RSI direction change -- no fixed thresholds.
        // RSI was falling and now turns up -> LONG
        // RSI was rising  and now turns down -> SHORT
        // Works at ANY RSI level: catches turns at 15, 25, 42, 58, 75, 85
        const double rsi = (m_bar_rsi > 0.0) ? m_bar_rsi : m_tick_rsi;
        if (rsi <= 0.0 || m_bar_rsi_prev <= 0.0) return;

        // Require RSI to have moved MIN_RSI_MOVE pts before reversing
        // prevents entering on 1-2pt noise oscillations
        const double rsi_move = std::fabs(rsi - m_rsi_peak);
        if (rsi_move < RSI_MIN_MOVE) return;

        // Direction change detection
        const bool was_falling = (m_bar_rsi_prev > rsi + 0.5);  // RSI was moving down
        const bool was_rising  = (m_bar_rsi_prev < rsi - 0.5);  // RSI was moving up
        // LONG: was falling, now rising, and RSI is in lower half (below 50)
        const bool rsi_turn_long  = was_falling && (rsi > m_bar_rsi_prev) && (rsi < 50.0);
        // SHORT: was rising, now falling, and RSI is in upper half (above 50)
        const bool rsi_turn_short = was_rising  && (rsi < m_bar_rsi_prev) && (rsi > 50.0);
        if (!rsi_turn_long && !rsi_turn_short) return;

        // Price confirmation: price must have already moved > PRICE_CONFIRM_PTS
        // since the RSI extreme. Ensures move is real and cost-covered before entry.
        // At 0.05 lots: 1pt = $5 gross vs ~$3.50 cost -- already profitable.
        const double price_move_since_extreme = rsi_turn_long
            ? (mid - m_price_at_extreme)   // LONG: price risen since RSI bottom
            : (m_price_at_extreme - mid);  // SHORT: price fallen since RSI top
        if (m_price_at_extreme > 0.0 && price_move_since_extreme < PRICE_CONFIRM_PTS) {
            static int64_t s_pc_log = 0;
            if (now_ms/1000 - s_pc_log >= 5) {
                s_pc_log = now_ms/1000;
                printf("[RSI-REV-BLOCK] price_confirm=%.2f < %.2f pts -- waiting for move\n",
                       price_move_since_extreme, PRICE_CONFIRM_PTS);
                fflush(stdout);
            }
            return;
        }
        m_rsi_peak = rsi;  // reset peak tracking on entry

        const bool is_long = rsi_turn_long;

        // ── DOM filter ────────────────────────────────────────────────────────
        bool vacuum_confirm = false;
        if (l2_real) {
            if (is_long) {
                // Don't bounce into ask-heavy book -- sellers still dominating
                if (l2_imbalance < L2_LONG_MIN) {
                    printf("[RSI-REV-BLOCK] LONG blocked: l2_imbalance=%.2f < %.2f (ask-heavy)\n",
                           l2_imbalance, L2_LONG_MIN);
                    fflush(stdout);
                    return;
                }
                // Wall below just got hit -- may not hold
                if (wall_below) {
                    printf("[RSI-REV-BLOCK] LONG blocked: wall_below (support may break)\n");
                    fflush(stdout);
                    return;
                }
                vacuum_confirm = vacuum_bid; // thin bid = reversal has room up
            } else {
                if (l2_imbalance > L2_SHORT_MAX) {
                    printf("[RSI-REV-BLOCK] SHORT blocked: l2_imbalance=%.2f > %.2f (bid-heavy)\n",
                           l2_imbalance, L2_SHORT_MAX);
                    fflush(stdout);
                    return;
                }
                if (wall_above) {
                    printf("[RSI-REV-BLOCK] SHORT blocked: wall_above (resistance may hold)\n");
                    fflush(stdout);
                    return;
                }
                vacuum_confirm = vacuum_ask; // thin ask = reversal has room down
            }
        }

        // ── Entry ─────────────────────────────────────────────────────────────
        const double entry   = is_long ? ask : bid;
        const double sl_dist = std::max(m_tick_atr * SL_ATR_MULT, spread * 2.0);

        pos.active    = true;
        pos.is_long   = is_long;
        pos.entry     = entry;
        pos.sl        = is_long ? (entry - sl_dist) : (entry + sl_dist);
        pos.atr       = m_tick_atr;
        pos.size      = 0.05;
        m_l2_at_entry = l2_imbalance;  // record L2 at entry for flip detection  // fixed 0.05 lots: 3pt move = $15 gross, ~$3.50 cost = ~$11.50 net
        pos.mfe       = 0.0;
        pos.be_locked = false;
        pos.entry_ts  = now_s;
        m_vacuum_confirm = vacuum_confirm;

        const char* pfx = shadow_mode ? "[RSI-REV-SHADOW]" : "[RSI-REV]";
        printf("%s %s entry=%.2f sl=%.2f rsi=%.1f atr=%.2f "
               "l2=%.2f wall_a=%d wall_b=%d vac=%d slot=%d\n",
               pfx, is_long ? "LONG" : "SHORT",
               entry, pos.sl, rsi, m_tick_atr,
               l2_imbalance, (int)wall_above, (int)wall_below,
               (int)vacuum_confirm, session_slot);
        fflush(stdout);
    }

    void patch_size(double lot) noexcept { if (pos.active) pos.size = lot; }

    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback on_close) noexcept {
        if (!pos.active) return;
        _close(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

    double tick_rsi() const noexcept { return m_tick_rsi; }
    // Called each tick from tick_gold.hpp to inject M1 bar RSI for entry signal
    void set_bar_rsi(double bar_rsi, double current_price = 0.0) noexcept {
        if (bar_rsi > 0.0 && bar_rsi < 100.0) {
            m_bar_rsi_prev = (m_bar_rsi > 0.0) ? m_bar_rsi : bar_rsi;
            m_bar_rsi = bar_rsi;
            if (!pos.active) {
                // Track RSI peak/trough and corresponding price
                if (bar_rsi > 50.0 && bar_rsi > m_rsi_peak) {
                    m_rsi_peak = bar_rsi;
                    if (current_price > 0.0) m_price_at_extreme = current_price;
                }
                if (bar_rsi < 50.0 && bar_rsi < m_rsi_peak) {
                    m_rsi_peak = bar_rsi;
                    if (current_price > 0.0) m_price_at_extreme = current_price;
                }
                if (m_rsi_peak == 50.0) {
                    m_rsi_peak = bar_rsi;
                    if (current_price > 0.0) m_price_at_extreme = current_price;
                }
            }
        }
    }
    double tick_atr() const noexcept { return m_tick_atr; }

private:
    double  m_tick_rsi      = 50.0;
    double  m_rsi_prev      = 50.0;
    // Bar RSI -- injected from g_bars_gold.m1.ind.rsi14 each tick
    // Used for ENTRY signal (smooth, matches chart). Tick RSI used for EXIT.
    double  m_bar_rsi       = 0.0;   // current bar RSI
    double  m_bar_rsi_prev  = 50.0;  // previous bar RSI (for turn detection)
    double  m_rsi_peak      = 50.0;  // tracks RSI extreme before reversal (reset on entry)
    double  m_price_at_extreme = 0.0; // price when RSI hit its extreme (for confirmation)
    double  m_l2_at_entry   = 0.5;   // L2 imbalance at entry (for flip detection)
    double  m_last_l2       = 0.5;   // most recent L2 imbalance (updated every tick)
    double  m_rsi_avg_gain  = 0.0;
    double  m_rsi_avg_loss  = 0.0;
    bool    m_rsi_init      = false;
    int     m_rsi_count     = 0;
    double  m_rsi_last_mid  = 0.0;
    std::deque<double> m_rsi_gains;
    std::deque<double> m_rsi_losses;

    double  m_tick_atr      = 0.0;
    double  m_atr_last_mid  = 0.0;
    bool    m_atr_init      = false;
    static constexpr double ATR_ALPHA = 2.0 / (14.0 + 1.0);

    int64_t m_cooldown_until  = 0;
    int     m_trade_id        = 0;
    bool    m_vacuum_confirm  = false;

    void _update_tick_rsi(double mid) noexcept {
        if (mid == m_rsi_last_mid) return;
        if (m_rsi_last_mid <= 0.0) { m_rsi_last_mid = mid; return; }
        const double change = mid - m_rsi_last_mid;
        m_rsi_last_mid = mid;
        m_rsi_prev = m_tick_rsi;
        ++m_rsi_count;
        const double gain = change > 0.0 ? change : 0.0;
        const double loss = change < 0.0 ? -change : 0.0;
        if (!m_rsi_init) {
            m_rsi_gains.push_back(gain);
            m_rsi_losses.push_back(loss);
            if ((int)m_rsi_gains.size() < RSI_PERIOD) return;
            double sum_g = 0.0, sum_l = 0.0;
            for (int i = 0; i < RSI_PERIOD; ++i) { sum_g += m_rsi_gains[i]; sum_l += m_rsi_losses[i]; }
            m_rsi_avg_gain = sum_g / RSI_PERIOD;
            m_rsi_avg_loss = sum_l / RSI_PERIOD;
            m_rsi_init = true;
            m_rsi_gains.clear(); m_rsi_losses.clear();
        } else {
            m_rsi_avg_gain = (m_rsi_avg_gain * (RSI_PERIOD - 1) + gain) / RSI_PERIOD;
            m_rsi_avg_loss = (m_rsi_avg_loss * (RSI_PERIOD - 1) + loss) / RSI_PERIOD;
        }
        m_tick_rsi = (m_rsi_avg_loss < 1e-10) ? 100.0
                   : 100.0 - 100.0 / (1.0 + m_rsi_avg_gain / m_rsi_avg_loss);
    }

    void _update_tick_atr(double mid, double spread) noexcept {
        if (mid == m_atr_last_mid && m_atr_init) return;
        const double range = (m_atr_last_mid > 0.0)
            ? std::max(std::fabs(mid - m_atr_last_mid), spread) : spread;
        m_atr_last_mid = mid;
        if (!m_atr_init) { m_tick_atr = range; m_atr_init = true; }
        else m_tick_atr = ATR_ALPHA * range + (1.0 - ATR_ALPHA) * m_tick_atr;
    }

    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if ((now_s - pos.entry_ts) < MIN_HOLD_S) return;
        if ((now_s - pos.entry_ts) > MAX_HOLD_S) {
            _close(pos.is_long ? bid : ask, "MAX_HOLD", now_s, on_close); return;
        }
        if (!pos.be_locked && move >= pos.atr * BE_ATR_MULT) {
            pos.sl = pos.entry; pos.be_locked = true;
            printf("[RSI-REV] BE_LOCK %s move=%.2f\n", pos.is_long ? "LONG" : "SHORT", move);
            fflush(stdout);
        }
        if (pos.be_locked) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - m_tick_atr * TRAIL_ATR_MULT)
                : (pos.entry - pos.mfe + m_tick_atr * TRAIL_ATR_MULT);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }
        if (m_rsi_count > RSI_PERIOD + 2) {
            // Primary exit: tick RSI reaches exit threshold
            const bool rsi_exit = pos.is_long ? (m_tick_rsi >= RSI_EXIT_LONG)
                                              : (m_tick_rsi <= RSI_EXIT_SHORT);
            if (rsi_exit) {
                const double exit_px = pos.is_long ? bid : ask;
                if ((pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px)) >= -(ask - bid) * 0.5) {
                    _close(exit_px, "RSI_TP", now_s, on_close); return;
                }
            }
            // Fast reversal exit: tick RSI turns against us AND we have some profit
            // This fires within seconds of a reversal -- no waiting for RSI threshold.
            const bool rsi_reversing = pos.is_long
                ? (m_tick_rsi < m_rsi_prev && m_tick_rsi > RSI_EXIT_LONG - 8.0)  // LONG: RSI turning down from above exit
                : (m_tick_rsi > m_rsi_prev && m_tick_rsi < RSI_EXIT_SHORT + 8.0); // SHORT: RSI turning up from below exit
            const double open_pnl = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (rsi_reversing && open_pnl > pos.atr * 0.3 && pos.be_locked) {
                _close(pos.is_long ? bid : ask, "RSI_TURN", now_s, on_close); return;
            }
        }
        // ?? L2 FLIP EXIT -- fires INSTANTLY when DOM flips against position ??????
        // L2 imbalance crossing 0.5 against us = sellers taking over on a LONG
        // or buyers taking over on a SHORT. This precedes price reversal by seconds.
        // Only fires when: profit > L2_EXIT_MIN_PROFIT AND BE is locked.
        if (pos.be_locked && m_last_l2 > 0.0) {
            const bool l2_flipped_against = pos.is_long
                ? (m_last_l2 < L2_EXIT_THRESHOLD && m_l2_at_entry >= L2_EXIT_THRESHOLD)
                : (m_last_l2 > L2_EXIT_THRESHOLD && m_l2_at_entry <= L2_EXIT_THRESHOLD);
            const double open_pnl = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (l2_flipped_against && open_pnl >= L2_EXIT_MIN_PROFIT) {
                printf("[RSI-REV] L2_FLIP_EXIT %s l2=%.3f was=%.3f profit=%.2f\n",
                       pos.is_long ? "LONG" : "SHORT",
                       m_last_l2, m_l2_at_entry, open_pnl);
                fflush(stdout);
                _close(pos.is_long ? bid : ask, "L2_FLIP", now_s, on_close); return;
            }
        }
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            _close(pos.is_long ? bid : ask,
                   pos.be_locked ? (std::fabs(pos.sl - pos.entry) < 0.01 ? "BE_HIT" : "TRAIL_HIT") : "SL_HIT",
                   now_s, on_close);
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;
        printf("[RSI-REV] EXIT %s @ %.2f reason=%s pnl_raw=%.4f mfe=%.2f\n",
               pos.is_long ? "LONG" : "SHORT", exit_px, reason, pnl, pos.mfe);
        fflush(stdout);
        omega::TradeRecord tr;
        tr.id = ++m_trade_id; tr.symbol = "XAUUSD";
        tr.side = pos.is_long ? "LONG" : "SHORT";
        tr.engine = "RSIReversal"; tr.regime = "RSI_REVERSAL";
        tr.entryPrice = pos.entry; tr.exitPrice = exit_px;
        tr.tp = 0.0; tr.sl = pos.sl;
        tr.size = pos.size;
        tr.pnl = pnl;        // raw pts*lots -- handle_closed_trade applies tick_mult
        tr.net_pnl = tr.pnl;
        tr.mfe = pos.mfe * pos.size; tr.mae = 0.0;
        tr.entryTs = pos.entry_ts; tr.exitTs = now_s;
        tr.exitReason = reason; tr.spreadAtEntry = 0.0;
        pos = Position{};
        // Vacuum confirm = shorter cooldown: re-enter faster on confirmed direction
        m_cooldown_until = now_s + (m_vacuum_confirm ? COOLDOWN_S_VACUUM : COOLDOWN_S);
        m_vacuum_confirm = false;
        if (on_close) on_close(tr);
    }
};

} // namespace omega
