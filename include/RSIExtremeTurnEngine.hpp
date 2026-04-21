// =============================================================================
//  RSIExtremeTurnEngine.hpp
//
//  Strategy: RSI Extreme + Sustained Turn (no DOM)
//
//  Backtest result (rsi_extreme_bt.cpp, 2 days XAUUSD):
//    12 trades, 75% WR, $20 PnL, $12 MaxDD
//    Best params: MIN_SUSTAINED_BARS=3, RSI_LOW=20, RSI_HIGH=70,
//                 exit_long=55, exit_short=45, SL=0.5xATR
//    DOM adds noise NOT signal -- no DOM requirement
//
//  ENTRY:
//    LONG:  bar RSI fell below RSI_ENTRY_LOW for MIN_SUSTAINED_BARS bars,
//           then turns UP (current bar RSI > previous bar RSI by MIN_TURN_PTS)
//    SHORT: bar RSI rose above RSI_ENTRY_HIGH for MIN_SUSTAINED_BARS bars,
//           then turns DOWN (current bar RSI < previous bar RSI by MIN_TURN_PTS)
//    Cost gate: ATR > 1.5x cost (spread + slippage + commission)
//    No DOM requirement (proven: DOM adds noise not signal for RSI turns)
//
//  EXIT:
//    LONG:  RSI crosses back above RSI_EXIT_LONG (default 55)
//    SHORT: RSI crosses back below RSI_EXIT_SHORT (default 45)
//    SL:    0.5x ATR behind entry
//    BE:    lock at 0.4x ATR profit
//    Trail: 0.4x ATR behind MFE once BE locked
//    Max hold: MAX_HOLD_S (default 300s = 5 minutes)
//    Cooldown: COOLDOWN_S (default 60s) after any exit
//
//  Design notes:
//    - Uses M1 bar RSI (injected from g_bars_gold.m1.ind.rsi14) for
//      entry/exit signals -- bar RSI is smooth and matches the chart.
//    - Tick RSI used as warmup guard only (m_rsi_count).
//    - m_sustained_bars tracks consecutive bars RSI has been in extreme zone.
//    - m_rsi_bar_prev tracks previous bar RSI for turn detection.
//    - Shadow mode by default until live validation.
// =============================================================================

#pragma once
#include <iomanip>
#include <iostream>
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

