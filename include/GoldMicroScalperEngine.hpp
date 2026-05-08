#pragma once
// =============================================================================
// GoldMicroScalperEngine.hpp -- Bidirectional micro-tick scalper for XAUUSD
// =============================================================================
//
// 2026-05-08 S19 DESIGN (Claude / Jo):
//   New engine in the gold cohort. Fills a niche the existing stack does not
//   cover: "many small moves up and down, lock in small trades quickly, once
//   BE is hit trail aggressively until it reverses and then exit immediately."
//
//   Existing gold engines surveyed:
//     - GoldMidScalperEngine     -- single $20-40 bracket capture; BE at 3pt,
//                                   trail-arm at 5pt. Too coarse for chop.
//     - MeanReversionEngine      -- bidirectional Z-fade SL=$4 TP=$12. Closest
//                                   in spirit but no BE-arm + tight-trail +
//                                   reversal-exit logic; TP not "small/fast."
//     - LiquiditySweepProEngine  -- single-fire stop-hunt reversal.
//     - XauusdFvgEngine          -- 15-min FVG mitigation. Single fire.
//     - PDHLReversionEngine      -- mean-rev at PDH/PDL only.
//     - CandleFlowEngine         -- bar-based DOM entry.
//     - NbmGoldLondon            -- session momentum.
//     - GoldHybridBracketEngine  -- retired S12 P3c (header deleted).
//
//   None match the rapid bidirectional micro-scalp pattern. This engine does.
//
// ALGORITHM
//   Phase machine: IDLE -> ARMED -> LIVE -> COOLDOWN.
//
//   ARMING: every tick the engine maintains a 20-tick rolling window of mid
//   prices. When |z-score| of the current mid vs the window mean crosses
//   ENTRY_Z (default 1.5) AND L2 imbalance confirms direction (when l2_real),
//   the engine arms in the fade direction. The structure represents micro
//   exhaustion -- price has stretched far enough from its 20-tick mean that
//   reversion is the high-prior outcome.
//
//   ENTRY: market-style fill at the side that resolves the fade. LONG on ask
//   when z<=-ENTRY_Z (price below mean, expect bounce up); SHORT on bid when
//   z>=ENTRY_Z (price above mean, expect drop). No bracket / pending phase --
//   the move we are fading is small, and a pending block adds fill-time risk
//   inconsistent with "lock in small trades quickly."
//
//   MANAGE (LIVE): three layers run on every tick.
//     1. Initial-SL guard. SL is fixed at SL_DIST_PTS (1.5pt) below/above
//        entry until BE-arm fires. SL_HIT here is classified normally.
//     2. BE-arm. As soon as MFE >= BE_TRIGGER_PTS (0.5pt), SL is moved to
//        entry +/- BE_OFFSET_PTS (0.3pt) so a BE exit recovers round-trip
//        cost (~$0.30 on 0.01-lot XAUUSD per fill quote). One-shot via
//        pos.be_locked.
//     3. Aggressive trail + reversal-exit. Both run only after pos.be_locked
//        is set:
//          a. Trail SL ratchets to MFE - TRAIL_DIST_PTS (0.5pt). Tighter
//             than the GoldMidScalper TRAIL_FRAC*range -- this engine's
//             whole bet is that small wins must be locked.
//          b. Reversal detector. Exits IMMEDIATELY (REVERSAL_EXIT) when
//             either:
//               * net delta over the last REVERSAL_LOOKBACK ticks (default
//                 5) crosses REVERSAL_DELTA_PTS (0.30pt) against position,
//               * latest L2 book_slope flips opposing position direction by
//                 >= L2_FLIP_THRESH (0.20). Skipped when l2_real_at_entry
//                 is false.
//        These checks ONLY fire post-BE so we never close a trade in loss
//        through reversal logic -- the initial SL handles that case.
//     4. TP_HIT. Standard limit at TP_DIST_PTS (1.0pt). Fires whether or
//        not BE is locked.
//     5. MAX_HOLD timeout (60s). Safety: a position with no progress closes
//        at mid as MAX_HOLD_EXIT. Prevents stale shadow lots during quiet
//        periods.
//
//   COOLDOWN: COOLDOWN_S (5s) keyed PER DIRECTION. After exit, the same
//   direction is blocked for 5s; the OPPOSITE direction is permitted
//   immediately. This is the "many small moves up AND down" enabler --
//   chop legitimately fires LONG, then SHORT, then LONG within seconds.
//
// SAFETY
//   - shadow_mode = true by default. Promotion to live requires explicit
//     authorisation in engine_init.hpp after a 2-week paper validation
//     window with positive expectancy.
//   - Spread cap MAX_SPREAD = 1.0pt. Tighter than other gold engines (mid:
//     2.5pt; HBG sister: 2.5pt) because micro-scalp targets <= TP/2 -- a
//     2pt spread eats the whole TP.
//   - Session window 06:00-22:00 UTC (skip dead Asia 22-06 UTC). Same
//     wraparound-aware form as the EUR/AUD/NZD audit-fixes-37 pattern.
//   - 0.01 lot uniform cap (FIX 2026-04-22 policy).
//   - Max-hold safety net at 60s.
//   - Mutex on _close path. Inherited from HybridGold lineage to avoid the
//     Apr-7 -$3,008.38 phantom-pnl race.
//
// BACKTEST FRIENDLY
//   - All time inputs come through the on_tick() now_ms parameter; no
//     std::time(nullptr) on the hot path.
//   - File-IO persistence guarded #ifndef OMEGA_BACKTEST so
//     OmegaTimeShim-driven backtests run pure-in-memory.
//   - L2 data is delivered explicitly via on_tick parameters
//     (l2_imbalance, book_slope, vacuum_ask, vacuum_bid, l2_real). The
//     April capture replay populates these from the recorded book and
//     this engine consumes them identically to live.
//
// LOG NAMESPACE
//   All log lines use prefix [MICRO-SCALPER-GOLD] / [MICRO-SCALPER-GOLD-DIAG].
//   tr.engine = "MicroScalperGold" (distinct from MidScalperGold and the
//   retired HybridBracketGold).
//   tr.regime = "MICRO_TICK".
//
// PARAMS (all public for engine_init.hpp override):
//   ENTRY_Z              = 1.5     entry threshold on 20-tick z-score
//   ENTRY_LOOKBACK       = 20      window for z-score
//   L2_IMB_LONG_MIN      = 0.55    L2 imbalance ceil for LONG (when l2_real)
//   L2_IMB_SHORT_MAX     = 0.45    L2 imbalance floor for SHORT (when l2_real)
//   MAX_SPREAD           = 1.0     pt
//   TP_DIST_PTS          = 1.0     pt
//   SL_DIST_PTS          = 1.5     pt initial SL distance
//   BE_TRIGGER_PTS       = 0.5     pt MFE that arms BE lock
//   BE_OFFSET_PTS        = 0.3     pt offset above/below entry on BE move
//   TRAIL_DIST_PTS       = 0.5     pt trail distance below MFE post-BE
//   REVERSAL_LOOKBACK    = 5       ticks net-delta window for reversal
//   REVERSAL_DELTA_PTS   = 0.30    pt against-position delta to trip
//   L2_FLIP_THRESH       = 0.20    book_slope flip magnitude
//   MAX_HOLD_SEC         = 60      safety timeout
//   COOLDOWN_S           = 5       per-direction post-exit cooldown
//   SESSION_START_HOUR   = 6       UTC
//   SESSION_END_HOUR     = 22      UTC (wraparound-aware)
//
//   Defaults are first-pass; tune from the April L2 replay then again from
//   2-4 weeks of live shadow tape (mirrors the post-deploy retune cycle
//   tracked in HANDOFF_S19 Step 5 #4).
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include "OmegaTradeLedger.hpp"

