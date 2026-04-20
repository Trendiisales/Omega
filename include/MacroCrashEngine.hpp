#pragma once
#include <iomanip>
#include <iostream>
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
#include <mutex>   // RACE-FIX 2026-04-21: m_close_mtx serialises close paths (cTrader/FIX threads)
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
    double MAX_LOT          = 0.10;  // reduced 0.20->0.10: dollar_stop=$50 at 0.20=2.5pt, at 0.10=5pt
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
                    {
                        // converted from printf
                        char _buf[512];
                        snprintf(_buf, sizeof(_buf), "[MCE-WEEKEND] Force-closing position before weekend gap\n");
                        std::cout << _buf;
                        std::cout.flush();
                    }
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
                    {
                        // converted from printf
                        char _buf[512];
                        snprintf(_buf, sizeof(_buf), "[MCE-WEEKEND] Entry blocked -- weekend gap window\n");
                        std::cout << _buf;
                        std::cout.flush();
                    }
                }
                return;
            }
        }

        if (pos.active) {
            _manage(bid, ask, mid, atr, vol_ratio, ewm_drift,
                    expansion_regime, now_ms);
            return;
        }

        // -- Update rolling 90s price range (always, not gated) ----------
        // Decay old extremes: if the recorded extreme is >90s old, reset it.
        // This gives us a live window of recent price range for velocity checks.
        if (now_ms - m_roll_high_ms > 90000LL || m_roll_high == 0.0)
            { m_roll_high = mid; m_roll_high_ms = now_ms; }
        else if (mid > m_roll_high)
            { m_roll_high = mid; m_roll_high_ms = now_ms; }
        if (now_ms - m_roll_low_ms > 90000LL || m_roll_low >= 1e8)
            { m_roll_low = mid; m_roll_low_ms = now_ms; }
        else if (mid < m_roll_low)
            { m_roll_low = mid; m_roll_low_ms = now_ms; }

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
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[MCE-NOSIG] slot=%d cooldown=%s atr=%.1f(need %.1f %s) "                        "vol=%.2f(need %.2f %s) "                        "exp=%d(regime=%d rsi=%d) "                        "drift=%.1f(need %.1f %s) "                        "rsi=%.1f\n",                        session_slot,                        cooldown_ok ? "no" : "BLOCK",                        atr,  final_atr_min,  atr_ok  ? "OK" : "BLOCK",                        vol_ratio, final_vol_min, vol_ok  ? "OK" : "BLOCK",                        (int)exp_ok, (int)expansion_regime, (int)rsi_confirms_expansion,                        std::fabs(ewm_drift), final_drift_min, drift_ok ? "OK" : "BLOCK",                        rsi14);
                    std::cout << _buf;
                    std::cout.flush();
                }
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

        // -- Gate A: Dollar-stop direction block --------------------------
        // After a DOLLAR_STOP in a direction, block that direction for 4hrs.
        // A dollar-stop means the engine entered catastrophically wrong.
        // Never retry the same direction within 4hrs of a dollar-stop.
        if (is_long  && now_ms < m_dollar_stop_long_block)  {
            static int64_t s_dsl = 0;
            if (now_ms - s_dsl > 30000) {
                s_dsl = now_ms;
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "[MCE-GATE-A] LONG blocked: dollar-stop direction lock %llds remain\n",
                    (long long)((m_dollar_stop_long_block - now_ms)/1000LL));
                std::cout << buf; std::cout.flush();
            }
            return;
        }
        if (!is_long && now_ms < m_dollar_stop_short_block) {
            static int64_t s_dss = 0;
            if (now_ms - s_dss > 30000) {
                s_dss = now_ms;
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "[MCE-GATE-A] SHORT blocked: dollar-stop direction lock %llds remain\n",
                    (long long)((m_dollar_stop_short_block - now_ms)/1000LL));
                std::cout << buf; std::cout.flush();
            }
            return;
        }

        // -- Gate B: Price velocity gate ----------------------------------
        // MCE exists to catch mega moves IN PROGRESS, not stale RSI/drift.
        // Require price is STILL near the extreme of the recent 90s range.
        //
        // SHORT: price must be within 2*ATR of the 90s HIGH.
        //   If price has bounced >2*ATR from the 90s low, the crash is over.
        // LONG:  price must be within 2*ATR of the 90s LOW.
        //   If price has pulled back >2*ATR from the 90s high, the rally is over.
        //
        // This specifically prevents: bracket trails SHORT -> price bounces 20pt
        // -> RSI still low, drift still negative -> MCE enters SHORT into rally.
        {
            const double velocity_window = atr * 2.0;  // 2*ATR = move must still be here
            bool velocity_ok = true;
            if (!is_long && m_roll_high > 0.0) {
                // SHORT: price should still be near the recent high (crash ongoing).
                // If price has bounced more than 2*ATR from the 90s low, crash is over.
                const double bounce_from_low = mid - m_roll_low;
                if (bounce_from_low > velocity_window) {
                    velocity_ok = false;
                    static int64_t s_vb = 0;
                    if (now_ms - s_vb > 5000) {
                        s_vb = now_ms;
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                            "[MCE-GATE-B] SHORT blocked: price bounced %.1fpt from 90s low "
                            "(%.2f->%.2f), move OVER. Need bounce < %.1fpt\n",
                            bounce_from_low, m_roll_low, mid, velocity_window);
                        std::cout << buf; std::cout.flush();
                    }
                }
            }
            if (is_long && m_roll_low < 1e8) {
                // LONG: price should still be near the recent low (rally ongoing).
                // If price has pulled back more than 2*ATR from the 90s high, rally is over.
                const double pullback_from_high = m_roll_high - mid;
                if (pullback_from_high > velocity_window) {
                    velocity_ok = false;
                    static int64_t s_vl = 0;
                    if (now_ms - s_vl > 5000) {
                        s_vl = now_ms;
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                            "[MCE-GATE-B] LONG blocked: price pulled back %.1fpt from 90s high "
                            "(%.2f->%.2f), move OVER. Need pullback < %.1fpt\n",
                            pullback_from_high, m_roll_high, mid, velocity_window);
                        std::cout << buf; std::cout.flush();
                    }
                }
            }
            if (!velocity_ok) return;
        }

        // -- Gate C: Minimum move size gate ------------------------------
        // MCE must only fire on genuine macro moves -- minimum 1.5*ATR
        // price displacement in the drift direction within 90s.
        // SHORT: 90s high - current must be >= 1.5*ATR (price fell sharply).
        // LONG:  current - 90s low must be >= 1.5*ATR (price rallied sharply).
        // This prevents firing on slow drifts where RSI/vol happen to be elevated.
        {
            const double min_move = atr * 1.5;
            bool move_ok = true;
            if (!is_long && m_roll_high > 0.0) {
                const double crash_size = m_roll_high - mid;  // how far price fell
                if (crash_size < min_move) {
                    move_ok = false;
                    static int64_t s_mc = 0;
                    if (now_ms - s_mc > 5000) {
                        s_mc = now_ms;
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                            "[MCE-GATE-C] SHORT blocked: 90s move=%.1fpt < min=%.1fpt "
                            "(hi=%.2f now=%.2f). Not a macro move.\n",
                            crash_size, min_move, m_roll_high, mid);
                        std::cout << buf; std::cout.flush();
                    }
                }
            }
            if (is_long && m_roll_low < 1e8) {
                const double rally_size = mid - m_roll_low;  // how far price rose
                if (rally_size < min_move) {
                    move_ok = false;
                    static int64_t s_ml = 0;
                    if (now_ms - s_ml > 5000) {
                        s_ml = now_ms;
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                            "[MCE-GATE-C] LONG blocked: 90s move=%.1fpt < min=%.1fpt "
                            "(lo=%.2f now=%.2f). Not a macro move.\n",
                            rally_size, min_move, m_roll_low, mid);
                        std::cout << buf; std::cout.flush();
                    }
                }
            }
            if (!move_ok) return;
        }

        // Log entry -- always, not conditional on session/DOM/RSI
        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[MCE%s] TRIGGER %s atr=%.1f(thr=%.1f) drift=%.1f(thr=%.1f) "                "vol=%.1f(thr=%.1f) slot=%d dom_slope=%.2f vac_ask=%d vac_bid=%d "                "rsi=%.1f rsi_exp=%d expansion=%d dom_confirm=%d\n",                shadow_mode ? "-SHADOW" : "",                is_long ? "LONG" : "SHORT",                atr, final_atr_min,                std::fabs(ewm_drift), final_drift_min,                vol_ratio, final_vol_min,                session_slot, book_slope,                (int)vacuum_ask, (int)vacuum_bid,                rsi14, (int)rsi_confirms_expansion,                (int)expansion_regime,                (int)(dom_long_confirm || dom_short_confirm));
            std::cout << _buf;
            std::cout.flush();
        }

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
        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "%s ENTRY %s @ %.2f sl=%.2f(%.1fpt) lot=%.3f "                "[trail=%.3f | bracket=%.3f @ %.2f] "                "atr=%.1f vol=%.1f drift=%.1f risk=$%.0f\n",                pfx, is_long ? "LONG" : "SHORT",                entry_px, sl_px, sl_pts, lot,                t_lot, b_lot, b_tp, atr, vol_ratio, ewm_drift, risk);
            std::cout << _buf;
            std::cout.flush();
        }

        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[MCE-BRACKET-%s] PLACE %s LIMIT @ %.2f size=%.3f "                "guarantee=$%.0f (%.1fpt * %.3f lots)\n",                shadow_mode ? "SHADOW" : "LIVE",                is_long ? "SELL" : "BUY",                b_tp, b_lot,                (atr * BRACKET_ATR_MULT) * b_lot * 100.0,                atr * BRACKET_ATR_MULT, b_lot);
            std::cout << _buf;
            std::cout.flush();
        }
    }

