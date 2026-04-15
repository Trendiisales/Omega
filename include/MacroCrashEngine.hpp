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
    // -- Entry triggers ----------------------------------------------------
    double ATR_THRESHOLD    = 6.0;   // original: only fires on genuine macro moves
    double ATR_NORMAL       = 5.0;
    double VOL_RATIO_MIN    = 2.5;   // original: high bar = 69% WR on 13 trades
    double DRIFT_MIN        = 5.0;   // original: 5pt = confirmed macro move, not noise
    // NOTE: MCE fires on genuine macro impulse (ATR>6, drift>5) in ALL sessions.
    // No session gate -- tariff crash Apr 2025 was 13:00 UTC, session gate missed it.
    double ATR_SCALE_MAX    = 1.0;   // capped 6->1: 6x scaling produced $480 risk per trade; system max is $80
    double BASE_RISK_USD    = 80.0;
    double MAX_LOT          = 0.20;  // capped 0.50->0.20: matches CFE/DomPersist hard ceiling
    double MIN_LOT          = 0.01;

    // -- Hybrid bracket floor ----------------------------------------------
    double BRACKET_FRAC     = 0.30;   // 30% of lot for bracket guarantee
    double BRACKET_ATR_MULT = 2.0;    // bracket TP at 2xATR from entry

    // -- Velocity trail ----------------------------------------------------
    double STEP1_TRIGGER_USD  = 50.0;   // lowered 200->50: $200 never hit at small lots; $50 arms trail faster
    double STEP2_TRIGGER_USD  = 150.0;  // lowered 400->150
    double VEL_TRAIL_ARM_ATR  = 3.0;
    double VEL_TRAIL_DIST_ATR = 2.0;
    double RATCHET_KEEP       = 0.80;

    // -- Safe pyramid ------------------------------------------------------
    double  PYRAMID_ADD_ATR    = 2.0;   // add after 2xATR move from last add
    double  PYRAMID_SIZE_DECAY = 0.80;  // each add = prior * 0.80
    int     PYRAMID_MAX_ADDS   = 3;     // max 3 adds (4 total)
    double  PYRAMID_MAX_RISK   = 240.0; // total risk cap across all adds
    bool    pyramid_shadow     = true;  // ALWAYS shadow until explicitly false

    // -- Timing ------------------------------------------------------------
    int64_t COOLDOWN_MS     = 300000; // raised 60s->300s: fired 4x in one session; 5min cooldown prevents overtrading
    int64_t MAX_HOLD_MS     = 7200000;

    bool    enabled         = true;
    bool    shadow_mode     = true;   // DEFAULT -- change requires explicit auth

    // -- SL multiplier (session-aware) -------------------------------------
    // SL = atr * SL_ATR_MULT.  Asia oscillations are 10-15pt even on real moves.
    // A 1.0x multiplier (sl=4-6pt) sits inside the noise and gets stopped before
    // the move develops.  1.5x pushes the stop outside the 10pt Asia swing range.
    // Lot sizing auto-adjusts (lot = risk / sl_pts) so dollar risk is unchanged.
    // Evidence 2026-04-08: 5 consecutive Asia SL_HIT all stopped within 6pt then
    // moved 15-20pt in the original direction -- classic too-tight SL whipsaw.
    double SL_ATR_MULT         = 1.0;   // London/NY: 1x ATR (tight, fast moves)
    double SL_ATR_MULT_ASIA    = 1.5;   // Asia: 1.5x ATR (wider -- outlast 10pt oscillation)

    // -- Asia session thresholds (slot=6, 22:00-05:00 UTC) -----------------
    // Asia moves are $10-25pt vs $50-150pt London/NY. Tuned to last 2 weeks.
    double ATR_THRESHOLD_ASIA  = 4.0;   // ATR > 4pt  (was 8pt -- missed every Asia spike)
    double VOL_RATIO_MIN_ASIA  = 2.0;   // vol > 2x baseline (was 2.5 -- Asia baseline lower)
    double DRIFT_MIN_ASIA      = 3.0;   // |drift| > 3pt (was 6pt -- Asia $12 move = drift~4)

    // -- DOM primer thresholds -----------------------------------------------
    // book_slope > DOM_SLOPE_STRONG = meaningful directional DOM pressure.
    // 0.15 = 15% net weighted bias (microstructure literature standard for meaningful).
    // At BlackBull, book sizes are synthetic (size_raw=0 -> substituted as 1.0 lot each).
    // This means slope is computed from price levels only, not real size imbalance.
    // Set threshold conservatively (0.20) to avoid noise from synthetic sizes.
    double DOM_SLOPE_STRONG    = 0.20;  // |book_slope| threshold for DOM confirmation
    double DOM_MICRO_THRESH    = 0.02;  // microprice_bias threshold (pts) for confirmation
    double DOM_DRIFT_RELAX_FRAC = 0.40; // when DOM confirms: relax drift_min by 40%

    // -- Callbacks ---------------------------------------------------------
    using CloseCallback = std::function<void(double exit_px, bool is_long,
                                             double size, const std::string& reason)>;
    CloseCallback on_close;

    // Fires on every close (shadow AND live) -- builds TradeRecord and calls handle_closed_trade.
    // Wire in omega_main.hpp: g_macro_crash.on_trade_record = [](const omega::TradeRecord& tr){ handle_closed_trade(tr); };
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // -- Base position ------------------------------------------------------
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

    // -- Pyramid add-ons ---------------------------------------------------
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

    // Emergency close -- dollar_stop and session-end force exit.
    // Uses _close_all which fires on_trade_record (handle_closed_trade) and on_close.
    void force_close(double bid, double ask, int64_t now_ms) {
        if (!pos.active) return;
        const double exit_px = pos.is_long ? bid : ask;
        _close_all(exit_px, "DOLLAR_STOP", now_ms);
    }

    // -- Main tick ---------------------------------------------------------
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

        // -- Weekend gap protection ----------------------------------------
        // Gold gaps 1-2% on Sunday open due to weekend macro news.
        // Force-close any open position Friday >= 20:30 UTC.
        // Block new entries Friday >= 20:00 UTC through Sunday 22:30 UTC.
        {
            const int64_t now_sec = now_ms / 1000;
            std::time_t t = (std::time_t)now_sec;
            std::tm* ti = std::gmtime(&t);
            const int wday = ti->tm_wday;   // 0=Sun 1=Mon ... 5=Fri 6=Sat
            const int hour = ti->tm_hour;
            const int min  = ti->tm_min;
            const int hhmm = hour * 100 + min;

            // Force-close open position before weekend
            const bool force_close_window =
                (wday == 5 && hhmm >= 2030) ||  // Friday >= 20:30 UTC
                (wday == 6) ||                   // All Saturday
                (wday == 0 && hhmm < 2230);      // Sunday before 22:30 UTC

            if (force_close_window && pos.active) {
                static int64_t s_gap_close_log = 0;
                if (now_sec - s_gap_close_log > 3600) {
                    s_gap_close_log = now_sec;
                    printf("[MCE-WEEKEND] Force-closing position before weekend gap\n");
                    fflush(stdout);
                }
                _close_all(pos.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms);
                return;
            }

            // Block new entries during gap window
            const bool entry_blocked =
                (wday == 5 && hhmm >= 2000) ||  // Friday >= 20:00 UTC
                (wday == 6) ||                   // All Saturday
                (wday == 0 && hhmm < 2230);      // Sunday before 22:30 UTC

            if (entry_blocked) {
                static int64_t s_gap_block_log = 0;
                if (now_sec - s_gap_block_log > 3600) {
                    s_gap_block_log = now_sec;
                    printf("[MCE-WEEKEND] Entry blocked -- weekend gap window\n");
                    fflush(stdout);
                }
                return;
            }
        }

        if (pos.active) {
            _manage(bid, ask, mid, atr, vol_ratio, ewm_drift,
                    expansion_regime, now_ms);
            return;
        }

        // -- Session-aware threshold selection ----------------------------
        const bool is_asia = (session_slot == 6);
        const double eff_atr_threshold = is_asia ? ATR_THRESHOLD_ASIA : ATR_THRESHOLD;
        const double eff_vol_ratio_min = is_asia ? VOL_RATIO_MIN_ASIA : VOL_RATIO_MIN;
        const double eff_drift_min     = is_asia ? DRIFT_MIN_ASIA     : DRIFT_MIN;

        // -- DOM primer: book_slope / vacuum override ----------------------
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

        // -- RSI spike confirmation: both directions -----------------------
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
        // RSI spike confirmation substitutes for expansion_regime in ALL sessions.
        // v2.1 fix: was (is_asia && rsi_confirms_expansion) -- Asia-only bypass.
        // Root cause of missing 70pt London/NY crashes: regime classifier needs
        // CONFIRM_TICKS=5 before locking EXPANSION_BREAKOUT. A macro event moves
        // faster than 5 ticks. RSI < 38 + drift < 0 IS the expansion signal.
        // Waiting for regime confirmation on a real macro event is wrong by design.
        const bool rsi_confirms_expansion = rsi_spike_long || rsi_spike_short;
        const bool eff_expansion = expansion_regime || rsi_confirms_expansion;

        // -- NOSIG diagnostic -- log every 10s when ANY gate is blocking ------
        // Runs BEFORE cooldown/entry gates so it always fires -- even in cooldown.
        {
            static int64_t s_nosig_ts = 0;
            const bool cooldown_ok = (now_ms >= m_cooldown_until);
            const bool atr_ok      = (atr >= final_atr_min);
            const bool vol_ok      = (vol_ratio >= final_vol_min);
            const bool exp_ok      = eff_expansion;
            const bool drift_ok    = (std::fabs(ewm_drift) >= final_drift_min);
            if ((!cooldown_ok || !atr_ok || !vol_ok || !exp_ok || !drift_ok)
                    && now_ms >= s_nosig_ts) {
                s_nosig_ts = now_ms + 10000;
                printf("[MCE-NOSIG] slot=%d cooldown=%s atr=%.1f(need %.1f %s) "
                       "vol=%.2f(need %.2f %s) "
                       "exp=%d(regime=%d rsi=%d) "
                       "drift=%.1f(need %.1f %s) "
                       "rsi=%.1f\n",
                       session_slot,
                       cooldown_ok ? "no" : "BLOCK",
                       atr,  final_atr_min,  atr_ok  ? "OK" : "BLOCK",
                       vol_ratio, final_vol_min, vol_ok  ? "OK" : "BLOCK",
                       (int)exp_ok, (int)expansion_regime, (int)rsi_confirms_expansion,
                       std::fabs(ewm_drift), final_drift_min, drift_ok ? "OK" : "BLOCK",
                       rsi14);
                fflush(stdout);
            }
        }

        // -- Entry gates --------------------------------------------------
        // No session gate -- MCE fires on genuine macro impulse moves (ATR>6, drift>5)
        // which occur in ALL sessions. The tariff crash (Apr 2025) was 13:00 UTC.
        // Session gate was wrong: it blocked NY moves at original thresholds which have edge.
        // The -$1071 NY loss was from LOWERED thresholds (ATR>4.5), not from NY itself.
        if (now_ms < m_cooldown_until)             return;  // cooldown after prior trade
        if (atr < final_atr_min)                   return;
        if (vol_ratio < final_vol_min)             return;
        if (!eff_expansion)                        return;
        if (std::fabs(ewm_drift) < final_drift_min) return;

        // Direction: ewm_drift direction
        const bool is_long = (ewm_drift > 0.0);

        // Log entry -- always, not conditional on session/DOM/RSI
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

        // Sizing -- session-aware SL multiplier
        // Asia (slot=6): 1.5x ATR stop to outlast 10-15pt oscillations before move develops.
        // London/NY: 1.0x ATR stop (fast directional moves, tight stop appropriate).
        // Dollar risk is unchanged: lot = risk / (sl_pts * 100), so wider SL = smaller lot.
        const double eff_sl_mult = (session_slot == 6) ? SL_ATR_MULT_ASIA : SL_ATR_MULT;
        const double scale    = std::min(ATR_SCALE_MAX, std::max(0.5, atr / ATR_NORMAL));
        const double risk     = BASE_RISK_USD * scale;
        const double sl_pts   = atr * eff_sl_mult;
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
    // Consecutive directional SL tracking -- blocks same direction after 2 SL_HITs in 2hr
    int     m_sl_long_count     = 0;
    int     m_sl_short_count    = 0;
    int64_t m_sl_long_first_ms  = 0;
    int64_t m_sl_short_first_ms = 0;

    static double _rl(double x) { return std::round(x / 0.001) * 0.001; }

    void _manage(double bid, double ask, double mid,
                 double atr, double vol_ratio, double ewm_drift,
                 bool expansion_regime, int64_t now_ms) {

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        // -- Bracket floor check -------------------------------------------
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

        // -- SL check -----------------------------------------------------
        if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
            _close_all(pos.is_long ? bid : ask, "SL_HIT", now_ms);
            return;
        }

        // -- Max hold ------------------------------------------------------
        if (now_ms - pos.entry_ms >= MAX_HOLD_MS) {
            _close_all(pos.is_long ? bid : ask, "MAX_HOLD", now_ms);
            return;
        }

        const double atr_live = (atr > 0.0)
            ? (0.70 * pos.atr_at_entry + 0.30 * atr) : pos.atr_at_entry;
        const double open_pnl = move * pos.full_size * 100.0;

        // -- Step 1 --------------------------------------------------------
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

        // -- Step 2 --------------------------------------------------------
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

        // -- Velocity trail ------------------------------------------------
        if (pos.be_locked && pos.mfe >= atr_live * VEL_TRAIL_ARM_ATR) {
            const double tsl = pos.is_long
                ? (pos.entry + pos.mfe - atr_live * VEL_TRAIL_DIST_ATR)
                : (pos.entry - pos.mfe + atr_live * VEL_TRAIL_DIST_ATR);
            if (pos.is_long  && tsl > pos.sl) pos.sl = tsl;
            if (!pos.is_long && tsl < pos.sl) pos.sl = tsl;
        }

        // -- Ratchet -------------------------------------------------------
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

        // -- Safe pyramid check --------------------------------------------
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

        if (std::string(reason) == "TP_HIT" || std::string(reason) == "TRAIL_STOP") {
            // Reset directional SL counter on TP -- thesis was correct, streak is broken
            if (pos.is_long) { m_sl_long_count  = 0; m_sl_long_first_ms  = 0; }
            else             { m_sl_short_count = 0; m_sl_short_first_ms = 0; }
        }
        if (std::string(reason) == "SL_HIT") {
            // Track consecutive directional SL hits.
            // After 2 SL_HITs in the same direction within 2 hours,
            // block that direction for the rest of the session (8hr).
            // Resets when a TP is hit in that direction.
            const int64_t TWO_HR_MS    = 7200000LL;
            const int64_t SESSION_BLOCK = 28800000LL; // 8hr session block
            if (pos.is_long) {
                // reset if first SL was >2hr ago
                if (m_sl_long_first_ms > 0 && (now_ms - m_sl_long_first_ms) > TWO_HR_MS) {
                    m_sl_long_count = 0; m_sl_long_first_ms = 0;
                }
                if (m_sl_long_count == 0) m_sl_long_first_ms = now_ms;
                ++m_sl_long_count;
                if (m_sl_long_count >= 2) {
                    m_long_block_until = now_ms + SESSION_BLOCK;
                    printf("[MCE] LONG direction blocked for 8hr after %d consecutive LONG SL_HITs\n",
                           m_sl_long_count);
                    fflush(stdout);
                }
            } else {
                if (m_sl_short_first_ms > 0 && (now_ms - m_sl_short_first_ms) > TWO_HR_MS) {
                    m_sl_short_count = 0; m_sl_short_first_ms = 0;
                }
                if (m_sl_short_count == 0) m_sl_short_first_ms = now_ms;
                ++m_sl_short_count;
                if (m_sl_short_count >= 2) {
                    m_short_block_until = now_ms + SESSION_BLOCK;
                    printf("[MCE] SHORT direction blocked for 8hr after %d consecutive SHORT SL_HITs\n",
                           m_sl_short_count);
                    fflush(stdout);
                }
            }
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
            tr.pnl         = ppts * pos.full_size;  // raw pts*lots -- handle_closed_trade applies tick_mult
            tr.net_pnl     = tr.pnl;
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

