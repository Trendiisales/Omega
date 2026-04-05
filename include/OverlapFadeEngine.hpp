#pragma once
// =============================================================================
// OverlapFadeEngine
// =============================================================================
// Built DIRECTLY from the 134M tick MFE/MAE structural scan result:
//
//   FADE_LONG_OVERLAP:  expect +0.081, MFEp50=1.45pt, MAEp50=1.41pt
//   FADE_SHORT_OVERLAP: expect +0.060, MFEp50=1.41pt, MAEp50=1.41pt
//
// What the scan measured:
//   After price makes a fast impulse move (overextended vs 100-tick baseline)
//   DURING OVERLAP SESSION (slots 3+4, 12:00-17:00 UTC),
//   the median maximum reversal over the next 2000 ticks is 1.45pt.
//
// Signal: price has moved > 3*ATR in last 100 ticks (overextension)
// Fade:   enter AGAINST the move direction
// TP:     1.20pt (83% of MFEp50)
// SL:     0.80pt (slightly above MAEp50 -- give it room)
// Session: OVERLAP only (slots 3+4)
//
// NO rolling ATR computation per tick (that caused 135s run time).
// ATR updated every 200 ticks via seed_atr() call from runner.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"

namespace omega {

class OverlapFadeEngine {
public:
    // Parameters from scan
    double TP_PTS         = 1.20;
    double SL_PTS         = 0.80;
    double OVEREXT_MULT   = 3.0;   // fade when move100 > ATR * OVEREXT_MULT
    int    LOOK           = 100;   // lookback ticks for overextension
    int64_t TIMEOUT_MS    = 120000;
    int64_t COOLDOWN_MS   = 45000;
    int    WARMUP_TICKS   = 500;
    double LOT_SIZE       = 0.16;
    bool   enabled        = true;

    using CloseCallback = std::function<void(const TradeRecord&)>;
    CloseCallback on_close;

    struct Position {
        bool   active    = false;
        bool   is_long   = false;
        double entry     = 0.0;
        double tp        = 0.0;
        double sl        = 0.0;
        double mfe       = 0.0;
        int64_t entry_ms = 0;
        double size      = 0.0;
    } pos;

    bool has_open_position() const { return pos.active; }

private:
    // Ring buffer -- only need LOOK ticks
    static constexpr int BUF = 512;
    double  m_buf[BUF]   = {};
    int     m_idx        = 0;
    int     m_count      = 0;
    double  m_atr        = 5.0;   // seeded externally every 200 ticks
    int64_t m_cooldown   = 0;
    int     m_trade_id   = 0;

public:
    // Call this every 200 ticks from the runner with a fresh ATR value
    void seed_atr(double atr) {
        if (atr > 0.0) m_atr = atr;
    }

    void on_tick(double bid, double ask, int64_t now_ms, int session_slot) {
        if (!enabled) return;
        const double mid = (bid + ask) * 0.5;

        m_buf[m_idx % BUF] = mid;
        m_idx++;
        m_count++;

        // Manage open position every tick
        if (pos.active) {
            _manage(bid, ask, mid, now_ms);
            return;
        }

        if (m_count < WARMUP_TICKS)    return;
        if (now_ms < m_cooldown)        return;
        if (session_slot != 3 && session_slot != 4) return;
        if ((ask - bid) > 1.5)         return;

        // Overextension: price moved > OVEREXT_MULT * ATR in last LOOK ticks
        if (m_count < LOOK + 1) return;
        const double price_look = m_buf[(m_idx - LOOK - 1 + BUF*4) % BUF];
        const double move100    = mid - price_look;

        // FADE_LONG: after large DOWN move, enter LONG
        // FADE_SHORT: after large UP move, enter SHORT
        const double threshold = m_atr * OVEREXT_MULT;
        bool fade_long  = (move100 < -threshold);
        bool fade_short = (move100 >  threshold);

        if (!fade_long && !fade_short) return;

        _enter(fade_long, bid, ask, mid, now_ms);
    }

private:
    void _enter(bool is_long, double bid, double ask, double mid, int64_t now_ms) {
        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = is_long ? ask : bid;
        pos.tp       = is_long ? (pos.entry + TP_PTS) : (pos.entry - TP_PTS);
        pos.sl       = is_long ? (pos.entry - SL_PTS) : (pos.entry + SL_PTS);
        pos.mfe      = 0.0;
        pos.entry_ms = now_ms;
        pos.size     = LOT_SIZE;
        m_trade_id++;

        printf("[OFE] ENTRY %s @ %.2f  TP=%.2f  SL=%.2f  atr=%.2f\n",
               is_long ? "LONG" : "SHORT",
               pos.entry, pos.tp, pos.sl, m_atr);
        fflush(stdout);
    }

    void _manage(double bid, double ask, double mid, int64_t now_ms) {
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        bool hit_tp  = pos.is_long ? (bid >= pos.tp) : (ask <= pos.tp);
        bool hit_sl  = pos.is_long ? (ask <= pos.sl) : (bid >= pos.sl);
        bool timeout = (now_ms - pos.entry_ms) >= TIMEOUT_MS;

        if (!hit_tp && !hit_sl && !timeout) return;

        const char* reason = hit_tp ? "TP_HIT" : (hit_sl ? "SL_HIT" : "TIMEOUT");
        const double exit_px = pos.is_long
            ? (hit_tp ? pos.tp : (hit_sl ? pos.sl : bid))
            : (hit_tp ? pos.tp : (hit_sl ? pos.sl : ask));

        const double pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double pnl_usd = pnl_pts * pos.size * 100.0;

        printf("[OFE] EXIT %s @ %.2f  %s  pnl=$%.0f  mfe=%.2fpt\n",
               pos.is_long ? "LONG" : "SHORT", exit_px, reason, pnl_usd, pos.mfe);
        fflush(stdout);

        if (on_close) {
            TradeRecord tr;
            tr.id         = m_trade_id;
            tr.symbol     = "XAUUSD";
            tr.side       = pos.is_long ? "LONG" : "SHORT";
            tr.engine     = "OverlapFade";
            tr.entryPrice = pos.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos.sl;
            tr.size       = pos.size;
            tr.pnl        = pnl_pts * pos.size;
            tr.mfe        = pos.mfe;
            tr.mae        = 0.0;
            tr.entryTs    = pos.entry_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "OVERLAP_FADE";
            on_close(tr);
        }

        pos.active = false;
        m_cooldown = now_ms + COOLDOWN_MS;
    }
};

} // namespace omega