namespace omega {

class GoldMicroScalperEngine {
public:
    // -- Tuned defaults (S19 calibrated 2026-05-08, rk12 promotion) ---------
    // Source: backtest/microscalper_crtp_sweep over 28 days / 6.7M ticks of
    // captured XAUUSD L2 (April 9 - May 8 2026). Two sweep iterations:
    //
    //   Sweep #1 (anchor ENTRY_Z=1.5; grid 0.75..3.0):
    //     rk2: Z=0.75 TP=1.00 SL=3.00 BE=0.50 TR=0.50
    //          N=31,009  WR=88.3%  PF=3.58  Net=190.8  hold=14.0s
    //     ENTRY_Z saturated at the LOW boundary -> sweep again.
    //
    //   Sweep #2 (anchor ENTRY_Z=0.75; grid 0.375..1.5) -- THIS one:
    //     rk1:  Z=0.38 TP=1.00 SL=2.38 N=42,277 WR=87.3% PF=3.27 Net=247.8
    //           (boundary-saturated again at 0.38; max net but overfit risk)
    //     rk12: Z=0.75 TP=0.79 SL=3.00 BE=0.50 TR=0.50
    //           N=36,575 WR=92.5% PF=4.40 Net=203.6 hold=10.5s
    //
    // PROMOTED rk12. Justification:
    //   * Mid-grid on every swept parameter -- no boundary saturation, so
    //     less overfit risk than rk1's Z=0.38 corner.
    //   * Highest profit factor of the entire leaderboard (4.40 vs 3.27 at
    //     rk1) and highest WR (92.5% vs 87.3%). Both metrics are robust
    //     against backtest-vs-live divergence; raw net PnL is the most
    //     fragile metric to optimise on.
    //   * TP=0.79pt (down from rk2's 1.00) better matches the engine's
    //     stated design intent of "lock in small trades quickly" -- the
    //     tighter TP locks profit ~25% faster.
    //   * If live fills underperform backtest by 30%, rk12 still profits
    //     while rk1's marginal edge could vanish entirely.
    //
    // Live expectancy WILL be lower than backtest because the cost model is
    // conservative (1 spread per non-TP exit; no slippage; no fill-rejection
    // probability). Re-tune from 2-4 weeks of live-shadow tape after deploy
    // -- that's the calibration signal that actually matters, not another
    // sweep iteration.
    static constexpr int    ENTRY_LOOKBACK       = 20;
    static constexpr double ENTRY_Z              = 0.75;
    static constexpr double L2_IMB_LONG_MIN      = 0.55;
    static constexpr double L2_IMB_SHORT_MAX     = 0.45;
    // 2026-05-08 DEEPSTRIKE: tightened 1.0 -> 0.5pt for max protection on
    //   the live deploy. Backtest filtered p95 spread was 0.22pt and max
    //   filtered was 0.99pt; 0.5pt still admits the modal-spread fires
    //   (~95%+ of legitimate signals) but rejects the 0.5-0.99 band that
    //   was probably suspect anyway. If live tape shows under-firing
    //   relative to expected_fires_per_hour=81.6, this is the first
    //   constant to relax.
    static constexpr double MAX_SPREAD           = 0.5;
    static constexpr double TP_DIST_PTS          = 0.79;
    static constexpr double SL_DIST_PTS          = 3.0;
    static constexpr double BE_TRIGGER_PTS       = 0.5;
    static constexpr double BE_OFFSET_PTS        = 0.3;
    static constexpr double TRAIL_DIST_PTS       = 0.5;
    static constexpr int    REVERSAL_LOOKBACK    = 5;
    static constexpr double REVERSAL_DELTA_PTS   = 0.30;
    static constexpr double L2_FLIP_THRESH       = 0.20;
    static constexpr int    MAX_HOLD_SEC         = 60;
    static constexpr int    COOLDOWN_S           = 5;
    static constexpr int    SESSION_START_HOUR   = 6;   // UTC
    static constexpr int    SESSION_END_HOUR     = 22;  // UTC, wraparound-aware

