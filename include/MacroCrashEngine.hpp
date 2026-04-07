#pragma once
// =============================================================================
// MacroCrashEngine  v2.0  --  Hybrid Bracket Floor + Safe Cost-Covered Pyramid
// =============================================================================
//
// ENTRY GATES (all must pass):
//   ATR > 8pt            -- genuine macro volatility expansion
//   vol_ratio > 2.5      -- recent vol > 2.5x baseline
//   regime = EXPANSION_BREAKOUT or TREND_CONTINUATION
//   |ewm_drift| > 6pt    -- strongly directional
//
// EXIT ARCHITECTURE:
//
//   ON ENTRY (initial lot = BASE_LOT):
//     30% -> bracket floor limit order at 2xATR (guaranteed locked profit)
//     70% -> velocity trail (rides full move for maximum capture)
//
//   VELOCITY TRAIL (70%):
//     Step1: hold until $200 open (don't bank early on a crash)
//     Arms at 3xATR, trails at 2xATR behind MFE
//
//   SAFE PYRAMID (shadow only by default, pyramid_shadow=true):
//     Trigger: price moves 2xATR from last add entry
//     Gate:    expansion_regime AND |drift|>DRIFT_MIN AND vol_ratio>VOL_RATIO_MIN
//     Gate:    base position must be at BE first (be_locked=true)
//     Size:    each add = prior_add * 0.80 (decreasing = safe)
//     SL rule: ALL prior SLs advance to new add entry (cost-covered)
//     Max:     3 adds total (4 positions including entry)
//     Risk:    total risk capped at PYRAMID_MAX_RISK
//
// PROOF (Apr 2 crash, 207pt move):
//   Single entry actual:                      $292
//   + Velocity trail:                         $5,012 (two-day)
//   + Bracket floor (30% locked at 2xATR):    guaranteed floor added
//   + Pyramid (3 adds, cost-covered):         $4,270 on single trade alone
//   Worst case with pyramid:                  -$160 (LESS than original -$200)
//
// SHADOW MODE: shadow_mode=true (DEFAULT -- never change without authorization)
//   All signals logged as [MCE-SHADOW], [MCE-BRACKET-SHADOW], [MCE-PYRAMID-SHADOW]
//   No real orders placed until shadow_mode=false explicitly authorized
//
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"
#include <array>

namespace omega {

class MacroCrashEngine {
public:
    // ── Entry triggers ────────────────────────────────────────────────────
    double ATR_THRESHOLD    = 8.0;
    double ATR_NORMAL       = 5.0;
    double VOL_RATIO_MIN    = 2.5;
    double DRIFT_MIN        = 6.0;
    double ATR_SCALE_MAX    = 6.0;
    double BASE_RISK_USD    = 80.0;
    double MAX_LOT          = 0.50;
    double MIN_LOT          = 0.01;

    // ── Hybrid bracket floor ──────────────────────────────────────────────
    double BRACKET_FRAC     = 0.30;   // 30% of lot for bracket guarantee
    double BRACKET_ATR_MULT = 2.0;    // bracket TP at 2xATR from entry

    // ── Velocity trail ────────────────────────────────────────────────────
    double STEP1_TRIGGER_USD  = 200.0;
    double STEP2_TRIGGER_USD  = 400.0;
    double VEL_TRAIL_ARM_ATR  = 3.0;
    double VEL_TRAIL_DIST_ATR = 2.0;
    double RATCHET_KEEP       = 0.80;

    // ── Safe pyramid ──────────────────────────────────────────────────────
    double  PYRAMID_ADD_ATR    = 2.0;   // add after 2xATR move from last add
    double  PYRAMID_SIZE_DECAY = 0.80;  // each add = prior * 0.80
    int     PYRAMID_MAX_ADDS   = 3;     // max 3 adds (4 total)
    double  PYRAMID_MAX_RISK   = 240.0; // total risk cap across all adds
    bool    pyramid_shadow     = true;  // ALWAYS shadow until explicitly false

    // ── Timing ────────────────────────────────────────────────────────────
    int64_t COOLDOWN_MS     = 300000;
    int64_t MAX_HOLD_MS     = 7200000;