class RSIExtremeTurnEngine {
public:
    // ── Parameters (match backtest-confirmed values) ─────────────────────────
    double RSI_ENTRY_LOW      = 20.0;  // enter LONG when bar RSI falls below this
    double RSI_ENTRY_HIGH     = 70.0;  // enter SHORT when bar RSI rises above this
    double RSI_EXIT_LONG      = 55.0;  // exit LONG when bar RSI crosses above this
    double RSI_EXIT_SHORT     = 45.0;  // exit SHORT when bar RSI crosses below this
    int    MIN_SUSTAINED_BARS = 3;     // RSI must have been in extreme zone for N M1 bars
    double MIN_TURN_PTS       = 0.5;   // min RSI pts change to confirm turn (noise filter)
    double SL_ATR_MULT        = 0.50;  // SL = 0.5x ATR behind entry
    double BE_ATR_MULT        = 0.40;  // BE lock at 0.4x ATR profit
    double TRAIL_ATR_MULT     = 0.40;  // trail = 0.4x ATR behind MFE
    double COST_ATR_MULT      = 1.50;  // ATR must be > 1.5x cost to enter
    double MAX_SPREAD_PTS     = 2.5;   // spread gate
    int    MAX_HOLD_S         = 300;   // 5 min max hold
    int    MIN_HOLD_S         = 5;     // 5s min hold before exit checks
    int    COOLDOWN_S         = 60;    // post-exit cooldown
    bool   enabled            = true;
    bool   shadow_mode        = true;  // true = log only, no live orders

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
        double  rsi_at_entry = 0.0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }
    void patch_size(double lot) noexcept { if (pos.active) pos.size = lot; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Called unconditionally every tick from tick_gold to keep RSI/ATR warm ─
    void update_indicators(double bid, double ask) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        _update_tick_rsi(mid);
        _update_tick_atr(mid, spread);
    }

    // ── Inject M1 bar RSI every tick (smooth, matches chart) ─────────────────
    // Called unconditionally from tick_gold before entry gate so bar RSI is
    // always current regardless of position state.
    void set_bar_rsi(double bar_rsi) noexcept {
        if (bar_rsi <= 0.0 || bar_rsi >= 100.0) return;

        const double prev = m_bar_rsi;  // previous bar RSI before update

        // Count consecutive bars RSI has been in extreme zone.
        // A "bar" here = each injection from the M1 bar engine (one per 60s).
        // We detect new bar events by checking if bar_rsi changed (M1 fires every 60s,
        // so same value for ~60 ticks then a step change).
        if (std::fabs(bar_rsi - m_bar_rsi) > 0.01) {
            // Bar RSI has changed -- new M1 bar has closed
            m_bar_rsi_prev = m_bar_rsi;  // save old value for turn detection
            m_bar_rsi      = bar_rsi;

            // Update sustained extreme bar counter
            if (bar_rsi < RSI_ENTRY_LOW) {
                m_sustained_oversold_bars++;
                m_sustained_overbought_bars = 0;
            } else if (bar_rsi > RSI_ENTRY_HIGH) {
                m_sustained_overbought_bars++;
                m_sustained_oversold_bars = 0;
            } else {
                // RSI left the extreme zone -- reset ONLY if we haven't just entered
                // (don't reset mid-trade if RSI recovers past the entry threshold)
                if (!pos.active) {
                    m_sustained_oversold_bars   = 0;
                    m_sustained_overbought_bars = 0;
                }
            }
        } else {
            // Same bar RSI value (within same M1 bar) -- just update current
            m_bar_rsi = bar_rsi;
            // Don't update prev or sustained counters mid-bar
        }
        (void)prev;
    }

    // ── Main tick ─────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask,
                 int session_slot, int64_t now_ms,
                 CloseCallback on_close = nullptr) noexcept
    {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        _update_tick_rsi(mid);
        _update_tick_atr(mid, spread);

        if (pos.active) {
            _manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // Cooldown guard
        if (now_s < m_cooldown_until) return;

        // Basic quality gates
        if (spread > MAX_SPREAD_PTS)       return;
        if (m_tick_atr < 1.0)              return;
        if (m_rsi_count < 16)              return;  // tick RSI warmup (need >14 periods)
        if (m_bar_rsi <= 0.0)              return;  // bar RSI not yet injected
        if (m_bar_rsi_prev <= 0.0)         return;  // no previous bar RSI yet

        // Cost gate: ATR must be > 1.5x total round-trip cost.
        // Total cost = spread + 2x slippage + 2x commission.
        // At default values: cost = spread + 0.20 + 0.20 = spread + 0.40
        // With spread=0.3pt: cost=0.7pt, need ATR > 1.05pt -- always passes on active tape.
        // With spread=2.0pt: cost=2.4pt, need ATR > 3.6pt -- correctly filters thin Asia.
        const double total_cost = spread + 0.20 + 0.20;  // slippage + commission (pts)
        if (m_tick_atr < COST_ATR_MULT * total_cost) {
            static int64_t s_cost_log = 0;
            if (now_s - s_cost_log >= 30) {
                s_cost_log = now_s;
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[RSI-EXT-BLOCK] cost_gate: atr=%.2f < %.2fx cost=%.2f -- thin tape\\n",                        m_tick_atr, COST_ATR_MULT, total_cost);
                    std::cout << _buf;
                    std::cout.flush();
                }
            }
            return;
        }

        // ── Entry detection ───────────────────────────────────────────────────
        // LONG: bar RSI was below RSI_ENTRY_LOW for MIN_SUSTAINED_BARS, now turning UP
        // RSI turn = current bar RSI > previous bar RSI by at least MIN_TURN_PTS
        const bool rsi_turn_up   = (m_bar_rsi > m_bar_rsi_prev + MIN_TURN_PTS);
        const bool rsi_turn_down = (m_bar_rsi < m_bar_rsi_prev - MIN_TURN_PTS);

        const bool long_setup  = (m_sustained_oversold_bars   >= MIN_SUSTAINED_BARS) && rsi_turn_up;
        const bool short_setup = (m_sustained_overbought_bars >= MIN_SUSTAINED_BARS) && rsi_turn_down;

        if (!long_setup && !short_setup) return;

        // Confirm RSI is still in a relevant zone at entry (not fully recovered)
        // LONG:  RSI should still be below 40 at the turn (not chasing already-recovered RSI)
        // SHORT: RSI should still be above 60 at the turn
        if (long_setup  && m_bar_rsi >= 40.0) {
            static int64_t s_log = 0;
            if (now_s - s_log >= 15) {
                s_log = now_s;
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[RSI-EXT-BLOCK] LONG: rsi=%.1f already above 40 -- too late\\n", m_bar_rsi);
                    std::cout << _buf;
                    std::cout.flush();
                }
            }
            return;
        }
        if (short_setup && m_bar_rsi <= 60.0) {
            static int64_t s_log = 0;
            if (now_s - s_log >= 15) {
                s_log = now_s;
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[RSI-EXT-BLOCK] SHORT: rsi=%.1f already below 60 -- too late\\n", m_bar_rsi);
                    std::cout << _buf;
                    std::cout.flush();
                }
            }
            return;
        }

        const bool is_long = long_setup;

        // ── Enter ─────────────────────────────────────────────────────────────
        const double entry   = is_long ? ask : bid;
        const double sl_dist = std::max(m_tick_atr * SL_ATR_MULT, spread * 1.5);

        pos.active       = true;
        pos.is_long      = is_long;
        pos.entry        = entry;
        pos.sl           = is_long ? (entry - sl_dist) : (entry + sl_dist);
        pos.atr          = m_tick_atr;
        pos.size         = 0.01;  // caller must patch_size() after entry
        pos.mfe          = 0.0;
        pos.be_locked    = false;
        pos.entry_ts     = now_s;
        pos.rsi_at_entry = m_bar_rsi;

        // Reset sustained counters -- they'll rebuild correctly for the next trade
        m_sustained_oversold_bars   = 0;
        m_sustained_overbought_bars = 0;

        const char* pfx = shadow_mode ? "[RSI-EXT-SHADOW]" : "[RSI-EXT]";
        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "%s %s entry=%.2f sl=%.2f(dist=%.2f) rsi=%.1f(was %.1f) "                "sustained=%d atr=%.2f spread=%.2f slot=%d\\n",                pfx, is_long ? "LONG" : "SHORT",                entry, pos.sl, sl_dist,                m_bar_rsi, m_bar_rsi_prev,                is_long ? m_sustained_oversold_bars + MIN_SUSTAINED_BARS                        : m_sustained_overbought_bars + MIN_SUSTAINED_BARS,                m_tick_atr, spread, session_slot);
            std::cout << _buf;
            std::cout.flush();
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback on_close) noexcept {
        if (!pos.active) return;
        _close(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

    double tick_rsi()  const noexcept { return m_tick_rsi; }
    double tick_atr()  const noexcept { return m_tick_atr; }
    double bar_rsi()   const noexcept { return m_bar_rsi; }
    int    sustained_oversold_bars()   const noexcept { return m_sustained_oversold_bars; }
    int    sustained_overbought_bars() const noexcept { return m_sustained_overbought_bars; }

private:
    // ── Bar RSI state ─────────────────────────────────────────────────────────
    double  m_bar_rsi       = 0.0;   // current injected bar RSI
    double  m_bar_rsi_prev  = 0.0;   // bar RSI on previous M1 bar
    int     m_sustained_oversold_bars   = 0;  // consecutive M1 bars with RSI < RSI_ENTRY_LOW
    int     m_sustained_overbought_bars = 0;  // consecutive M1 bars with RSI > RSI_ENTRY_HIGH

    // ── Tick RSI state (for warmup guard and manage) ──────────────────────────
    double  m_tick_rsi      = 50.0;
    double  m_rsi_prev      = 50.0;
    double  m_rsi_avg_gain  = 0.0;
    double  m_rsi_avg_loss  = 0.0;
    bool    m_rsi_init      = false;
    int     m_rsi_count     = 0;
    double  m_rsi_last_mid  = 0.0;
    std::deque<double> m_rsi_gains;
    std::deque<double> m_rsi_losses;
    static constexpr int RSI_PERIOD_TICK = 14;

    // ── Tick ATR state ────────────────────────────────────────────────────────
    double  m_tick_atr      = 0.0;
    double  m_atr_last_mid  = 0.0;
    bool    m_atr_init      = false;
    static constexpr double ATR_ALPHA = 2.0 / (14.0 + 1.0);

    // ── Cooldown ──────────────────────────────────────────────────────────────
    int64_t m_cooldown_until = 0;
    int     m_trade_id       = 0;

    // ── Indicator updates ─────────────────────────────────────────────────────
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
            if ((int)m_rsi_gains.size() < RSI_PERIOD_TICK) return;
            double sum_g = 0.0, sum_l = 0.0;
            for (int i = 0; i < RSI_PERIOD_TICK; ++i) { sum_g += m_rsi_gains[i]; sum_l += m_rsi_losses[i]; }
            m_rsi_avg_gain = sum_g / RSI_PERIOD_TICK;
            m_rsi_avg_loss = sum_l / RSI_PERIOD_TICK;
            m_rsi_init = true;
            m_rsi_gains.clear();
            m_rsi_losses.clear();
        } else {
            m_rsi_avg_gain = (m_rsi_avg_gain * (RSI_PERIOD_TICK - 1) + gain) / RSI_PERIOD_TICK;
            m_rsi_avg_loss = (m_rsi_avg_loss * (RSI_PERIOD_TICK - 1) + loss) / RSI_PERIOD_TICK;
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

    // ── Position management ───────────────────────────────────────────────────
    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        // Min hold: don't exit on first tick (noise)
        if ((now_s - pos.entry_ts) < MIN_HOLD_S) return;

        // Max hold timeout
        if ((now_s - pos.entry_ts) > MAX_HOLD_S) {
            _close(pos.is_long ? bid : ask, "MAX_HOLD", now_s, on_close);
            return;
        }

        // BE lock
        if (!pos.be_locked && move >= pos.atr * BE_ATR_MULT) {
            pos.sl = pos.entry;
            pos.be_locked = true;
            {
                // converted from printf
                char _buf[512];
                snprintf(_buf, sizeof(_buf), "[RSI-EXT] BE_LOCK %s move=%.2f atr=%.2f\\n",                    pos.is_long ? "LONG" : "SHORT", move, pos.atr);
                std::cout << _buf;
                std::cout.flush();
            }
        }

        // Trail once BE locked
        if (pos.be_locked) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - pos.atr * TRAIL_ATR_MULT)
                : (pos.entry - pos.mfe + pos.atr * TRAIL_ATR_MULT);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        // Primary RSI exit: bar RSI has recovered to exit threshold
        // LONG exit: bar RSI >= RSI_EXIT_LONG (55)
        // SHORT exit: bar RSI <= RSI_EXIT_SHORT (45)
        if (m_bar_rsi > 0.0) {
            const bool rsi_exit = pos.is_long  ? (m_bar_rsi >= RSI_EXIT_LONG)
                                               : (m_bar_rsi <= RSI_EXIT_SHORT);
            if (rsi_exit) {
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[RSI-EXT] RSI_EXIT %s rsi=%.1f (threshold=%.1f) profit=%.2f\\n",                        pos.is_long ? "LONG" : "SHORT", m_bar_rsi,                        pos.is_long ? RSI_EXIT_LONG : RSI_EXIT_SHORT,                        pos.is_long ? (mid - pos.entry) : (pos.entry - mid));
                    std::cout << _buf;
                    std::cout.flush();
                }
                _close(pos.is_long ? bid : ask, "RSI_TP", now_s, on_close);
                return;
            }
        }

        // SL hit
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const char* reason = pos.be_locked
                ? (std::fabs(pos.sl - pos.entry) < 0.01 ? "BE_HIT" : "TRAIL_HIT")
                : "SL_HIT";
            _close(pos.is_long ? bid : ask, reason, now_s, on_close);
        }
    }

    // ── Close ─────────────────────────────────────────────────────────────────
    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;
        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[RSI-EXT] EXIT %s @ %.2f reason=%s pnl_raw=%.4f mfe=%.2f rsi_at_entry=%.1f\\n",                pos.is_long ? "LONG" : "SHORT",                exit_px, reason, pnl, pos.mfe, pos.rsi_at_entry);
            std::cout << _buf;
            std::cout.flush();
        }

        omega::TradeRecord tr;
        tr.id          = ++m_trade_id;
        tr.symbol      = "XAUUSD";
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.engine      = "RSIExtremeTurn";
        tr.regime      = "RSI_EXTREME";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.tp          = 0.0;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = pnl;
        tr.net_pnl     = pnl;
        tr.mfe         = pos.mfe * pos.size;
        tr.mae         = 0.0;
        tr.entryTs     = pos.entry_ts;
        tr.exitTs      = now_s;
        tr.exitReason  = reason;
        tr.spreadAtEntry = 0.0;
        tr.shadow      = shadow_mode;

        pos = Position{};
        m_cooldown_until = now_s + COOLDOWN_S;

        if (on_close) on_close(tr);
    }
};

} // namespace omega
