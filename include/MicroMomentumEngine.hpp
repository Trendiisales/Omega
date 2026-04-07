#pragma once
// =============================================================================
// MicroMomentumEngine.hpp  --  Tick momentum capture on XAUUSD
// =============================================================================
//
// SIGNAL:
//   rsi_delta = RSI_now - RSI_N_ticks_ago  (rolling window, level-agnostic)
//   disp      = mid - anchor               (EWM anchor, α=0.15)
//
//   LONG:  rsi_delta > +RSI_DELTA_MIN  AND  disp >  ENTRY_DISP_PTS
//   SHORT: rsi_delta < -RSI_DELTA_MIN  AND  disp < -ENTRY_DISP_PTS
//
//   Captures ALL RSI swings regardless of level:
//     40→60: delta=+20 → LONG
//     60→40: delta=-20 → SHORT
//     75→55: delta=-20 → SHORT (reversal also caught)
//
// PROTECTION LADDER (fires as often as possible, locks profit immediately):
//
//   Step 0 -- Entry SL:
//     SL = max(ATR * 0.5, spread * 2.0)   typically ~1.0-1.5pt
//     Max loss per trade kept tiny.
//
//   Step 1 -- Breakeven at 1.0pt profit:
//     Once price moves 1.0pt in our favour, SL moves to entry (BE).
//     We cannot lose money from this point.
//
//   Step 2 -- Lock 50% at 2.0pt profit:
//     Once price moves 2.0pt, SL advances to entry + 1.0pt (locks half TP).
//     We are guaranteed at least +1.0pt even if price reverses.
//
//   Step 3 -- Tight trail above 2.0pt:
//     Trail distance = 1.0pt. SL follows price tick-by-tick.
//     Price keeps moving → SL keeps advancing → we capture the full move.
//     Price reverses 1.0pt → we exit with whatever we had.
//
//   TP: 4.0pt fixed -- if price runs straight to TP we take it.
//       Trail may exit before TP if price stalls -- that's fine.
//
//   RSI_DELTA reversal exit:
//     If rsi_delta reverses past threshold while in profit > 0.8pt,
//     exit immediately -- momentum has died.
//
// NET EFFECT:
//   - Small move (1-2pt): BE or +1pt locked, exit on trail/RSI reversal
//   - Medium move (2-4pt): +1pt locked minimum, trail captures most of it
//   - Full TP (4pt): full TP taken
//   - Wrong direction: tight initial SL, out fast
//
// COOLDOWN: 20s -- fires again quickly after close
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <functional>
#include <string>
#include <deque>
#include "OmegaTradeLedger.hpp"

namespace omega {

class MicroMomentumEngine {
public:
    // ── Parameters ────────────────────────────────────────────────────────────
    double ENTRY_DISP_PTS  = 1.0;   // price displacement from anchor to trigger
    double RSI_DELTA_MIN   = 8.0;   // RSI must have moved this many units over window
    int    RSI_DELTA_WINDOW = 10;   // ticks for RSI delta measurement
    double TP_PTS          = 4.0;   // fixed TP -- trail may exit before this
    double SL_ATR_MULT     = 0.5;   // initial SL = max(ATR*mult, spread*2)
    double BE_TRIGGER_PTS  = 1.0;   // profit to trigger breakeven
    double LOCK_TRIGGER_PTS = 2.0;  // profit to lock 50% (SL → entry + 1.0pt)
    double LOCK_SL_PTS     = 1.0;   // SL offset from entry once locked
    double TRAIL_DIST_PTS  = 1.0;   // trail distance once above LOCK_TRIGGER
    double MAX_SPREAD_PTS  = 2.5;
    int    COOLDOWN_S      = 20;
    int    MAX_HOLD_S      = 240;
    int    MIN_HOLD_S      = 3;
    int    WARMUP_TICKS    = 25;
    bool   enabled         = true;
    bool   shadow_mode     = true;