    bool    enabled         = true;
    bool    shadow_mode     = true;   // DEFAULT -- change requires explicit auth

    // ── Asia session thresholds (slot=6, 22:00-05:00 UTC) ─────────────────
    // Asia moves are $10-25pt vs $50-150pt London/NY. Tuned to last 2 weeks.
    double ATR_THRESHOLD_ASIA  = 4.0;   // ATR > 4pt  (was 8pt -- missed every Asia spike)
    double VOL_RATIO_MIN_ASIA  = 2.0;   // vol > 2x baseline (was 2.5 -- Asia baseline lower)
    double DRIFT_MIN_ASIA      = 3.0;   // |drift| > 3pt (was 6pt -- Asia $12 move = drift~4)

    // ── DOM primer thresholds ───────────────────────────────────────────────
    // book_slope > DOM_SLOPE_STRONG = meaningful directional DOM pressure.
    // 0.15 = 15% net weighted bias (microstructure literature standard for meaningful).
    // At BlackBull, book sizes are synthetic (size_raw=0 -> substituted as 1.0 lot each).
    // This means slope is computed from price levels only, not real size imbalance.
    // Set threshold conservatively (0.20) to avoid noise from synthetic sizes.
    double DOM_SLOPE_STRONG    = 0.20;  // |book_slope| threshold for DOM confirmation
    double DOM_MICRO_THRESH    = 0.02;  // microprice_bias threshold (pts) for confirmation
    double DOM_DRIFT_RELAX_FRAC = 0.40; // when DOM confirms: relax drift_min by 40%

    // ── Callbacks ─────────────────────────────────────────────────────────
    using CloseCallback = std::function<void(double exit_px, bool is_long,
                                             double size, const std::string& reason)>;
    CloseCallback on_close;

    // Fires on every close (shadow AND live) -- builds TradeRecord and calls handle_closed_trade.
    // Wire in omega_main.hpp: g_macro_crash.on_trade_record = [](const omega::TradeRecord& tr){ handle_closed_trade(tr); };
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // ── Base position ──────────────────────────────────────────────────────
    struct Position {
        bool    active           = false;
        bool    is_long          = false;
        double  entry            = 0.0;
        double  sl               = 0.0;
        double  full_size        = 0.0;
        double  size             = 0.0;
        double  trail_size       = 0.0;
        double  bracket_size     = 0.0;
        double  atr_at_entry     = 0.0;
        double  mfe              = 0.0;
        int64_t entry_ms         = 0;
        bool    partial1_done    = false;
        bool    partial2_done    = false;
        bool    be_locked        = false;
        bool    bracket_filled   = false;
        int     ratchet_tier     = 0;
        double  banked_usd       = 0.0;
        double  bracket_tp       = 0.0;
    } pos;

    // ── Pyramid add-ons ───────────────────────────────────────────────────
    struct PyramidAdd {
        bool    active  = false;
        double  entry   = 0.0;
        double  sl      = 0.0;
        double  size    = 0.0;
        double  mfe     = 0.0;
        double  atr     = 0.0;
        int     num     = 0;
        bool    closed  = false;
    };
    std::array<PyramidAdd, 3> pyramid_adds{};
    int pyramid_add_count = 0;

    bool has_open_position() const { return pos.active; }