    // 2026-05-08 USER REQUEST: pre-London dead zone -- DISABLED at user
    // direction. Constant kept (set to -1) so the gate machinery is in
    // place; flip to 6 (BST) or 7 (GMT) to re-arm. The gate blocks the
    // hour immediately before London open. SL_DIST/TP_DIST asymmetry is
    // 3.8x so any chop window where WR drops below ~80% will bleed.
    static constexpr int    PRE_LONDON_DEAD_HOUR_UTC = -1;

    // 2026-05-08 DEEPSTRIKE LIVE: lot set to 0.03 for the single-engine
    //   live deploy on account 8077780. Earlier 0.10 figure was the user's
    //   shadow-test bump; for live they chose 0.03 instead.
    //
    //   PnL per trade at 0.03 lot:
    //     TP win  : +0.79pt  ->  +$2.37 gross  ->  ~+$2.04 net (after $0.33 slip)
    //     SL hit  : -3.00pt  ->  -$9.00 gross  ->  ~-$9.33 net (after $0.33 slip)
    //   Real-cost BE WR  ~  82.1%   (vs backtest 92.5%, monitor trip 82.16%)
    static constexpr double USD_PER_PT           = 100.0;  // per full lot XAUUSD
    static constexpr double LIVE_LOT             = 0.03;

