#pragma once
// =============================================================================
// StructuralEdgeEngines.hpp
// All engines derived directly from 134M tick MFE/MAE structural scan.
// Each engine is independent -- no shared state, no conflicts.
//
// SCAN RESULTS (134M ticks, 2023-2025 XAUUSD, window=2000t ~3min):
//   FADE_LONG_OVERLAP:    expect +0.081  MFEp50=1.45pt  MAEp50=1.41pt
//   FADE_SHORT_OVERLAP:   expect +0.060  MFEp50=1.41pt  MAEp50=1.41pt
//   PERSIST_BULL_OVERLAP: expect +0.069  MFEp50=1.41pt  MAEp50=1.38pt
//   PERSIST_BEAR_OVERLAP: expect +0.057  MFEp50=1.40pt  MAEp50=1.38pt
//   PERSIST_BULL_ASIA:    expect +0.034  MFEp50=1.34pt  MAEp50=1.30pt
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"

namespace omega {

// =============================================================================
// Base engine -- shared position management, TP/SL/timeout exit
// =============================================================================
struct EdgeEngineBase {
    double TP_PTS      = 1.20;
    double SL_PTS      = 0.80;
    int64_t TIMEOUT_MS = 120000;
    int64_t COOLDOWN_MS= 45000;
    int WARMUP_TICKS   = 500;
    double LOT_SIZE    = 0.16;
    bool enabled       = true;
    std::string name   = "EdgeEngine";

    using CloseCallback = std::function<void(const TradeRecord&)>;
    CloseCallback on_close;

    struct Pos {
        bool   active  = false;
        bool   is_long = false;
        double entry   = 0, tp = 0, sl = 0, mfe = 0, size = 0;
        int64_t entry_ms = 0;
    } pos;

    bool has_open_position() const { return pos.active; }

protected:
    int64_t m_cooldown = 0;
    int     m_count    = 0;
    int     m_trade_id = 0;

    static constexpr int PBUF = 256;
    double m_prices[PBUF] = {};
    int    m_pidx = 0;
    double m_atr  = 5.0;

    void _feed(double mid) {
        m_prices[m_pidx % PBUF] = mid;
        m_pidx++;
        m_count++;
    }

    // Call every 200 ticks -- cheap ATR update
    void _update_atr() {
        int look = std::min(m_count - 1, 100);
        if (look < 10) return;
        double a = 0;
        for (int k = 1; k <= look; k++)
            a += std::fabs(m_prices[(m_pidx-k+PBUF*4)%PBUF]
                         - m_prices[(m_pidx-k-1+PBUF*4)%PBUF]);
        m_atr = a / look;
    }

    void _enter(bool is_long, double bid, double ask, int64_t now_ms) {
        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = is_long ? ask : bid;
        pos.tp       = is_long ? pos.entry + TP_PTS : pos.entry - TP_PTS;
        pos.sl       = is_long ? pos.entry - SL_PTS : pos.entry + SL_PTS;
        pos.mfe      = 0;
        pos.entry_ms = now_ms;
        pos.size     = LOT_SIZE;
        m_trade_id++;
        printf("[%s] ENTRY %s @ %.2f  TP=%.2f SL=%.2f atr=%.2f\n",
               name.c_str(), is_long?"LONG":"SHORT",
               pos.entry, pos.tp, pos.sl, m_atr);
        fflush(stdout);
    }

    void _manage(double bid, double ask, double mid, int64_t now_ms) {
        const double move = pos.is_long ? (mid-pos.entry) : (pos.entry-mid);
        if (move > pos.mfe) pos.mfe = move;

        bool hit_tp  = pos.is_long ? bid >= pos.tp  : ask <= pos.tp;
        bool hit_sl  = pos.is_long ? ask <= pos.sl  : bid >= pos.sl;
        bool timeout = now_ms - pos.entry_ms >= TIMEOUT_MS;
        if (!hit_tp && !hit_sl && !timeout) return;

        const char* reason = hit_tp ? "TP_HIT" : hit_sl ? "SL_HIT" : "TIMEOUT";
        double exit_px = pos.is_long
            ? (hit_tp ? pos.tp : hit_sl ? pos.sl : bid)
            : (hit_tp ? pos.tp : hit_sl ? pos.sl : ask);

        double pnl_pts = pos.is_long ? exit_px-pos.entry : pos.entry-exit_px;
        printf("[%s] EXIT %s @ %.2f %s pnl=$%.0f mfe=%.2f\n",
               name.c_str(), pos.is_long?"LONG":"SHORT",
               exit_px, reason, pnl_pts*pos.size*100, pos.mfe);
        fflush(stdout);

        if (on_close) {
            TradeRecord tr;
            tr.id = m_trade_id; tr.symbol = "XAUUSD";
            tr.side = pos.is_long ? "LONG" : "SHORT";
            tr.engine = name; tr.entryPrice = pos.entry;
            tr.exitPrice = exit_px; tr.sl = pos.sl;
            tr.size = pos.size; tr.pnl = pnl_pts * pos.size;
            tr.mfe = pos.mfe; tr.mae = 0;
            tr.entryTs = pos.entry_ms/1000; tr.exitTs = now_ms/1000;
            tr.exitReason = reason; tr.regime = name;
            on_close(tr);
        }
        pos.active = false;
        m_cooldown = now_ms + COOLDOWN_MS;
    }
};

// =============================================================================
// 1. OverlapMomentumEngine
// Source: PERSIST_BULL/BEAR_OVERLAP  expect +0.057 to +0.069
// Signal: 70% tick persistence in 50 ticks + vol expanding
// Session: OVERLAP (3,4) only
// =============================================================================
class OverlapMomentumEngine : public EdgeEngineBase {
public:
    double PERSIST_THRESH = 0.70;  // 70% of last 50 ticks same direction
    int    PERSIST_LOOK   = 50;
    double VOL_RATIO_MIN  = 1.3;   // vol expanding vs baseline