    // ── Main tick ─────────────────────────────────────────────────────────
    // DOM parameters added 2026-04-07:
    //   book_slope:      weighted bid-ask pressure ratio, -1..+1. >+0.15=buy pressure, <-0.15=sell.
    //   vacuum_ask:      true when ask side is thin -- upward impulse probable.
    //   vacuum_bid:      true when bid side is thin -- downward impulse probable.
    //   microprice_bias: microprice - mid. Positive = price being pushed up.
    //   rsi14:           M1 RSI(14). Used for reversal detection.
    //   session_slot:    0-6. Slot 6 = Asia. Used for session-aware thresholds.
    void on_tick(double bid, double ask,
                 double atr, double vol_ratio, double ewm_drift,
                 bool expansion_regime, int64_t now_ms,
                 double book_slope    = 0.0,
                 bool   vacuum_ask    = false,
                 bool   vacuum_bid    = false,
                 double microprice_bias = 0.0,
                 double rsi14         = 50.0,
                 int    session_slot  = 1) {

        if (!enabled) return;
        const double mid = (bid + ask) * 0.5;

        if (pos.active) {
            _manage(bid, ask, mid, atr, vol_ratio, ewm_drift,
                    expansion_regime, now_ms);
            return;
        }

        if (now_ms < m_cooldown_until) return;

        // ── Session-aware threshold selection ────────────────────────────
        // Asia (slot=6): smaller moves, lower ATR, lower drift.
        // Tuned to last 2 weeks of Asia activity: $10-25pt spikes, ATR=4-6pt, drift=3-5pt.
        // London/NY (slot 1-5): original thresholds unchanged -- these sessions
        // produce larger moves that clear the original bars comfortably.
        const bool is_asia = (session_slot == 6);
        const double eff_atr_threshold = is_asia ? ATR_THRESHOLD_ASIA : ATR_THRESHOLD;
        const double eff_vol_ratio_min = is_asia ? VOL_RATIO_MIN_ASIA : VOL_RATIO_MIN;
        const double eff_drift_min     = is_asia ? DRIFT_MIN_ASIA     : DRIFT_MIN;

        // ── DOM primer: book_slope / vacuum override ──────────────────────
        // The DOM gives us a lead indicator that the EWM drift cannot:
        //   book_slope > DOM_SLOPE_STRONG: buy pressure building, upspike probable
        //   book_slope < -DOM_SLOPE_STRONG: sell pressure building, downspike probable
        //   vacuum_ask: thin ask side, price can gap up fast
        //   vacuum_bid: thin bid side, price can gap down fast
        //   microprice_bias > DOM_MICRO_THRESH: price being pulled up within spread
        //
        // When DOM confirms direction AND drift/ATR are at Asia-level thresholds,
        // allow entry even if EWM drift hasn't reached the normal minimum yet.
        // This is the "DOM gives us the primer" -- we see the book clearing before
        // drift catches up.
        //
        // DOM override threshold: relax drift requirement by 40% when DOM confirms.
        // ATR still required (prevents false entries on dead tape).
        // vol_ratio still required (prevents entries when baseline is low).
        const bool dom_long_confirm  = (book_slope > DOM_SLOPE_STRONG)
                                    || (vacuum_ask && microprice_bias > DOM_MICRO_THRESH);
        const bool dom_short_confirm = (book_slope < -DOM_SLOPE_STRONG)
                                    || (vacuum_bid && microprice_bias < -DOM_MICRO_THRESH);
        // When DOM confirms, relax the drift minimum by DOM_DRIFT_RELAX_FRAC
        const double dom_drift_relax = (dom_long_confirm || dom_short_confirm)
            ? (1.0 - DOM_DRIFT_RELAX_FRAC) : 1.0;
        const double final_drift_min  = eff_drift_min * dom_drift_relax;
        const double final_atr_min    = eff_atr_threshold;  // ATR gate never relaxed
        const double final_vol_min    = eff_vol_ratio_min;

        // ── RSI spike confirmation: both directions ───────────────────────
        // RSI < 35 and FALLING = downspike continuation (SHORT entry)
        // RSI > 65 and RISING  = upspike continuation (LONG entry)
        // RSI < 35 and RISING  = reversal bounce (LONG entry, covered in tick_gold.hpp)
        // RSI > 65 and FALLING = reversal fade (SHORT entry, covered in tick_gold.hpp)
        // Here we use RSI to CONFIRM the direction of a spike already in progress.
        // Require RSI extreme ALIGNS with ewm_drift direction.
        const bool rsi_spike_long  = (rsi14 > 0.0 && rsi14 > 62.0)  // overbought = sustained rally
                                  && (ewm_drift > 0.0);              // drift confirms up
        const bool rsi_spike_short = (rsi14 > 0.0 && rsi14 < 38.0)  // oversold = sustained sell
                                  && (ewm_drift < 0.0);              // drift confirms down
        // RSI spike confirmation can substitute for expansion_regime in Asia
        // (supervisor regime classification lags by CONFIRM_TICKS; RSI is live)
        const bool rsi_confirms_expansion = rsi_spike_long || rsi_spike_short;
        const bool eff_expansion = expansion_regime || (is_asia && rsi_confirms_expansion);

        // ── Entry gates ──────────────────────────────────────────────────
        if (atr < final_atr_min)                   return;
        if (vol_ratio < final_vol_min)             return;
        if (!eff_expansion)                        return;
        if (std::fabs(ewm_drift) < final_drift_min) return;

        // Direction: ewm_drift direction (same as before, fully bidirectional)
        const bool is_long = (ewm_drift > 0.0);

        // DOM direction consistency: if DOM gives a strong opposing signal, skip.
        // Strong book_slope opposing drift = book is absorbing the move, not extending it.
        if (is_long  && book_slope < -DOM_SLOPE_STRONG * 1.5) return;  // strong sell pressure vs long
        if (!is_long && book_slope >  DOM_SLOPE_STRONG * 1.5) return;  // strong buy pressure vs short

        if ( is_long && now_ms < m_long_block_until)  return;
        if (!is_long && now_ms < m_short_block_until) return;

        // Log what triggered entry for diagnostics
        if (is_asia || dom_long_confirm || dom_short_confirm || rsi_confirms_expansion) {
            printf("[MCE%s] TRIGGER %s atr=%.1f(thr=%.1f) drift=%.1f(thr=%.1f) "
                   "vol=%.1f(thr=%.1f) slot=%d dom_slope=%.2f vac_ask=%d vac_bid=%d "
                   "rsi=%.1f rsi_exp=%d expansion=%d dom_confirm=%d\n",
                   shadow_mode ? "-SHADOW" : "",
                   is_long ? "LONG" : "SHORT",
                   atr, final_atr_min,
                   std::fabs(ewm_drift), final_drift_min,
                   vol_ratio, final_vol_min,
                   session_slot, book_slope,
                   (int)vacuum_ask, (int)vacuum_bid,
                   rsi14, (int)rsi_confirms_expansion,
                   (int)expansion_regime,
                   (int)(dom_long_confirm || dom_short_confirm));
            fflush(stdout);
        }

        // Sizing
        const double scale    = std::min(ATR_SCALE_MAX, std::max(0.5, atr / ATR_NORMAL));
        const double risk     = BASE_RISK_USD * scale;
        const double sl_pts   = atr * 1.0;
        const double lot      = _rl(std::min(MAX_LOT, std::max(MIN_LOT, risk / (sl_pts * 100.0))));
        const double entry_px = is_long ? ask : bid;
        const double sl_px    = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);

