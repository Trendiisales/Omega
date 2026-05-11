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
// ALGORITHM (S19 ORIGINAL -- see S33 block below for the wider-geometry port)
//   Phase machine: IDLE -> ARMED -> LIVE -> COOLDOWN.
//
//   ARMING: every tick the engine maintains a rolling window of mid prices
//   (ENTRY_LOOKBACK ticks). When |z-score| of the current mid vs the window
//   mean crosses ENTRY_Z AND L2 imbalance confirms direction (when l2_real),
//   the engine arms in the fade direction. The structure represents micro
//   exhaustion -- price has stretched far enough from its window mean that
//   reversion is the high-prior outcome.
//
//   ENTRY: market-style fill at the side that resolves the fade. LONG on ask
//   when z<=-ENTRY_Z (price below mean, expect bounce up); SHORT on bid when
//   z>=ENTRY_Z (price above mean, expect drop). No bracket / pending phase --
//   the move we are fading is small, and a pending block adds fill-time risk
//   inconsistent with "lock in small trades quickly."
//
//   MANAGE (LIVE): three layers run on every tick.
//     1. Initial-SL guard. SL is fixed at SL_DIST_PTS below/above entry
//        until BE-arm fires. SL_HIT here is classified normally.
//     2. BE-arm. As soon as MFE >= BE_TRIGGER_PTS, SL is moved to entry +/-
//        BE_OFFSET_PTS so a BE exit recovers round-trip cost. One-shot via
//        pos.be_locked.
//     3. Aggressive trail + reversal-exit. Both run only after pos.be_locked
//        is set:
//          a. Trail SL ratchets to MFE - TRAIL_DIST_PTS.
//          b. Reversal detector. Exits IMMEDIATELY (REVERSAL_EXIT) when
//             either tick-momentum or L2 slope flips against position.
//        These checks ONLY fire post-BE so we never close a trade in loss
//        through reversal logic -- the initial SL handles that case.
//     4. TP_HIT. Standard limit at TP_DIST_PTS. Fires whether or not BE
//        is locked.
//     5. MAX_HOLD timeout (60s S19 / 7200s S33). Safety: a position with no
//        progress closes at mid as MAX_HOLD_EXIT.
//
//   COOLDOWN: COOLDOWN_S keyed PER DIRECTION. After exit, the same
//   direction is blocked for COOLDOWN_S; the OPPOSITE direction is permitted
//   immediately.
//
// SAFETY
//   - shadow_mode = true by default. Promotion to live requires explicit
//     authorisation in engine_init.hpp.
//   - Spread cap MAX_SPREAD = 0.5pt (S20 DEEPSTRIKE) / 1.0pt (S33 wide port).
//   - Session window 00:00-24:00 UTC (S24 full day) / 00:00-07:00 UTC
//     (S33 Asia-only port).
//   - 0.01 lot uniform cap (FIX 2026-04-22 policy).
//   - Mutex on _close path. Inherited from HybridGold lineage.
//
// BACKTEST FRIENDLY
//   - All time inputs come through the on_tick() now_ms parameter; no
//     std::time(nullptr) on the hot path.
//   - File-IO persistence guarded #ifndef OMEGA_BACKTEST.
//   - L2 data is delivered explicitly via on_tick parameters.
//
// LOG NAMESPACE
//   All log lines use prefix [MICRO-SCALPER-GOLD] / [MICRO-SCALPER-GOLD-DIAG].
//   tr.engine = "MicroScalperGold". tr.regime = "MICRO_TICK".
//
// =============================================================================
// 2026-05-09 S22 RE-GEOMETRY -- ATTEMPTED, REVERTED in S23 same day. The
//   theory (raise ENTRY_Z and TP together to escape an impossible BE-WR
//   target) was correct in algebra but wrong on real broker tape: the
//   wider TP=1.75 reverted only 44% of the time and dropped WR from 95%
//   to 70%. See S23 block below for the reversion math.
// =============================================================================
//
// 2026-05-09 S23 REVERT TO S19/S21 GEOMETRY -- restored Z=0.75 TP=0.79
//   SL=3.00 BE_TRIG=0.50 MAX_HOLD=60. May 7 replay confirmed pre-S22
//   geometry produced +0.6274 pt/trade gross at 95.62% WR vs the
//   attempted Option B's -0.0042 pt/trade at 70.32% WR. The "structurally
//   negative" diagnosis that motivated S22 had double-counted spread; the
//   replay already eats spread on market entries/exits, so the residual
//   real-cost overlay is only 0.20-0.30 pt/trade, not 0.75. Original
//   geometry is profitable across all 13 active days in the replay sample.
// =============================================================================
//
// =============================================================================
// 2026-05-09 S24 DEPLOY: 24-HOUR SESSION + REGIME GATE (authorised by user
//   in chat: "no we can deploy this, what i do want is the best settings
//   we can get and then if this engine works well in chop surely it would
//   fire well in Asia session")
// =============================================================================
//
// EVIDENCE -- 15-day multi-day replay (April 22 to May 8 2026) of the
// reverted-S23 geometry against captured XAUUSD broker tapes. Two
// independent improvements identified and validated:
//
// A. ASIA SESSION INCLUSION -- biggest single win
//   Pre-S24:  Session window 06:00-22:00 UTC (Asia 22-06 UTC excluded).
//   Post-S24: Session window 00:00-24:00 UTC (full day).
//
//   The original 06-22 window was inherited from "skip dead Asia" defaults
//   that assumed Asia was too quiet for the strategy. The multi-day replay
//   proved the opposite: Asia is the BEST session for this engine.
//
//   Multi-day stats (all 15 tapes, ER<0.18 regime gate applied):
//     Session 06-22 UTC : 47,941 trades, 90.4% WR, +$700,895 gross @ 0.30 lot
//     Full 24h          : 71,931 trades, 91.0% WR, +$1,105,509 gross @ 0.30 lot
//     Asia contribution :  23,990 trades, +$404,614 gross @ 0.30 lot
//     Asia avg pt/trade : +0.5622 pt  (vs full-tape avg +0.4818 -- BETTER)
//
//   At 0.25pt real-cost overlay @ 0.30 lot:
//     Session 06-22 net : +$341,338
//     Full 24h net      : +$566,027
//     Improvement       : +$224,689 (+65.8%)
//
//   Why Asia works: mean-reversion thrives in low-volatility chop, which is
//   exactly what Asia 22-06 UTC delivers (no major news, no London/NY open
//   spikes, narrow ranges with frequent z-score deviations that revert
//   quickly). The "skip Asia" rule was inherited from momentum-strategy
//   defaults and is wrong for this engine.
//
// B. REGIME GATE -- small but real
//   Added: Kaufman Efficiency Ratio (ER) over a 200-tick rolling window.
//     ER = |last - first| / sum(|tick_i - tick_{i-1}|)
//     ER -> 0 = perfect chop; ER -> 1 = perfect trend.
//   Entry rule: refuse new entries when ER >= REGIME_THRESHOLD (0.18).
//   Existing positions are managed normally regardless of regime.
//
//   Multi-day stats (15 tapes, 06-22 UTC session for fair compare):
//     No gate           : 48,663 trades, +$338,337 net @ 0.30 lot, 0.25pt cost
//     ER<0.18 (winner)  : 47,941 trades, +$341,338 net @ 0.30 lot, 0.25pt cost
//     Improvement       : +$3,001  (+0.9%)
//
//   The improvement is modest in dollar terms but the gate provides
//   structural protection: when sustained trend regimes appear (multi-tick
//   directional moves that previously stopped out the chop engine), the
//   gate keeps the engine flat through the bad windows. The 1% gain is
//   the floor; the value is robustness to regime shifts not in the
//   training data.
//
//   Threshold of 0.18 was selected from a 4-point sweep (0.18, 0.25, 0.32,
//   no-gate). 0.18 was the only setting that beat no-gate at every cost
//   level. Don't lower further without re-running the sweep.
//
// COMBINED A+B EXPECTED IMPACT:
//   The replay didn't run A and B together (we tested ER<0.18 with 06-22
//   session, then session-on vs session-off with ER<0.18). Linear-combine
//   estimate: pre-S24 had +$338K net; +66% from Asia + 1% from regime gate
//   gives ~+$566K net @ 0.30 lot for the 13 active days. Per-day average
//   ~$43,500 @ 0.30 lot, ~$1,450 @ 0.01 lot. Live execution will deflate
//   that further (TP exits eat spread the replay doesn't model + slippage
//   variance + fill rejection risk in fast markets).
//
// NUMERIC CHANGES (S24, this file only):
//   SESSION_START_HOUR     6  -> 0   (open Asia)
//   SESSION_END_HOUR       22 -> 24  (open Asia / overnight)
//   REGIME_LOOKBACK        n/a -> 200  (NEW: Kaufman ER window)
//   REGIME_THRESHOLD       n/a -> 0.18 (NEW: ER threshold for trend gate)
//   All other rk12 constants UNCHANGED from S23 revert state.
//
// SAFETY ENVELOPE FOR FIRST LIVE WEEK (S24 plan -- superseded by S33 below):
//   - LIVE_LOT stays at 0.01 (no scaling until live tape confirms numbers)
//   - max_lot_gold in omega_config.ini stays at 0.01
//   - mode=LIVE on demo account 2067070 first; only swap to 8077780 after
//     50+ orph=0 round trips with realised_pnl matching engine_pnl ±5%
//   - If 3-day rolling WR drops below 80% live, halt and audit (the
//     April regime in the replay had 75% WR -- if live drops below that
//     band, something is fundamentally different)
//
// VERIFICATION PROTOCOL POST-S24 DEPLOY (historical -- see S33 below for
// the active criteria):
//   1. Demo account 2067070, mode=LIVE, lot=0.01.
//   2. Sun 22:00 UTC market open -- verify engine fires through Asia hours
//      (will see [MICRO-SCALPER-GOLD] FIRE lines in latest.log overnight).
//   3. After 50 round trips: confirm orph=0, realised_pnl ~= engine_pnl.
//   4. If 24h Asia firing matches replay's ~1,600 trades/day average,
//      the geometry is behaving as expected.
//   5. After 200 trades clean: swap config back to live 8077780 at 0.01.
//   6. Hold 0.01 lot for at least 5 trading days; only scale up if
//      realised_pnl tracks +/- 25% of replay's expected band.
//
// AUTHORISATION TRAIL: explicit user instruction in chat 2026-05-09
// after Asia comparison test showed +$404,614 gross uplift from including
// 22-06 UTC ticks. User said "no we can deploy this, what i do want is
// the best settings we can get".
// =============================================================================
//
// =============================================================================
// 2026-05-08 (FRIDAY) LIVE BLEED -- S24 geometry ran live on account
//   8077780 with lot scaled 0.03 -> 0.20 -> 0.30. Net result: -NZ$310 across
//   two orphan-pair incidents. A 21-day "honest fill" backtest sweep
//   subsequently scored 0/21 profitable days for this geometry at every
//   z value tested. Mode flipped LIVE -> SHADOW pending re-validation.
//   See HANDOFF_S22_DEEPSTRIKE_LIVE.md and omega_config.ini:67-75 for the
//   full incident record.
// =============================================================================
//
// =============================================================================
// 2026-05-11 S33 OPTION A SHADOW PORT (authorised by user in chat:
//   "put the new settings onto the vps so we can test them" + Option A
//   selection from the S32 §3 / §6.3 menu -- "port geometry only, apply
//   §3.1-3.3 verbatim".)
// =============================================================================
//
// PURPOSE
//   Port the S30/S31 256-cell wide-fine sweep TOP-1 (Dukascopy 623-day
//   corpus) into the live engine in SHADOW so we can compare paper-trade
//   PnL against the +$2.31/day backtest claim BEFORE any LIVE flip.
//   The S24 geometry that bled live on May 8 is EXPLICITLY abandoned here.
//   See HANDOFF_S30/S31/S32 for the sweep results and S32 §3 for the
//   exact change list reproduced in this block.
//
// PRIMARY GEOMETRY (S32 §3.1):
//   ENTRY_Z              0.75  -> 2.0     (backtest z_thresh)
//   ENTRY_LOOKBACK       20    -> 200     (backtest window W in ticks)
//   TP_DIST_PTS          0.79  -> 35.0    (backtest tp_pts)
//   SL_DIST_PTS          3.0   -> 12.0    (backtest sl_pts)
//   SESSION_START_HOUR   0     -> 0       (unchanged)
//   SESSION_END_HOUR     24    -> 7       (Asia-only; backtest --session 0-7)
//   LIVE_LOT             0.01  -> 0.01    (unchanged; broker minimum)
//
// SECONDARY DISABLES (S32 §3.2 -- the trap that breaks literal porting):
//   The backtest harness does NOT model BE / trail / reversal / L2-flip /
//   MAX_HOLD / L2-gate. Leaving these at the S24 values means a $35 TP
//   never gets a chance to fire -- the position gets snapped to BE at
//   $0.50 or timed out at 60s instead. The disables below restore the
//   "open-and-let-it-run-to-TP-or-SL" behaviour the backtest assumed.
//
//     BE_TRIGGER_PTS       0.50  -> 999.0   (disable BE arm; sentinel value
//                                            larger than any plausible MFE)
//     BE_OFFSET_PTS        0.3   -> 0.0     (irrelevant once BE disabled)
//     TRAIL_DIST_PTS       0.5   -> 999.0   (disable trail)
//     REVERSAL_LOOKBACK    5     -> 0       (disable reversal exit -- safe
//                                            because _detect_reversal is only
//                                            called when pos.be_locked is true,
//                                            and BE_TRIGGER_PTS=999 prevents
//                                            be_locked from ever flipping. The
//                                            =0 setting is documentation-only
//                                            for that reason.)
//     REVERSAL_DELTA_PTS   0.30  -> 999.0   (belt-and-braces disable; would
//                                            also block the reversal even if
//                                            _detect_reversal somehow ran)
//     L2_FLIP_THRESH       0.20  -> 999.0   (disable the L2-slope reversal arm)
//     MAX_HOLD_SEC         60    -> 7200    (2 hours; mean-reversion at 35pt
//                                            TP needs minutes-to-hours)
//     COOLDOWN_S           5     -> 60      (backtest 100 ticks ~= 60s at
//                                            typical XAU tick rate)
//     L2_IMB_LONG_MIN      0.55  -> 0.50    (S32 instruction: "disable L2
//     L2_IMB_SHORT_MAX     0.45  -> 0.50    gate" -- but see CAVEAT 1 below;
//                                            the 0.50/0.50 setting still
//                                            partially gates by direction)
//     MAX_SPREAD           0.5   -> 1.0     (backtest --max-spread default)
//
// KAUFMAN GATE (S32 §3.3 Option A):
//   REGIME_THRESHOLD     0.18  -> 1.0     (gate always passes; ER cannot
//                                          reach 1.0 in practice. Faithful
//                                          to backtest which had no Kaufman
//                                          filter.)
//
// CAVEAT 1 -- L2 gate at 0.50/0.50 is NOT fully ungated:
//   With l2_real==true the entry path requires (l2_imbalance >= 0.50)
//   for LONG and (l2_imbalance <= 0.50) for SHORT. Since L2 imbalance
//   varies continuously in (0,1), this still admits the directional
//   half but rejects the contra-direction half. To make truly ungated:
//   set L2_IMB_LONG_MIN=0.0 and L2_IMB_SHORT_MAX=1.0. Following S32 §3.2
//   verbatim per operator selection of Option A; flag for re-tune if
//   shadow trade count diverges materially from backtest's ~1.16/day.
//
// CAVEAT 2 -- shadow PnL will NOT exactly match backtest +$2.31/day:
//   Engine has internal logic (fill model, spread filter, tick-cooldown
//   semantics, L2 imbalance updates, regime state) the backtest harness
//   simplifies. Expect 10-50% divergence. That divergence is the
//   diagnostic we want -- it tells us which engine internal is the source
//   of the live-vs-backtest gap that bled money on Friday May 8.
//
// CAVEAT 3 -- the cTrader real-cost number is still missing:
//   S32 §4 / §6.2 requires the realized $-per-RT from account 8077780's
//   May 8-9 ledger. Operator chose to ship anyway with the BlackBull
//   web-spec $0.06/RT default; if real cost is materially higher the
//   +$2.31/day backtest headline shrinks or inverts. Re-derive the
//   expected daily PnL once the ledger arrives.
//
// SAFETY POSTURE (unchanged from S24 + S32):
//   - Engine starts with shadow_mode = true unless engine_init.hpp pins
//     it live; even if pinned, order_exec.hpp:135 gates send_live_order
//     on g_cfg.mode == "LIVE", and omega_config.ini is committed at
//     mode=SHADOW alongside this file (S33 commit).
//   - KILL_MICROSCALPER sentinel on the VPS forces shadow_mode=true
//     within 100 ticks of detection; do NOT remove until 24h of clean
//     shadow telemetry on the new geometry.
//   - LIVE_LOT stays at 0.01 (broker minimum). Do not raise without
//     explicit forward-test sign-off.
//   - max_lot_gold in omega_config.ini stays at 0.01.
//   - mode=SHADOW until explicit operator authorisation to flip LIVE.
//
// VERIFICATION CRITERIA AFTER 24H SHADOW (operator-judged):
//   - Trade count in the same order of magnitude as backtest (ballpark
//     ~1.16 fires/day on the Asia-only 0-7 UTC window; deviations of
//     10x or more in either direction warrant investigation BEFORE any
//     LIVE flip).
//   - First [MICRO-SCALPER-GOLD] FIRE log line MUST end with [SHADOW].
//     If it ends with [LIVE], stop the service immediately.
//   - Realized shadow PnL distribution centred on a positive median
//     (NOT the +$2.31 headline -- the median, since the backtest
//     distribution had wide tails).
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <deque>
#include <fstream>
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
    // -- Tuned defaults --------------------------------------------------------
    //   S19 calibrated 2026-05-08; S22 attempted then reverted in S23 same day;
    //   S24 added 24h session + regime gate after multi-day replay validation
    //   2026-05-09; S33 ported the S30/S31 wide-fine TOP-1 (z=2.0 W=200
    //   TP=35 SL=12 Asia 0-7 UTC) into the engine in SHADOW for forward
    //   evaluation 2026-05-11. See block at top of file for the full S33
    //   change list and caveats.
    // ------------------------------------------------------------------------

    // 2026-05-11 S33: 20 -> 200. Backtest sweep TOP-1 window (W). Maintains
    //   a 200-tick rolling window for the z-score; std mid is computed over
    //   the trailing 200 mids in _rolling_stats(). Warmup is now ~200
    //   ticks before the first entry can fire.
    static constexpr int    ENTRY_LOOKBACK       = 200;

    // 2026-05-11 S33: 0.75 -> 2.0. Backtest z_thresh. Wider entry; fewer
    //   fires (~1.16/day vs S24's ~1,600/day on Asia-only 0-7 UTC).
    static constexpr double ENTRY_Z              = 2.0;

    // 2026-05-11 S33: 0.55 -> 0.50, 0.45 -> 0.50. Per S32 §3.2 "disable L2
    //   gate". CAVEAT: 0.50/0.50 does NOT fully disable -- it still requires
    //   l2_imbalance >= 0.50 for LONG and <= 0.50 for SHORT, biasing
    //   entries to the dominant book side. To truly ungate set 0.0 / 1.0.
    static constexpr double L2_IMB_LONG_MIN      = 0.50;
    static constexpr double L2_IMB_SHORT_MAX     = 0.50;

    // 2026-05-11 S33: 0.5 -> 1.0. Backtest --max-spread default.
    static constexpr double MAX_SPREAD           = 1.0;

    // 2026-05-11 S33: 0.79 -> 35.0. Backtest tp_pts. 44x wider than S24;
    //   requires a wider SL to absorb mean-reversion overshoots.
    static constexpr double TP_DIST_PTS          = 35.0;

    // 2026-05-11 S33: 3.0 -> 12.0. Backtest sl_pts.
    static constexpr double SL_DIST_PTS          = 12.0;

    // 2026-05-11 S33: 0.50 -> 999.0. SENTINEL DISABLE. Backtest harness
    //   has no break-even arm; leaving BE active would snap the new $35 TP
    //   trade to a $0.50 BE within the first few seconds.
    static constexpr double BE_TRIGGER_PTS       = 999.0;

    // 2026-05-11 S33: 0.3 -> 0.0. Irrelevant once BE_TRIGGER_PTS makes
    //   pos.be_locked unreachable, but kept in this position to preserve
    //   the constants-block layout.
    static constexpr double BE_OFFSET_PTS        = 0.0;

    // 2026-05-11 S33: 0.5 -> 999.0. SENTINEL DISABLE. Trail only fires
    //   once pos.be_locked is true; BE_TRIGGER_PTS=999 prevents that.
    //   Sentinel kept for documentation.
    static constexpr double TRAIL_DIST_PTS       = 999.0;

    // 2026-05-11 S33: 5 -> 0. Reversal exit only fires once
    //   pos.be_locked is true (see _manage), which BE_TRIGGER_PTS=999
    //   prevents. The =0 value is documentation-only and is safe BECAUSE
    //   _detect_reversal is unreachable; if you ever re-enable BE, you
    //   MUST also restore REVERSAL_LOOKBACK to a positive value first or
    //   the m_micro[n - REVERSAL_LOOKBACK] indexing will be UB.
    static constexpr int    REVERSAL_LOOKBACK    = 0;

    // 2026-05-11 S33: 0.30 -> 999.0. Belt-and-braces sentinel disable;
    //   even if _detect_reversal somehow ran, no realistic delta would
    //   reach 999.
    static constexpr double REVERSAL_DELTA_PTS   = 999.0;

    // 2026-05-11 S33: 0.20 -> 999.0. Disables the L2-slope reversal arm
    //   inside _detect_reversal AND the entry-path l2_ok flip-block.
    static constexpr double L2_FLIP_THRESH       = 999.0;

    // 2026-05-11 S33: 60 -> 7200. Mean-reversion at $35 TP needs minutes
    //   to hours; 60s would force a MAX_HOLD_EXIT before TP can resolve.
    //   2 hours is a generous ceiling; backtest had no time exit at all.
    static constexpr int    MAX_HOLD_SEC         = 7200;

    // 2026-05-11 S33: 5 -> 60. Backtest 100-tick cooldown ~= 60s at
    //   typical XAU tick rate. Per-direction cooldown still applies.
    static constexpr int    COOLDOWN_S           = 60;

    // 2026-05-11 S33: SESSION window narrowed from full-24h (S24) to
    //   Asia-only 0-7 UTC. Backtest sweep TOP-1 cell was --session 0-7;
    //   the wider geometry's edge concentrates in low-vol Asia hours.
    //   With START=0, END=7 the in_window check (h >= 0 && h < 7) admits
    //   hours 0-6 UTC inclusive, totalling 7 trading hours/day.
    static constexpr int    SESSION_START_HOUR   = 0;   // UTC, was 0 (S24)
    static constexpr int    SESSION_END_HOUR     = 7;   // UTC, was 24 (S24)

    // 2026-05-08 USER REQUEST: pre-London dead zone -- DISABLED at user
    //   direction. Constant kept (set to -1) so the gate machinery is in
    //   place; flip to 6 (BST) or 7 (GMT) to re-arm. Less critical
    //   post-S24 since the session is fully open, but the optional
    //   dead-zone hour cut still works if a problematic hour emerges.
    //   2026-05-11 S33: still -1; no per-hour dead-zone needed for the
    //   wider geometry's 7-hour Asia window.
    static constexpr int    PRE_LONDON_DEAD_HOUR_UTC = -1;

    // 2026-05-09 S24 DEPLOY: NEW regime classifier constants ---------------
    //
    //   Kaufman Efficiency Ratio (ER) over the last REGIME_LOOKBACK ticks:
    //     ER = |price_now - price_n_ago| / sum(|tick_i - tick_{i-1}|)
    //     ER ~ 0 = perfect chop (price wandering, returns cancel out)
    //     ER ~ 1 = perfect trend (price walking, returns reinforce)
    //
    //   S24 threshold 0.18 from multi-day sweep; S33 disabled for backtest
    //   parity (REGIME_THRESHOLD=1.0 means the gate always passes since
    //   ER cannot reach 1.0 in practice). New entries are refused when
    //   m_regime_is_trend == true. Existing positions continue to be
    //   managed normally regardless of regime -- the gate is on entries
    //   only.
    //
    //   Warmup: until the regime window is full (200 ticks), the
    //   classifier reports chop (default-allow), so very-early entries
    //   in a session aren't blocked by an empty classifier.
    static constexpr int    REGIME_LOOKBACK      = 200;

    // 2026-05-11 S33: 0.18 -> 1.0. S32 §3.3 Option A. ER cannot reach 1.0
    //   in real-tick data, so this effectively disables the gate while
    //   leaving the classifier code path intact for diagnostics.
    static constexpr double REGIME_THRESHOLD     = 1.0;

    // 2026-05-08 LOT BUMP / 2026-05-09 LOT REDUCED 0.30 -> 0.01 history
    //   retained in repo notes. S24 deploy stays at 0.01 for live
    //   verification; S33 keeps 0.01 (broker minimum) for shadow forward
    //   test. Promote ONLY after operator sign-off + 50+ clean demo round
    //   trips + broker-pnl reconciliation within ±5% of engine_pnl.
    static constexpr double USD_PER_PT           = 100.0;
    static constexpr double LIVE_LOT             = 0.01;

    // 2026-05-11 S33: warmup gate -- need at least ENTRY_LOOKBACK ticks
    //   to compute the rolling z-score, so the practical warmup is now
    //   max(MIN_ENTRY_TICKS, ENTRY_LOOKBACK) = 200. MIN_ENTRY_TICKS=30
    //   is kept as a hard lower bound for the very-first-tick guard; the
    //   ENTRY_LOOKBACK gate (line below in on_tick) handles the rest.
    static constexpr int    MIN_ENTRY_TICKS      = 30;
    static constexpr int    DIAG_EVERY_N_TICKS   = 600;

    enum class Phase { IDLE, ARMED, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    bool shadow_mode = true;
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

        // 2026-05-08 S21 HEDGING-MODE FIX: BlackBull is hedging, not
        //   netting. Close orders must reference the broker position via
        //   FIX tag 1006 (Spotware) / 721 (FIX 4.4 standard). See
        //   include/order_exec.hpp + include/trade_lifecycle.hpp.
        std::string broker_position_id;
        std::string entry_clOrdId;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    GoldMicroScalperEngine() noexcept = default;

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

        // 2026-05-09 S24: regime classifier update. Maintain a separate
        //   rolling window of REGIME_LOOKBACK mid prices and compute ER
        //   on every tick. Result is cached in m_regime_is_trend for the
        //   entry gate below. Cost: ~200 fabs+sums per tick which is
        //   negligible vs the engine's overall budget. S33: classifier
        //   still runs (kept for diagnostics in the FIRE/DIAG log lines)
        //   but REGIME_THRESHOLD=1.0 makes m_regime_is_trend never true.
        m_regime_window.push_back(mid);
        if ((int)m_regime_window.size() > REGIME_LOOKBACK) {
            m_regime_window.pop_front();
        }
        if ((int)m_regime_window.size() == REGIME_LOOKBACK) {
            const double net = std::fabs(m_regime_window.back() - m_regime_window.front());
            double gross = 0.0;
            for (size_t i = 1; i < m_regime_window.size(); ++i) {
                gross += std::fabs(m_regime_window[i] - m_regime_window[i - 1]);
            }
            m_regime_er = (gross > 1e-9) ? (net / gross) : 0.0;
            m_regime_is_trend = (m_regime_er >= REGIME_THRESHOLD);
        }
        // Pre-warmup: m_regime_is_trend stays at default false (allow entry).

        if (m_ticks_received <= 30 || (m_ticks_received % DIAG_EVERY_N_TICKS) == 0) {
            double mean = 0.0, sd = 0.0, z = 0.0;
            const bool stats_ok = _rolling_stats(mean, sd);
            if (stats_ok && sd > 0.0) z = (mid - mean) / sd;
            char _buf[640];
            std::snprintf(_buf, sizeof(_buf),
                "[MICRO-SCALPER-GOLD-DIAG] ticks=%d phase=%d window=%d/%d mid=%.2f "
                "spread=%.2f z=%.2f l2_imb=%.2f slope=%.2f l2_real=%d "
                "regime_er=%.3f trend=%d\n",
                m_ticks_received, (int)phase, (int)m_window.size(), ENTRY_LOOKBACK,
                mid, spread, z, l2_imbalance, book_slope, (int)l2_real,
                m_regime_er, (int)m_regime_is_trend);
            std::cout << _buf;
            std::cout.flush();
        }

        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) {
                phase = Phase::IDLE;
                m_cooldown_dir = 0;
            }
        }

        if (phase == Phase::LIVE) {
            _manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // DEEPSTRIKE kill-switch
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

        // 2026-05-11 S33: SESSION_START_HOUR=0, SESSION_END_HOUR=7 means
        //   the in_window check below admits hours 0-6 UTC inclusive
        //   (h >= 0 && h < 7), totalling 7 hours/day. Backtest sweep
        //   TOP-1 was --session 0-7 (Asia only).
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

            if constexpr (PRE_LONDON_DEAD_HOUR_UTC >= 0) {
                if (h == PRE_LONDON_DEAD_HOUR_UTC) return;
            }
        }

        // 2026-05-09 S24: REGIME GATE. S33 disabled by setting
        //   REGIME_THRESHOLD=1.0 (m_regime_is_trend never flips true).
        //   Code path retained so the classifier diagnostics still
        //   populate the DIAG/FIRE log lines.
        if (m_regime_is_trend) return;

        // -- Entry signal: 200-tick z-score with L2 confirmation (S33) --------
        double mean = 0.0, sd = 0.0;
        if (!_rolling_stats(mean, sd)) return;
        if (sd < 0.05) return;
        const double z = (mid - mean) / sd;

        const bool block_long  = (phase == Phase::COOLDOWN && m_cooldown_dir == +1);
        const bool block_short = (phase == Phase::COOLDOWN && m_cooldown_dir == -1);

        const bool z_signals_long  = (z <= -ENTRY_Z);
        const bool z_signals_short = (z >=  ENTRY_Z);

        bool l2_ok_long  = true;
        bool l2_ok_short = true;
        if (l2_real) {
            l2_ok_long  = (l2_imbalance >= L2_IMB_LONG_MIN)  || vacuum_ask;
            l2_ok_short = (l2_imbalance <= L2_IMB_SHORT_MAX) || vacuum_bid;
            if (book_slope <= -L2_FLIP_THRESH) l2_ok_long  = false;
            if (book_slope >=  L2_FLIP_THRESH) l2_ok_short = false;
        }

        const bool fire_long  = z_signals_long  && l2_ok_long  && !block_long;
        const bool fire_short = z_signals_short && l2_ok_short && !block_short;

        if (!fire_long && !fire_short) return;
        if (fire_long && fire_short) return;

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

        char _fbuf[640];
        std::snprintf(_fbuf, sizeof(_fbuf),
            "[MICRO-SCALPER-GOLD] FIRE %s @ %.2f sl=%.2f tp=%.2f z=%.2f "
            "spread=%.2f l2_imb=%.2f slope=%.2f l2_real=%d regime_er=%.3f %s\n",
            is_long ? "LONG" : "SHORT",
            fill_px, sl_px, tp_px, z, spread,
            l2_imbalance, book_slope, (int)l2_real,
            m_regime_er,
            shadow_mode ? "[SHADOW]" : "[LIVE]");
        std::cout << _fbuf;
        std::cout.flush();

        if (on_fire_hook) on_fire_hook(now_s);
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double exit_px = pos.is_long ? bid : ask;
        _close(exit_px, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

private:
    int                 m_ticks_received = 0;
    int64_t             m_last_tick_s    = 0;
    int64_t             m_cooldown_start = 0;
    int                 m_cooldown_dir   = 0;
    int                 m_trade_id       = 0;

    std::deque<double>  m_window;
    std::deque<double>  m_micro;

    // 2026-05-09 S24: regime classifier state. Independent of m_window
    //   because REGIME_LOOKBACK (200) was historically much larger than
    //   ENTRY_LOOKBACK (20). After S33's ENTRY_LOOKBACK=200 they happen
    //   to match in size but remain logically separate windows.
    std::deque<double>  m_regime_window;
    double              m_regime_er       = 0.0;
    bool                m_regime_is_trend = false;

    double m_last_book_slope = 0.0;
    double m_last_l2_imb     = 0.5;
    bool   m_last_vacuum_ask = false;
    bool   m_last_vacuum_bid = false;
    bool   m_last_l2_real    = false;

    mutable std::mutex m_close_mtx;

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

    bool _detect_reversal() const noexcept {
        // 2026-05-11 S33: this function is unreachable in the S33 build
        //   because its only call site (_manage, line `if (pos.be_locked
        //   && _detect_reversal())`) is gated on pos.be_locked, which the
        //   S33 BE_TRIGGER_PTS=999 sentinel prevents from ever flipping
        //   true. Logic preserved unchanged so re-enabling BE in a future
        //   tune produces the historically-validated behaviour. If you
        //   re-enable BE you MUST also restore REVERSAL_LOOKBACK to a
        //   positive value (>= 2) before this function executes; the
        //   m_micro[n - REVERSAL_LOOKBACK] indexing below is undefined
        //   behaviour at REVERSAL_LOOKBACK=0 (= m_micro[n], past end).
        if (!pos.active) return false;
        const int n = static_cast<int>(m_micro.size());
        if (n >= REVERSAL_LOOKBACK && REVERSAL_LOOKBACK > 0) {
            const double last  = m_micro[n - 1];
            const double prior = m_micro[n - REVERSAL_LOOKBACK];
            const double delta = last - prior;
            if (pos.is_long  && delta <= -REVERSAL_DELTA_PTS) return true;
            if (!pos.is_long && delta >=  REVERSAL_DELTA_PTS) return true;
        }
        if (pos.l2_real_at_entry && m_last_l2_real) {
            if (pos.is_long  && m_last_book_slope <= -L2_FLIP_THRESH) return true;
            if (!pos.is_long && m_last_book_slope >=  L2_FLIP_THRESH) return true;
        }
        return false;
    }

    void _manage(double bid, double ask, double mid,
                 int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

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

        if (pos.be_locked) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - TRAIL_DIST_PTS)
                : (pos.entry - pos.mfe + TRAIL_DIST_PTS);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        if (pos.be_locked && _detect_reversal()) {
            const double exit_px = pos.is_long ? bid : ask;
            _close(exit_px, "REVERSAL_EXIT", now_s, on_close);
            return;
        }

        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) {
            _close(pos.tp, "TP_HIT", now_s, on_close);
            return;
        }

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

        if ((now_s - pos.entry_ts) >= MAX_HOLD_SEC) {
            _close(mid, "MAX_HOLD_EXIT", now_s, on_close);
            return;
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        std::lock_guard<std::mutex> _lk(m_close_mtx);
        if (!pos.active) return;

        const bool    is_long_   = pos.is_long;
        const double  entry_     = pos.entry;
        const double  sl_        = pos.sl;
        const double  tp_        = pos.tp;
        const double  size_      = pos.size;
        const double  mfe_       = pos.mfe;
        const double  mae_       = pos.mae;
        const double  spread_e_  = pos.spread_at_entry;
        const int64_t entry_ts_  = pos.entry_ts;

        const double pnl =
            (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;

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
        tr.mfe           = mfe_ * size_;
        tr.mae           = mae_ * size_;
        tr.entryTs       = entry_ts_;
        tr.exitTs        = now_s;
        tr.exitReason    = reason;
        tr.spreadAtEntry = spread_e_;
        tr.shadow        = shadow_mode;
        tr.entry_clOrdId = pos.entry_clOrdId;

        m_cooldown_start = now_s;
        m_cooldown_dir   = is_long_ ? +1 : -1;

        // 2026-05-08 S21 HEDGING-MODE FIX (CRITICAL ORDER): on_close MUST run
        //   BEFORE pos is reset so microscalper_on_close can read
        //   pos.broker_position_id (populated by handle_execution_report
        //   on the entry-side ACK). See include/order_exec.hpp.
        if (on_close) on_close(tr);

        pos = LivePos{};
        phase = Phase::COOLDOWN;
    }
};

} // namespace omega