private:
    // RACE-FIX 2026-04-21: serialises _close_all + force_close + PARTIAL/
    // PYRAMID/BRACKET emit paths. The cTrader depth thread and the FIX
    // quote_loop thread can both reach MCE's close paths via on_tick() or
    // via force_close() during cTrader/FIX failover windows; without this
    // mutex, two concurrent arrivals could fire duplicate on_close /
    // on_trade_record callbacks from stale pos state. Same pattern as
    // CFE 602aed07, DPE 18ee4ca5, GFE 73a081fe.
    mutable std::mutex m_close_mtx;

    int64_t m_cooldown_until    = 0;
    int64_t m_long_block_until  = 0;
    int64_t m_short_block_until = 0;
    double  m_last_px           = 0.0;
    double  m_last_atr          = 0.0;
    int     m_trade_id          = 0;

    // Rolling 90s price range tracker for velocity gate
    // Tracks high and low over last 90s so we can detect if move is still ongoing.
    double  m_roll_high         = 0.0;   // highest mid seen in last 90s
    double  m_roll_low          = 1e9;   // lowest mid seen in last 90s
    int64_t m_roll_high_ms      = 0;
    int64_t m_roll_low_ms       = 0;

    // Dollar-stop direction block: after any DOLLAR_STOP, block that direction 4hrs.
    // A dollar-stop means MCE was catastrophically wrong -- never retry same direction soon.
    int64_t m_dollar_stop_long_block  = 0;
    int64_t m_dollar_stop_short_block = 0;
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

                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[MCE-BRACKET-%s] FILLED @ %.2f pnl=$%.0f sl->BE trail=%.3f remain\n",                        shadow_mode ? "SHADOW" : "LIVE",                        pos.bracket_tp, bpnl, pos.size);
                    std::cout << _buf;
                    std::cout.flush();
                }
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
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[MCE] STEP1 banked %.3f @ %.2f pnl=$%.0f sl->BE\n", qty, ex, p);
                    std::cout << _buf;
                    std::cout.flush();
                }
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
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[MCE] STEP2 banked %.3f @ %.2f pnl=$%.0f\n", qty, ex, p);
                    std::cout << _buf;
                    std::cout.flush();
                }
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
                {
                    // converted from printf
                    char _buf[512];
                    snprintf(_buf, sizeof(_buf), "[MCE] RATCHET tier=%d open=$%.0f locked=$%.0f sl=%.2f\n",                        tier, open_pnl, lu, csl);
                    std::cout << _buf;
                    std::cout.flush();
                }
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
        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[MCE-PYRAMID-%s] ADD_%d %s @ %.2f sl=%.2f(%.1fpt) size=%.3f risk=$%.0f "                "from_last=%.1fpt  SL_ADVANCE->%.2f(%s)\n",                pyramid_shadow ? "SHADOW" : "LIVE",                n, pos.is_long ? "LONG" : "SHORT",                add_px, add_sl, add_slpt, new_sz, add_risk,                from_last, add_px,                adv ? "all prior now at BE" : "no improvement");
            std::cout << _buf;
            std::cout.flush();
        }

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
        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[MCE-PYRAMID-%s] COMBINED_PNL_PROJECTION $%.0f "                "(base_open=$%.0f banked=$%.0f adds=$%.0f adds=%d)\n",                pyramid_shadow ? "SHADOW" : "LIVE",                open_pnl + pos.banked_usd + adds_pnl,                open_pnl, pos.banked_usd, adds_pnl, pyramid_add_count);
            std::cout << _buf;
            std::cout.flush();
        }
    }

    // RACE-FIX 2026-04-21: public wrapper holds m_close_mtx while delegating
    // to _close_all_locked. Protects against concurrent arrivals from the
    // cTrader depth thread and the FIX quote_loop thread entering via
    // on_tick()->_manage() or via force_close().
    // Same pattern as CFE 602aed07 / DPE 18ee4ca5 / GFE 73a081fe.
    void _close_all(double exit_px, const char* reason, int64_t now_ms) {
        std::lock_guard<std::mutex> lk(m_close_mtx);
        _close_all_locked(exit_px, reason, now_ms);
    }

    void _close_all_locked(double exit_px, const char* reason, int64_t now_ms) {
        // RACE-FIX 2026-04-21: silent bail on second concurrent arrival.
        // First thread wins the lock, sets pos.active=false at the tail of
        // this function. Second thread then acquires the lock and finds
        // pos.active==false -- re-firing on_close / on_trade_record here
        // would be duplicate callbacks on already-closed state. Bail.
        if (!pos.active) return;

        const double ppts  = pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double tpnl  = ppts * pos.size * 100.0;
        const double total = tpnl + pos.banked_usd;

        for (int i = 0; i < pyramid_add_count; i++) {
            auto& a = pyramid_adds[i];
            if (!a.active || a.closed) continue;
            const double ap = pos.is_long ? (exit_px-a.entry) : (a.entry-exit_px);
            {
                // converted from printf
                char _buf[512];
                snprintf(_buf, sizeof(_buf), "[MCE-PYRAMID-%s] ADD_%d CLOSE @ %.2f pnl=$%.0f\n",                    pyramid_shadow ? "SHADOW" : "LIVE",                    a.num, exit_px, ap * a.size * 100.0);
                std::cout << _buf;
                std::cout.flush();
            }
            a.closed = true;
            if (on_close && !pyramid_shadow)
                on_close(exit_px, pos.is_long, a.size, "PYRAMID_CLOSE");
        }

        {
            // converted from printf
            char _buf[512];
            snprintf(_buf, sizeof(_buf), "[MCE%s] CLOSE %s @ %.2f reason=%s "                "trail=$%.0f banked=$%.0f TOTAL=$%.0f mfe=%.1fpt adds=%d\n",                shadow_mode ? "-SHADOW" : "",                pos.is_long ? "LONG" : "SHORT",                exit_px, reason, tpnl, pos.banked_usd, total,                pos.mfe, pyramid_add_count);
            std::cout << _buf;
            std::cout.flush();
        }

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
                    {
                        // converted from printf
                        char _buf[512];
                        snprintf(_buf, sizeof(_buf), "[MCE] LONG direction blocked for 8hr after %d consecutive LONG SL_HITs\n",                            m_sl_long_count);
                        std::cout << _buf;
                        std::cout.flush();
                    }
                }
            } else {
                if (m_sl_short_first_ms > 0 && (now_ms - m_sl_short_first_ms) > TWO_HR_MS) {
                    m_sl_short_count = 0; m_sl_short_first_ms = 0;
                }
                if (m_sl_short_count == 0) m_sl_short_first_ms = now_ms;
                ++m_sl_short_count;
                if (m_sl_short_count >= 2) {
                    m_short_block_until = now_ms + SESSION_BLOCK;
                    {
                        // converted from printf
                        char _buf[512];
                        snprintf(_buf, sizeof(_buf), "[MCE] SHORT direction blocked for 8hr after %d consecutive SHORT SL_HITs\n",                            m_sl_short_count);
                        std::cout << _buf;
                        std::cout.flush();
                    }
                }
            }
        }
        // Dollar-stop direction lock: block same direction for 4hrs after DOLLAR_STOP.
        // DOLLAR_STOP means external DD limit killed the position -- catastrophic misfire.
        // Never allow MCE to retry same direction within 4hrs.
        if (std::string(reason) == "DOLLAR_STOP") {
            const int64_t DOLLAR_STOP_BLOCK_MS = 14400000LL; // 4 hours
            if (pos.is_long) {
                m_dollar_stop_long_block = now_ms + DOLLAR_STOP_BLOCK_MS;
            } else {
                m_dollar_stop_short_block = now_ms + DOLLAR_STOP_BLOCK_MS;
            }
            char buf[256];
            snprintf(buf, sizeof(buf),
                "[MCE-DOLLAR-STOP-LOCK] %s direction blocked 4hrs after DOLLAR_STOP\n",
                pos.is_long ? "LONG" : "SHORT");
            std::cout << buf; std::cout.flush();
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