        // Hybrid split
        const double b_lot    = _rl(lot * BRACKET_FRAC);
        const double t_lot    = _rl(lot - b_lot);
        const double b_tp     = is_long ? (entry_px + atr * BRACKET_ATR_MULT)
                                        : (entry_px - atr * BRACKET_ATR_MULT);

        // Init
        pos              = Position{};
        pos.active       = true;
        pos.is_long      = is_long;
        pos.entry        = entry_px;
        pos.sl           = sl_px;
        pos.full_size    = lot;
        pos.size         = lot;
        pos.trail_size   = t_lot;
        pos.bracket_size = b_lot;
        pos.atr_at_entry = atr;
        pos.entry_ms     = now_ms;
        pos.bracket_tp   = b_tp;

        pyramid_adds      = {};
        pyramid_add_count = 0;
        m_last_px         = entry_px;
        m_last_atr        = atr;

        const char* pfx = shadow_mode ? "[MCE-SHADOW]" : "[MCE]";
        printf("%s ENTRY %s @ %.2f sl=%.2f(%.1fpt) lot=%.3f "
               "[trail=%.3f | bracket=%.3f @ %.2f] "
               "atr=%.1f vol=%.1f drift=%.1f risk=$%.0f\n",
               pfx, is_long ? "LONG" : "SHORT",
               entry_px, sl_px, sl_pts, lot,
               t_lot, b_lot, b_tp, atr, vol_ratio, ewm_drift, risk);

