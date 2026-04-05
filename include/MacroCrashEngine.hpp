#pragma once
// =============================================================================
// MacroCrashEngine
// =============================================================================
// Purpose: Capture macro expansion events (tariff crashes, Fed spikes, etc.)
// These are 50-150pt moves that dwarf normal daily ATR.
//
// This is NOT a microstructure engine. It activates only when:
//   - ATR > 8pt (2x normal 5pt baseline = genuine volatility expansion)
//   - vol_ratio > 2.5 (recent vol > 2.5x baseline = confirmed expansion)
//   - Regime = EXPANSION_BREAKOUT or TREND_CONTINUATION
//   - ewm_drift strongly directional (|drift| > 6pt)
//
// Exit logic: velocity trail (proved on Apr 2 = $5,352)
//   - Step1 held until $200 open (not $35) -- don't bank early on a crash
//   - Velocity trail: arms at 3xATR, trails at 2xATR
//   - Lot-scaled ratchet: $400/tier at ATR=10, locks 80% per tier
//
// Sizing: ATR-proportional, scale ceiling 6x
//   ATR=5 (normal): 1.0x scale, 0.16 lots at $80 base risk
//   ATR=10 (crash): 2.0x scale, 0.32 lots
//   ATR=15 (spike): 3.0x scale, 0.48 lots (capped at 0.50 max)
//
// Sessions: ALL sessions active -- macro events don't respect session time.
//   Apr 2 crash started at 04:05 UTC (Asia session). Must be active 24/7.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <algorithm>
#include <deque>

namespace omega {

class MacroCrashEngine {
public:
    // ── Entry triggers ────────────────────────────────────────────────────
    double ATR_THRESHOLD    = 8.0;   // minimum ATR to consider macro event
    double ATR_NORMAL       = 5.0;   // baseline ATR for sizing scale
    double VOL_RATIO_MIN    = 2.5;   // vol expansion required
    double DRIFT_MIN        = 6.0;   // |ewm_drift| required for direction
    double ATR_SCALE_MAX    = 6.0;   // max size multiplier
    double BASE_RISK_USD    = 80.0;  // base risk per trade in USD
    double MAX_LOT          = 0.50;  // hard lot ceiling
    double MIN_LOT          = 0.01;

    // ── Exit parameters (velocity trail) ─────────────────────────────────
    double STEP1_TRIGGER_USD     = 200.0;  // hold until $200 open (not $35)
    double STEP2_TRIGGER_USD     = 400.0;  // step2 at $400
    double VEL_TRAIL_ARM_ATR     = 3.0;   // arm velocity trail after 3xATR
    double VEL_TRAIL_DIST_ATR    = 2.0;   // trail 2xATR behind MFE
    double RATCHET_KEEP          = 0.80;  // lock 80% of each tier

    // ── Timing ────────────────────────────────────────────────────────────
    int64_t COOLDOWN_MS     = 300000; // 5 min cooldown -- macro events are rare
    int64_t MAX_HOLD_MS     = 7200000;// 2 hour max hold (crash can last hours)

    bool    enabled         = true;
    bool    shadow_mode     = true;   // default: shadow until confirmed working

    // ── Callbacks ─────────────────────────────────────────────────────────
    using CloseCallback = std::function<void(double exit_px, bool is_long,
                                             double size, const std::string& reason)>;
    CloseCallback on_close;

    // ── Position ──────────────────────────────────────────────────────────
    struct Position {
        bool   active         = false;
        bool   is_long        = false;
        double entry          = 0.0;
        double sl             = 0.0;
        double full_size      = 0.0;
        double size           = 0.0;   // remaining after partials
        double atr_at_entry   = 0.0;
        double mfe            = 0.0;
        int64_t entry_ms      = 0;
        bool   partial1_done  = false;
        bool   partial2_done  = false;
        bool   be_locked      = false;
        int    ratchet_tier   = 0;
        double banked_usd     = 0.0;
    } pos;

    bool has_open_position() const { return pos.active; }

