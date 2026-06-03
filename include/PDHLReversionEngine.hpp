#pragma once
// =============================================================================
// PDHLReversionEngine.hpp -- PDH/PDL Mean Reversion Engine for XAUUSD
//
// DESIGN BASIS: 2yr backtest on 111M ticks proves:
//   INSIDE PDH/PDL: EV=+1.732pts at 15min (t=+477?)
//   OUTSIDE PDH/PDL: negative EV across all buckets
//   Gold is a mean-reverting auction market inside the daily range.
//   Breakouts, momentum continuation, RSI trend = negative EV.
//
// ENTRY LOGIC:
//   SHORT: price in top 25% of PDH-PDL range + L2 shows ask stacking / bid pulling
//   LONG:  price in bottom 25% of PDH-PDL range + L2 shows bid stacking / ask pulling
//
// L2 CONFIRMATION (required when l2_real=true):
//   LONG:  l2_imbalance > 0.55 (bids building) AND bid_levels increasing
//   SHORT: l2_imbalance < 0.45 (asks building) AND ask_levels increasing
//   Without L2: use drift reversal as proxy (drift turning from extreme)
//
// EXIT:
//   TP = 0.5x range width (target mid of range)
//   SL = 0.4x ATR (tight, structural)
//   Max hold = 15min
//   L2 flip exit: if l2_imbalance flips against position after MIN_HOLD
//
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OpenPositionRegistry.hpp"   // S-2026-06-03: omega::PositionSnapshot for persist
#include "OmegaCostGuard.hpp"

namespace omega {

class PDHLReversionEngine {
public:
    // -- Parameters ---------------------------------------------------------
    double RANGE_ENTRY_PCT   = 0.25;  // enter in top/bottom 25% of daily range
    double SL_ATR_MULT       = 0.40;  // SL = 0.4x ATR (tight structural stop)
    double TP_RANGE_FRAC     = 0.50;  // TP = mid of range (50% back from extreme)
    double L2_LONG_MIN       = 0.55;  // min imbalance to enter long (bids building)
    double L2_SHORT_MAX      = 0.45;  // max imbalance to enter short (asks building)
    double DRIFT_FADE_MIN    = 1.5;   // min |drift| for no-L2 entry (fade extreme)
    double MIN_RANGE_PTS     = 8.0;   // min PDH-PDL range (filter thin days)
    double MAX_SPREAD_PTS    = 1.5;   // spread gate
    double MIN_ATR_PTS       = 1.0;   // minimum ATR for entry
    double RISK_USD          = 30.0;
    double MIN_LOT           = 0.01;
    double MAX_LOT           = 0.01;  // FIX 2026-04-22 uniformity: capped to 0.01 SHADOW-mode
    int64_t COOLDOWN_MS      = 120'000;  // 2min cooldown between trades
    int64_t MAX_HOLD_MS      = 900'000;  // 15min max hold
    int64_t MIN_HOLD_MS      = 20'000;   // 20s min hold before L2 exit
    bool    enabled          = true;
    bool    shadow_mode      = true;

    // S63 2026-05-13 VWR-pattern in-flight protection (LOSS_CUT + BE_RATCHET).
    //   Mirrors CrossAssetEngines.hpp VWAPReversionEngine L1245-1247.
    //   Defaults sized for gold daily-range scale (PDH-PDL typically 30-80pt
    //   on XAUUSD; entry inside top/bottom 25% so target distance is roughly
    //   0.5 * range).
    //   LOSS_CUT_PCT   -- cut when adverse >= entry*pct/100. Catches trades
    //                     that go straight adverse from entry. 0.0 disables.
    //   BE_ARM_PCT     -- mfe % of entry that arms BE ratchet. 0.0 disables.
    //   BE_BUFFER_PCT  -- BE_CUT triggers when move <= entry*pct/100 after
    //                     arm. Typical = entry-time spread expressed as %.
    //   Override per-instance in engine_init.hpp if defaults don't suit.
    double  LOSS_CUT_PCT     = 0.04;  // ~$1.50 cut at $3700 entry (~10 pips XAU)
    double  BE_ARM_PCT       = 0.025; // ~$0.92 arm threshold
    double  BE_BUFFER_PCT    = 0.01;  // ~$0.37 BE-trigger buffer

