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
//   All sessions (Asia + London + NY). Dead zone 05-07 UTC blocked.
//
// PROTECTION:
//   SL = 0.6x ATR from entry
//   BE lock at 0.4x ATR profit
//   Trail = 0.4x ATR behind MFE
//   RSI exit: LONG exits when tick RSI recovers to 52, SHORT exits at 48
//   Max hold 10 minutes, cooldown 60s after close
//
// ATR: also computed tick-level from EWM of |price change| -- no bars needed
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
    double RSI_OVERSOLD       = 42.0;  // LONG when tick RSI < this
    double RSI_OVERBOUGHT     = 58.0;  // SHORT when tick RSI > this
    double RSI_EXIT_LONG      = 52.0;  // exit LONG when RSI recovers here
    double RSI_EXIT_SHORT     = 48.0;  // exit SHORT when RSI recovers here
    int    RSI_PERIOD         = 14;    // tick RSI period
    double SL_ATR_MULT        = 0.6;
    double TRAIL_ATR_MULT     = 0.40;
    double BE_ATR_MULT        = 0.40;
    double MAX_SPREAD_PTS     = 2.5;
    double MIN_ATR_PTS        = 1.0;   // minimum tick ATR (dead tape filter)
    int    COOLDOWN_S         = 60;
    int    MAX_HOLD_S         = 600;
    int    MIN_HOLD_S         = 8;
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

    // ── Main tick ─────────────────────────────────────────────────────────────
    // Called every XAUUSD tick. No bars_ready dependency.
    void on_tick(double bid, double ask,
                 int session_slot, int64_t now_ms,
                 CloseCallback on_close) noexcept
    {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        // Update tick-level indicators on every tick
        _update_tick_rsi(mid);
        _update_tick_atr(mid, spread);

        // ── Manage open position ─────────────────────────────────────────────
        if (pos.active) {
            _manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // ── Cooldown ─────────────────────────────────────────────────────────
        if (now_s < m_cooldown_until) return;

        // ── Entry gates ───────────────────────────────────────────────────────
        if (spread > MAX_SPREAD_PTS)        return;
        if (m_tick_atr < MIN_ATR_PTS)       return;
        if (m_rsi_count < RSI_PERIOD + 2)   return;  // RSI not warmed yet
        if (session_slot == 0)              return;  // dead zone
        if (_in_dead_zone())                return;  // 05-07 UTC

        const double rsi = m_tick_rsi;

        // ── RSI extreme + turning ─────────────────────────────────────────────
        const bool rsi_oversold   = (rsi < RSI_OVERSOLD);
        const bool rsi_overbought = (rsi > RSI_OVERBOUGHT);
        if (!rsi_oversold && !rsi_overbought) return;

        // Must be TURNING -- not still falling/rising
        if (rsi_oversold  && rsi <= m_rsi_prev) return;  // still falling
        if (rsi_overbought && rsi >= m_rsi_prev) return;  // still rising

        // ── Entry ─────────────────────────────────────────────────────────────
        const bool is_long   = rsi_oversold;
        const double entry   = is_long ? ask : bid;
        const double sl_dist = std::max(m_tick_atr * SL_ATR_MULT, spread * 2.0);

        pos.active    = true;
        pos.is_long   = is_long;
        pos.entry     = entry;
        pos.sl        = is_long ? (entry - sl_dist) : (entry + sl_dist);
        pos.atr       = m_tick_atr;
        pos.size      = 0.01;
        pos.mfe       = 0.0;
        pos.be_locked = false;
        pos.entry_ts  = now_s;

        const char* pfx = shadow_mode ? "[RSI-REV-SHADOW]" : "[RSI-REV]";
        printf("%s %s entry=%.2f sl=%.2f rsi=%.1f atr=%.2f spread=%.2f slot=%d\n",
               pfx, is_long ? "LONG" : "SHORT",
               entry, pos.sl, rsi, m_tick_atr, spread, session_slot);
        fflush(stdout);
    }

    void patch_size(double lot) noexcept {
        if (pos.active) pos.size = lot;
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback on_close) noexcept {
        if (!pos.active) return;
        const double exit_px = pos.is_long ? bid : ask;
        _close(exit_px, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

    double tick_rsi() const noexcept { return m_tick_rsi; }
    double tick_atr() const noexcept { return m_tick_atr; }

private:
    // ── Tick RSI state ────────────────────────────────────────────────────────
    double  m_tick_rsi      = 50.0;
    double  m_rsi_prev      = 50.0;
    double  m_rsi_avg_gain  = 0.0;
    double  m_rsi_avg_loss  = 0.0;
    bool    m_rsi_init      = false;
    int     m_rsi_count     = 0;
    double  m_rsi_last_mid  = 0.0;
    std::deque<double> m_rsi_gains;  // seed gains
    std::deque<double> m_rsi_losses; // seed losses

    // ── Tick ATR state ────────────────────────────────────────────────────────
    double  m_tick_atr      = 0.0;
    double  m_atr_last_mid  = 0.0;
    bool    m_atr_init      = false;
    // EWM ATR: alpha = 2/(period+1) with period=14
    static constexpr double ATR_ALPHA = 2.0 / (14.0 + 1.0);

    // ── Other state ───────────────────────────────────────────────────────────
    int64_t m_cooldown_until = 0;
    int     m_trade_id       = 0;

    void _update_tick_rsi(double mid) noexcept {
        if (m_rsi_last_mid <= 0.0) { m_rsi_last_mid = mid; return; }
        const double change = mid - m_rsi_last_mid;
        m_rsi_last_mid = mid;
        m_rsi_prev = m_tick_rsi;
        ++m_rsi_count;

        const double gain = change > 0.0 ? change : 0.0;
        const double loss = change < 0.0 ? -change : 0.0;

        if (!m_rsi_init) {
            // Collect RSI_PERIOD samples of both gains and losses for proper seed
            m_rsi_gains.push_back(gain);
            m_rsi_losses.push_back(loss);
            if ((int)m_rsi_gains.size() < RSI_PERIOD) return;
            // Seed avg gain and avg loss from first RSI_PERIOD ticks (Wilder standard)
            double sum_g = 0.0, sum_l = 0.0;
            for (int i = 0; i < RSI_PERIOD; ++i) {
                sum_g += m_rsi_gains[i];
                sum_l += m_rsi_losses[i];
            }
            m_rsi_avg_gain = sum_g / RSI_PERIOD;
            m_rsi_avg_loss = sum_l / RSI_PERIOD;
            m_rsi_init = true;
            m_rsi_gains.clear();
            m_rsi_losses.clear();
        } else {
            m_rsi_avg_gain = (m_rsi_avg_gain * (RSI_PERIOD - 1) + gain) / RSI_PERIOD;
            m_rsi_avg_loss = (m_rsi_avg_loss * (RSI_PERIOD - 1) + loss) / RSI_PERIOD;
        }

        if (m_rsi_avg_loss < 1e-10) {
            m_tick_rsi = 100.0;
        } else {
            m_tick_rsi = 100.0 - 100.0 / (1.0 + m_rsi_avg_gain / m_rsi_avg_loss);
        }
    }

    void _update_tick_atr(double mid, double spread) noexcept {
        const double range = (m_atr_last_mid > 0.0)
            ? std::max(std::fabs(mid - m_atr_last_mid), spread)
            : spread;
        m_atr_last_mid = mid;
        if (!m_atr_init) {
            m_tick_atr = range;
            m_atr_init = true;
        } else {
            m_tick_atr = ATR_ALPHA * range + (1.0 - ATR_ALPHA) * m_tick_atr;
        }
    }

    static bool _in_dead_zone() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return (ti.tm_hour >= 5 && ti.tm_hour < 7);
    }

    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        if ((now_s - pos.entry_ts) < MIN_HOLD_S) return;

        // Max hold
        if ((now_s - pos.entry_ts) > MAX_HOLD_S) {
            _close(pos.is_long ? bid : ask, "MAX_HOLD", now_s, on_close);
            return;
        }

        // BE lock
        if (!pos.be_locked && move >= pos.atr * BE_ATR_MULT) {
            pos.sl       = pos.entry;
            pos.be_locked = true;
            printf("[RSI-REV] BE_LOCK %s move=%.2f\n",
                   pos.is_long ? "LONG" : "SHORT", move);
            fflush(stdout);
        }

        // Trail once BE locked
        if (pos.be_locked) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - m_tick_atr * TRAIL_ATR_MULT)
                : (pos.entry - pos.mfe + m_tick_atr * TRAIL_ATR_MULT);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        // RSI exit -- take profit when RSI returns to neutral
        if (m_rsi_count > RSI_PERIOD + 2) {
            const bool rsi_exit_long  = pos.is_long  && (m_tick_rsi >= RSI_EXIT_LONG);
            const bool rsi_exit_short = !pos.is_long && (m_tick_rsi <= RSI_EXIT_SHORT);
            if (rsi_exit_long || rsi_exit_short) {
                const double exit_px   = pos.is_long ? bid : ask;
                const double exit_move = pos.is_long
                    ? (exit_px - pos.entry) : (pos.entry - exit_px);
                const double min_profit = -(ask - bid) * 0.5;
                if (exit_move >= min_profit) {
                    printf("[RSI-REV] RSI_EXIT %s rsi=%.1f move=%.2f\n",
                           pos.is_long ? "LONG" : "SHORT", m_tick_rsi, exit_move);
                    fflush(stdout);
                    _close(exit_px, "RSI_TP", now_s, on_close);
                    return;
                }
            }
        }

        // SL
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
        const double pnl = (pos.is_long
            ? (exit_px - pos.entry)
            : (pos.entry - exit_px)) * pos.size * 100.0;

        printf("[RSI-REV] EXIT %s @ %.2f reason=%s pnl=%.2f mfe=%.2f\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl, pos.mfe);
        fflush(stdout);

        omega::TradeRecord tr;
        tr.id          = ++m_trade_id;
        tr.symbol      = "XAUUSD";
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.engine      = "RSIReversal";
        tr.regime      = "RSI_REVERSAL";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = pnl / 100.0;
        tr.mfe         = pos.mfe * pos.size;
        tr.mae         = 0.0;
        tr.entryTs     = pos.entry_ts;
        tr.exitTs      = now_s;
        tr.exitReason  = reason;
        tr.spreadAtEntry = 0.0;

        pos = Position{};
        m_cooldown_until = now_s + COOLDOWN_S;

        if (on_close) on_close(tr);
    }
};

} // namespace omega