    // ── Main tick function ────────────────────────────────────────────────
    void on_tick(double bid, double ask,
                 double atr,           // current ATR
                 double vol_ratio,     // recent_vol / baseline
                 double ewm_drift,     // directional drift signal
                 bool   expansion_regime, // supervisor confirmed expansion
                 int64_t now_ms) {

        if (!enabled) return;

        const double mid = (bid + ask) * 0.5;

        // Manage open position first
        if (pos.active) {
            _manage(bid, ask, mid, atr, vol_ratio, ewm_drift, now_ms);
            return;
        }

        // Cooldown gate
        if (now_ms < m_cooldown_until) return;

        // ── ENTRY GATE -- ALL conditions must pass ────────────────────────
        // 1. ATR must be elevated (genuine macro volatility)
        if (atr < ATR_THRESHOLD) return;

        // 2. Vol ratio must confirm expansion (not just high baseline ATR)
        if (vol_ratio < VOL_RATIO_MIN) return;

        // 3. Regime must be confirmed expansion
        if (!expansion_regime) return;

        // 4. Drift must be strongly directional
        const double drift_abs = std::fabs(ewm_drift);
        if (drift_abs < DRIFT_MIN) return;

        // Direction: long if drift > 0 (surge), short if drift < 0 (crash)
        const bool is_long = (ewm_drift > 0.0);

        // 5. Don't re-enter same direction within first 60s after a loss
        if (is_long  && now_ms < m_long_block_until)  return;
        if (!is_long && now_ms < m_short_block_until) return;

        // ── ENTRY ─────────────────────────────────────────────────────────
        const double atr_scale = std::min(ATR_SCALE_MAX,
                                          std::max(0.5, atr / ATR_NORMAL));
        const double risk      = BASE_RISK_USD * atr_scale;
        const double sl_pts    = atr * 1.0;  // 1xATR stop
        const double lot       = std::min(MAX_LOT,
                                 std::max(MIN_LOT,
                                 risk / (sl_pts * 100.0)));

        const double entry_px  = is_long ? ask : bid;
        const double sl_px     = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);

        pos.active        = true;
        pos.is_long       = is_long;
        pos.entry         = entry_px;
        pos.sl            = sl_px;
        pos.full_size     = lot;
        pos.size          = lot;
        pos.atr_at_entry  = atr;
        pos.mfe           = 0.0;
        pos.entry_ms      = now_ms;
        pos.partial1_done = false;
        pos.partial2_done = false;
        pos.be_locked     = false;
        pos.ratchet_tier  = 0;
        pos.banked_usd    = 0.0;

        if (shadow_mode) {
            printf("[MCE-SHADOW] WOULD ENTER %s @ %.2f sl=%.2f atr=%.1f "
                   "vol_ratio=%.1f drift=%.1f lot=%.3f risk=$%.0f\n",
                   is_long ? "LONG" : "SHORT",
                   entry_px, sl_px, atr, vol_ratio, ewm_drift, lot, risk);
        } else {
            printf("[MCE] ENTRY %s @ %.2f sl=%.2f atr=%.1f "
                   "vol_ratio=%.1f drift=%.1f lot=%.3f risk=$%.0f\n",
                   is_long ? "LONG" : "SHORT",
                   entry_px, sl_px, atr, vol_ratio, ewm_drift, lot, risk);
        }
        fflush(stdout);
    }

private:
    int64_t m_cooldown_until   = 0;
    int64_t m_long_block_until = 0;
    int64_t m_short_block_until= 0;

    void _manage(double bid, double ask, double mid,
                 double atr, double vol_ratio, double ewm_drift, int64_t now_ms) {

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        // SL hit
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            _close(pos.is_long ? bid : ask, "SL_HIT", now_ms);
            return;
        }

        // Max hold timeout
        if (now_ms - pos.entry_ms >= MAX_HOLD_MS) {
            _close(pos.is_long ? bid : ask, "MAX_HOLD", now_ms);
            return;
        }

        const double open_pnl = move * pos.full_size * 100.0;
        const double atr_live = (atr > 0.0)
            ? (0.70 * pos.atr_at_entry + 0.30 * atr)
            : pos.atr_at_entry;

        // ── STEP 1: hold until $200 open (velocity mode -- don't bank early) ──
        if (!pos.partial1_done && open_pnl >= STEP1_TRIGGER_USD) {
            const double close_qty = std::max(MIN_LOT,
                std::min(pos.size, std::floor(pos.full_size * 0.33 / MIN_LOT) * MIN_LOT));
            const double exit_px = pos.is_long ? bid : ask;
            const double pnl_this = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                                    * close_qty * 100.0;
            pos.banked_usd += pnl_this;
            pos.size -= close_qty;
            pos.partial1_done = true;
            pos.be_locked     = true;
            // SL moves to entry (BE)
            pos.sl = pos.entry;

            printf("[MCE] STEP1 banked %.3f lots @ %.2f pnl=$%.0f  sl->BE=%.2f\n",
                   close_qty, exit_px, pnl_this, pos.entry);
            fflush(stdout);

            if (on_close) on_close(exit_px, pos.is_long, close_qty, "PARTIAL_1");
        }