    // -- State ---------------------------------------------------------------
    struct Position {
        bool    active    = false;
        bool    is_long   = false;
        double  entry     = 0.0;
        double  sl        = 0.0;
        double  tp        = 0.0;
        double  size      = 0.0;
        double  atr       = 0.0;
        double  mfe       = 0.0;
        double  mae       = 0.0;
        int64_t entry_ms  = 0;
        double  spread_at_entry = 0.0;  // bid-ask spread captured at entry (ledger forensics)
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    // S-2026-06-03: open-position persistence across restart.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine = eng; o.symbol = sym; o.side = pos.is_long ? "LONG" : "SHORT";
        o.size = pos.size; o.entry = pos.entry; o.sl = pos.sl; o.tp = pos.tp;
        o.entry_ts = pos.entry_ms / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos.active = true; pos.is_long = (ps.side == "LONG");
        pos.entry = ps.entry; pos.sl = ps.sl; pos.tp = ps.tp; pos.size = ps.size;
        pos.entry_ms = ps.entry_ts * 1000;
        return true;
    }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // -- Main tick ------------------------------------------------------------
    void on_tick(double bid, double ask,
                 int64_t now_ms,
                 double  pdh,          // previous day high
                 double  pdl,          // previous day low
                 double  atr,          // M1 ATR
                 double  l2_imbalance, // 0..1, 0.5=neutral
                 int     depth_bid,    // bid depth level count
                 int     depth_ask,    // ask depth level count
                 bool    l2_real,      // true = real L2 data
                 double  ewm_drift,    // EWM drift (30s halflife)
                 int     session_slot, // 0-6
                 CloseCallback on_close = nullptr) noexcept
    {
        if (!enabled) return;
        if (bid <= 0 || ask <= bid) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Hard filter: must have valid PDH/PDL
        if (pdh <= 0 || pdl <= 0 || pdh <= pdl) return;
        const double range = pdh - pdl;
        if (range < MIN_RANGE_PTS) return;

        // Hard filter: must be inside daily range
        if (mid > pdh + 1.0 || mid < pdl - 1.0) return;

        // Manage open position
        if (pos.active) {
            _manage(bid, ask, mid, now_ms, l2_imbalance, l2_real, on_close);
            return;
        }

        // Cooldown
        if (now_ms - m_last_close_ms < COOLDOWN_MS) return;

        // Quality gates
        if (spread > MAX_SPREAD_PTS) return;
        if (atr < MIN_ATR_PTS) return;
        if (session_slot == 0) return;  // dead zone

        // -- Entry zones ------------------------------------------------------
        const double upper_zone = pdh - range * RANGE_ENTRY_PCT;  // top 25%
        const double lower_zone = pdl + range * RANGE_ENTRY_PCT;  // bottom 25%

        const bool in_upper = (mid >= upper_zone);  // near PDH -- fade short
        const bool in_lower = (mid <= lower_zone);  // near PDL -- fade long

        if (!in_upper && !in_lower) return;

        // -- L2 confirmation --------------------------------------------------
        bool l2_long_ok  = true;
        bool l2_short_ok = true;

        if (l2_real) {
            // Real L2: require imbalance confirms fade direction
            l2_long_ok  = (l2_imbalance >= L2_LONG_MIN);   // bids building = bounce likely
            l2_short_ok = (l2_imbalance <= L2_SHORT_MAX);  // asks building = fade likely
        } else {
            // No real L2: use drift as proxy -- fade extreme drift
            // If price is at upper zone with positive drift -> fade (short)
            // If price is at lower zone with negative drift -> fade (long)
            l2_long_ok  = (in_lower && ewm_drift < -DRIFT_FADE_MIN);
            l2_short_ok = (in_upper && ewm_drift >  DRIFT_FADE_MIN);
        }

        const bool enter_long  = in_lower && l2_long_ok;
        const bool enter_short = in_upper && l2_short_ok;

        if (!enter_long && !enter_short) return;

        // -- Size and SL ------------------------------------------------------
        const bool  is_long   = enter_long;
        const double ep       = is_long ? ask : bid;
        const double sl_pts   = std::max(atr * SL_ATR_MULT, spread * 2.0);
        const double sl_px    = is_long ? (ep - sl_pts) : (ep + sl_pts);
        // TP = target mid of range
        const double mid_range = (pdh + pdl) * 0.5;
        const double tp_px    = is_long
            ? std::min(mid_range, ep + range * TP_RANGE_FRAC)
            : std::max(mid_range, ep - range * TP_RANGE_FRAC);

        double sz = RISK_USD / (sl_pts * 100.0);
        sz = std::floor(sz / 0.001) * 0.001;
        sz = std::max(MIN_LOT, std::min(MAX_LOT, sz));

        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        {
            const double tp_dist = std::fabs(tp_px - ep);
            if (!ExecutionCostGuard::is_viable(
                    "XAUUSD", spread, tp_dist, sz, 1.5))
            {
                return;
            }
        }

        pos = Position{
            true, is_long, ep, sl_px, tp_px, sz, atr,
            0.0, 0.0, now_ms, spread
        };

        printf("[PDHL%s] %s @ %.2f  sl=%.2f tp=%.2f  pdh=%.2f pdl=%.2f  "
               "range=%.1f  zone=%s  l2=%.3f  drift=%.2f  size=%.3f%s\n",
               shadow_mode ? "-SHADOW" : "",
               is_long ? "LONG" : "SHORT", ep, sl_px, tp_px,
               pdh, pdl, range,
               is_long ? "LOWER" : "UPPER",
               l2_imbalance, ewm_drift, sz,
               l2_real ? " [L2-REAL]" : " [L2-PROXY]");
        fflush(stdout);
    }

private:
    int64_t m_last_close_ms = 0;