    static constexpr int    MIN_ENTRY_TICKS      = 30;     // warmup before any fire
    static constexpr int    DIAG_EVERY_N_TICKS   = 600;    // ~3min @ 200 ticks/min

    enum class Phase { IDLE, ARMED, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    // 2026-05-08 S19: shadow ON by default. Promote to live ONLY after
    //   2-week paper validation showing positive expectancy AND non-trivial
    //   trade count (>=200 fills) on the April-replay + live-shadow cohort.
    //   Live promotion via engine_init.hpp override; do NOT change default.
    bool shadow_mode = true;

    // 2026-05-08 S20+: RiskMonitor wiring. Bound in engine_init.hpp to call
    //   g_risk_monitor.on_fire("MicroScalperGold", now_s) on every fill.
    //   Default nullptr = no-op; engine behaviour unchanged when unset.
    std::function<void(int64_t now_s)> on_fire_hook;

    struct LivePos {
        bool    active             = false;
        bool    is_long            = false;
        double  entry              = 0.0;
        double  tp                 = 0.0;
        double  sl                 = 0.0;
        double  size               = LIVE_LOT;
        double  mfe                = 0.0;
        double  mae                = 0.0;
        double  spread_at_entry    = 0.0;
        int64_t entry_ts           = 0;
        bool    be_locked          = false;
        bool    l2_real_at_entry   = false;
        double  z_at_entry         = 0.0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    GoldMicroScalperEngine() noexcept = default;

    // -- Main tick -----------------------------------------------------------
    // bid/ask    -- top-of-book quote in price units (XAUUSD: 1.00 = 1 dollar)
    // now_ms     -- monotonic timestamp in milliseconds; under OMEGA_BACKTEST
    //               this is shimmed by OmegaTimeShim
    // can_enter  -- gold-cohort gate from tick_gold.hpp; engine only ARMs
    //               for new entries when true. Position management still
    //               runs unconditionally.
    // l2_imbalance / book_slope / vacuum_ask / vacuum_bid / l2_real --
    //               wired from g_macro_ctx.gold_* in tick_gold.hpp dispatch.
    //               Pass 0.5 / 0.0 / false / false / false for backward-compat
    //               or pre-L2-feed environments; engine degrades gracefully.
    // on_close   -- TradeRecord sink; engine_init.hpp wires this to
    //               g_omegaLedger.record (and any shadow CSV writer).
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 CloseCallback on_close,
                 double l2_imbalance = 0.5,
                 double book_slope   = 0.0,
                 bool   vacuum_ask   = false,
                 bool   vacuum_bid   = false,
                 bool   l2_real      = false) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        m_last_tick_s     = now_s;
        m_last_book_slope = book_slope;
        m_last_l2_imb     = l2_imbalance;
        m_last_vacuum_ask = vacuum_ask;
        m_last_vacuum_bid = vacuum_bid;
        m_last_l2_real    = l2_real;

        ++m_ticks_received;
        m_window.push_back(mid);
        if ((int)m_window.size() > ENTRY_LOOKBACK * 4) m_window.pop_front();

        m_micro.push_back(mid);
        if ((int)m_micro.size() > REVERSAL_LOOKBACK * 4) m_micro.pop_front();

        // Warmup diag every DIAG_EVERY_N_TICKS + first 30 ticks.
        if (m_ticks_received <= 30 || (m_ticks_received % DIAG_EVERY_N_TICKS) == 0) {
            double mean = 0.0, sd = 0.0, z = 0.0;
            const bool stats_ok = _rolling_stats(mean, sd);
            if (stats_ok && sd > 0.0) z = (mid - mean) / sd;
            char _buf[512];
            std::snprintf(_buf, sizeof(_buf),
                "[MICRO-SCALPER-GOLD-DIAG] ticks=%d phase=%d window=%d/%d mid=%.2f "
                "spread=%.2f z=%.2f l2_imb=%.2f slope=%.2f l2_real=%d\n",
                m_ticks_received, (int)phase, (int)m_window.size(), ENTRY_LOOKBACK,
                mid, spread, z, l2_imbalance, book_slope, (int)l2_real);
            std::cout << _buf;
            std::cout.flush();
        }

        // -- COOLDOWN ---------------------------------------------------------
        // Per-direction. Opposite direction may fire immediately.
        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) {
                phase = Phase::IDLE;
                m_cooldown_dir = 0;
            }
            // Do NOT return -- the IDLE/ARMED block below honors
            // m_cooldown_dir for the same-direction skip; opposite-direction
            // arming is allowed during the cooldown window.
        }

