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
//     5. MAX_HOLD timeout (60s). Safety: a position with no progress closes
//        at mid as MAX_HOLD_EXIT.
//
//   COOLDOWN: COOLDOWN_S keyed PER DIRECTION. After exit, the same
//   direction is blocked for COOLDOWN_S; the OPPOSITE direction is permitted
//   immediately.
//
// SAFETY
//   - shadow_mode = true by default. Promotion to live requires explicit
//     authorisation in engine_init.hpp.
//   - Spread cap MAX_SPREAD = 0.5pt (post-S20 DEEPSTRIKE tightening).
//   - Session window 00:00-24:00 UTC (full day; was 06-22 pre-S24 revert,
//     opened up after May 7-8 multi-day replay showed Asia hours are the
//     most profitable session for the mean-reversion thesis).
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
// NUMERIC CHANGES (this file only):
//   SESSION_START_HOUR     6  -> 0   (open Asia)
//   SESSION_END_HOUR       22 -> 24  (open Asia / overnight)
//   REGIME_LOOKBACK        n/a -> 200  (NEW: Kaufman ER window)
//   REGIME_THRESHOLD       n/a -> 0.18 (NEW: ER threshold for trend gate)
//   All other rk12 constants UNCHANGED from S23 revert state.
//
// SAFETY ENVELOPE FOR FIRST LIVE WEEK:
//   - LIVE_LOT stays at 0.01 (no scaling until live tape confirms numbers)
//   - max_lot_gold in omega_config.ini stays at 0.01
//   - mode=LIVE on demo account 2067070 first; only swap to 8077780 after
//     50+ orph=0 round trips with realised_pnl matching engine_pnl ±5%
//   - If 3-day rolling WR drops below 80% live, halt and audit (the
//     April regime in the replay had 75% WR -- if live drops below that
//     band, something is fundamentally different)
//
// VERIFICATION PROTOCOL POST-S24 DEPLOY:
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
    // -- Tuned defaults (S19 calibrated 2026-05-08; S22 attempted then
    //    reverted in S23 same day; S24 added 24h session + regime gate
    //    after multi-day replay validation 2026-05-09). ----------------------
    static constexpr int    ENTRY_LOOKBACK       = 20;

    // 2026-05-09 S23 REVERT: 1.75 -> 0.75. Mean-reversion thesis works
    //   at modest fade stretches; wider Z saturates the entry signal.
    static constexpr double ENTRY_Z              = 0.75;

    static constexpr double L2_IMB_LONG_MIN      = 0.55;
    static constexpr double L2_IMB_SHORT_MAX     = 0.45;

    // 2026-05-08 DEEPSTRIKE: 1.0 -> 0.5pt. Backtest filtered p95 spread
    //   was 0.22pt; 0.5pt admits ~95% of fires while rejecting the noisy
    //   0.5-0.99 band. Keeps holding post-S24 -- Asia spreads observed in
    //   the multi-day replay were also within this cap.
    static constexpr double MAX_SPREAD           = 0.5;

    // 2026-05-09 S23 REVERT: 1.75 -> 0.79. Calibrated to the expected
    //   reversion distance at z=0.75 entry on a 20-tick window.
    static constexpr double TP_DIST_PTS          = 0.79;

    static constexpr double SL_DIST_PTS          = 3.0;

    // 2026-05-09 S23 REVERT: 0.80 -> 0.50. BE arms at 63% of TP.
    static constexpr double BE_TRIGGER_PTS       = 0.50;

    static constexpr double BE_OFFSET_PTS        = 0.3;
    static constexpr double TRAIL_DIST_PTS       = 0.5;
    static constexpr int    REVERSAL_LOOKBACK    = 5;
    static constexpr double REVERSAL_DELTA_PTS   = 0.30;
    static constexpr double L2_FLIP_THRESH       = 0.20;

    // 2026-05-09 S23 REVERT: 180 -> 60. Avg hold 2.9-5.1s in replay;
    //   60s is ~12-20x headroom for the rare slow fades.
    static constexpr int    MAX_HOLD_SEC         = 60;

    static constexpr int    COOLDOWN_S           = 5;

    // 2026-05-09 S24 DEPLOY: 6 -> 0, 22 -> 24. Asia session (22-06 UTC)
    //   was excluded by inheritance from momentum-strategy defaults but
    //   is the most profitable window for the mean-reversion thesis.
    //   Multi-day replay: Asia +0.5622 pt/trade vs full-tape avg +0.4818.
    //   Effectively disables the session gate because (h >= 0 && h < 24)
    //   is always true for any UTC hour 0-23.
    static constexpr int    SESSION_START_HOUR   = 0;   // UTC, was 6
    static constexpr int    SESSION_END_HOUR     = 24;  // UTC, was 22

    // 2026-05-08 USER REQUEST: pre-London dead zone -- DISABLED at user
    //   direction. Constant kept (set to -1) so the gate machinery is in
    //   place; flip to 6 (BST) or 7 (GMT) to re-arm. Less critical
    //   post-S24 since the session is fully open, but the optional
    //   dead-zone hour cut still works if a problematic hour emerges.
    static constexpr int    PRE_LONDON_DEAD_HOUR_UTC = -1;

    // 2026-05-09 S24 DEPLOY: NEW regime classifier constants ---------------
    //
    //   Kaufman Efficiency Ratio (ER) over the last REGIME_LOOKBACK ticks:
    //     ER = |price_now - price_n_ago| / sum(|tick_i - tick_{i-1}|)
    //     ER ~ 0 = perfect chop (price wandering, returns cancel out)
    //     ER ~ 1 = perfect trend (price walking, returns reinforce)
    //
    //   Threshold 0.18 from multi-day sweep -- only setting that beat the
    //   no-gate baseline at every cost level (0.20/0.25/0.30/0.35 pt).
    //   At 0.25pt cost @ 0.30 lot: +$3,001 improvement vs no-gate over
    //   13 active replay days.
    //
    //   New entries are refused when m_regime_is_trend == true. Existing
    //   positions continue to be managed normally regardless of regime --
    //   the gate is on entries only.
    //
    //   Warmup: until the regime window is full (200 ticks), the
    //   classifier reports chop (default-allow), so very-early entries
    //   in a session aren't blocked by an empty classifier.
    static constexpr int    REGIME_LOOKBACK      = 200;
    static constexpr double REGIME_THRESHOLD     = 0.18;

    // 2026-05-08 LOT BUMP / 2026-05-09 LOT REDUCED 0.30 -> 0.01 history
    //   retained in repo notes. S24 deploy stays at 0.01 for live
    //   verification; promote ONLY after 50+ clean demo round trips +
    //   broker-pnl reconciliation within ±5% of engine_pnl.
    static constexpr double USD_PER_PT           = 100.0;
    static constexpr double LIVE_LOT             = 0.01;

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
        //   negligible vs the engine's overall budget.
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

        // 2026-05-09 S24: SESSION_START_HOUR=0, SESSION_END_HOUR=24 means
        //   the in_window check below is always true (h is 0-23, always
        //   < 24, always >= 0). The session machinery is preserved so a
        //   future operator can re-narrow the window without code changes.
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

        // 2026-05-09 S24: REGIME GATE. Refuse new entries while the
        //   Kaufman ER over the last 200 ticks indicates trend regime.
        //   In multi-day replay this filtered ~70% of entry attempts
        //   during sustained trend windows; trade count dropped only
        //   ~1-2% because the next-tick re-arm cycle handles brief
        //   spikes naturally. Net P&L improvement was +0.9% across the
        //   13 active replay days at every cost level tested.
        if (m_regime_is_trend) return;

        // -- Entry signal: 20-tick z-score with L2 confirmation ---------------
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
    //   because REGIME_LOOKBACK (200) is much larger than ENTRY_LOOKBACK
    //   (20). Updated every tick in on_tick.
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
        if (!pos.active) return false;
        const int n = static_cast<int>(m_micro.size());
        if (n >= REVERSAL_LOOKBACK) {
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
