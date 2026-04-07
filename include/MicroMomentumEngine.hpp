#pragma once
// =============================================================================
// MicroMomentumEngine.hpp  --  RSI-extreme momentum capture on XAUUSD
// =============================================================================
//
// DESIGN:
//   Catches the 20-40pt moves visible on the cTrader tick chart.
//   These moves have a clear RSI signature: RSI spikes to extreme (>72 or <28),
//   then TURNS while price has already displaced from a recent anchor.
//   Entry is on the TURN not the extreme -- confirmed by RSI slope reversing.
//
// SIGNAL (both conditions must hold):
//   SHORT: RSI > RSI_HIGH_THRESH (e.g. 70) AND rsi_slope < -RSI_SLOPE_MIN
//          AND price has displaced UP from anchor (confirming the prior spike)
//   LONG:  RSI < RSI_LOW_THRESH  (e.g. 30) AND rsi_slope >  RSI_SLOPE_MIN
//          AND price has displaced DOWN from anchor
//
//   This catches the chart pattern you showed: RSI peaked ~82, started falling,
//   price was above anchor → SHORT. The old engine blocked this (RSI > 58 gate).
//
// ALSO CATCHES continuation momentum (RSI 45-70 rising / 55-30 falling):
//   When RSI slope is strong AND price is displacing in slope direction,
//   enter in the direction of slope (momentum continuation mode).
//   Threshold: RSI_SLOPE_MIN * 2.0 required (stronger signal needed for continuation).
//
// EXIT:
//   TP: fixed TP_PTS (default 4.0pt)
//   SLOPE_EXIT: RSI slope reverses while in profit >= 0.8pt
//   SL: max(ATR * SL_ATR_MULT, spread * 2.0) -- tight but not sub-spread
//   MAX_HOLD: 4 minutes
//
// COST COVERAGE:
//   At 0.10 lots, 4pt TP = $40 gross. Spread + commission ~$0.82. Net ~$39.
//   SL at ~1.5pt = $15 loss. RR ~2.5:1 minimum.
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
    double ENTRY_DISP_PTS    = 1.2;   // price must be displaced this far from anchor
    double RSI_SLOPE_MIN     = 0.25;  // min EWM RSI slope for entry
    double RSI_HIGH_THRESH   = 70.0;  // RSI above this = overbought reversal zone
    double RSI_LOW_THRESH    = 30.0;  // RSI below this = oversold reversal zone
    double RSI_CONT_MULT     = 2.0;   // continuation mode needs slope * this
    double TP_PTS            = 4.0;   // fixed TP in price points
    double SL_ATR_MULT       = 0.5;   // SL = max(ATR*mult, spread*2)
    double MAX_SPREAD_PTS    = 2.5;   // don't enter if spread wider than this
    int    COOLDOWN_S        = 20;    // reduced: 20s between trades
    int    MAX_HOLD_S        = 240;   // 4 min hard exit
    int    MIN_HOLD_S        = 3;     // minimum hold before exits checked
    int    WARMUP_TICKS      = 20;    // ticks before signals valid
    bool   enabled           = true;
    bool   shadow_mode       = true;

    // ── State ─────────────────────────────────────────────────────────────────
    struct Position {
        bool    active    = false;
        bool    is_long   = false;
        double  entry     = 0.0;
        double  tp        = 0.0;
        double  sl        = 0.0;
        double  size      = 0.01;
        double  mfe       = 0.0;
        int64_t entry_ts  = 0;
        char    mode[16]  = {};  // "REVERSAL" or "CONTINUATION"
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
        if (now_s < m_cooldown_until)    return;
        if (m_tick_count < WARMUP_TICKS) return;
        if (spread > MAX_SPREAD_PTS)     return;
        if (!m_rsi_seeded)               return;

        const double rsi   = m_tick_rsi;
        const double slope = m_rsi_slope;
        const double disp  = mid - m_anchor;

        // ── Mode 1: REVERSAL -- RSI at extreme, slope turning ─────────────────
        // Chart pattern: RSI peaked 80+, now falling → SHORT into the reversal
        // Chart pattern: RSI troughed 20-, now rising → LONG into the reversal
        const bool reversal_short = (rsi  > RSI_HIGH_THRESH)           // RSI was high
                                 && (slope < -RSI_SLOPE_MIN)            // slope now falling
                                 && (disp  >  ENTRY_DISP_PTS);          // price above anchor (confirms prior spike up)

        const bool reversal_long  = (rsi  < RSI_LOW_THRESH)            // RSI was low
                                 && (slope >  RSI_SLOPE_MIN)            // slope now rising
                                 && (disp  < -ENTRY_DISP_PTS);          // price below anchor (confirms prior dip)

        // ── Mode 2: CONTINUATION -- RSI mid-range, strong slope, price moving ─
        // Catches: RSI 50→70 rising fast with price up, RSI 50→30 falling fast with price down
        const double cont_thresh = RSI_SLOPE_MIN * RSI_CONT_MULT;
        const bool cont_long  = (rsi  > 40.0 && rsi  < 65.0)          // RSI mid-range rising
                             && (slope >  cont_thresh)                  // strong positive slope
                             && (disp  >  ENTRY_DISP_PTS);              // price above anchor

        const bool cont_short = (rsi  > 35.0 && rsi  < 60.0)          // RSI mid-range falling
                             && (slope < -cont_thresh)                  // strong negative slope
                             && (disp  < -ENTRY_DISP_PTS);              // price below anchor

        const bool go_long  = reversal_long  || cont_long;
        const bool go_short = reversal_short || cont_short;

        if (!go_long && !go_short) {
            // Diagnostic: log why blocked every 30s so we can tune
            if (now_s - m_last_block_log >= 30) {
                m_last_block_log = now_s;
                printf("[MICROMOM-BLOCK] rsi=%.1f slope=%.3f disp=%.2f spread=%.2f "
                       "cooldown=%llds warmup=%d/%d\n",
                       rsi, slope, disp, spread,
                       (long long)(m_cooldown_until - now_s),
                       m_tick_count, WARMUP_TICKS);
                fflush(stdout);
            }
            return;
        }

        // If both fire (edge case), prefer reversal signal; if both same mode, prefer short
        const bool is_long = go_long && !go_short;
        const char* mode_str = ((is_long ? reversal_long : reversal_short)) ? "REVERSAL" : "CONTINUATION";

        const double entry  = is_long ? ask : bid;
        const double sl_pts = std::max(m_tick_atr * SL_ATR_MULT, spread * 2.0);

        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = entry;
        pos.tp       = is_long ? (entry + TP_PTS) : (entry - TP_PTS);
        pos.sl       = is_long ? (entry - sl_pts)  : (entry + sl_pts);
        pos.size     = 0.01;
        pos.mfe      = 0.0;
        pos.entry_ts = now_s;
        std::snprintf(pos.mode, sizeof(pos.mode), "%s", mode_str);

        const char* pfx = shadow_mode ? "[MICROMOM-SHADOW]" : "[MICROMOM]";
        printf("%s %s mode=%s entry=%.2f tp=%.2f sl=%.2f(dist=%.2f) "
               "rsi=%.1f slope=%.3f disp=%.2f atr=%.2f spread=%.2f slot=%d\n",
               pfx, is_long ? "LONG" : "SHORT", mode_str,
               entry, pos.tp, pos.sl, sl_pts,
               rsi, slope, disp, m_tick_atr, spread, session_slot);
        fflush(stdout);
    }

    void patch_size(double lot) noexcept { if (pos.active) pos.size = lot; }

    double rsi_val()   const noexcept { return m_tick_rsi; }
    double rsi_slope_val() const noexcept { return m_rsi_slope; }
    double anchor_disp(double mid) const noexcept { return mid - m_anchor; }