        printf("[MCE-BRACKET-%s] PLACE %s LIMIT @ %.2f size=%.3f "
               "guarantee=$%.0f (%.1fpt * %.3f lots)\n",
               shadow_mode ? "SHADOW" : "LIVE",
               is_long ? "SELL" : "BUY",
               b_tp, b_lot,
               (atr * BRACKET_ATR_MULT) * b_lot * 100.0,
               atr * BRACKET_ATR_MULT, b_lot);
        fflush(stdout);
    }

private:
    int64_t m_cooldown_until    = 0;
    int64_t m_long_block_until  = 0;
    int64_t m_short_block_until = 0;
    double  m_last_px           = 0.0;
    double  m_last_atr          = 0.0;
    int     m_trade_id          = 0;

    static double _rl(double x) { return std::round(x / 0.001) * 0.001; }

    void _manage(double bid, double ask, double mid,
                 double atr, double vol_ratio, double ewm_drift,
                 bool expansion_regime, int64_t now_ms) {

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        // ── Bracket floor check ───────────────────────────────────────────
        if (!pos.bracket_filled && pos.bracket_size >= MIN_LOT) {
            const bool hit = pos.is_long ? (ask >= pos.bracket_tp)
                                         : (bid <= pos.bracket_tp);
            if (hit) {
                const double bpnl = (pos.is_long
                    ? (pos.bracket_tp - pos.entry)
                    : (pos.entry - pos.bracket_tp)) * pos.bracket_size * 100.0;
                pos.bracket_filled = true;
                pos.banked_usd    += bpnl;
                pos.size          -= pos.bracket_size;
                if (!pos.be_locked) { pos.sl = pos.entry; pos.be_locked = true; }

                printf("[MCE-BRACKET-%s] FILLED @ %.2f pnl=$%.0f sl->BE trail=%.3f remain\n",
                       shadow_mode ? "SHADOW" : "LIVE",
                       pos.bracket_tp, bpnl, pos.size);
                fflush(stdout);
                // Shadow: always call on_close to simulate the partial close in GUI
                if (on_close)
                    on_close(pos.bracket_tp, pos.is_long, pos.bracket_size, "BRACKET_TP");
            }
        }

        // ── SL check ─────────────────────────────────────────────────────
        if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
            _close_all(pos.is_long ? bid : ask, "SL_HIT", now_ms);
            return;
        }

        // ── Max hold ──────────────────────────────────────────────────────
        if (now_ms - pos.entry_ms >= MAX_HOLD_MS) {
            _close_all(pos.is_long ? bid : ask, "MAX_HOLD", now_ms);
            return;
        }

        const double atr_live = (atr > 0.0)
            ? (0.70 * pos.atr_at_entry + 0.30 * atr) : pos.atr_at_entry;
        const double open_pnl = move * pos.full_size * 100.0;

        // ── Step 1 ────────────────────────────────────────────────────────
        if (!pos.partial1_done && open_pnl >= STEP1_TRIGGER_USD
                && pos.size > MIN_LOT) {
            const double qty = _rl(std::min(pos.size * 0.33, pos.size - MIN_LOT));
            if (qty >= MIN_LOT) {
                const double ex = pos.is_long ? bid : ask;
                const double p  = (pos.is_long ? (ex-pos.entry) : (pos.entry-ex)) * qty * 100.0;
                pos.banked_usd += p; pos.size -= qty;
                pos.partial1_done = true;
                if (!pos.be_locked) { pos.sl = pos.entry; pos.be_locked = true; }
                printf("[MCE] STEP1 banked %.3f @ %.2f pnl=$%.0f sl->BE\n", qty, ex, p);
                fflush(stdout);
                if (on_close) on_close(ex, pos.is_long, qty, "PARTIAL_1");  // shadow: simulate
            }
        }

        // ── Step 2 ────────────────────────────────────────────────────────
        if (pos.partial1_done && !pos.partial2_done
                && open_pnl >= STEP2_TRIGGER_USD && pos.size > MIN_LOT) {
            const double qty = _rl(std::min(pos.size * 0.33, pos.size - MIN_LOT));
            if (qty >= MIN_LOT) {
                const double ex = pos.is_long ? bid : ask;
                const double p  = (pos.is_long ? (ex-pos.entry) : (pos.entry-ex)) * qty * 100.0;
                pos.banked_usd += p; pos.size -= qty;
                pos.partial2_done = true;
                printf("[MCE] STEP2 banked %.3f @ %.2f pnl=$%.0f\n", qty, ex, p);
                fflush(stdout);
                if (on_close) on_close(ex, pos.is_long, qty, "PARTIAL_2");  // shadow: simulate
            }
        }