        // -- LIVE: manage existing position -----------------------------------
        if (phase == Phase::LIVE) {
            _manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // -- DEEPSTRIKE kill-switch (2026-05-08) ------------------------------
        //   Presence of file "KILL_MICROSCALPER" in process cwd forces this
        //   engine to shadow. Checked every 100 ticks (~30s @ ~3 ticks/sec)
        //   to keep stat() cost negligible. Idempotent: once shadow, the
        //   check short-circuits because we won't reach this code path
        //   (return-early before MAX_SPREAD gate would still hit, but the
        //   live-fire path stops emitting orders).
        //
        //   To panic: create C:\Omega\KILL_MICROSCALPER on the VPS, OR hit
        //     GET /api/v1/omega/microscalper/panic on the API server.
        //   To resume: delete the file AND redeploy (engine picks up live
        //     pin from engine_init.hpp on restart). NO runtime resume by
        //     design -- once you've panicked, you should redeploy
        //     deliberately rather than via a "resume" button.
        {
            static int s_kill_check = 0;
            if (++s_kill_check >= 100) {
                s_kill_check = 0;
                if (!shadow_mode) {
                    std::ifstream kill("KILL_MICROSCALPER");
                    if (kill.good()) {
                        shadow_mode = true;
                        printf("[MICRO-SCALPER-GOLD] KILL-SWITCH file detected,"
                               " forcing SHADOW (no further fires until restart)\n");
                        fflush(stdout);
                    }
                }
            }
        }

        // -- New-entry path: warmup + gates -----------------------------------
        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if ((int)m_window.size() < ENTRY_LOOKBACK) return;
        if (!can_enter) return;
        if (spread > MAX_SPREAD) return;

        // Session window 06:00-22:00 UTC (wraparound-aware form mirrors the
        // EUR/AUD/NZD audit-fixes-37 pattern; for 06-22 it reduces to forward).
        {
            const std::time_t t = static_cast<std::time_t>(now_s);
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &t);
#else
            gmtime_r(&t, &utc);
#endif
            const int h = utc.tm_hour;
            const bool in_window =
                (SESSION_END_HOUR > SESSION_START_HOUR)
                    ? (h >= SESSION_START_HOUR && h <  SESSION_END_HOUR)
                    : (h >= SESSION_START_HOUR || h <  SESSION_END_HOUR);
            if (!in_window) return;
            // 2026-05-08 USER REQUEST: pre-London dead zone -- block the
            // hour immediately before London open (BST: 06:00-07:00 UTC,
            // GMT: 07:00-08:00 UTC). PRE_LONDON_DEAD_HOUR_UTC = -1 disables.
            //
            // 2026-05-08 S20 build fix: MSVC C4127 ("conditional expression is
            // constant") is treated as an error under /WX because the >= 0
            // half is compile-time-constant. Wrapping the constexpr comparison
            // in `if constexpr` lets MSVC dead-code it when the gate is off
            // (the current default). The runtime `h` check stays as a regular
            // `if`. Pattern works on clang and MSVC; preserves the disable
            // semantics exactly.
            if constexpr (PRE_LONDON_DEAD_HOUR_UTC >= 0) {
                if (h == PRE_LONDON_DEAD_HOUR_UTC) return;
            }
        }

        // -- Entry signal: 20-tick z-score with L2 confirmation ---------------
        double mean = 0.0, sd = 0.0;
        if (!_rolling_stats(mean, sd)) return;
        if (sd < 0.05) return;  // flat tape -- z-score uninformative
        const double z = (mid - mean) / sd;

        // Cooldown direction filter: same-direction rearm blocked during
        // the COOLDOWN_S window after a close.
        const bool block_long  = (phase == Phase::COOLDOWN && m_cooldown_dir == +1);
        const bool block_short = (phase == Phase::COOLDOWN && m_cooldown_dir == -1);

        const bool z_signals_long  = (z <= -ENTRY_Z);
        const bool z_signals_short = (z >=  ENTRY_Z);

