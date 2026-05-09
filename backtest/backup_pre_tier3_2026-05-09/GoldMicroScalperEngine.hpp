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
// PARAMS (all public for engine_init.hpp override) -- DESIGN-INTENT defaults;
//   for CURRENT PRODUCTION VALUES see the S23 REVERT block below.
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
//
// =============================================================================
// 2026-05-09 S22 RE-GEOMETRY -- ATTEMPTED, REVERTED. See S23 block below.
// =============================================================================
//   Brief history (kept here so future readers don't repeat the mistake):
//   S22 swapped ENTRY_Z 0.75->1.75 and TP 0.79->1.75 on the theory that the
//   structurally-impossible 98.9% break-even WR of the original geometry made
//   it negative under realistic 0.75pt costs. The theory ASSUMED real-world
//   WR at the new geometry would degrade gracefully from backtest 92.5% down
//   to ~85%. The replay proved that wrong; see S23.
// =============================================================================
//
// =============================================================================
// 2026-05-09 S23 REVERT TO S19/S21 GEOMETRY (authorised by user in chat:
//   "so change back and rerun the number, show me the result")
// =============================================================================
//
// EVIDENCE -- May 7 2026 full-day XAUUSD tape (347,083 ticks, 16h in-session,
// data/l2_ticks_XAUUSD_2026-05-07.csv) replayed through the engine logic via
// backtest/replay_microscalper_2026-05-09.cpp:
//
//   PRE-S22 (Z=0.75 / TP=0.79 / SL=3.00 / BE=0.50 / MAX_HOLD=60):
//     trades       = 8,000
//     win rate     = 95.62%
//     avg pt/trade = +0.6274 pt   (gross, replay already eats spread on
//                                  market exits at bid/ask)
//     avg hold     = 2.9 s
//     TP hit rate  = 94.2%
//     max DD       = 15.32 pt (raw)
//     gross @ 0.30 lot = +$150,564.60
//
//   S22 OPTION B (Z=1.75 / TP=1.75 / SL=3.00 / BE=0.80 / MAX_HOLD=180):
//     trades       = 1,624        (-80% vs pre)
//     win rate     = 70.32%       (-25.3pp)
//     avg pt/trade = -0.0042 pt   (essentially zero; flat-to-negative gross)
//     avg hold     = 23.9 s       (8x longer)
//     TP hit rate  = 44.1%        (half the trades fail to reach TP)
//     max DD       = 101.19 pt    (6.6x worse)
//     gross @ 0.30 lot = -$203.55
//
// WHY THE S22 PREDICTION WAS WRONG:
//   The Option B EV+0.49pt projection assumed live WR would track 85% --
//   a naive linear haircut from backtest 92.5% as TP/SL ratio widened
//   0.26 -> 0.58. The actual physics on real broker ticks is non-linear:
//   at z=1.75 the price has often broken out (continuation) rather than
//   exhausted (reversion). Mean reversion at 1.75 standard deviations is
//   far less reliable than at 0.75. Net: the wider TP is reached less
//   than half the time, and the longer holds (23.9s vs 2.9s) bleed
//   harder when the move continues against position before BE-arm fires.
//
// WHY PRE-S22 IS ACTUALLY VIABLE (re-evaluated post-replay):
//   The original "cost = 0.75pt/trade" estimate was double-counting spread.
//   The replay already eats spread at every market entry/exit (long enters
//   at ask, exits at bid). So the residual real-cost stack on top of the
//   replay gross is just slip + commission + adverse-selection ~= 0.20-
//   0.30pt/trade. At 0.25pt extra cost, pre-S22 net = +0.377pt/trade =
//   ~+$90,500/day at 0.30 lot in this tape's volatility regime. The live
//   8077780 -$757 outcome was driven by the S21-fixed hedging-close bug,
//   NOT the geometry. With orphan-pair correctly closed via tag 721,
//   the underlying edge is real on this data.
//
// NUMERIC CHANGES (S23 = revert to S19/S21 production values):
//   ENTRY_Z              1.75 -> 0.75   (back to fade-on-modest-stretch)
//   TP_DIST_PTS          1.75 -> 0.79   (back to micro-target sized for
//                                        the 0.75-z reversion physics)
//   SL_DIST_PTS          3.00 -> 3.00   (UNCHANGED)
//   BE_TRIGGER_PTS       0.80 -> 0.50   (BE arms at 63% of new TP, which
//                                        is the original lock-the-small-win
//                                        cadence)
//   BE_OFFSET_PTS        0.30 -> 0.30   (UNCHANGED)
//   TRAIL_DIST_PTS       0.50 -> 0.50   (UNCHANGED)
//   MAX_HOLD_SEC         180  -> 60     (back to 60s; 0.79pt fades resolve
//                                        in ~2.9s avg per replay, so 60s
//                                        is plenty of headroom)
//   REVERSAL_DELTA_PTS   0.30 -> 0.30   (UNCHANGED)
//   COOLDOWN_S           5    -> 5      (UNCHANGED)
//   MAX_SPREAD           0.5  -> 0.5    (UNCHANGED)
//   LIVE_LOT             0.01 -> 0.01   (UNCHANGED -- demo verification
//                                        size; promote only after orph=0
//                                        across 50+ round trips on 2067070)
//
// VERIFICATION PROTOCOL POST-REVERT:
//   1. Rebuild + redeploy on VPS. Confirm engine fires at expected
//      frequency (~5-15/min in active session per the replay's 8K/day
//      cadence over 16 in-session hours).
//   2. Run on demo account 2067070 (mode=LIVE, max_lot_gold=0.01).
//      Markets reopen Sun 22:00 UTC -- can start verifying right away.
//   3. Confirm broker reconciliation banner shows orph=0 across the
//      first 50 round trips. If orph > 0 even once, halt and audit.
//   4. After 50 clean round trips, compare engine_pnl vs realised_pnl.
//      Disparity should be < 5%. The S21 hedging-close fix is the gate
//      here -- if disparity > 5%, the bug isn't fully fixed and lot size
//      cannot scale.
//   5. After 200 trades on demo with orph=0 and disparity < 5%, swap
//      omega_config.ini back to live account 8077780 at 0.01 lot. Hold
//      0.01 for at least 2 sessions to confirm live economics before
//      considering any size increase.
//
// AUTHORISATION TRAIL: explicit user instruction in chat 2026-05-09 ("so
// change back and rerun the number, show me the result") after the May 7
// replay produced the numbers above.
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
    // -- Tuned defaults (S19 calibrated 2026-05-08, rk12 promotion;
    //    S22 RE-GEOMETRY 2026-05-09 -- attempted, REVERTED in S23 the
    //    same day after May 7 replay disproved the EV projection.) -----------
    // Source: backtest/microscalper_crtp_sweep over 28 days / 6.7M ticks of
    // captured XAUUSD L2 (April 9 - May 8 2026). Two sweep iterations:
    //
    //   Sweep #1 (anchor ENTRY_Z=1.5; grid 0.75..3.0):
    //     rk2: Z=0.75 TP=1.00 SL=3.00 BE=0.50 TR=0.50
    //          N=31,009  WR=88.3%  PF=3.58  Net=190.8  hold=14.0s
    //     ENTRY_Z saturated at the LOW boundary -> sweep again.
    //
    //   Sweep #2 (anchor ENTRY_Z=0.75; grid 0.375..1.5) -- rk12 was the S19
    //   live-promoted point AND is back on the live deploy after S23:
    //     rk1:  Z=0.38 TP=1.00 SL=2.38 N=42,277 WR=87.3% PF=3.27 Net=247.8
    //           (boundary-saturated again at 0.38; max net but overfit risk)
    //     rk12: Z=0.75 TP=0.79 SL=3.00 BE=0.50 TR=0.50
    //           N=36,575 WR=92.5% PF=4.40 Net=203.6 hold=10.5s
    //
    //   2026-05-09 S22: rk12 was retired in error. Replay on a real-broker
    //   tape (May 7 2026, 347K ticks) showed Option B (Z=1.75 / TP=1.75)
    //   produced -0.0042pt/trade gross at 70% WR vs rk12's +0.627pt at
    //   95.6% WR on the same tape. S23 reverts to rk12 immediately.
    //   Live expectancy WILL still be lower than this replay because the
    //   replay does not model fill rejection, latency-induced adverse
    //   selection beyond the spread it already eats, or commission. Add
    //   ~0.20-0.30pt/trade on top of the replay gross for a real-cost
    //   estimate. Re-tune from 2-4 weeks of live tape after S21 hedging-
    //   close fix is verified.
    static constexpr int    ENTRY_LOOKBACK       = 20;

    // 2026-05-09 S23 REVERT: 1.75 -> 0.75. May 7 replay showed wider Z
    //   produces flat-to-negative gross because mean-reversion at z=1.75
    //   is unreliable (price has often broken out by then, not exhausted).
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
    //   2026-05-09 S22 -> S23: kept at 0.5pt across both attempts. The
    //   spread-cap binding constraint is unchanged by geometry choice.
    static constexpr double MAX_SPREAD           = 0.5;

    // 2026-05-09 S23 REVERT: 1.75 -> 0.79. Matches the expected reversion
    //   distance at z=0.75 entry (~0.75 std-devs of the 20-tick window).
    //   The 0.79 was rk12's calibrated value from the April-May L2 sweep.
    static constexpr double TP_DIST_PTS          = 0.79;

    static constexpr double SL_DIST_PTS          = 3.0;

    // 2026-05-09 S23 REVERT: 0.80 -> 0.50. BE arms at 63% of TP, locking
    //   the small win quickly per the engine's design intent ("small
    //   trades quickly").
    static constexpr double BE_TRIGGER_PTS       = 0.50;

    static constexpr double BE_OFFSET_PTS        = 0.3;
    static constexpr double TRAIL_DIST_PTS       = 0.5;
    static constexpr int    REVERSAL_LOOKBACK    = 5;
    static constexpr double REVERSAL_DELTA_PTS   = 0.30;
    static constexpr double L2_FLIP_THRESH       = 0.20;

    // 2026-05-09 S23 REVERT: 180 -> 60. May 7 replay showed avg hold for
    //   pre-S22 geometry is 2.9s; 60s is ~20x headroom. The S22 bump to
    //   180s was correlated with degraded outcomes (positions sat in
    //   trouble waiting for a 1.75pt reversion that didn't come).
    static constexpr int    MAX_HOLD_SEC         = 60;

    static constexpr int    COOLDOWN_S           = 5;
    static constexpr int    SESSION_START_HOUR   = 6;   // UTC
    static constexpr int    SESSION_END_HOUR     = 22;  // UTC, wraparound-aware

    // 2026-05-08 USER REQUEST: pre-London dead zone -- DISABLED at user
    // direction. Constant kept (set to -1) so the gate machinery is in
    // place; flip to 6 (BST) or 7 (GMT) to re-arm. The gate blocks the
    // hour immediately before London open. SL_DIST/TP_DIST asymmetry is
    // 3.8x post-S23 revert (was the original rk12 ratio).
    static constexpr int    PRE_LONDON_DEAD_HOUR_UTC = -1;

    // 2026-05-08 DEEPSTRIKE LIVE: lot set to 0.03 for the single-engine
    //   live deploy on account 8077780. Earlier 0.10 figure was the user's
    //   shadow-test bump; for live they chose 0.03 instead.
    //
    //   PnL per trade at 0.03 lot:
    //     TP win  : +0.79pt  ->  +$2.37 gross  ->  ~+$2.04 net (after $0.33 slip)
    //     SL hit  : -3.00pt  ->  -$9.00 gross  ->  ~-$9.33 net (after $0.33 slip)
    //   Real-cost BE WR  ~  82.1%   (vs backtest 92.5%, monitor trip 82.16%)
    //
    //   2026-05-08 LOT BUMP (S21, authorised by user in chat):
    //     LIVE_LOT raised 0.03 -> 0.20 (6.67x), then 0.20 -> 0.30 (current).
    //     10x vs original 0.03 deploy. Per-trade PnL at 0.30 lot:
    //       TP win : +0.79pt -> +$23.70 gross -> ~+$20.40 net (after ~$3.30 slip)
    //       SL hit : -3.00pt -> -$90.00 gross -> ~-$93.30 net (after ~$3.30 slip)
    //     BE_WR unchanged in % terms; RiskMonitor TRIP_WR=0.8216 stays
    //     anchored to backtest expectancy (not a $ threshold). The $
    //     drawdown velocity per losing trade is 10x the original 0.03 lot
    //     deploy -- operator attention required.
    //     OMEGA.ps1 stop on the VPS is the manual kill if needed (note:
    //     proper shutdown-force-close is queued for next deploy; today
    //     stop+manual-cTrader-close is the workaround if a position is open).
    //     max_lot_gold in omega_config.ini also raised 0.03 -> 0.30
    //     (the per-symbol cap would reject the new lot otherwise).
    //
    //   2026-05-08 LIVE-MODE FLIP (S21 audit, authorised by user in chat):
    //     omega_config.ini mode flipped SHADOW -> LIVE. Reason: the previous
    //     DEEPSTRIKE comment block in engine_init.hpp claimed shadow_mode=
    //     false alone was enough to make this engine live; that was wrong.
    //     order_exec.hpp:72 hard-gates send_live_order on g_cfg.mode=="LIVE",
    //     so with mode=SHADOW every microscalper close was being silently
    //     dropped at the broker submit boundary (paper P&L accumulating in
    //     the dashboard, NZD 5,000.00 untouched on BlackBull account
    //     8077780). To preserve single-engine semantics under mode=LIVE, the
    //     wire_bracket lambda in engine_init.hpp was hard-pinned to shadow.
    static constexpr double USD_PER_PT           = 100.0;  // per full lot XAUUSD

    // 2026-05-09 LOT REDUCED 0.30 -> 0.01 (operator order, after orphan-pair
    // bleed). At 0.01 lot, per-trade exposure is 30x smaller: TP win ~+$0.79
    // gross, SL hit ~-$3.00 gross. Even worst-case orphan bleed at this size
    // is fractions of a dollar per pair, not $5-15 per pair as at 0.30.
    //
    // 2026-05-09 S23 REVERT: STAYS AT 0.01. Per-trade exposure at the
    //   reverted (rk12) geometry, 0.01 lot:
    //     TP win  : +0.79pt  ->  +$0.79 gross  ->  ~+$0.54 net (after ~$0.25 cost)
    //     SL hit  : -3.00pt  ->  -$3.00 gross  ->  ~-$3.25 net (after ~$0.25 cost)
    //   Promote lot ONLY after demo-account run shows orph=0 across 50+
    //   round trips and realised_pnl matches engine_pnl within +/- 5%.
    static constexpr double LIVE_LOT             = 0.01;

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

        // 2026-05-08 S21 HEDGING-MODE FIX (authorised by user in chat after the
        //   NZ$459 hedging incident):
        //   BlackBull account 8077780 is in HEDGING mode, not netting. Sending an
        //   opposite-direction market order to "close" a position OPENS A NEW
        //   OPPOSING position instead of netting the existing one. To close a
        //   specific position in hedging mode, the close FIX 35=D message must
        //   reference the broker's position ID (Spotware uses tag 1006; FIX 4.4
        //   standard is tag 721 PosMaintRptID). The close path also benefits
        //   from FIX standard tag 77=C (PositionEffect=Close) as a hint.
        //
        //   Population path:
        //     1. Entry-side send_live_order returns clOrdId; we store it here
        //        as entry_clOrdId immediately after dispatch (in tick_gold.hpp).
        //     2. ExecutionReport arrives ~50-200ms later. handle_execution_report
        //        matches by clOrdId, extracts tag 1006 or 721 (whichever is
        //        present), and writes broker_position_id here.
        //     3. On exit, microscalper_on_close inspects broker_position_id:
        //          - non-empty: send close FIX message with tag 1006 + 721 + 77=C
        //          - empty: REFUSE the close, auto-shadow the engine, log
        //            [MICROSCALPER-NO-POSID]. Operator manually flattens the
        //            orphan position in cTrader. Better to leave one orphan
        //            than to double down on every exit.
        //
        //   Race window: between entry _open() and the entry ACK arriving back
        //   (~50-200ms), broker_position_id is empty. If the engine fires an
        //   exit within that window (extremely unlikely for microscalper which
        //   needs at least one tick of management), the close refuses to send.
        //   Engine internal pos.active goes false on exit regardless, so the
        //   engine returns to a flat state -- but the broker still has the
        //   long/short orphan. Auto-shadow + log + operator manual close.
        std::string broker_position_id;
        std::string entry_clOrdId;
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
        // 2026-05-09 BROKER RECONCILIATION: persist the entry-side clOrdId
        // captured at send_live_order dispatch into the ledger record so
        // handle_execution_report can match inbound ExecReports to the
        // correct trade. close_clOrdId is stamped post-on_close in
        // microscalper_on_close (it doesn't exist yet at this point because
        // the close FIX message hasn't been built).
        tr.entry_clOrdId = pos.entry_clOrdId;

        // Per-direction cooldown stamp. Same direction is blocked for
        // COOLDOWN_S after exit; opposite direction may fire immediately.
        m_cooldown_start = now_s;
        m_cooldown_dir   = is_long_ ? +1 : -1;

        // 2026-05-08 S21 HEDGING-MODE FIX (CRITICAL ORDER): on_close MUST run
        //   BEFORE pos is reset. The microscalper_on_close callback reads
        //   pos.broker_position_id (populated by handle_execution_report on
        //   the entry-side ACK) to send the hedging-aware close order. If
        //   pos is reset first via `pos = LivePos{}`, broker_position_id
        //   becomes empty and the close-side safety branch refuses to send,
        //   auto-shadowing the engine. Confirmed in latest.log on 11:45:41
        //   2026-05-08: every close hit [MICROSCALPER-NO-POSID] refusal
        //   despite a clean [MICROSCALPER-POSID-CAPTURED] entry ACK two
        //   seconds prior.
        //
        //   Safety: m_close_mtx is held throughout this block, so no other
        //   thread can observe the brief window where pos.active is true
        //   but the trade has just been emitted. The on_close callback
        //   doesn't call back into engine state that depends on
        //   has_open_position(); it calls handle_closed_trade and
        //   send_live_order, both of which are pos-agnostic.
        if (on_close) on_close(tr);

        pos = LivePos{};
        phase = Phase::COOLDOWN;
    }
};

} // namespace omega