private:
    // ── Indicators ────────────────────────────────────────────────────────────
    double  m_tick_rsi      = 50.0;
    double  m_rsi_prev      = 50.0;
    double  m_rsi_avg_gain  = 0.0;
    double  m_rsi_avg_loss  = 0.0;
    bool    m_rsi_seeded    = false;
    std::deque<double> m_rsi_seed_g;
    std::deque<double> m_rsi_seed_l;
    double  m_rsi_last_mid  = 0.0;
    static constexpr int RSI_PERIOD = 14;

    // RSI slope: EWM of per-tick RSI change
    double  m_rsi_slope      = 0.0;
    static constexpr double SLOPE_ALPHA = 0.35;  // slightly slower than before -- less noise

    // Price anchor: EWM of mid -- α=0.12 = ~7-tick memory (faster than before)
    // Faster anchor means displacement registers sooner after a move starts
    double  m_anchor        = 0.0;
    bool    m_anchor_init   = false;
    static constexpr double ANCHOR_ALPHA = 0.12;

    // Tick ATR
    double  m_tick_atr      = 1.0;
    double  m_atr_last_mid  = 0.0;
    bool    m_atr_init      = false;
    static constexpr double ATR_ALPHA = 2.0 / 15.0;

    int     m_tick_count     = 0;
    int64_t m_cooldown_until = 0;
    int64_t m_last_block_log = 0;
    int     m_trade_id       = 0;

    void _update(double mid, double spread) noexcept {
        ++m_tick_count;

        // Anchor
        if (!m_anchor_init) { m_anchor = mid; m_anchor_init = true; }
        else m_anchor = ANCHOR_ALPHA * mid + (1.0 - ANCHOR_ALPHA) * m_anchor;

        // Tick ATR
        if (!m_atr_init) { m_tick_atr = std::max(spread, 0.1); m_atr_init = true; }
        else {
            const double chg = (m_atr_last_mid > 0.0)
                ? std::max(std::fabs(mid - m_atr_last_mid), spread)
                : spread;
            m_tick_atr = ATR_ALPHA * chg + (1.0 - ATR_ALPHA) * m_tick_atr;
        }
        m_atr_last_mid = mid;

        // RSI (Wilder 14)
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
                for (int i = 0; i < RSI_PERIOD; ++i) { sg += m_rsi_seed_g[i]; sl += m_rsi_seed_l[i]; }
                m_rsi_avg_gain = sg / RSI_PERIOD;
                m_rsi_avg_loss = sl / RSI_PERIOD;
                m_rsi_seeded = true;
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

        // RSI slope: EWM of per-tick change (no acceleration check -- just direction + magnitude)
        const double raw_slope = m_tick_rsi - m_rsi_prev;
        m_rsi_slope = SLOPE_ALPHA * raw_slope + (1.0 - SLOPE_ALPHA) * m_rsi_slope;
    }

    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

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

        // Slope reversal exit -- momentum exhausted, take profit while we have it
        // Require 0.8pt in profit before slope exit to avoid noise exits
        if (move > 0.8) {
            const bool slope_died = pos.is_long ? (m_rsi_slope < -RSI_SLOPE_MIN * 0.5)
                                                : (m_rsi_slope >  RSI_SLOPE_MIN * 0.5);
            if (slope_died) {
                _close(pos.is_long ? bid : ask, "SLOPE_EXIT", now_s, on_close);
                return;
            }
        }

        // SL
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            _close(pos.is_long ? bid : ask, "SL_HIT", now_s, on_close);
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        const double pnl_pts = pos.is_long
            ? (exit_px - pos.entry) : (pos.entry - exit_px);

        printf("[MICROMOM] EXIT %s mode=%s @ %.2f reason=%s pnl_pts=%.2f mfe=%.2f\n",
               pos.is_long ? "LONG" : "SHORT", pos.mode,
               exit_px, reason, pnl_pts, pos.mfe);
        fflush(stdout);

        omega::TradeRecord tr;
        tr.id            = ++m_trade_id;
        tr.symbol        = "XAUUSD";
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.engine        = "MicroMomentum";
        tr.regime        = pos.mode;
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = pos.tp;
        tr.sl            = pos.sl;
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