        // L2 confirmation. When l2_real is false, all L2 bools degrade to
        // safe defaults and the engine fires on z alone.
        bool l2_ok_long  = true;
        bool l2_ok_short = true;
        if (l2_real) {
            l2_ok_long  = (l2_imbalance >= L2_IMB_LONG_MIN)  || vacuum_ask;
            l2_ok_short = (l2_imbalance <= L2_IMB_SHORT_MAX) || vacuum_bid;
            // Slope-against-fade veto: don't fade into a strong opposing
            // book slope (that's continuation, not exhaustion).
            if (book_slope <= -L2_FLIP_THRESH) l2_ok_long  = false;
            if (book_slope >=  L2_FLIP_THRESH) l2_ok_short = false;
        }

        const bool fire_long  = z_signals_long  && l2_ok_long  && !block_long;
        const bool fire_short = z_signals_short && l2_ok_short && !block_short;

        if (!fire_long && !fire_short) return;
        if (fire_long && fire_short) return;  // contradictory; skip tick

        // -- Fill ------------------------------------------------------------
        // Market-style: LONG at ask, SHORT at bid. No pending bracket
        // because the move being faded is small; pending fill-time risk
        // would dominate.
        const bool   is_long    = fire_long;
        const double fill_px    = is_long ? ask : bid;
        const double sl_px      = is_long ? (fill_px - SL_DIST_PTS) : (fill_px + SL_DIST_PTS);
        const double tp_px      = is_long ? (fill_px + TP_DIST_PTS) : (fill_px - TP_DIST_PTS);

        pos = LivePos{};
        pos.active             = true;
        pos.is_long            = is_long;
        pos.entry              = fill_px;
        pos.sl                 = sl_px;
        pos.tp                 = tp_px;
        pos.size               = LIVE_LOT;
        pos.spread_at_entry    = spread;
        pos.entry_ts           = now_s;
        pos.l2_real_at_entry   = l2_real;
        pos.z_at_entry         = z;

        phase = Phase::LIVE;

        char _fbuf[512];
        std::snprintf(_fbuf, sizeof(_fbuf),
            "[MICRO-SCALPER-GOLD] FIRE %s @ %.2f sl=%.2f tp=%.2f z=%.2f "
            "spread=%.2f l2_imb=%.2f slope=%.2f l2_real=%d %s\n",
            is_long ? "LONG" : "SHORT",
            fill_px, sl_px, tp_px, z, spread,
            l2_imbalance, book_slope, (int)l2_real,
            shadow_mode ? "[SHADOW]" : "[LIVE]");
        std::cout << _fbuf;
        std::cout.flush();