    // ── Position ──────────────────────────────────────────────────────────────
    struct Position {
        bool    active    = false;
        bool    is_long   = false;
        double  entry     = 0.0;
        double  tp        = 0.0;
        double  sl        = 0.0;   // live SL -- advances with ladder
        double  sl_init   = 0.0;   // original SL at entry (for logging)
        double  size      = 0.01;
        double  mfe       = 0.0;
        int64_t entry_ts  = 0;
        int     step      = 0;     // 0=initial 1=BE 2=locked 3=trailing
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // ── Main tick ──────────────────────────────────────────────────────────────
    void on_tick(double bid, double ask,
                 int session_slot, int64_t now_ms,
                 CloseCallback on_close) noexcept
    {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        _update(mid, spread);

        if (pos.active) {
            _manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // ── Entry gates ───────────────────────────────────────────────────────
        if (now_s < m_cooldown_until)                         return;
        if (m_tick_count < WARMUP_TICKS)                      return;
        if (spread > MAX_SPREAD_PTS)                          return;
        if (!m_rsi_seeded)                                    return;
        if ((int)m_rsi_window.size() < RSI_DELTA_WINDOW)     return;

        const double rsi_now   = m_tick_rsi;
        const double rsi_delta = rsi_now - m_rsi_window.front();
        const double disp      = mid - m_anchor;

        const bool long_signal  = (rsi_delta >  RSI_DELTA_MIN)
                                && (disp     >  ENTRY_DISP_PTS);
        const bool short_signal = (rsi_delta < -RSI_DELTA_MIN)
                                && (disp     < -ENTRY_DISP_PTS);

        if (!long_signal && !short_signal) {
            if (now_s - m_last_block_log >= 30) {
                m_last_block_log = now_s;
                printf("[MICROMOM-BLOCK] rsi=%.1f delta=%.2f(need %.1f) "
                       "disp=%.2f(need %.1f) spread=%.2f cooldown=%llds\n",
                       rsi_now, rsi_delta, RSI_DELTA_MIN,
                       disp, ENTRY_DISP_PTS, spread,
                       (long long)std::max((int64_t)0, m_cooldown_until - now_s));
                fflush(stdout);
            }
            return;
        }

        const bool   is_long = long_signal;
        const double entry   = is_long ? ask : bid;
        const double sl_pts  = std::max(m_tick_atr * SL_ATR_MULT, spread * 2.0);

        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = entry;
        pos.tp       = is_long ? (entry + TP_PTS) : (entry - TP_PTS);
        pos.sl       = is_long ? (entry - sl_pts)  : (entry + sl_pts);
        pos.sl_init  = pos.sl;
        pos.size     = 0.01;
        pos.mfe      = 0.0;
        pos.entry_ts = now_s;
        pos.step     = 0;

        const char* pfx = shadow_mode ? "[MICROMOM-SHADOW]" : "[MICROMOM]";
        printf("%s %s entry=%.2f tp=%.2f sl=%.2f(%.2fpt) "
               "rsi=%.1f delta=%.2f disp=%.2f spread=%.2f slot=%d\n",
               pfx, is_long ? "LONG" : "SHORT",
               entry, pos.tp, pos.sl, sl_pts,
               rsi_now, rsi_delta, disp, spread, session_slot);
        fflush(stdout);
    }

    void patch_size(double lot) noexcept { if (pos.active) pos.size = lot; }

    double rsi_val()       const noexcept { return m_tick_rsi; }
    double rsi_delta_val() const noexcept {
        if ((int)m_rsi_window.size() < RSI_DELTA_WINDOW) return 0.0;
        return m_tick_rsi - m_rsi_window.front();
    }
    double anchor_disp(double mid) const noexcept { return mid - m_anchor; }

private:
    // ── RSI (Wilder 14) ───────────────────────────────────────────────────────
    double  m_tick_rsi     = 50.0;
    double  m_rsi_prev     = 50.0;
    double  m_rsi_avg_gain = 0.0;
    double  m_rsi_avg_loss = 0.0;
    bool    m_rsi_seeded   = false;
    std::deque<double> m_rsi_seed_g;
    std::deque<double> m_rsi_seed_l;
    double  m_rsi_last_mid = 0.0;
    static constexpr int RSI_PERIOD = 14;

    std::deque<double> m_rsi_window;  // rolling RSI values for delta

    // ── Price anchor (EWM α=0.15, ~6-tick memory) ────────────────────────────
    double  m_anchor      = 0.0;
    bool    m_anchor_init = false;
    static constexpr double ANCHOR_ALPHA = 0.15;

    // ── Tick ATR ──────────────────────────────────────────────────────────────
    double  m_tick_atr     = 1.0;
    double  m_atr_last_mid = 0.0;
    bool    m_atr_init     = false;
    static constexpr double ATR_ALPHA = 2.0 / 15.0;

    int     m_tick_count     = 0;
    int64_t m_cooldown_until = 0;
    int64_t m_last_block_log = 0;
    int     m_trade_id       = 0;

    // ── Indicator update ──────────────────────────────────────────────────────
    void _update(double mid, double spread) noexcept {
        ++m_tick_count;

        // Anchor
        if (!m_anchor_init) { m_anchor = mid; m_anchor_init = true; }
        else m_anchor = ANCHOR_ALPHA * mid + (1.0 - ANCHOR_ALPHA) * m_anchor;

        // ATR
        if (!m_atr_init) { m_tick_atr = std::max(spread, 0.1); m_atr_init = true; }
        else {
            const double chg = (m_atr_last_mid > 0.0)
                ? std::max(std::fabs(mid - m_atr_last_mid), spread)
                : spread;
            m_tick_atr = ATR_ALPHA * chg + (1.0 - ATR_ALPHA) * m_tick_atr;
        }
        m_atr_last_mid = mid;

        // RSI
        if (m_rsi_last_mid <= 0.0) { m_rsi_last_mid = mid; return; }
        const double chg  = mid - m_rsi_last_mid;
        m_rsi_last_mid = mid;
        const double gain = chg > 0.0 ?  chg : 0.0;
        const double loss = chg < 0.0 ? -chg : 0.0;

        if (!m_rsi_seeded) {
            m_rsi_seed_g.push_back(gain);
            m_rsi_seed_l.push_back(loss);
            if ((int)m_rsi_seed_g.size() >= RSI_PERIOD) {
                double sg = 0.0, sl = 0.0;
                for (int i = 0; i < RSI_PERIOD; ++i) {
                    sg += m_rsi_seed_g[i];
                    sl += m_rsi_seed_l[i];
                }
                m_rsi_avg_gain = sg / RSI_PERIOD;
                m_rsi_avg_loss = sl / RSI_PERIOD;
                m_rsi_seeded   = true;
                m_rsi_seed_g.clear();
                m_rsi_seed_l.clear();
            }
        } else {
            m_rsi_avg_gain = (m_rsi_avg_gain * (RSI_PERIOD - 1) + gain) / RSI_PERIOD;
            m_rsi_avg_loss = (m_rsi_avg_loss * (RSI_PERIOD - 1) + loss) / RSI_PERIOD;
        }
        if (!m_rsi_seeded) return;

        m_rsi_prev = m_tick_rsi;
        m_tick_rsi = (m_rsi_avg_loss < 1e-10) ? 100.0
                   : 100.0 - 100.0 / (1.0 + m_rsi_avg_gain / m_rsi_avg_loss);

        m_rsi_window.push_back(m_tick_rsi);
        if ((int)m_rsi_window.size() > RSI_DELTA_WINDOW)
            m_rsi_window.pop_front();
    }

    // ── Position management -- protection ladder ───────────────────────────────
    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        // ── Advance protection ladder ─────────────────────────────────────────
        if (pos.step == 0 && move >= BE_TRIGGER_PTS) {
            // Step 1: move SL to breakeven
            pos.sl   = pos.entry;
            pos.step = 1;
            printf("[MICROMOM-BE] %s BE locked entry=%.2f sl=%.2f move=%.2f\n",
                   pos.is_long ? "LONG" : "SHORT", pos.entry, pos.sl, move);
            fflush(stdout);
        }

        if (pos.step <= 1 && move >= LOCK_TRIGGER_PTS) {
            // Step 2: lock in LOCK_SL_PTS profit
            pos.sl   = pos.is_long ? (pos.entry + LOCK_SL_PTS)
                                   : (pos.entry - LOCK_SL_PTS);
            pos.step = 2;
            printf("[MICROMOM-LOCK] %s profit locked sl=%.2f(+%.2fpt) move=%.2f\n",
                   pos.is_long ? "LONG" : "SHORT", pos.sl, LOCK_SL_PTS, move);
            fflush(stdout);
        }

        if (pos.step >= 2) {
            // Step 3: trail -- SL follows best price, never retreats
            const double trail_sl = pos.is_long ? (mid - TRAIL_DIST_PTS)
                                                : (mid + TRAIL_DIST_PTS);
            if (pos.is_long  && trail_sl > pos.sl) { pos.sl = trail_sl; pos.step = 3; }
            if (!pos.is_long && trail_sl < pos.sl) { pos.sl = trail_sl; pos.step = 3; }
        }

        if ((now_s - pos.entry_ts) < MIN_HOLD_S) return;

        // Max hold
        if ((now_s - pos.entry_ts) >= MAX_HOLD_S) {
            _close(pos.is_long ? bid : ask, "MAX_HOLD", now_s, on_close);
            return;
        }

        // TP
        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) {
            _close(pos.is_long ? pos.tp : pos.tp, "TP_HIT", now_s, on_close);
            return;
        }