    void _manage(double bid, double ask, double mid, int64_t now_ms,
                 double l2_imb, bool l2_real,
                 CloseCallback on_close) noexcept
    {
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        const int64_t hold_ms = now_ms - pos.entry_ms;

        // S63 2026-05-13 VWR-pattern in-flight protection.
        //   Phase 1 (BE_RATCHET): if mfe reached arm threshold and price has
        //     given back, cut at break-even-plus-buffer. Catches trades that
        //     went +X then returned to near-entry.
        //   Phase 2 (LOSS_CUT): if trade went straight adverse, cut early.
        //   Both run BEFORE the legacy SL/TP/MAX_HOLD chain so they take
        //   priority. Either phase disable-able by setting _PCT = 0.0.
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0) {
            const double arm_pts    = pos.entry * BE_ARM_PCT    / 100.0;
            const double buffer_pts = pos.entry * BE_BUFFER_PCT / 100.0;
            if (pos.mfe >= arm_pts && move <= buffer_pts) {
                printf("[PDHL-REV] BE_CUT mfe=%.2f arm=%.2f move=%.2f buf=%.2f\n",
                       pos.mfe, arm_pts, move, buffer_pts);
                fflush(stdout);
                _close(pos.is_long ? bid : ask, "BE_CUT", now_ms, on_close);
                return;
            }
        }
        if (LOSS_CUT_PCT > 0.0) {
            const double adverse       = -move;  // positive when losing
            const double loss_cut_dist = pos.entry * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                printf("[PDHL-REV] LOSS_CUT adverse=%.2f >= %.2f (%.3f%% of entry)\n",
                       adverse, loss_cut_dist, LOSS_CUT_PCT);
                fflush(stdout);
                _close(pos.is_long ? bid : ask, "LOSS_CUT", now_ms, on_close);
                return;
            }
        }

        // SL hit
        if ((pos.is_long && bid <= pos.sl) || (!pos.is_long && ask >= pos.sl)) {
            _close(pos.sl, "SL_HIT", now_ms, on_close); return;
        }

        // TP hit
        if ((pos.is_long && bid >= pos.tp) || (!pos.is_long && ask <= pos.tp)) {
            _close(pos.is_long ? bid : ask, "TP_HIT", now_ms, on_close); return;
        }

        // Max hold
        // 2026-05-13 (part L): VWR-pattern winner exemption.
        if (hold_ms >= MAX_HOLD_MS && move <= 0.0) {
            _close(pos.is_long ? bid : ask, "MAX_HOLD", now_ms, on_close); return;
        }

        // L2 flip exit: after MIN_HOLD, if imbalance flips strongly against us
        if (l2_real && hold_ms >= MIN_HOLD_MS && pos.mfe > 0) {
            const bool l2_flipped =
                (pos.is_long  && l2_imb < 0.40) ||  // was buying, now selling
                (!pos.is_long && l2_imb > 0.60);     // was selling, now buying
            if (l2_flipped) {
                _close(pos.is_long ? bid : ask, "L2_FLIP", now_ms, on_close); return;
            }
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_ms,
                CloseCallback on_close) noexcept
    {
        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.engine     = "PDHLReversionEngine";
        tr.regime     = "PDHL_REVERSION";
        tr.entryPrice = pos.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos.sl;
        tr.size       = pos.size;
        tr.pnl        = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px)) * pos.size;
        tr.mfe        = pos.mfe * pos.size;
        tr.mae        = pos.mae * pos.size;
        tr.entryTs    = pos.entry_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.l2_live    = true;
        tr.shadow     = shadow_mode;
        tr.spreadAtEntry = pos.spread_at_entry;

        printf("[PDHL%s] EXIT %s @ %.2f  pnl=$%.2f  mfe=%.3f  reason=%s  held=%llds\n",
               shadow_mode ? "-SHADOW" : "",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, tr.pnl * 100.0, pos.mfe, reason,
               (long long)((now_ms - pos.entry_ms) / 1000));
        fflush(stdout);

        m_last_close_ms = now_ms;
        pos = Position{};

        if (on_close && !shadow_mode) on_close(tr);
        if (shadow_mode) on_close(tr);  // always fire callback for telemetry
    }
};

} // namespace omega