        // ── STEP 2: at $400 open ──────────────────────────────────────────
        if (pos.partial1_done && !pos.partial2_done && open_pnl >= STEP2_TRIGGER_USD) {
            const double close_qty = std::max(MIN_LOT,
                std::min(pos.size, std::floor(pos.size * 0.33 / MIN_LOT) * MIN_LOT));
            const double exit_px = pos.is_long ? bid : ask;
            const double pnl_this = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                                    * close_qty * 100.0;
            pos.banked_usd += pnl_this;
            pos.size -= close_qty;
            pos.partial2_done = true;

            printf("[MCE] STEP2 banked %.3f lots @ %.2f pnl=$%.0f\n",
                   close_qty, exit_px, pnl_this);
            fflush(stdout);

            if (on_close) on_close(exit_px, pos.is_long, close_qty, "PARTIAL_2");
        }

        // ── VELOCITY TRAIL ────────────────────────────────────────────────
        if (pos.be_locked && pos.mfe > 0.0) {
            if (pos.mfe >= atr_live * VEL_TRAIL_ARM_ATR) {
                // Trail armed: 2xATR behind MFE
                const double trail_dist = atr_live * VEL_TRAIL_DIST_ATR;
                const double trail_sl   = pos.is_long
                    ? (pos.entry + pos.mfe - trail_dist)
                    : (pos.entry - pos.mfe + trail_dist);

                // SL only moves forward (ratchet principle)
                if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
                if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
            }
        }

        // ── LOT-SCALED RATCHET ────────────────────────────────────────────
        // Fires every 1xATR of open PnL -- locks 80% of each tier
        // At ATR=10, 0.40 lots: $400/tier -- sensible intervals
        const double ratchet_step = std::max(50.0, 1.0 * pos.atr_at_entry * pos.full_size * 100.0);
        const int tier_now = (int)(open_pnl / ratchet_step);

        if (tier_now > pos.ratchet_tier && tier_now >= 1) {
            const double locked_usd  = tier_now * ratchet_step * RATCHET_KEEP;
            const double locked_pts  = (pos.size > 0.0)
                ? locked_usd / (pos.size * 100.0) : 0.0;
            const double min_pts     = pos.atr_at_entry * 0.5; // breathing room
            const double eff_pts     = std::max(locked_pts, min_pts);

            const double ratchet_sl = pos.is_long
                ? (pos.entry + eff_pts)
                : (pos.entry - eff_pts);

            // Cap: never beyond current price
            const double buf = 0.5;
            const double capped_sl = pos.is_long
                ? std::min(ratchet_sl, mid - buf)
                : std::max(ratchet_sl, mid + buf);

            const bool improves = pos.is_long
                ? (capped_sl > pos.sl)
                : (capped_sl < pos.sl);

            if (improves) {
                pos.sl           = capped_sl;
                pos.ratchet_tier = tier_now;
                printf("[MCE] RATCHET tier=%d open=$%.0f locked=$%.0f sl=%.2f\n",
                       tier_now, open_pnl, locked_usd, capped_sl);
                fflush(stdout);
            }
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms) {
        const double pnl_pts = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double pnl_usd = pnl_pts * pos.size * 100.0 + pos.banked_usd;

        printf("[MCE] %sCLOSE %s @ %.2f reason=%s pnl=$%.0f mfe=%.1fpt banked=$%.0f\n",
               shadow_mode ? "SHADOW-" : "",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_usd, pos.mfe, pos.banked_usd);
        fflush(stdout);

        // Direction block after SL hit
        if (std::string(reason) == "SL_HIT") {
            if (pos.is_long)  m_long_block_until  = now_ms + 60000;
            else              m_short_block_until = now_ms + 60000;
        }

        if (on_close) on_close(exit_px, pos.is_long, pos.size, reason);

        pos.active = false;
        m_cooldown_until = now_ms + COOLDOWN_MS;
    }
};

} // namespace omega