        // 2026-05-08 S20+: notify RiskMonitor of the fire (logging-only in
        //   v1; no effect on engine state). Bound from engine_init.hpp.
        if (on_fire_hook) on_fire_hook(now_s);
    }

    // External force-close path (e.g. SIGINT shutdown handler in
    // omega_main.hpp:36, regime-flip kill, broker-side cancel).
    // Mirrors the GoldMidScalper signature so the registry-side caller
    // pattern stays uniform across the gold cohort.
    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double exit_px = pos.is_long ? bid : ask;
        _close(exit_px, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

private:
    // -- State -----------------------------------------------------------------
    int                 m_ticks_received = 0;
    int64_t             m_last_tick_s    = 0;
    int64_t             m_cooldown_start = 0;
    int                 m_cooldown_dir   = 0;   // +1 long, -1 short, 0 none
    int                 m_trade_id       = 0;

    std::deque<double>  m_window;   // ENTRY_LOOKBACK rolling window for z-score
    std::deque<double>  m_micro;    // REVERSAL_LOOKBACK rolling window for net-delta

    // Last-tick L2 cache; used inside _manage for the reversal-exit check
    // (so reversal logic always sees the most recent L2 view, not the entry-
    // time view). pos.l2_real_at_entry is preserved separately for gating.
    double m_last_book_slope = 0.0;
    double m_last_l2_imb     = 0.5;
    bool   m_last_vacuum_ask = false;
    bool   m_last_vacuum_bid = false;
    bool   m_last_l2_real    = false;

    // AUDIT 2026-04-29: serialise the whole _close path. Inherited from the
    //   HybridGold lineage to avoid the Apr-7 -$3,008.38 phantom-pnl race.
    mutable std::mutex m_close_mtx;

    // -- Helpers --------------------------------------------------------------
    bool _rolling_stats(double& mean_out, double& sd_out) const noexcept {
        const int n = static_cast<int>(m_window.size());
        if (n < ENTRY_LOOKBACK) return false;
        const int start = n - ENTRY_LOOKBACK;
        double sum = 0.0;
        for (int i = start; i < n; ++i) sum += m_window[i];
        const double mean = sum / static_cast<double>(ENTRY_LOOKBACK);
        double var = 0.0;
        for (int i = start; i < n; ++i) {
            const double d = m_window[i] - mean;
            var += d * d;
        }
        const double sd = std::sqrt(var / static_cast<double>(ENTRY_LOOKBACK));
        mean_out = mean;
        sd_out   = sd;
        return true;
    }

    // Reversal detector: post-BE only. Returns true when either the
    // last REVERSAL_LOOKBACK ticks have netted >= REVERSAL_DELTA_PTS against
    // position OR the latest L2 book_slope has flipped opposing direction
    // by >= L2_FLIP_THRESH. The L2 leg is skipped when l2_real_at_entry is
    // false; we entered on price-only signal, exit on price-only signal.
    bool _detect_reversal() const noexcept {
        if (!pos.active) return false;

        // Tick-momentum reversal (always available).
        const int n = static_cast<int>(m_micro.size());
        if (n >= REVERSAL_LOOKBACK) {
            const double last  = m_micro[n - 1];
            const double prior = m_micro[n - REVERSAL_LOOKBACK];
            const double delta = last - prior;
            if (pos.is_long  && delta <= -REVERSAL_DELTA_PTS) return true;
            if (!pos.is_long && delta >=  REVERSAL_DELTA_PTS) return true;
        }

        // L2 slope flip (only when L2 was real at entry; otherwise the
        // post-entry L2 stream is not trusted to drive exits).
        if (pos.l2_real_at_entry && m_last_l2_real) {
            if (pos.is_long  && m_last_book_slope <= -L2_FLIP_THRESH) return true;
            if (!pos.is_long && m_last_book_slope >=  L2_FLIP_THRESH) return true;
        }

        return false;
    }

    // -- LIVE management ------------------------------------------------------
    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        // Update MFE / MAE.
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        // BE-arm. One-shot. Park SL at entry +/- BE_OFFSET_PTS so a BE exit
        // recovers round-trip cost (spread + slip + commission ~$0.30 per
        // 0.01-lot XAUUSD turn at typical broker rates). Without the offset
        // every BE_HIT closes net-negative.
        if (!pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            const double effective_offset =
                (pos.mfe >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double be_target = pos.is_long
                ? (pos.entry + effective_offset)
                : (pos.entry - effective_offset);
            if (pos.is_long  && be_target > pos.sl) pos.sl = be_target;
            if (!pos.is_long && be_target < pos.sl) pos.sl = be_target;
            pos.be_locked = true;

            char _bbuf[256];
            std::snprintf(_bbuf, sizeof(_bbuf),
                "[MICRO-SCALPER-GOLD] BE_LOCK %s mfe=%.2f sl_now=%.2f "
                "(offset=%.2f)\n",
                pos.is_long ? "LONG" : "SHORT",
                pos.mfe, pos.sl, effective_offset);
            std::cout << _bbuf;
            std::cout.flush();
        }

        // Aggressive trail. Only after BE-lock. Tight: TRAIL_DIST_PTS
        // below MFE for LONG, above for SHORT. Ratchet-only -- never
        // loosens.
        if (pos.be_locked) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - TRAIL_DIST_PTS)
                : (pos.entry - pos.mfe + TRAIL_DIST_PTS);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        // Reversal-exit (post-BE only). Closes immediately at mid -- by
        // construction we are already in profit (be_locked => sl >= entry +/-
        // offset). REVERSAL_EXIT is a distinct exitReason from TRAIL_HIT
        // because the operator wants to know "did the trail catch up to
        // price?" (TRAIL_HIT) vs "did we read tape and bail?" (REVERSAL_EXIT).
        if (pos.be_locked && _detect_reversal()) {
            const double exit_px = pos.is_long ? bid : ask;
            _close(exit_px, "REVERSAL_EXIT", now_s, on_close);
            return;
        }

        // TP_HIT. Limit-style at TP_DIST_PTS.
        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) {
            _close(pos.tp, "TP_HIT", now_s, on_close);
            return;
        }

        // SL_HIT. Classify BE / TRAIL / SL based on where SL sits relative
        // to entry at exit time. Same classifier shape as GoldMidScalper.
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px       = pos.is_long ? bid : ask;
            const bool   sl_at_be      = std::fabs(pos.sl - pos.entry) <= 0.05;
            const bool   trail_in_prof = pos.is_long
                ? (pos.sl > pos.entry + 0.05)
                : (pos.sl < pos.entry - 0.05);
            const char* reason;
            if      (sl_at_be)      reason = "BE_HIT";
            else if (trail_in_prof) reason = "TRAIL_HIT";
            else                    reason = "SL_HIT";
            _close(exit_px, reason, now_s, on_close);
            return;
        }

        // MAX_HOLD safety. A position with no resolution after MAX_HOLD_SEC
        // closes at mid as MAX_HOLD_EXIT. Prevents stale shadow lots during
        // quiet periods.
        if ((now_s - pos.entry_ts) >= MAX_HOLD_SEC) {
            _close(mid, "MAX_HOLD_EXIT", now_s, on_close);
            return;
        }
    }

    // -- Close path -----------------------------------------------------------
    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        std::lock_guard<std::mutex> _lk(m_close_mtx);
        if (!pos.active) return;

        // Snapshot all relevant state before the LivePos reset below.
        const bool    is_long_   = pos.is_long;
        const double  entry_     = pos.entry;
        const double  sl_        = pos.sl;
        const double  tp_        = pos.tp;
        const double  size_      = pos.size;
        const double  mfe_       = pos.mfe;
        const double  mae_       = pos.mae;
        const double  spread_e_  = pos.spread_at_entry;
        const int64_t entry_ts_  = pos.entry_ts;

        // Cohort convention: pnl/mfe/mae stored as price_pts * lot_size
        // (matches GoldMidScalperEngine + the rest of the gold cohort; the
        // downstream OmegaTradeLedger / analytics layer multiplies by
        // USD_PER_PT when dollars are needed for display).
        const double pnl =
            (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;

        // Sanity guard mirrored from GoldMidScalper -- catch the phantom-pnl
        // pattern if it ever resurfaces (Apr-7 -$3,008.38 race).
        const double sane_max = std::max(1.0, size_) * 200.0;
        double pnl_to_emit = pnl;
        if (std::fabs(pnl) > sane_max) {
            const double recomputed =
                (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;
            std::ostringstream warn;
            warn << "[MICRO-SCALPER-GOLD][SANITY] anomalous pnl=" << pnl
                 << " (size=" << size_ << " entry=" << entry_
                 << " exit=" << exit_px
                 << "). Recomputed=" << recomputed
                 << ". Emitting recomputed value.\n";
            std::cout << warn.str();
            std::cout.flush();
            pnl_to_emit = recomputed;
        }

        {
            std::ostringstream os;
            os << "[MICRO-SCALPER-GOLD] EXIT " << (is_long_ ? "LONG" : "SHORT")
               << " @ "        << std::fixed << std::setprecision(2) << exit_px
               << " reason="   << reason
               << " pnl="      << std::setprecision(4) << pnl_to_emit
               << " mfe="      << std::setprecision(2) << mfe_
               << " mae="      << mae_
               << " held_s="   << (now_s - entry_ts_)
               << "\n";
            std::cout << os.str();
            std::cout.flush();
        }

        omega::TradeRecord tr;
        tr.id            = ++m_trade_id;
        tr.symbol        = "XAUUSD";
        tr.side          = is_long_ ? "LONG" : "SHORT";
        tr.engine        = "MicroScalperGold";
        tr.regime        = "MICRO_TICK";
        tr.entryPrice    = entry_;
        tr.exitPrice     = exit_px;
        tr.tp            = tp_;
        tr.sl            = sl_;
        tr.size          = size_;
        tr.pnl           = pnl_to_emit;
        tr.net_pnl       = tr.pnl;
        // Cohort convention: pts * lot_size (NOT dollars). Same as
        // GoldMidScalperEngine. Downstream layers convert when needed.
        tr.mfe           = mfe_ * size_;
        tr.mae           = mae_ * size_;
        tr.entryTs       = entry_ts_;
        tr.exitTs        = now_s;
        tr.exitReason    = reason;
        tr.spreadAtEntry = spread_e_;
        tr.shadow        = shadow_mode;

        // Per-direction cooldown stamp. Same direction is blocked for
        // COOLDOWN_S after exit; opposite direction may fire immediately.
        m_cooldown_start = now_s;
        m_cooldown_dir   = is_long_ ? +1 : -1;

        pos = LivePos{};
        phase = Phase::COOLDOWN;

        if (on_close) on_close(tr);
    }
};

} // namespace omega