    OverlapMomentumEngine() { name = "OverlapMomentum"; TP_PTS = 1.20; SL_PTS = 0.80; }

    void on_tick(double bid, double ask, int64_t now_ms, int session_slot) {
        if (!enabled) return;
        const double mid = (bid+ask)*0.5;
        _feed(mid);
        if ((m_count % 200) == 0) _update_atr();
        if (m_count < WARMUP_TICKS) return;
        if (pos.active) { _manage(bid, ask, mid, now_ms); return; }
        if (now_ms < m_cooldown) return;
        if (session_slot != 3 && session_slot != 4) return;
        if ((ask-bid) > 1.5) return;
        if (m_count < PERSIST_LOOK + 1) return;

        // Count up/down ticks
        int up = 0, dn = 0;
        for (int k = 1; k <= PERSIST_LOOK; k++) {
            double a = m_prices[(m_pidx-k  +PBUF*4)%PBUF];
            double b = m_prices[(m_pidx-k-1+PBUF*4)%PBUF];
            if (a > b) up++; else if (a < b) dn++;
        }
        double pers = (double)std::max(up,dn) / PERSIST_LOOK;
        if (pers < PERSIST_THRESH) return;

        // Vol expansion: recent 10t vs ATR baseline
        double vol10 = std::fabs(mid - m_prices[(m_pidx-10+PBUF*4)%PBUF]);
        if (vol10 < m_atr * 10 * (VOL_RATIO_MIN - 1)) return;

        bool is_long = (up > dn);
        _enter(is_long, bid, ask, now_ms);
    }
};

// =============================================================================
// 2. AsiaMomentumEngine
// Source: PERSIST_BULL_ASIA  expect +0.034
// Signal: slow drift persistence in Asia (low vol = real directional bias)
// Session: ASIA (6) only
// =============================================================================
class AsiaMomentumEngine : public EdgeEngineBase {
public:
    double PERSIST_THRESH = 0.72;  // slightly stricter in Asia (more noise)
    int    PERSIST_LOOK   = 80;    // longer look -- Asia moves are slow

    AsiaMomentumEngine() {
        name = "AsiaMomentum";
        TP_PTS = 1.00;  // lower target -- Asia moves are smaller
        SL_PTS = 0.70;
        COOLDOWN_MS = 120000; // longer cooldown in Asia
    }

    void on_tick(double bid, double ask, int64_t now_ms, int session_slot) {
        if (!enabled) return;
        const double mid = (bid+ask)*0.5;
        _feed(mid);
        if ((m_count % 200) == 0) _update_atr();
        if (m_count < WARMUP_TICKS) return;
        if (pos.active) { _manage(bid, ask, mid, now_ms); return; }
        if (now_ms < m_cooldown) return;
        if (session_slot != 6) return;  // Asia only
        if ((ask-bid) > 0.80) return;   // tighter spread gate in Asia
        if (m_atr < 0.5 || m_atr > 4.0) return;  // only trade when ATR in range
        if (m_count < PERSIST_LOOK + 1) return;

        int up = 0, dn = 0;
        for (int k = 1; k <= PERSIST_LOOK; k++) {
            double a = m_prices[(m_pidx-k  +PBUF*4)%PBUF];
            double b = m_prices[(m_pidx-k-1+PBUF*4)%PBUF];
            if (a > b) up++; else if (a < b) dn++;
        }
        double pers = (double)std::max(up,dn) / PERSIST_LOOK;
        if (pers < PERSIST_THRESH) return;

        bool is_long = (up > dn);
        _enter(is_long, bid, ask, now_ms);
    }
};

// =============================================================================
// 3. LondonCoreFadeEngine
// Source: FADE_LONG_LON_CORE  expect +0.007 (marginal but real)
//         FADE_SHORT_OVERLAP also works
// Signal: overextension during London core / overlap
// =============================================================================
class LondonCoreFadeEngine : public EdgeEngineBase {
public:
    double OVEREXT_MULT = 3.0;
    int    LOOK         = 100;

    LondonCoreFadeEngine() {
        name = "LondonCoreFade";
        TP_PTS = 1.10;
        SL_PTS = 0.72;
    }

    void on_tick(double bid, double ask, int64_t now_ms, int session_slot) {
        if (!enabled) return;
        const double mid = (bid+ask)*0.5;
        _feed(mid);
        if ((m_count % 200) == 0) _update_atr();
        if (m_count < WARMUP_TICKS) return;
        if (pos.active) { _manage(bid, ask, mid, now_ms); return; }
        if (now_ms < m_cooldown) return;
        // London core (2) + overlap (3,4)
        if (session_slot != 2 && session_slot != 3 && session_slot != 4) return;
        if ((ask-bid) > 1.5) return;
        if (m_count < LOOK + 1) return;

        double price_look = m_prices[(m_pidx-LOOK-1+PBUF*4)%PBUF];
        double move = mid - price_look;
        double threshold = m_atr * OVEREXT_MULT;

        bool fade_long  = move < -threshold;
        bool fade_short = move >  threshold;
        if (!fade_long && !fade_short) return;

        _enter(fade_long, bid, ask, now_ms);
    }
};

} // namespace omega