        // RSI delta reversal -- momentum dead, exit if in profit
        if (move > 0.8 && (int)m_rsi_window.size() >= RSI_DELTA_WINDOW) {
            const double cur_delta = m_tick_rsi - m_rsi_window.front();
            const bool reversed    = pos.is_long ? (cur_delta < -(RSI_DELTA_MIN * 0.4))
                                                 : (cur_delta >  (RSI_DELTA_MIN * 0.4));
            if (reversed) {
                _close(pos.is_long ? bid : ask, "SLOPE_EXIT", now_s, on_close);
                return;
            }
        }

        // SL (covers initial SL, BE, lock, and trail)
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const char*  reason  = pos.step == 0 ? "SL_HIT"
                                 : pos.step == 1 ? "BE_HIT"
                                 : "TRAIL_HIT";
            _close(exit_px, reason, now_s, on_close);
        }
    }

    // ── Close ─────────────────────────────────────────────────────────────────
    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        const double pnl_pts = pos.is_long
            ? (exit_px - pos.entry) : (pos.entry - exit_px);

        printf("[MICROMOM] EXIT %s @ %.2f reason=%s pnl_pts=%.2f mfe=%.2f step=%d\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts, pos.mfe, pos.step);
        fflush(stdout);

        omega::TradeRecord tr;
        tr.id            = ++m_trade_id;
        tr.symbol        = "XAUUSD";
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.engine        = "MicroMomentum";
        tr.regime        = "MOMENTUM";
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = pos.tp;
        tr.sl            = pos.sl_init;  // original SL for risk reporting
        tr.size          = pos.size;
        tr.pnl           = pnl_pts * pos.size;  // raw pts*lots -- handle_closed_trade applies tick_mult
        tr.net_pnl       = tr.pnl;
        tr.mfe           = pos.mfe * pos.size;
        tr.mae           = 0.0;
        tr.entryTs       = pos.entry_ts;
        tr.exitTs        = now_s;
        tr.exitReason    = reason;
        tr.spreadAtEntry = 0.0;

        pos = Position{};
        m_cooldown_until = now_s + COOLDOWN_S;

        if (on_close) on_close(tr);
    }
};

} // namespace omega