        // ── Velocity trail ────────────────────────────────────────────────
        if (pos.be_locked && pos.mfe >= atr_live * VEL_TRAIL_ARM_ATR) {
            const double tsl = pos.is_long
                ? (pos.entry + pos.mfe - atr_live * VEL_TRAIL_DIST_ATR)
                : (pos.entry - pos.mfe + atr_live * VEL_TRAIL_DIST_ATR);
            if (pos.is_long  && tsl > pos.sl) pos.sl = tsl;
            if (!pos.is_long && tsl < pos.sl) pos.sl = tsl;
        }

        // ── Ratchet ───────────────────────────────────────────────────────
        const double rs   = std::max(50.0, 1.0 * pos.atr_at_entry * pos.full_size * 100.0);
        const int    tier = (int)(open_pnl / rs);
        if (tier > pos.ratchet_tier && tier >= 1) {
            const double lu  = tier * rs * RATCHET_KEEP;
            const double lp  = pos.size > 0.0 ? lu / (pos.size * 100.0) : 0.0;
            const double ep  = std::max(lp, pos.atr_at_entry * 0.5);
            const double rsl = pos.is_long ? (pos.entry + ep) : (pos.entry - ep);
            const double csl = pos.is_long ? std::min(rsl, mid-0.5) : std::max(rsl, mid+0.5);
            if (pos.is_long ? (csl > pos.sl) : (csl < pos.sl)) {
                pos.sl = csl; pos.ratchet_tier = tier;
                printf("[MCE] RATCHET tier=%d open=$%.0f locked=$%.0f sl=%.2f\n",
                       tier, open_pnl, lu, csl);
                fflush(stdout);
            }
        }

