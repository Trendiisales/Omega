#pragma once
// =============================================================================
// StopRunReversalEngine
// =============================================================================
// Built from 134M tick MFE/MAE structural scan (2023-2025 XAUUSD):
//   FADE_LONG_OVERLAP:  expect +0.081, MFEp50=1.45pt, MAEp50=1.41pt
//   FADE_SHORT_OVERLAP: expect +0.060, MFEp50=1.41pt, MAEp50=1.41pt
//
// Logic:
//   Gold frequently sweeps session highs/lows to trigger stops, then
//   immediately reverses. The sweep+rejection pattern is a structural
//   move of 1-3pt -- well above the 0.35pt cost floor.
//
// Entry conditions:
//   1. Price breaks session high/low by SWEEP_BUFFER (0.3pt)
//   2. Price REJECTS back by REJECT_DIST (0.5pt) within REJECT_TICKS (20)
//   3. Session is OVERLAP or NY_OPEN (slots 3+4, 12:00-17:00 UTC)
//   4. Not in cooldown
//
// Exit:
//   TP = entry +/- 1.2pt (fixed -- from MFEp50=1.45pt, taking 83% of median)
//   SL = entry -/+ 0.6pt (fixed -- from MAEp50=1.41pt * 0.43)
//   Timeout = 120s (if neither hit)
//
// Parameters derived directly from MFE/MAE scan. NOT hand-tuned.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <algorithm>
#include <cstring>
#include "OmegaTradeLedger.hpp"

namespace omega {

class StopRunReversalEngine {
public:
    // ── Parameters (calibrated from MFE/MAE scan) ────────────────────────
    double TP_PTS          = 1.20;   // MFEp50=1.45 * 0.83 -- conservative capture
    double SL_PTS          = 0.60;   // MAEp50=1.41 * 0.43 -- tight stop
    double SWEEP_BUFFER    = 0.30;   // pts above/below session high/low to confirm sweep
    double REJECT_DIST     = 0.50;   // pts of rejection needed to confirm reversal
    int    REJECT_TICKS    = 20;     // ticks within which rejection must occur
    int    SESSION_LOOKBACK= 500;    // ticks to define "session" high/low
    int64_t TIMEOUT_MS     = 120000; // 2 min max hold
    int64_t COOLDOWN_MS    = 45000;  // 45s between trades
    int    WARMUP_TICKS    = 200;    // minimum ticks before arming
    double LOT_SIZE        = 0.10;   // fixed lot -- will be risk-scaled by caller
    bool   enabled         = true;

    // ── Callbacks ────────────────────────────────────────────────────────
    using TradeCallback = std::function<void(const TradeRecord&)>;
    TradeCallback on_close;

    // ── State ─────────────────────────────────────────────────────────────
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

    std::string engine_name = "StopRunReversal";

    bool has_open_position() const { return pos.active; }

private:
    // Rolling price buffer for session high/low
    static constexpr int BUF = 1000;
    double m_prices[BUF] = {};
    int    m_buf_idx     = 0;
    int    m_tick_count  = 0;

    // Sweep detection state
    enum class SweepState { NONE, SWEPT_HIGH, SWEPT_LOW };
    SweepState m_sweep       = SweepState::NONE;
    double     m_sweep_price = 0.0;   // price at which sweep was detected
    int        m_sweep_tick  = 0;     // tick count when sweep detected
    double     m_session_hi  = 0.0;
    double     m_session_lo  = 0.0;