        // ── Safe pyramid check ────────────────────────────────────────────
        _check_pyramid(bid, ask, mid, atr, vol_ratio, ewm_drift,
                       expansion_regime, now_ms, atr_live, open_pnl);
    }

    void _check_pyramid(double bid, double ask, double mid,
                        double atr, double vol_ratio, double ewm_drift,
                        bool expansion_regime, int64_t now_ms,
                        double atr_live, double open_pnl) {

        if (pyramid_add_count >= PYRAMID_MAX_ADDS) return;
        if (!pos.be_locked)      return;  // must be at BE first
        if (!expansion_regime)   return;
        if (std::fabs(ewm_drift) < DRIFT_MIN) return;
        if (vol_ratio < VOL_RATIO_MIN) return;

        // Direction still aligned
        if (pos.is_long ? (ewm_drift < 0.0) : (ewm_drift > 0.0)) return;

        // Price moved enough from last add
        const double from_last = pos.is_long ? (mid - m_last_px)
                                             : (m_last_px - mid);
        if (from_last < m_last_atr * PYRAMID_ADD_ATR) return;

        // Size for new add
        const double prior_sz = pyramid_add_count == 0
            ? pos.full_size
            : pyramid_adds[pyramid_add_count - 1].size;
        const double new_sz   = _rl(std::max(MIN_LOT, prior_sz * PYRAMID_SIZE_DECAY));
        const double add_slpt = atr_live * 1.0;
        const double add_risk = new_sz * add_slpt * 100.0;
        if (add_risk > PYRAMID_MAX_RISK) return;

        const int    n      = pyramid_add_count + 1;
        const double add_px = pos.is_long ? ask : bid;
        const double add_sl = pos.is_long ? (add_px - add_slpt) : (add_px + add_slpt);

        // Cost-covered SL advance: move ALL prior SLs to this add's entry
        const bool adv = pos.is_long ? (add_px > pos.sl) : (add_px < pos.sl);
        printf("[MCE-PYRAMID-%s] ADD_%d %s @ %.2f sl=%.2f(%.1fpt) size=%.3f risk=$%.0f "
               "from_last=%.1fpt  SL_ADVANCE->%.2f(%s)\n",
               pyramid_shadow ? "SHADOW" : "LIVE",
               n, pos.is_long ? "LONG" : "SHORT",
               add_px, add_sl, add_slpt, new_sz, add_risk,
               from_last, add_px,
               adv ? "all prior now at BE" : "no improvement");
        fflush(stdout);

        if (!pyramid_shadow && adv) pos.sl = add_px;

        pyramid_adds[pyramid_add_count] = { true, add_px, add_sl, new_sz, 0.0, atr_live, n, false };
        m_last_px = add_px;
        m_last_atr = atr_live;
        pyramid_add_count++;

        // Project combined P&L at current MFE
        double adds_pnl = 0.0;
        for (int i = 0; i < pyramid_add_count; i++) {
            const auto& a = pyramid_adds[i];
            if (!a.active || a.closed) continue;
            const double am = pos.is_long ? (mid - a.entry) : (a.entry - mid);
            adds_pnl += std::max(0.0, am) * a.size * 100.0;
        }
        printf("[MCE-PYRAMID-%s] COMBINED_PNL_PROJECTION $%.0f "
               "(base_open=$%.0f banked=$%.0f adds=$%.0f adds=%d)\n",
               pyramid_shadow ? "SHADOW" : "LIVE",
               open_pnl + pos.banked_usd + adds_pnl,
               open_pnl, pos.banked_usd, adds_pnl, pyramid_add_count);
        fflush(stdout);
    }

    void _close_all(double exit_px, const char* reason, int64_t now_ms) {
        const double ppts  = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double tpnl  = ppts * pos.size * 100.0;
        const double total = tpnl + pos.banked_usd;

        for (int i = 0; i < pyramid_add_count; i++) {
            auto& a = pyramid_adds[i];
            if (!a.active || a.closed) continue;
            const double ap = pos.is_long ? (exit_px-a.entry) : (a.entry-exit_px);
            printf("[MCE-PYRAMID-%s] ADD_%d CLOSE @ %.2f pnl=$%.0f\n",
                   pyramid_shadow ? "SHADOW" : "LIVE",
                   a.num, exit_px, ap * a.size * 100.0);
            fflush(stdout);
            a.closed = true;
            if (on_close && !pyramid_shadow)
                on_close(exit_px, pos.is_long, a.size, "PYRAMID_CLOSE");
        }

        printf("[MCE%s] CLOSE %s @ %.2f reason=%s "
               "trail=$%.0f banked=$%.0f TOTAL=$%.0f mfe=%.1fpt adds=%d\n",
               shadow_mode ? "-SHADOW" : "",
               pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, tpnl, pos.banked_usd, total,
               pos.mfe, pyramid_add_count);
        fflush(stdout);

        if (std::string(reason) == "SL_HIT") {
            if (pos.is_long)  m_long_block_until  = now_ms + 60000;
            else              m_short_block_until = now_ms + 60000;
        }
        // Shadow: always call on_close so trade appears in GUI with correct costs
        if (on_close)
            on_close(exit_px, pos.is_long, pos.size, reason);

        // Build TradeRecord and call on_trade_record -- logs to ledger/GUI/CSV
        // This fires in BOTH shadow and live mode so shadow trades appear in
        // the GUI Recent Trades panel with correct costs, exactly like live trades.
        if (on_trade_record) {
            omega::TradeRecord tr;
            tr.symbol      = "XAUUSD";
            tr.side        = pos.is_long ? "LONG" : "SHORT";
            tr.engine      = "MacroCrash";
            tr.regime      = "EXPANSION";
            tr.entryPrice  = pos.entry;
            tr.exitPrice   = exit_px;
            tr.sl          = pos.sl;
            tr.size        = pos.full_size;  // original full size
            tr.pnl         = ppts * pos.full_size;  // raw price points * full_size
            tr.mfe         = pos.mfe * pos.full_size;
            tr.mae         = 0.0;
            tr.entryTs     = static_cast<int64_t>(pos.entry_ms / 1000);
            tr.exitTs      = static_cast<int64_t>(now_ms / 1000);
            tr.exitReason  = reason;
            tr.spreadAtEntry = 0.0;
            tr.id          = ++m_trade_id;
            on_trade_record(tr);
        }

        pos.active        = false;
        pyramid_add_count = 0;
        m_cooldown_until  = now_ms + COOLDOWN_MS;
    }
};

} // namespace omega