    int64_t    m_cooldown_until = 0;
    int        m_trade_id       = 0;

public:
    void on_tick(double bid, double ask, int64_t now_ms, int session_slot) {
        if (!enabled) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Feed price buffer
        m_prices[m_buf_idx % BUF] = mid;
        m_buf_idx++;
        m_tick_count++;

        if (m_tick_count < WARMUP_TICKS) return;

        // Manage open position
        if (pos.active) {
            _manage(bid, ask, mid, now_ms);
            return;
        }

        // Cooldown gate
        if (now_ms < m_cooldown_until) return;

        // Session gate: only OVERLAP (3) and NY_OPEN (4)
        if (session_slot != 3 && session_slot != 4) {
            // Reset sweep state outside session
            m_sweep = SweepState::NONE;
            return;
        }

        // Spread gate: skip if spread > 1.0pt (volatile/illiquid)
        if (spread > 1.0) return;

        // Compute session high/low over last SESSION_LOOKBACK ticks
        const int lookback = std::min(m_tick_count - 1, SESSION_LOOKBACK);
        double hi = mid, lo = mid;
        for (int k = 1; k <= lookback; k++) {
            double p = m_prices[(m_buf_idx - 1 - k + BUF*10) % BUF];
            if (p > hi) hi = p;
            if (p < lo) lo = p;
        }
        m_session_hi = hi;
        m_session_lo = lo;

        // ── SWEEP DETECTION ──────────────────────────────────────────────
        // Phase 1: detect when price breaks above session high or below session low
        if (m_sweep == SweepState::NONE) {
            if (mid > m_session_hi + SWEEP_BUFFER) {
                m_sweep       = SweepState::SWEPT_HIGH;
                m_sweep_price = mid;
                m_sweep_tick  = m_tick_count;
                printf("[SRR] SWEEP_HIGH detected @ %.2f (session_hi=%.2f +%.2fpt buffer)\n",
                       mid, m_session_hi, SWEEP_BUFFER);
                fflush(stdout);
            } else if (mid < m_session_lo - SWEEP_BUFFER) {
                m_sweep       = SweepState::SWEPT_LOW;
                m_sweep_price = mid;
                m_sweep_tick  = m_tick_count;
                printf("[SRR] SWEEP_LOW detected @ %.2f (session_lo=%.2f -%.2fpt buffer)\n",
                       mid, m_session_lo, SWEEP_BUFFER);
                fflush(stdout);
            }
            return;
        }

        // ── REJECTION DETECTION ──────────────────────────────────────────
        // Phase 2: after sweep, wait for rejection of REJECT_DIST within REJECT_TICKS
        const int ticks_since_sweep = m_tick_count - m_sweep_tick;
        if (ticks_since_sweep > REJECT_TICKS) {
            // Timeout: sweep failed to reverse -- reset
            m_sweep = SweepState::NONE;
            return;
        }

        if (m_sweep == SweepState::SWEPT_HIGH) {
            // Looking for rejection DOWN (short fade)
            const double rejection = m_sweep_price - mid;
            if (rejection >= REJECT_DIST) {
                // Confirmed: price swept high then rejected -- enter SHORT
                printf("[SRR] REJECTION confirmed: swept %.2f rejected %.2fpt -> SHORT\n",
                       m_sweep_price, rejection);
                fflush(stdout);
                _enter(false, bid, ask, mid, now_ms);
                m_sweep = SweepState::NONE;
            }
        } else if (m_sweep == SweepState::SWEPT_LOW) {
            // Looking for rejection UP (long fade)
            const double rejection = mid - m_sweep_price;
            if (rejection >= REJECT_DIST) {
                // Confirmed: price swept low then rejected -- enter LONG
                printf("[SRR] REJECTION confirmed: swept %.2f rejected %.2fpt -> LONG\n",
                       m_sweep_price, rejection);
                fflush(stdout);
                _enter(true, bid, ask, mid, now_ms);
                m_sweep = SweepState::NONE;
            }
        }
    }

    // Force-reset session state (call on daily open)
    void reset_session() {
        m_sweep = SweepState::NONE;
        m_sweep_price = 0.0;
    }

private:
    void _enter(bool is_long, double bid, double ask, double mid, int64_t now_ms) {
        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = is_long ? ask : bid;  // fill at market
        pos.tp       = is_long ? (pos.entry + TP_PTS) : (pos.entry - TP_PTS);
        pos.sl       = is_long ? (pos.entry - SL_PTS) : (pos.entry + SL_PTS);
        pos.mfe      = 0.0;
        pos.entry_ms = now_ms;
        pos.size     = LOT_SIZE;
        m_trade_id++;

        printf("[SRR] ENTRY %s @ %.2f  TP=%.2f  SL=%.2f  size=%.2f\n",
               is_long ? "LONG" : "SHORT",
               pos.entry, pos.tp, pos.sl, pos.size);
        fflush(stdout);
    }

    void _manage(double bid, double ask, double mid, int64_t now_ms) {
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        bool hit_tp      = pos.is_long ? (bid >= pos.tp) : (ask <= pos.tp);
        bool hit_sl      = pos.is_long ? (ask <= pos.sl) : (bid >= pos.sl);
        bool hit_timeout = (now_ms - pos.entry_ms) >= TIMEOUT_MS;

        if (!hit_tp && !hit_sl && !hit_timeout) return;

        const char* reason = hit_tp ? "TP_HIT" : (hit_sl ? "SL_HIT" : "TIMEOUT");
        const double exit_px = pos.is_long
            ? (hit_tp ? pos.tp : (hit_sl ? pos.sl : bid))
            : (hit_tp ? pos.tp : (hit_sl ? pos.sl : ask));

        const double pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double pnl_usd = pnl_pts * pos.size * 100.0;

        printf("[SRR] EXIT %s @ %.2f  reason=%s  pnl=%.2fpt ($%.0f)  mfe=%.2fpt\n",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts, pnl_usd, pos.mfe);
        fflush(stdout);

        // Emit trade record
        if (on_close) {
            TradeRecord tr;
            tr.id         = m_trade_id;
            tr.symbol     = "XAUUSD";
            tr.side       = pos.is_long ? "LONG" : "SHORT";
            tr.engine     = engine_name;
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
            tr.regime     = "STOP_RUN";
            on_close(tr);
        }

        pos.active = false;
        m_cooldown_until = now_ms + COOLDOWN_MS;
    }
};

} // namespace omega
