// =============================================================================
//  GoldFlowEngine.hpp
//  L2 order-flow engine for XAUUSD
//
//  Architecture (prop-desk methodology):
//
//  ENTRY: 3-layer confirmation
//    1. L2 imbalance persistence  -- sustained order-book pressure, not a spike
//       fast  window: 30 ticks  (1-3s)   -- detects signal
//       slow  window: 100 ticks (5-10s)  -- confirms it is sustained flow
//    2. EWM drift confirmation    -- price is actually moving in signal direction
//    3. Momentum confirmation     -- mid moving in signal direction over 5 ticks
//
//  SL: ATR(20) * 1.0
//    Sized to actual market volatility.
//    If price moves 1 ATR against the trade, the thesis is invalidated.
//    Typical gold: 8-12pts normal, 15-20pts volatile.
//
//  SIZING: risk_dollars / SL_pts
//    Fixed dollar risk per trade. Adapts automatically to volatility.
//
//  EXIT: Staircase + Tiered trail
//    Step 1 (+1x ATR):  bank 33%, SL ? entry (BE). Trail arms at 1.0x ATR.
//    Step 2 (+2x ATR):  bank 33% of remainder. Trail tightens to 0.5x ATR.
//    Step 3 (+3x ATR):  bank 33% of remainder. Trail tightens to 0.25x ATR.
//    Final remainder:   0.25x ATR tight trail -- protect every tick.
//
//  This captures $100-400 trend moves while keeping risk tight on entry.
//  On today's $400 gold drop: enter short at compression, trail tightens
//  progressively, exits near the low with most of the move captured.
//
//  COOLDOWN: 30s after any exit. Prevents overtrading after a move exhausts.
//
//  KNOWN GAP -- MAKER EXECUTION (Problem 3):
//    Current engine uses market orders (entry_px = ask for long, bid for short).
//    The flow intelligence spec calls for LIMIT orders at mid to save ~$0.30/trade
//    (half-spread). At 20 trades/day that's ~$6/day or ~$1500/year on $30 risk.
//    Implementation requires:
//      1. Send LIMIT order at mid price via FIX tag 40=2
//      2. Track pending limit order ID
//      3. On fill ACK ? transition to LIVE
//      4. On timeout (e.g. 500ms) ? cancel limit, optionally send market
//    This is a main.cpp FIX dispatch change, not an engine change.
//    Currently deferred -- market orders work correctly, limit adds fill-rate risk.
//
//    #include "GoldFlowEngine.hpp"
//    static GoldFlowEngine g_gold_flow;
//    // each tick:
//    g_gold_flow.on_tick(bid, ask, l2_imb, ewm_drift, now_ms, on_close);
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include "OmegaTradeLedger.hpp"

// -----------------------------------------------------------------------------
//  Config constants
// -----------------------------------------------------------------------------
static constexpr int    GFE_FAST_TICKS        = 30;    // fast persistence window
static constexpr int    GFE_SLOW_TICKS        = 60;    // slow confirmation window -- lowered 100?60:
                                                        // 100 ticks @10/s = 10s of unbroken flow required.
                                                        // Any micro-oscillation in a grind reset the window.
                                                        // 60 ticks = ~6s, still meaningful confirmation.
static constexpr double GFE_LONG_THRESHOLD    = 0.75;  // bid-heavy: long signal
static constexpr double GFE_SHORT_THRESHOLD   = 0.25;  // ask-heavy: short signal
static constexpr double GFE_DRIFT_MIN         = 0.0;   // drift must be non-zero same dir
// Drift-persistence fallback (used when L2 size data is unavailable -- imbalance always 0.5)
// On BlackBull, L2 size data is NEVER sent (tag-271 always omitted) so this is the
// PERMANENT operating mode, not an exceptional fallback. Threshold lowered 1.5?0.5:
// 1.5 was calibrated for mid-session L2 dropout protection but blocks real slow trends
// where ewm_drift reaches $0.8-$1.2 but never $1.5. The chop guard (drift range>4.0)
// handles chop protection -- that's the correct filter, not a high threshold.
static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.5;  // was 1.5 -- too strict for no-L2 broker
// 20 ticks (~2s London) -- sufficient for real directional moves.
// The chop guard (drift range > 4.0) blocks oscillating markets regardless.
// 40 was too long for slow grinding trends where drift is consistent but small.
static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker
static constexpr int    GFE_ATR_PERIOD        = 100;   // ATR lookback ticks -- raised 20?100:
static constexpr int    GFE_ATR_RANGE_WINDOW  = 100;   // raised 20?100: 20 ticks = 2s window at London
                                                        // = pure spread noise ($0.2-0.5pts), not real ATR.
                                                        // 100 ticks = ~10s, captures genuine intrabar moves.
                                                        // Evidence: 20-tick range was 0.3pts on active tape,
                                                        // EWM smoothed to ~0.5pts ? ATR_MIN floor of 5pts
                                                        // was the only thing preventing $0.5 SLs.
                                                        // 20-tick hi-lo range was a 2-second window,
                                                        // producing SL of $0.3-5 depending on micro-volatility.
                                                        // 100 ticks = ~10-30s, EWM-smoothed, stable across sessions.
static constexpr double GFE_ATR_EWM_ALPHA     = 0.05;  // EWM smoothing alpha for ATR (20-tick equivalent half-life)
static constexpr double GFE_ATR_MIN           = 5.0;   // raised 2.0?5.0: XAUUSD@$4554, 2pt ATR = 0.044%
                                                        // = noise level, hit by spread fluctuation alone.
                                                        // 5pt minimum = 0.11% = survives a real tick move.
                                                        // VIX27 day real ATR is 8-18pts, this is a safe floor.
static constexpr double GFE_ATR_SL_MULT       = 1.0;   // SL = ATR * this
// Trail distance constants -- tiered trail now uses inline trail_mult in manage_position().
// These constants are retired (tiered: 1.0x?0.5x?0.25x ATR by staircase stage).
// Kept as named values only for any external code that may reference them.
static constexpr double GFE_TRAIL_STAGE2_MULT = 0.50;  // retired -- see tiered trail in manage_position
static constexpr double GFE_TRAIL_STAGE3_MULT = 0.25;  // retired -- was 0.35
static constexpr double GFE_TRAIL_STAGE4_MULT = 0.25;  // retired -- final stage
static constexpr double GFE_BE_ATR_MULT       = 1.0;   // BE lock fires with stair step 1
static constexpr double GFE_PARTIAL_EXIT_R    = 1.0;   // stair step 1 trigger (1?ATR)
static constexpr double GFE_PARTIAL_EXIT_FRAC = 0.50;  // fraction to close at partial exit trigger
static constexpr double GFE_STAGE2_ATR_MULT   = 1.0;   // stair step 1 (same as BE)
static constexpr double GFE_STAGE3_ATR_MULT   = 2.0;   // stair step 2
static constexpr double GFE_STAGE4_ATR_MULT   = 6.0;   // full trail stage (GUI badge)
static constexpr double GFE_MAX_SPREAD        = 2.5;   // pts -- London gold spread $1.50-$4.00; old 0.6 blocked all entries
static constexpr int    GFE_MIN_HOLD_MS       = 5000;   // 5s minimum hold
static constexpr int    GFE_MAX_HOLD_MS       = 3600000; // 60 min -- EA has no hold limit, keep generous -- prevents indefinite holds on flat tape
                                                          // If the trail has not advanced past Stage 2 by 30 min, the thesis is stale.
                                                          // Observed: held 31 min with no exit because gold went flat after entry,
                                                          // Stage 1 BE locked but trail never tightened -- exit at BE/mid.
static constexpr int    GFE_COOLDOWN_MS       = 30000;  // 30s cooldown after exit
// Minimum ticks that must be RECEIVED (not just warmed-up) before any entry is
// allowed. seed() and load_atr_state() bypass the ATR warmup counter so the
// engine can fire within seconds of a restart on seeded data. This guard is
// unconditional -- it cannot be bypassed by seed or load -- and ensures the
// persistence windows have been fed enough REAL market ticks before entry.
// At ~5 ticks/s Asia / ~10 ticks/s London: 50 ticks ? 5-10s real time.
// Reduced from 150: ATR loads from disk (gold_flow_atr.dat) so the 100-tick
// ATR blind zone is already bypassed. 50 ticks is enough for persistence
// windows to fill with real market data before first entry is allowed.
static constexpr int    GFE_MIN_ENTRY_TICKS   = 50;    // ~5-10s real time before first entry allowed
static constexpr double GFE_RISK_DOLLARS      = 30.0;  // $ risk per trade (fallback)
static constexpr double GFE_MIN_LOT           = 0.01;
static constexpr double GFE_MAX_LOT           = 1.0;
static constexpr int    GFE_MOMENTUM_TICKS    = 12;    // ticks back for momentum check (int -- used as array index)
                                                        // 12 ticks = ~120-240ms at London, ~2-4s at Asia -- real directional move.
static constexpr int    GFE_MOMENTUM_BUF_SIZE = 64;    // independent momentum history buffer (separate from ATR buffer)

// Asia/dead-zone session hardening -- prevents entries on choppy mean-reverting tape
// Asia gold (22:00-07:00 UTC) has: thin liquidity, micro-oscillations that fake
// directional flow, tight ATR that produces SL easily hit by noise.
// These thresholds require genuinely committed moves before entry.
static constexpr double GFE_ASIA_ATR_MIN           = 1.5;    // $1.5 ATR floor -- real XAUUSD Asia ATR is 1.5-3.5pts; 5.0 was blocking all legitimate Asia moves
static constexpr double GFE_ASIA_MAX_SPREAD        = 2.5;    // raised $1.50?$2.50: $1.50 blocked gap-open moves where spread
                                                              // is temporarily $2-3 even as ATR is $15+. The ATR/spread ratio
                                                              // guard (GFE_ASIA_ATR_SPREAD_RATIO=4.0) is the real noise filter:
                                                              // on a $100 drop ATR>=$15, spread $2.50 ? ratio=6.0 >> 4.0 ? PASS.
                                                              // On thin chop ATR=$2, spread $2.50 ? ratio=0.8 << 4.0 ? BLOCK.
                                                              // The spread cap alone was a blunt instrument; ratio does it better.
static constexpr double GFE_ASIA_DRIFT_MIN         = 1.50;   // ALIGNED with outer asia_trend_ok gate (main.cpp: |drift|>=1.5).
                                                              // Was 0.50: GoldFlow inner gate allowed entries at $0.50 drift while
                                                              // the outer bracket/flow gate required $1.50. Gap meant GFE fired
                                                              // on weak Asia moves that had already been blocked at the outer gate,
                                                              // creating phantom entries in log with no corresponding bracket entry.
                                                              // Both gates now require $1.50 -- consistent Asia drift threshold.
static constexpr double GFE_ASIA_MOMENTUM_MIN      = 0.30;   // mid must move $0.30+ over momentum ticks (vs $0 normal)
static constexpr int    GFE_ASIA_COOLDOWN_MS       = 60000;  // 60s cooldown (vs 30s normal) -- fewer attempts on bad tape
static constexpr double GFE_ASIA_ATR_SPREAD_RATIO  = 4.0;    // ATR must be >= 4x spread -- the primary noise filter.
                                                              // $100 drop: ATR~$15, spread $2 ? ratio=7.5 ? PASS.
                                                              // Normal chop: ATR~$2, spread $1.50 ? ratio=1.3 ? BLOCK.
// Persistence thresholds for Asia: 90% of window must be directional (vs 75% normal)
// On choppy tape, 75% easily fills from random oscillations. 90% requires real conviction.
static constexpr int    GFE_ASIA_FAST_DIR_THRESHOLD = (GFE_FAST_TICKS * 4) / 5;   // 24/30 ticks (80%) -- was 90%, too strict for Asia tape
static constexpr int    GFE_ASIA_SLOW_DIR_THRESHOLD = (GFE_SLOW_TICKS * 4) / 5;   // 80/100 ticks (80%) -- was 90%
// Dominance: max 2 opposing ticks in fast window (vs 7 normal)
static constexpr int    GFE_ASIA_DOMINANCE_MAX_OPPOSING = 4;  // 13% opposing allowed -- was 2 (6.6%) which rejected any micro-oscillation

// -----------------------------------------------------------------------------
struct GoldFlowEngine {

    // -------------------------------------------------------------------------
    // Public config -- set after construction
    double risk_dollars   = GFE_RISK_DOLLARS;  // override from main.cpp

    // -------------------------------------------------------------------------
    // Observable state
    enum class Phase { IDLE, FLOW_BUILDING, LIVE, COOLDOWN } phase = Phase::IDLE;

    struct OpenPos {
        bool    active           = false;
        bool    is_long          = false;
        double  entry            = 0.0;
        double  sl               = 0.0;
        double  size             = 0.01;
        double  mfe              = 0.0;  // max favourable excursion (pts)
        double  atr_at_entry     = 0.0;  // ATR when trade was entered
        bool    be_locked        = false;
        int     trail_stage      = 0;    // 0=initial, 1=step1, 2=step2, 3=full trail, 4=extended
        bool    stage2_tight     = false; // unused by staircase -- kept for ABI compat
        bool    partial_closed   = false; // true = stair step 1 (PARTIAL_1R) already taken
        bool    partial_closed_2 = false; // true = stair step 2 (PARTIAL_2R) already taken
        double  full_size        = 0.0;  // original size before any partial -- for reporting
        int64_t entry_ts         = 0;
        // ?? Dollar-ratchet lock ???????????????????????????????????????????????
        // Tracks the highest dollar-ratchet tier that has been locked in.
        // Each tier represents a $50 profit interval. At each tier the SL is
        // moved to guarantee (tier ? $50) - $10 of profit is kept regardless
        // of future price action.
        // Example with 0.1 lot (1pt=$10):
        //   Tier 1 ($50 open): SL ? entry + 4pts  (locks $40 minimum)
        //   Tier 2 ($100 open): SL ? entry + 9pts  (locks $90 minimum)
        //   Tier 3 ($150 open): SL ? entry + 14pts (locks $140 minimum)
        // Ratchet only moves SL forward -- never backward.
        int     dollar_lock_tier = 0;    // highest tier fired (0=none)
        double  dollar_lock_sl   = 0.0;  // SL level set by dollar ratchet
    } pos;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // -------------------------------------------------------------------------
    // Main tick function -- call every tick with fresh data
    // bid, ask       : current quotes
    // l2_imb         : imbalance 0..1 from L2Book::imbalance()
    // ewm_drift      : GoldEngineStack::ewm_drift() -- fast-slow EWM
    // now_ms         : current epoch ms
    // on_close       : callback when position closes
    // -------------------------------------------------------------------------
    // session_slot: 0=dead, 1=London, 2=London_core, 3=overlap, 4=NY, 5=NY_late, 6=Asia
    void on_tick(double bid, double ask,
                 double l2_imb, double ewm_drift,
                 int64_t now_ms,
                 CloseCallback on_close,
                 int session_slot = -1) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        m_last_session_slot = session_slot;

        // Session classification: Asia (slot 6) and dead zone (slot 0) are low-quality tape
        const bool is_low_quality_session = (session_slot == 6 || session_slot == 0);
        const double eff_max_spread = is_low_quality_session ? GFE_ASIA_MAX_SPREAD : GFE_MAX_SPREAD;

        // Feed ATR and momentum history (always -- warmup must run every tick)
        update_atr(spread, mid);

        // Cooldown phase -- duration set by exit reason (see close_position)
        if (phase == Phase::COOLDOWN) {
            const int cd = (m_exit_cooldown_ms > 0) ? m_exit_cooldown_ms
                         : (is_low_quality_session  ? GFE_ASIA_COOLDOWN_MS : GFE_COOLDOWN_MS);
            if (now_ms - m_cooldown_start >= cd)
                phase = Phase::IDLE;
            else return;
        }

        // Manage open position
        if (phase == Phase::LIVE) {
            manage_position(bid, ask, mid, spread, l2_imb, now_ms, on_close);
            return;
        }

        // ?? FIX (claim 3): Block ALL signal accumulation until ATR is warmed up ??
        if (m_atr_warmup_ticks < GFE_ATR_PERIOD) return;

        // ?? Cold-start entry gate -- cannot be bypassed by seed() or load_atr_state() ??
        // seed() sets m_atr_warmup_ticks = GFE_ATR_PERIOD immediately, bypassing the
        // 100-tick ATR gate above. That means entries could fire within seconds of a
        // restart on seeded data before the persistence windows contain real market
        // ticks. m_ticks_received is incremented every tick in update_atr() and is
        // never touched by seed/load -- it is the only uncircumventable warmup guard.
        // If ALL telemetry fields are zero (xau_recent_vol_pct=0, xau_baseline_vol_pct=0)
        // then this gate is what prevents entry. Block signal accumulation too so the
        // persistence windows do not pre-fill with cold-start ticks that will fire
        // immediately once the gate lifts.
        if (m_ticks_received < GFE_MIN_ENTRY_TICKS) return;

        // ?? FIX (claim 4): Update persistence BEFORE spread gate ??????????????
        update_persistence(l2_imb, now_ms);

        // Spread gate -- session-aware: tighter during Asia/dead zone
        if (spread > eff_max_spread) return;

        // ?? Asia/dead-zone ATR quality gate ??????????????????????????????????
        // On low-quality tape, reject if ATR is too low (noise-dominated) or
        // ATR-to-spread ratio is too small (SL within spread fluctuation range).
        if (is_low_quality_session) {
            if (m_atr < GFE_ASIA_ATR_MIN) return;  // tape too dead
            if (spread > 0.0 && m_atr / spread < GFE_ASIA_ATR_SPREAD_RATIO) return;  // SL within noise
        }

        // ?? L2 availability detection ?????????????????????????????????????????
        // BlackBull FIX feed sends no tag-271 size data ? imbalance() always 0.500.
        // When L2 is structurally unavailable, the persistence windows fill with
        // all-neutral ticks and can NEVER reach directional threshold.
        // In that case use DRIFT-PERSISTENCE mode: EWM drift sustained for
        // GFE_DRIFT_PERSIST_TICKS consecutive ticks + stronger momentum threshold.
        // This is still a real confirmation signal -- just price-based not book-based.
        //
        // CRITICAL: If L2 was live earlier this session but is now 0.500 (neutral),
        // it means the depth feed dropped mid-session. Block entries entirely rather
        // than falling back to drift -- a temporarily broken feed is not the same as
        // a structurally unavailable feed (different broker). Drift fallback caused
        // massive overtrading when depth feed dropped today (10+ losses vs 3 on Friday).
        const bool l2_data_live = (std::fabs(l2_imb - 0.5) > 0.001);
        // Track whether L2 was ever live this session -- if it was and now isn't, block
        if (l2_data_live) {
            m_l2_was_live = true;
        } else if (!m_l2_was_live) {
            // L2 never live this session -- log once so it's visible in startup output
            static bool s_no_l2_logged = false;
            if (!s_no_l2_logged && m_ticks_received >= GFE_MIN_ENTRY_TICKS) {
                s_no_l2_logged = true;
                printf("[GFE] *** L2 SIZE DATA UNAVAILABLE -- broker not sending tag-271. "
                       "Operating in drift-persistence mode permanently. "
                       "threshold=%.1f persist_ticks=%d ***\n",
                       GFE_DRIFT_FALLBACK_THRESHOLD, GFE_DRIFT_PERSIST_TICKS);
                fflush(stdout);
            }
        }
        if (!l2_data_live && m_l2_was_live && !is_low_quality_session) return; // L2 dropped mid-session -- block

        // Session-aware persistence thresholds: Asia requires 90% dominance, normal 75%
        // Continuation mode: lower persistence threshold for first re-entry after
        // a profitable close. 60% instead of 75% (18/30 instead of 23/30 fast ticks).
        // Clears if expired or after entry (entry() clears fast/slow windows).
        if (m_continuation_mode && now_ms > m_continuation_expires_ms) {
            m_continuation_mode = false;
        }
        const bool cont_mode = m_continuation_mode && !is_low_quality_session;
        const int eff_fast_thresh = is_low_quality_session ? GFE_ASIA_FAST_DIR_THRESHOLD
                                  : (cont_mode ? (GFE_FAST_TICKS * 3 / 5)  // 60% = 18/30
                                               : GFE_FAST_DIR_THRESHOLD);   // 75% = 23/30
        const int eff_slow_thresh = is_low_quality_session ? GFE_ASIA_SLOW_DIR_THRESHOLD
                                  : (cont_mode ? (GFE_SLOW_TICKS * 3 / 5)  // 60% = 60/100
                                               : GFE_SLOW_DIR_THRESHOLD);   // 75% = 75/100

        bool fast_long, fast_short, slow_long, slow_short;
        if (l2_data_live) {
            // Normal path: rolling window imbalance persistence
            fast_long  = (m_fast_long_count  >= eff_fast_thresh);
            fast_short = (m_fast_short_count >= eff_fast_thresh);
            slow_long  = (m_slow_long_count  >= eff_slow_thresh);
            slow_short = (m_slow_short_count >= eff_slow_thresh);
        } else {
            // Fallback path: L2 unavailable -- use drift persistence counter
            // Update drift persistence window (stores raw drift values for chop detection)
            const int drift_dir = (ewm_drift > GFE_DRIFT_FALLBACK_THRESHOLD)  ?  1
                                 : (ewm_drift < -GFE_DRIFT_FALLBACK_THRESHOLD) ? -1
                                 : 0;
            m_drift_persist_window.push_back(drift_dir);
            m_drift_val_window.push_back(ewm_drift);  // raw values for range check
            if ((int)m_drift_persist_window.size() > GFE_DRIFT_PERSIST_TICKS)
                m_drift_persist_window.pop_front();
            if ((int)m_drift_val_window.size() > GFE_DRIFT_PERSIST_TICKS)
                m_drift_val_window.pop_front();

            // ?? Chop guard: if drift range (max-min) > 4.0 over window, market
            // is oscillating -- do not enter regardless of persistence count.
            // Evidence: 2026-03-30 drift swung +2.2/-1.8 every 10s; persistence
            // window filled with 14/20 directional ticks from noise alone.
            double drift_min = ewm_drift, drift_max = ewm_drift;
            for (double v : m_drift_val_window) {
                if (v < drift_min) drift_min = v;
                if (v > drift_max) drift_max = v;
            }
            const bool drift_choppy = (drift_max - drift_min) > 4.0;

            int drift_long_count = 0, drift_short_count = 0;
            for (int d : m_drift_persist_window) {
                if (d ==  1) ++drift_long_count;
                if (d == -1) ++drift_short_count;
            }
            // Require 70% of drift ticks directional (stricter than L2 path's 75% of 30)
            const int drift_thresh = (GFE_DRIFT_PERSIST_TICKS * 7) / 10;
            fast_long  = !drift_choppy && (drift_long_count  >= drift_thresh);
            fast_short = !drift_choppy && (drift_short_count >= drift_thresh);
            if (drift_choppy) {
                static int64_t s_chop_log = 0;
                if (now_ms - s_chop_log > 10000) {
                    s_chop_log = now_ms;
                    std::cout << "[GFE-CHOP] drift range=" << (drift_max - drift_min)
                              << " (min=" << drift_min << " max=" << drift_max
                              << ") -- chop guard active, no entry\n";
                    std::cout.flush();
                }
            }
            // For slow confirmation, require the same drift ratio over the full window
            slow_long  = fast_long;   // drift persistence IS the slow confirmation
            slow_short = fast_short;
        }

        if (fast_long || fast_short) phase = Phase::FLOW_BUILDING;
        else { phase = Phase::IDLE; return; }

        // ?? FIX (claim 5): Directional dominance -- reject mixed windows ????????
        // OLD: fast_long checked m_fast_long_count >= threshold independently.
        // A window of 23 long + 7 short ticks passed even though 30% was opposing.
        // NEW: require the opposing direction count is < 25% of the window.
        // This ensures the window is genuinely one-directional, not mixed.
        // dominance_threshold: normal = 25% opposing allowed (7/30), Asia = max 2 opposing
        static constexpr int GFE_DOMINANCE_MAX_OPPOSING = GFE_FAST_TICKS / 4; // 7/30
        const int eff_dominance_max = is_low_quality_session ? GFE_ASIA_DOMINANCE_MAX_OPPOSING : GFE_DOMINANCE_MAX_OPPOSING;
        if (fast_long  && m_fast_short_count >= eff_dominance_max) {
            phase = Phase::IDLE; return;  // too much opposing pressure -- not clean flow
        }
        if (fast_short && m_fast_long_count  >= eff_dominance_max) {
            phase = Phase::IDLE; return;
        }

        // Momentum: mid vs GFE_MOMENTUM_TICKS ticks ago (12 ticks = ~120-240ms London)
        const double momentum = mid_momentum();

        // Drift threshold: session-aware + L2-availability-aware.
        // Normal session: L2 available = just non-zero, L2 unavailable = $0.30 fallback.
        // Asia/dead zone: always at least $0.50 drift required -- micro-oscillations don't count.
        double drift_threshold = l2_data_live ? GFE_DRIFT_MIN : GFE_DRIFT_FALLBACK_THRESHOLD;
        if (is_low_quality_session) drift_threshold = std::max(drift_threshold, GFE_ASIA_DRIFT_MIN);

        // Momentum floor: Asia requires $0.30+ price movement (vs any non-zero normally).
        // Prevents entries on sub-$0.10 momentum ticks that dominate choppy overnight tape.
        const double momentum_floor = is_low_quality_session ? GFE_ASIA_MOMENTUM_MIN : 0.0;

        // ?? Chop filter: require price to be actually moving, not oscillating ??
        // Even with clean tick direction, if price range < 0.8x ATR it's noise.
        // Reduces false entries on dead tape / tight-range chop during Asia.
        // Skip this check if ATR not yet built (is_impulsive() returns true in that case).
        const bool price_expanding = is_impulsive();

        // ?? Macro trend bias filter ???????????????????????????????????????????
        // Block counter-trend entries when VWAP deviation signals strong directionality.
        // Without this, GFE's local EWM momentum catches micro-pullbacks INSIDE a trend
        // and fires counter-trend entries (evidence: 3? SHORT at 4440 on +120pt Friday).
        //
        // Two-level gate (BOTH must agree to block):
        //   Level 1: |momentum| > 0.05% of price = price meaningfully above/below VWAP
        //   Level 2 (soft): supervisor conf > 1.5 OR it's a TREND_CONTINUATION regime
        //     ? Level 2 off when VWAP not yet warmed (momentum=0) ? no false blocks
        //
        // SHORT blocked when: momentum > +0.05%  (price above VWAP = uptrend)
        // LONG  blocked when: momentum < -0.05%  (price below VWAP = downtrend)
        // Neutral zone (-0.05%, +0.05%): both directions open -- no regime detected
        static constexpr double GFE_TREND_BIAS_THRESHOLD = 0.05; // % of price
        const bool vwap_trend_up   = (m_trend_momentum >  GFE_TREND_BIAS_THRESHOLD)
                                   && (m_sup_conf > 1.5 || m_sup_is_trend);
        const bool vwap_trend_down = (m_trend_momentum < -GFE_TREND_BIAS_THRESHOLD)
                                   && (m_sup_conf > 1.5 || m_sup_is_trend);

        const bool long_signal  = fast_long
                                  && slow_long
                                  && ewm_drift > drift_threshold
                                  && momentum > momentum_floor
                                  && price_expanding
                                  && !vwap_trend_down;   // don't go long in confirmed downtrend

        const bool short_signal = fast_short
                                  && slow_short
                                  && ewm_drift < -drift_threshold
                                  && momentum < -momentum_floor
                                  && price_expanding
                                  && !vwap_trend_up;     // don't go short in confirmed uptrend

        if (!long_signal && !short_signal) return;

        // ATR check (redundant after warmup gate above, but kept as safety)
        if (m_atr <= 0.0) return;

        // ?? Stale imbalance check at entry ????????????????????????????????????
        // Persistence windows confirm imbalance was sustained over last 30/100 ticks,
        // but current tick's imbalance may have already flipped neutral.
        //
        // BROKER FALLBACK: BlackBull's FIX feed sends tag 269= (side) but NOT tag 271=
        // (MDEntrySize). With zero sizes on both sides, imbalance() returns exactly 0.5
        // Stale imbalance check -- only when L2 data is actually live.
        // Strong drift (>$3) overrides the stale check -- a $3+ EWM drift signals
        // a genuine directional move regardless of book balance. Large traders
        // often split orders across both sides (neutral book) while still moving
        // price strongly. The 22:00 UTC $20 drop had drift=7 but imb=0.502 -- the
        // stale check was blocking valid high-momentum entries.
        if (l2_data_live) {
            const double strong_drift = 3.0;  // $3+ drift overrides stale check
            if (long_signal  && l2_imb < 0.60 && ewm_drift < strong_drift) {
                std::cout << "[GOLD-FLOW] SIGNAL_STALE long -- imb=" << l2_imb
                          << " (need >0.60 or drift>" << strong_drift << ")\n";
                std::cout.flush();
                return;
            }
            if (short_signal && l2_imb > 0.40 && ewm_drift > -strong_drift) {
                std::cout << "[GOLD-FLOW] SIGNAL_STALE short -- imb=" << l2_imb
                          << " (need <0.40 or drift<-" << strong_drift << ")\n";
                std::cout.flush();
                return;
            }
        }

        // Fire entry
        enter(long_signal, mid, bid, ask, spread, now_ms);
    }

    bool has_open_position() const noexcept {
        return phase == Phase::LIVE;
    }

    bool is_in_cooldown() const noexcept {
        return phase == Phase::COOLDOWN;
    }

    bool is_in_continuation_mode() const noexcept {
        return m_continuation_mode;
    }

    // reset_drift_persistence() -- clears the 20-tick drift-persistence window.
    // Called alongside GoldEngineStack::reset_drift_on_reversal() when a confirmed
    // reversal is detected after a GFE close. Without this, the old directional
    // ticks (e.g. 18/20 SHORT ticks from Move 1) block the LONG signal for another
    // 14 ticks even after the EWM drift has been snapped. Clearing the window lets
    // incoming LONG ticks immediately build the 14/20 threshold needed to fire.
    void reset_drift_persistence() noexcept {
        m_drift_persist_window.clear();
        m_drift_val_window.clear();
        // Also reset the fast/slow direction windows -- they still contain stale ticks
        // from the prior move. Without this, even with positive drift, the persistence
        // gate requires 23/30 LONG ticks to accumulate. Clearing lets fresh ticks dominate.
        m_fast_window.clear(); m_slow_window.clear();
        m_fast_long_count = m_fast_short_count = 0;
        m_slow_long_count = m_slow_short_count = 0;
        printf("[GFE-PERSIST-RESET] Direction windows cleared for reversal\n");
        fflush(stdout);
    }

    // ?? Reload API -- called from main.cpp ?????????????????????????????????????
    // reload_pending(): true when a PARTIAL_1R has fired and a reload entry is
    //   waiting for confirmation ticks.
    bool reload_pending() const noexcept { return m_reload_pending; }

    // reload_direction(): +1 = reload is LONG, -1 = reload is SHORT
    int  reload_direction() const noexcept {
        return m_reload_pending ? (m_reload_is_long ? 1 : -1) : 0;
    }

    // reload_is_long(): direction of armed reload
    bool reload_is_long() const noexcept { return m_reload_is_long; }

    // reload_atr(): ATR at time of arming -- for main.cpp to size the reload
    double reload_atr() const noexcept { return m_reload_atr_at_arm; }

    // cancel_reload(): disarm without firing -- called when conditions fail
    void cancel_reload() noexcept {
        if (m_reload_pending) {
            printf("[GFE-RELOAD] CANCELLED price_at_arm=%.2f tick_count=%d\n",
                   m_reload_price_at_arm, m_reload_tick_count);
            fflush(stdout);
        }
        m_reload_pending   = false;
        m_reload_tick_count = 0;
    }

    // try_reload(): called every tick from main.cpp when reload_pending().
    //   bid/ask/mid: current quotes
    //   ewm_drift:   current GoldStack drift (direction confirmation)
    //   now_ms:      current epoch ms
    //
    // Returns true when all confirmation gates pass -- main.cpp then fires
    // force_entry() on g_gold_flow_reload (separate independent instance).
    // Returns false if still waiting for confirmation OR if cancelled.
    //
    // Safety gates (ALL must pass to fire):
    //   1. Price moved >= RELOAD_MIN_CONFIRM_PTS further in reload direction since arm
    //   2. No retrace >= RELOAD_CANCEL_RETRACE ? ATR from arm price
    //   3. Spread <= original entry spread ? 1.5
    //   4. Drift still aligned with reload direction
    //   5. Armed within RELOAD_TIMEOUT_MS (5s) -- don't chase stale signal
    //   6. At least RELOAD_MIN_CONFIRM_TICKS ticks of confirmation
    //
    static constexpr double  RELOAD_MIN_CONFIRM_PTS   = 0.3;  // min pts move after arm
    static constexpr double  RELOAD_CANCEL_RETRACE    = 0.5;  // cancel if retraces 0.5?ATR
    static constexpr int64_t RELOAD_TIMEOUT_MS        = 5000; // 5s timeout
    static constexpr int     RELOAD_MIN_CONFIRM_TICKS = 3;    // minimum confirmation ticks

    bool try_reload(double bid, double ask, double mid, double spread,
                    double ewm_drift, int64_t now_ms) noexcept
    {
        if (!m_reload_pending) return false;

        ++m_reload_tick_count;

        // ?? Timeout gate ??????????????????????????????????????????????????????
        if (now_ms - m_reload_armed_ms > RELOAD_TIMEOUT_MS) {
            printf("[GFE-RELOAD] TIMEOUT after %dms -- cancelled\n",
                   static_cast<int>(now_ms - m_reload_armed_ms));
            fflush(stdout);
            cancel_reload();
            return false;
        }

        // ?? Retrace cancel gate ???????????????????????????????????????????????
        const double retrace = m_reload_is_long
            ? (m_reload_price_at_arm - mid)
            : (mid - m_reload_price_at_arm);
        if (retrace >= m_reload_atr_at_arm * RELOAD_CANCEL_RETRACE) {
            printf("[GFE-RELOAD] RETRACE_CANCEL retrace=%.2f >= %.2f?ATR(%.2f)\n",
                   retrace, RELOAD_CANCEL_RETRACE, m_reload_atr_at_arm);
            fflush(stdout);
            cancel_reload();
            return false;
        }

        // ?? Drift gate ????????????????????????????????????????????????????????
        const bool drift_aligned = m_reload_is_long ? (ewm_drift >= 0.0) : (ewm_drift <= 0.0);
        if (!drift_aligned) {
            printf("[GFE-RELOAD] DRIFT_CANCEL drift=%.3f not aligned with %s\n",
                   ewm_drift, m_reload_is_long ? "LONG" : "SHORT");
            fflush(stdout);
            cancel_reload();
            return false;
        }

        // ?? Confirmation: price still moving in reload direction ??????????????
        const double progress = m_reload_is_long
            ? (mid - m_reload_price_at_arm)
            : (m_reload_price_at_arm - mid);
        if (progress < RELOAD_MIN_CONFIRM_PTS) return false;
        if (m_reload_tick_count < RELOAD_MIN_CONFIRM_TICKS) return false;

        // ?? Spread gate ???????????????????????????????????????????????????????
        if (spread > m_spread_at_entry * 1.5) {
            printf("[GFE-RELOAD] SPREAD_CANCEL spread=%.3f > 1.5?entry_spread=%.3f\n",
                   spread, m_spread_at_entry);
            fflush(stdout);
            cancel_reload();
            return false;
        }

        // ?? ATR gate ??????????????????????????????????????????????????????????
        if (m_atr <= 0.0) { cancel_reload(); return false; }

        // ?? ALL GATES PASSED ??????????????????????????????????????????????????
        printf("[GFE-RELOAD] SIGNAL %s @ %.2f progress=%.2fpts ticks=%d drift=%.3f atr=%.2f\n",
               m_reload_is_long ? "LONG" : "SHORT",
               mid, progress, m_reload_tick_count, ewm_drift, m_atr);
        fflush(stdout);

        m_reload_pending    = false;
        m_reload_tick_count = 0;
        return true;
    }

    // Force-close any open position (used during disconnect cleanup)
    void force_close(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!has_open_position()) return;
        const double exit_px = pos.is_long ? bid : ask;
        close_position(exit_px, "FORCE_CLOSE", now_ms, on_close);
    }

    double current_atr() const noexcept { return m_atr; }

    // Feed bar ATR from cTrader M1 trendbar API into the engine.
    // Real OHLC true-range ATR is more accurate than tick-based estimation.
    // Called from main.cpp Gate 3 each tick when bars are seeded.
    // Only updates when bar_atr is in a valid range (1-50pts for gold).
    // Uses a slow blend (5%) so a single outlier bar doesn't whipsaw SL sizing.
    void seed_bar_atr(double bar_atr) noexcept {
        if (bar_atr <= 0.0) return;
        if (m_atr <= 0.0) {
            m_atr = bar_atr;  // cold start: accept immediately
        } else {
            m_atr = 0.95 * m_atr + 0.05 * bar_atr;  // slow blend toward bar ATR
        }
        if (m_atr_ewm <= 0.0) m_atr_ewm = m_atr;
        else m_atr_ewm = 0.95 * m_atr_ewm + 0.05 * bar_atr;
    }

    // is_impulsive(): true when recent price range >= GFE_IMPULSE_ATR_MULT * ATR
    // Chop = price oscillates within noise band (range < ATR) ? skip entry
    // Impulse = price expands beyond noise band (range >= ATR) ? allow entry
    // Core chop/noise filter: tick direction can be clean but if price hasn't
    // actually moved beyond normal noise it's not worth trading.
    bool is_impulsive() const noexcept {
        if (m_atr <= 0.0 || m_range_hi <= m_range_lo) return true; // no data, allow
        return (m_range_hi - m_range_lo) >= GFE_IMPULSE_ATR_MULT * m_atr;
    }

    // ?? Macro trend bias -- set each tick from main.cpp before on_tick() ?????
    // Prevents GFE entering counter-trend on strong directional days.
    // bias > +GFE_TREND_BIAS_THRESHOLD ? macro trend is UP   ? block SHORT entries
    // bias < -GFE_TREND_BIAS_THRESHOLD ? macro trend is DOWN ? block LONG entries
    // bias in (-threshold, +threshold) ? no bias ? both directions allowed
    // Source: gold_momentum = (mid - VWAP) / mid * 100 from GoldEngineStack.
    //   VWAP deviation > +0.05% = price firmly above session VWAP = uptrend.
    //   Confirmed by supervisor TREND_CONTINUATION confidence > 1.5 as secondary check.
    // Evidence: Friday 27 Mar -- engine went SHORT 3? at 4440 during a +120pt UP day.
    //   At entry: gold_momentum = +0.86% (price 0.86% above VWAP). A SHORT here was
    //   directly counter-trend. All 3 exits were FORCE_CLOSE. Net: +$317 vs $3,550 missed.
    void set_trend_bias(double momentum_pct, double supervisor_conf, bool sup_is_trend,
                        bool wall_ahead = false) noexcept {
        m_trend_momentum   = momentum_pct;
        m_sup_conf         = supervisor_conf;
        m_sup_is_trend     = sup_is_trend;
        m_wall_ahead       = wall_ahead;
    }
    // save_atr_state: write current ATR to disk at rollover/shutdown
    // Saves the RANGE-based ATR (m_atr) + timestamp so load can validate freshness.
    // load_atr_state: restore on startup -- bypasses 100-tick warmup blind zone.
    // Only accepts saved values that are:
    //   1. Range-based ATR >= GFE_ATR_MIN (not tick-to-tick noise)
    //   2. Saved within the last 4 hours (not overnight stale)
    //   3. Saved during an active session (ATR >= 1.5pts)
    void save_atr_state(const std::string& path) const noexcept {
        if (m_atr_warmup_ticks < GFE_ATR_PERIOD) return;
        // Do not save a near-zero or unrealistically small ATR -- it would corrupt
        // the next startup. Gold ATR below 3pts is dead-tape noise, not a usable seed.
        if (m_atr < 3.0) {
            printf("[GFE] ATR save skipped (atr=%.4f < 3.0 -- too small to be useful)\n", m_atr);
            return;
        }
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        const int64_t now_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // Save range-based m_atr (not raw atr_ewm which is tick-to-tick noise)
        fprintf(f, "atr=%.6f atr_ewm=%.6f warmed=1 last_mid=%.5f saved_ts=%lld\n",
                m_atr, m_atr_ewm, m_last_mid_atr, (long long)now_s);
        fclose(f);
    }
    void load_atr_state(const std::string& path) noexcept {
        if (m_atr_warmup_ticks >= GFE_ATR_PERIOD) return;
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return;
        double atr = 0.0, atr_ewm = 0.0, last_mid = 0.0;
        long long saved_ts = 0;
        int warmed = 0;
        const int parsed = fscanf(f, "atr=%lf atr_ewm=%lf warmed=%d last_mid=%lf saved_ts=%lld",
                                  &atr, &atr_ewm, &warmed, &last_mid, &saved_ts);
        fclose(f);

        if (parsed < 3 || warmed != 1 || atr <= 0.0) {
            printf("[GFE] ATR state rejected (bad format or zero atr -- parsed=%d atr=%.4f)\n",
                   parsed, atr);
            return;
        }

        const int64_t now_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const int64_t age_s = now_s - saved_ts;

        // Reject if saved more than 4 hours ago -- overnight/weekend stale
        if (saved_ts > 0 && age_s > 4 * 3600) {
            printf("[GFE] ATR state rejected (age=%lldmin > 240min -- too stale)\n",
                   (long long)(age_s / 60));
            return;
        }
        // Reject if ATR is below minimum viable -- saved during dead tape or format mismatch.
        // Raised 1.5?5.0: a $2 ATR on XAUUSD@$4554 produces a 0.044% SL = noise level.
        // Any valid London/NY session ATR is >= 5pts. Dead tape / corrupt values are < 3pts.
        if (atr < 5.0) {
            printf("[GFE] ATR state rejected (atr=%.4f < 5.0 -- dead session or stale value)\n", atr);
            return;
        }
        // Valid -- restore
        m_atr              = std::max(GFE_ATR_MIN, atr);
        m_atr_seed_lock    = 200; // hold loaded ATR for 200 ticks (~20s London) before EWM takes over
        m_atr_ewm          = atr_ewm > 0.0 ? atr_ewm : m_atr;
        m_atr_warmup_ticks = GFE_ATR_PERIOD;
        m_last_mid_atr     = last_mid;
        printf("[GFE] ATR state loaded: atr=%.4f age=%lldmin\n",
               m_atr, (long long)(age_s / 60));
    }

    // seed() -- pre-warm ATR and direction windows from a single price on reconnect.
    // Without this the engine is blind for GFE_ATR_PERIOD (100) ticks after every
    // restart/reconnect -- blocking all entries until warmup completes.
    // Assumes flat/neutral market at seed price (conservative: no directional bias).
    // No-op if already warmed up.
    // seed() -- vix_level passed from main.cpp so seed ATR scales with
    // real observed volatility rather than a hardcoded flat value.
    // Called on first XAUUSD tick after restart. load_atr_state() runs first;
    // if a valid recent ATR file exists this is a no-op (warmup already set).
    void seed(double mid, double vix_level = 0.0) noexcept {
        if (mid <= 0.0 || m_atr_warmup_ticks >= GFE_ATR_PERIOD) return;

        // VIX-scaled ATR seed -- matches real observed XAUUSD ATR ranges:
        //   VIX < 15  (quiet)    ? ATR ~5pts   Asia dead tape
        //   VIX 15-20 (normal)   ? ATR ~8pts   normal London/NY
        //   VIX 20-25 (elevated) ? ATR ~12pts  active trending day
        //   VIX 25+   (high vol) ? ATR ~18pts  macro event / panic
        // Evidence: today VIX=27, gold moved $140, 3pt seed produced a 2pt SL
        // that got stopped on the first micro-fluctuation after entry.
        // If vix_level not provided (0.0), use conservative 10pts.
        double seed_range;
        if      (vix_level <= 0.0)  seed_range = 10.0;  // unknown -- use safe default
        else if (vix_level <  15.0) seed_range =  5.0;  // quiet
        else if (vix_level <  20.0) seed_range =  8.0;  // normal
        else if (vix_level <  25.0) seed_range = 12.0;  // elevated
        else                        seed_range = 18.0;  // high vol -- VIX 25+
        printf("[GFE-SEED] mid=%.2f vix=%.1f seed_atr=%.1f (SL will be ~%.1fpts)\n",
               mid, vix_level, seed_range, seed_range);
        fflush(stdout);
        m_atr_ewm          = seed_range;
        m_atr_warmup_ticks = GFE_ATR_PERIOD;
        m_atr              = seed_range;
        m_atr_seed_lock    = 200; // hold seed for 200 ticks (~20s London) before EWM takes over
        m_last_mid_atr     = mid;
        // Pre-fill price window at seed mid so range calc starts immediately
        m_atr_price_window.clear();
        for (int i = 0; i < GFE_ATR_RANGE_WINDOW; ++i)
            m_atr_price_window.push_back(mid);

        // Seed momentum window flat (no directional bias)
        m_momentum_window.clear();
        for (int i = 0; i < GFE_SLOW_TICKS; ++i)
            m_momentum_window.push_back(mid);

        // Seed range window flat
        m_range_window.clear();
        for (int i = 0; i < GFE_RANGE_WINDOW; ++i) m_range_window.push_back(mid);
        m_range_hi = mid; m_range_lo = mid;

        // Range expansion tracking -- update hi/lo window
        m_range_window.push_back(mid);
        if ((int)m_range_window.size() > GFE_RANGE_WINDOW) m_range_window.pop_front();
        if ((int)m_range_window.size() >= 5) {
            m_range_hi = *std::max_element(m_range_window.begin(), m_range_window.end());
            m_range_lo = *std::min_element(m_range_window.begin(), m_range_window.end());
        }

        // Seed direction windows neutral
        m_fast_window.clear();
        m_slow_window.clear();
        m_fast_long_count = m_fast_short_count = 0;
        m_slow_long_count = m_slow_short_count = 0;
        for (int i = 0; i < GFE_FAST_TICKS; ++i) m_fast_window.push_back(0);
        for (int i = 0; i < GFE_SLOW_TICKS;  ++i) m_slow_window.push_back(0);
    }

    // -------------------------------------------------------------------------
    // force_entry() -- direct entry bypass for reload instance.
    // Called by main.cpp after try_reload() signals confirmation.
    // Skips all persistence/drift gates -- confirmation already validated by try_reload().
    // Seeds ATR from provided value and fires enter() immediately.
    // Returns true if entry succeeded (phase == LIVE after call).
    bool force_entry(bool is_long, double bid, double ask,
                     double atr_seed, int64_t now_ms) noexcept {
        if (has_open_position()) return false;
        // Seed ATR so SL sizing is correct immediately
        if (atr_seed > 0.0) {
            m_atr              = std::max(GFE_ATR_MIN, atr_seed);
            m_atr_ewm          = m_atr;
            m_atr_warmup_ticks = GFE_ATR_PERIOD; // skip warmup
            m_ticks_received   = GFE_MIN_ENTRY_TICKS; // skip cold-start gate
            m_atr_seed_lock    = 50; // hold for 50 ticks before EWM takes over
        }
        const double mid = (bid + ask) * 0.5;
        enter(is_long, mid, bid, ask, ask - bid, now_ms);
        return has_open_position();
    }

private:

    // ATR calculation -- EWM-smoothed tick-to-tick range, 100-tick warmup
    double              m_atr           = 0.0;   // exposed ATR (0 until warmup complete)
    bool                m_l2_was_live   = false; // true once L2 imbalance != 0.5 seen this session
    std::deque<double>  m_atr_price_window;         // rolling mid price window for range-based ATR
    double              m_atr_ewm       = 0.0;   // internal EWM accumulator
    double              m_last_mid_atr  = 0.0;   // previous mid for tick-range computation
    int                 m_atr_warmup_ticks = 0;  // counts ticks until GFE_ATR_PERIOD reached
    int                 m_atr_seed_lock    = 0;  // ticks remaining where EWM cannot overwrite seeded ATR
    int                 m_ticks_received   = 0;  // raw tick count since construction -- NEVER reset by seed/load
                                                  // used as the cold-start entry gate (see GFE_MIN_ENTRY_TICKS)
    std::deque<double>  m_atr_window;             // spread data (retained for compat)
    // Momentum buffer: SEPARATE from ATR buffer.
    // OLD: m_mid_window was shared -- ATR_PERIOD*3 trim implicitly controlled momentum history.
    // Tuning ATR period would silently change how far back momentum looked.
    // NEW: independent buffer, fixed size GFE_MOMENTUM_BUF_SIZE (64 entries).
    // Momentum only ever looks back GFE_MOMENTUM_TICKS=12 entries -- 64 is ample.
    std::deque<double>  m_momentum_window;  // independent mid history for momentum only

    // Persistence windows -- rolling window count of directional ticks
    // Fast: counts how many of the last GFE_FAST_TICKS ticks were directional
    // Slow: counts how many of the last GFE_SLOW_TICKS ticks were directional
    // OLD design used streak counters (+1 per matching tick, -1 per neutral tick).
    //   Problem: one neutral tick decayed 99 built ticks by 1, making the slow
    //   counter almost impossible to reach 100 on real noisy market data.
    // NEW design: circular ring buffer counting directional ticks in the window.
    //   Neutral ticks simply push out old directional ticks as the window advances.
    //   Threshold = 75% of window must be directional (vs 100% in old streak design).
    static constexpr int GFE_FAST_DIR_THRESHOLD = (GFE_FAST_TICKS * 3) / 4;  // 23/30 ticks directional
    static constexpr int GFE_SLOW_DIR_THRESHOLD = (GFE_SLOW_TICKS * 3) / 4;  // 75/100 ticks directional

    int     m_fast_long_count  = 0;
    int     m_fast_short_count = 0;
    int     m_slow_long_count  = 0;
    int     m_slow_short_count = 0;

    // Rolling window buffers: 1=long, -1=short, 0=neutral
    std::deque<int> m_fast_window;   // last GFE_FAST_TICKS direction values
    std::deque<int> m_slow_window;   // last GFE_SLOW_TICKS direction values

    // Drift persistence fallback (used when L2 size data is unavailable -- imbalance always 0.5)
    // Tracks ewm_drift direction over GFE_DRIFT_PERSIST_TICKS=20 ticks.
    // Replaces L2 persistence windows when broker doesn't send tag-271 size data.
    std::deque<int>    m_drift_persist_window;  // direction counts for fallback persistence
    std::deque<double> m_drift_val_window;       // raw drift values for chop range guard

    // ?? Range expansion tracker -- chop vs impulse detection ?????????????????
    // Tracks price range over last N ticks to detect whether a move is
    // larger than normal noise (impulse) or within chop band (noise).
    // Updated every tick. Used to gate entries: only trade when price is
    // expanding beyond the recent noise band.
    static constexpr int    GFE_RANGE_WINDOW    = 50;   // 50 ticks ~5-10s
    static constexpr double GFE_IMPULSE_ATR_MULT = 0.3; // lowered 0.8->0.3: burst-only filter was
                                                         // blocking slow grinding trends (vol_range=2.56
                                                         // vs ATR~18 = ratio 0.14, blocked at 0.8).
                                                         // 0.3x ATR still kills dead flat tape.
    std::deque<double> m_range_window;   // recent mid prices for hi/lo calc
    double m_range_hi = 0.0;
    double m_range_lo = 0.0;

    int64_t m_cooldown_start   = 0;
    int     m_exit_cooldown_ms = 0;  // cooldown duration set per exit reason
    int     m_trade_id         = 0;
    double  m_spread_at_entry  = 0.0;

    // Macro trend bias -- updated each tick via set_trend_bias() before on_tick()
    double  m_trend_momentum   = 0.0;  // (mid-VWAP)/mid*100 from GoldEngineStack
    double  m_sup_conf         = 0.0;  // supervisor confidence score
    bool    m_sup_is_trend     = false; // supervisor classified TREND_CONTINUATION
    bool    m_wall_ahead       = false; // significant L2 wall within 2?ATR ahead of entry

    // Continuation mode: set after a profitable close (TRAIL_HIT/BE_HIT).
    // Lowers persistence threshold from 75% ? 60% for first re-entry only.
    // Allows faster re-entry when trend is still active post close rather than
    // waiting for a full 30-tick window to refill from scratch.
    // Cleared after one use (next entry) or if cooldown expires without entry.
    bool    m_continuation_mode = false;
    int64_t m_continuation_expires_ms = 0; // ms timestamp when mode expires
    int     m_last_session_slot = -1;

    // ?? Reload state ??????????????????????????????????????????????????????????
    // Armed when PARTIAL_1R fires (step 1 of staircase banks first 33%).
    // main.cpp reads reload_pending() each tick and calls try_reload() when:
    //   1. Price moved >= RELOAD_MIN_CONFIRM pts further in reload direction
    //   2. Spread is normal (not anomalous)
    //   3. No reversal of >= RELOAD_CANCEL_RETRACE ? ATR since arming
    //   4. Reload armed within RELOAD_TIMEOUT_MS (timed out = cancel)
    // Consumed on first successful reload entry or on timeout/cancel.
    bool    m_reload_pending      = false; // true = reload armed, waiting confirmation
    bool    m_reload_is_long      = false; // direction (same as original trade)
    double  m_reload_price_at_arm = 0.0;  // mid price when reload was armed
    double  m_reload_atr_at_arm   = 0.0;  // ATR at arm time (for SL + cancel calc)
    int64_t m_reload_armed_ms     = 0;    // epoch ms when armed
    int     m_reload_tick_count   = 0;    // confirmation tick counter

    void update_atr(double spread, double mid) noexcept {
        // ATR: EWM-smoothed using RANGE over a rolling price window.
        // Previously used tick-to-tick moves (0.05-0.20pts) which produced
        // tiny ATR values even on active London tape. Now uses the actual
        // hi-lo range over the last GFE_ATR_RANGE_WINDOW ticks -- this
        // matches real session volatility and is what drives SL sizing.
        m_atr_price_window.push_back(mid);
        if ((int)m_atr_price_window.size() > GFE_ATR_RANGE_WINDOW)
            m_atr_price_window.pop_front();

        if (m_last_mid_atr > 0.0 && (int)m_atr_price_window.size() >= GFE_ATR_RANGE_WINDOW) {
            const double hi = *std::max_element(m_atr_price_window.begin(), m_atr_price_window.end());
            const double lo = *std::min_element(m_atr_price_window.begin(), m_atr_price_window.end());
            const double window_range = std::max(hi - lo, spread);
            if (m_atr_ewm <= 0.0)
                m_atr_ewm = window_range;
            else
                m_atr_ewm = GFE_ATR_EWM_ALPHA * window_range + (1.0 - GFE_ATR_EWM_ALPHA) * m_atr_ewm;
        }
        m_last_mid_atr = mid;

        ++m_atr_warmup_ticks;
        if (m_atr_warmup_ticks >= GFE_ATR_PERIOD) {
            if (m_atr_seed_lock > 0) {
                --m_atr_seed_lock;  // EWM blocked -- seed/loaded ATR still in control
            } else {
                m_atr = std::max(GFE_ATR_MIN, m_atr_ewm);
            }
        }
        // Always increment -- not reset by seed() or load_atr_state()
        ++m_ticks_received;

        m_atr_window.push_back(spread);
        if ((int)m_atr_window.size() > GFE_ATR_PERIOD * 3)
            m_atr_window.pop_front();

        // Momentum buffer: INDEPENDENT of ATR, fixed size GFE_MOMENTUM_BUF_SIZE.
        // Separated so ATR configuration changes don't silently alter momentum lookback.
        m_momentum_window.push_back(mid);
        if ((int)m_momentum_window.size() > GFE_MOMENTUM_BUF_SIZE)
            m_momentum_window.pop_front();
    }

    double mid_momentum() const noexcept {
        if ((int)m_momentum_window.size() < GFE_MOMENTUM_TICKS + 1) return 0.0;
        return m_momentum_window.back()
               - m_momentum_window[m_momentum_window.size() - 1 - GFE_MOMENTUM_TICKS];
    }

    void update_persistence(double l2_imb, int64_t /*now_ms*/) noexcept {
        // Classify this tick's direction
        const int dir = (l2_imb > GFE_LONG_THRESHOLD) ? 1
                      : (l2_imb < GFE_SHORT_THRESHOLD) ? -1
                      : 0;

        // Fast window -- incremental update: subtract outgoing, add incoming.
        // OLD: full O(n) recount every tick (30 + 100 iterations = 130 ops/tick).
        // NEW: O(1) -- track only the outgoing and incoming values.
        if ((int)m_fast_window.size() >= GFE_FAST_TICKS) {
            const int outgoing = m_fast_window.front();
            if (outgoing ==  1) --m_fast_long_count;
            if (outgoing == -1) --m_fast_short_count;
            m_fast_window.pop_front();
        }
        m_fast_window.push_back(dir);
        if (dir ==  1) ++m_fast_long_count;
        if (dir == -1) ++m_fast_short_count;

        // Slow window -- same incremental logic
        if ((int)m_slow_window.size() >= GFE_SLOW_TICKS) {
            const int outgoing = m_slow_window.front();
            if (outgoing ==  1) --m_slow_long_count;
            if (outgoing == -1) --m_slow_short_count;
            m_slow_window.pop_front();
        }
        m_slow_window.push_back(dir);
        if (dir ==  1) ++m_slow_long_count;
        if (dir == -1) ++m_slow_short_count;
    }

    void enter(bool is_long, double mid, double bid, double ask,
               double spread, int64_t now_ms) noexcept
    {
        // ?? FIX 2: SL floor -- ATR floor alone is insufficient ????????????????
        // GFE_ATR_MIN = $2.0 prevents sub-$2 SL from EWM ATR on dead tape.
        // BUT: spread = $0.60 at max entry. $2.0 SL / $0.60 spread = 3.3x spread.
        // A normal spread fluctuation can close 30% of that SL gap instantly.
        // Any tick-rate noise on Asia tape hits a $2 SL in seconds.
        //
        // Minimum SL = max(ATR * mult, spread * 3.0).
        // spread * 3.0 ensures SL is never reachable by spread noise alone.
        // Example: spread=$0.45 ? min SL=$1.35, but ATR=$3 wins ? SL=$3.
        //          spread=$0.55 ? min SL=$1.65, ATR=$2 ? SL=$2 (ATR wins).
        //          spread=$0.60 ? min SL=$1.80, ATR=$2 ? SL=$2 (ATR wins).
        // The spread gate (GFE_MAX_SPREAD=$0.60) already caps spread at entry.
        const double atr_sl  = m_atr * GFE_ATR_SL_MULT;
        const double min_sl  = spread * 5.0;  // raised 3x?5x: 3x was 0.66pts on 0.22 spread, too tight for London gold vol
        const double sl_pts  = std::max(atr_sl, min_sl);
        if (sl_pts <= 0.0) return;

        // Size: fixed dollar risk / SL_pts
        // 1 lot gold = $100/pt at BlackBull.
        // Cap at 0.08 lots: prevents oversizing when ATR collapses on overnight tape.
        // Round to 0.001 lot precision (many brokers support this for gold).
        // Old rounding to 0.01 could inflate risk by up to 1% per trade.
        static constexpr double GFE_MAX_LOT_FLOW = 0.50;  // raised 0.08?0.50: matches max_lot_gold. At ATR=$2 SL, $30 risk = 0.15 lots -- 0.08 cap was cutting that to $16 max loss.
        static constexpr double GFE_LOT_STEP     = 0.001; // broker lot precision
        const double tick_mult = 100.0;
        double size = risk_dollars / (sl_pts * tick_mult);
        // Round down to nearest lot step (never round up -- never risk more than intended)
        size = std::floor(size / GFE_LOT_STEP) * GFE_LOT_STEP;
        size = std::max(GFE_MIN_LOT, std::min(GFE_MAX_LOT_FLOW, size));

        const double entry_px = is_long ? ask : bid; // market order, pay spread
        const double sl_px    = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);

        pos.active       = true;
        pos.is_long      = is_long;
        pos.entry        = entry_px;
        pos.sl           = sl_px;
        m_spread_at_entry = spread;
        pos.size         = size;
        pos.mfe          = 0.0;
        pos.atr_at_entry = m_atr;
        pos.be_locked        = false;
        pos.trail_stage      = 0;
        pos.stage2_tight     = false;
        pos.partial_closed   = false;
        pos.partial_closed_2 = false;
        pos.full_size        = size;
        pos.entry_ts      = now_ms / 1000; // seconds
        pos.dollar_lock_tier = 0;          // reset dollar ratchet for new trade
        pos.dollar_lock_sl   = 0.0;
        phase            = Phase::LIVE;
        ++m_trade_id;

        // Reset persistence so we don't re-enter immediately
        m_fast_long_count = m_fast_short_count = 0;
        m_slow_long_count = m_slow_short_count = 0;
        m_fast_window.clear();
        m_slow_window.clear();
        m_drift_persist_window.clear();
        m_drift_val_window.clear();
        m_continuation_mode = false;  // one-shot -- clears after first re-entry

        std::cout << "[GOLD-FLOW] ENTRY " << (is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px
                  << " sl_pts=" << sl_pts
                  << " (atr=" << m_atr << " spread_floor=" << min_sl << ")"
                  << " size=" << size
                  << " spread=" << spread
                  << " session=" << m_last_session_slot << "\n";
        std::cout.flush();
    }

    void manage_position(double bid, double ask, double mid, double spread,
                         double l2_imb, int64_t now_ms, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        // atr_step: frozen at entry -- used for staircase step triggers only.
        // Keeps step distances consistent even as gold vol changes mid-trade.
        const double atr_step = pos.atr_at_entry;
        // atr_live: current market ATR -- used for trail and ratchet SL placement.
        // After step 1 the trade is in profit -- use live ATR so trail reflects
        // actual current volatility. Blend: 70% entry + 30% live prevents a
        // single volatile bar from wildly moving the SL.
        const double atr_live = (m_atr > 0.0)
            ? (0.70 * pos.atr_at_entry + 0.30 * m_atr)
            : pos.atr_at_entry;
        const double atr      = atr_step; // alias for staircase steps (keep old name)
        const double move     = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);

        // Track MFE
        if (move > pos.mfe) pos.mfe = move;

        // ??????????????????????????????????????????????????????????????????????
        // STAIRCASE BANKING -- bank profit every ATR step, SL locks to each exit
        //
        // Design: every time profit reaches another ATR multiple, close 33% of
        // the REMAINING position at market and move SL to that exit price.
        // This means you can NEVER give back what was banked at a prior step.
        //
        // Step 1: +1?ATR ? close 33% of full size,  SL ? entry (BE)
        // Step 2: +2?ATR ? close 33% of remaining,  SL ? step1 exit price
        // Step 3: +3?ATR ? close 33% of remaining,  SL ? step2 exit price
        // Final:  remainder trails 0.25?ATR behind MFE peak (very tight)
        //
        // SL for each step is the EXIT PRICE of that step + small buffer
        // so a tick back through the exit price doesn't immediately stop out.
        // Buffer = 0.25?ATR (covers spread + 1 tick noise).
        //
        // On a 16pt move (e.g. 4569?4553 with ATR=13.5):
        //   Step 1 fires at 4555.9 ? bank $192, SL=4559.2 (not 4569!)
        //   If reverses from 4553 ? SL hit at 4559.2 ? exit with $292 extra
        //   Total: $484 LOCKED vs $0 with old BE-only system
        // ??????????????????????????????????????????????????????????????????????
        static constexpr double STEP_FRAC   = 0.33;  // close 33% each step
        static constexpr double SL_BUFFER   = 0.25;  // SL buffer = 0.25?ATR beyond exit

        // ?? Dollar-ratchet constants ??????????????????????????????????????????
        // DOLLAR_RATCHET_STEP: open profit interval at which ratchet advances ($50)
        // DOLLAR_RATCHET_KEEP: fraction of each step that is locked in (80%)
        //   At $50 open: lock $40. At $100 open: lock $90. At $150: lock $140.
        // DOLLAR_RATCHET_MIN_TIER: ratchet only activates after step 1 (BE locked).
        //   Prevents premature SL tightening before the trade has breathed.
        // Tick value: 1 lot XAUUSD = $100/pt. So $50 / (size ? $100/pt) = pts per tier.
        static constexpr double DOLLAR_RATCHET_STEP = 50.0;   // $50 per tier
        static constexpr double DOLLAR_RATCHET_KEEP = 0.80;   // keep 80% of each tier

        // Helper to fire a partial close record and update position
        auto fire_stair = [&](int step_num, const char* label) {
            const double exit_px   = pos.is_long ? bid : ask;
            const double close_qty = std::floor(pos.size * STEP_FRAC / 0.001) * 0.001;
            if (close_qty < GFE_MIN_LOT) {
                // Position too small to split further -- mark step done
                if (step_num == 1) pos.partial_closed   = true;
                else               pos.partial_closed_2 = true;
                return;
            }
            const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                            : (pos.entry - exit_px)) * close_qty;

            omega::TradeRecord ptr;
            ptr.id            = m_trade_id;
            ptr.symbol        = "XAUUSD";
            ptr.side          = pos.is_long ? "LONG" : "SHORT";
            ptr.entryPrice    = pos.entry;
            ptr.exitPrice     = exit_px;
            ptr.sl            = pos.sl;
            ptr.size          = close_qty;
            ptr.pnl           = pnl;
            ptr.mfe           = pos.mfe * close_qty;
            ptr.mae           = 0.0;
            ptr.entryTs       = pos.entry_ts;
            ptr.exitTs        = now_ms / 1000;
            ptr.exitReason    = label;
            ptr.engine        = "GoldFlowEngine";
            ptr.regime        = "FLOW";
            ptr.spreadAtEntry = m_spread_at_entry;

            // Lock SL to exit price + buffer -- can never give back this profit
            const double new_sl = pos.is_long
                ? (exit_px - atr * SL_BUFFER)   // LONG: SL below exit
                : (exit_px + atr * SL_BUFFER);   // SHORT: SL above exit
            if (pos.is_long  && new_sl > pos.sl) pos.sl = new_sl;
            if (!pos.is_long && new_sl < pos.sl) pos.sl = new_sl;

            pos.size -= close_qty;
            if (step_num == 1) { pos.partial_closed   = true; pos.be_locked = true; }
            else               { pos.partial_closed_2 = true; }

            std::cout << "[GOLD-FLOW] STAIR-" << label
                      << (pos.is_long ? " LONG" : " SHORT")
                      << " @ " << std::fixed << std::setprecision(2) << exit_px
                      << " qty=" << close_qty
                      << " remaining=" << pos.size
                      << " pnl_pts=" << std::setprecision(2) << (pnl / close_qty)
                      << " pnl_usd=" << std::setprecision(0) << (pnl * 100.0)
                      << " new_sl=" << std::setprecision(2) << pos.sl << "\n";
            std::cout.flush();
            if (on_close) on_close(ptr);

            // ?? Arm reload on step 1 (PARTIAL_1R) ????????????????????????????
            // After banking the first 33%, arm a reload so a fresh full-size
            // position can be entered if the move continues.
            // The reload fires when main.cpp calls try_reload() and confirms:
            //   price is still moving in the same direction (not reversing)
            //   drift is still aligned
            //   spread is normal
            // This gives us: original remainder (protected by ratchet) PLUS a
            // fresh full-size position catching the continuation.
            if (step_num == 1) {
                m_reload_pending       = true;
                m_reload_is_long       = pos.is_long;
                m_reload_price_at_arm  = (pos.is_long ? bid : ask); // current price at arm
                m_reload_atr_at_arm    = atr;
                m_reload_armed_ms      = now_ms;
                m_reload_tick_count    = 0;
                printf("[GFE-RELOAD] ARMED %s @ %.2f atr=%.2f -- waiting confirmation\n",
                       pos.is_long ? "LONG" : "SHORT",
                       m_reload_price_at_arm, atr);
                fflush(stdout);
            }
        };

        // Step 1: +1?ATR -- bank first 33%, SL ? entry+buffer
        if (!pos.partial_closed && move >= atr * 1.0) {
            fire_stair(1, "PARTIAL_1R");
        }

        // Step 2: +2?ATR -- bank another 33% of remaining, SL ? step1 exit
        if (pos.partial_closed && !pos.partial_closed_2 && move >= atr * 2.0) {
            fire_stair(2, "PARTIAL_2R");
        }

        // ?? Tiered trail -- widens while momentum runs, tightens as each step banks ??
        //
        // Fixed 0.25?ATR was firing on the same candle that triggered step 1,
        // exiting mid-move before the full extension was reached.
        //
        // Trail distance shrinks as each partial locks more profit:
        //   Before step 2 fires (step 1 done, move still running): 1.0?ATR
        //     ? full ATR of breathing room while momentum is live
        //   After step 2, move < 3?ATR (move maturing):           0.5?ATR
        //     ? step 2 banked another 33%, tighten slightly
        //   After step 3 fires / move >= 3?ATR (final remainder):  0.25?ATR
        //     ? final squeeze -- protect every tick of remaining profit
        //
        // One-way ratchet -- SL never moves backward.
        // Uses atr_live (70% entry + 30% current ATR).
        if (pos.be_locked && pos.mfe > 0.0) {
            const double trail_mult =
                (!pos.partial_closed_2)   ? 1.00 :  // step1 done, step2 pending -- stay wide
                (move < atr * 3.0)        ? 0.50 :  // step2 done, below 3?ATR -- mid tighten
                                            0.25;   // step3 territory / final remainder
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - atr_live * trail_mult)
                : (pos.entry - pos.mfe + atr_live * trail_mult);
            if ((pos.is_long  && trail_sl > pos.sl) ||
                (!pos.is_long && trail_sl < pos.sl)) {
                pos.sl = trail_sl;
            }
        }

        // ?? Dollar-ratchet lock ???????????????????????????????????????????????
        // Runs AFTER the ATR trail -- one-way ratchet, only moves SL forward.
        //
        // DESIGN FIX from audit:
        //   Previously used pos.size (remaining lots) for USD calc. After PARTIAL_1R
        //   reduces size from 0.15?0.10 lots, the $50 tier required 7pts on 0.10
        //   lots = correct for the REMAINDER. But the user experience is "lock in
        //   $50 of total trade profit" -- which means we measure against full_size.
        //
        //   Using full_size: at 0.15 lots, $50 tier fires at 50/(0.15?100) = 3.33pts
        //   Using remaining: at 0.10 lots, $50 tier fires at 50/(0.10?100) = 5.00pts
        //
        //   We use full_size for the TIER TRIGGER (when to advance the ratchet)
        //   but remaining size for LOCKED_PTS (how much price movement locks the $).
        //   This correctly represents "I want $50 of the original trade locked in."
        //
        // Active from first tick -- no be_locked gate.
        // Before step 1: ratchet moves SL from loss territory toward BE.
        // After step 1:  ratchet locks profit above the staircase SL.
        // Both improve the worst-case outcome.
        if (pos.size > 0.0 && pos.full_size > 0.0) {
            // Tier trigger: measure total trade open P&L using full_size
            const double open_pnl_usd_full = move * pos.full_size * 100.0;
            const int    tier_now = static_cast<int>(open_pnl_usd_full / DOLLAR_RATCHET_STEP);

            if (tier_now > pos.dollar_lock_tier && tier_now >= 1) {
                // SL placement: use remaining size so locked_pts correctly maps
                // to price distance needed to guarantee locked_usd on remainder
                const double locked_usd  = tier_now * DOLLAR_RATCHET_STEP * DOLLAR_RATCHET_KEEP;
                const double locked_pts  = locked_usd / (pos.size * 100.0);
                const double ratchet_sl  = pos.is_long
                    ? (pos.entry + locked_pts)
                    : (pos.entry - locked_pts);

                const bool sl_improves = pos.is_long
                    ? (ratchet_sl > pos.sl)
                    : (ratchet_sl < pos.sl);

                if (sl_improves) {
                    pos.sl               = ratchet_sl;
                    pos.dollar_lock_sl   = ratchet_sl;
                    pos.dollar_lock_tier = tier_now;

                    std::cout << "[GOLD-FLOW] DOLLAR-RATCHET tier=" << tier_now
                              << " open_pnl_full=$" << std::fixed << std::setprecision(0)
                              << open_pnl_usd_full
                              << " locked=$" << locked_usd
                              << " locked_pts=" << std::setprecision(2) << locked_pts
                              << " new_sl=" << ratchet_sl
                              << " be_locked=" << pos.be_locked
                              << (pos.is_long ? " LONG" : " SHORT") << "\n";
                    std::cout.flush();
                }
            }
        }

        // Advance trail_stage for GUI display (stage badge in live map)
        if (pos.trail_stage < 1 && pos.be_locked)          pos.trail_stage = 1;
        if (pos.trail_stage < 2 && pos.partial_closed_2)   pos.trail_stage = 2;
        if (pos.trail_stage < 3 && move >= atr * 3.0)      pos.trail_stage = 3;
        if (pos.trail_stage < 4 && move >= atr * 6.0)      pos.trail_stage = 4;

        // ---- Max hold timeout -------------------------------------------
        // Only exit at timeout if step 1 hasn't fired yet (no profit banked).
        // Once step 1 fires, the staircase manages the exit -- no timeout needed.
        const bool is_low_qual = (m_last_session_slot == 6 || m_last_session_slot == 0);
        const int64_t eff_max_hold = is_low_qual
            ? static_cast<int64_t>(GFE_MAX_HOLD_MS) * 2
            : static_cast<int64_t>(GFE_MAX_HOLD_MS);
        const int64_t held_ms = now_ms - (pos.entry_ts * 1000LL);
        if (held_ms >= eff_max_hold && !pos.partial_closed) {
            std::cout << "[GOLD-FLOW] MAX_HOLD_TIMEOUT"
                      << " held=" << held_ms / 1000 << "s"
                      << " move=" << move
                      << " -- no step banked, exiting stale thesis\n";
            std::cout.flush();
            const double exit_px = pos.is_long ? bid : ask;
            close_position(exit_px, "MAX_HOLD_TIMEOUT", now_ms, on_close);
            return;
        }

        // ---- SL check ---------------------------------------------------
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (!sl_hit) return;

        // ---- Exit -------------------------------------------------------
        const double exit_px = pos.is_long ? bid : ask;
        const char*  reason  = pos.be_locked
            ? (std::fabs(pos.sl - pos.entry) > 0.01 ? "TRAIL_HIT" : "BE_HIT")
            : "SL_HIT";
        close_position(exit_px, reason, now_ms, on_close);
    }

    void close_position(double exit_px, const char* reason,
                        int64_t now_ms, CloseCallback on_close) noexcept
    {
        omega::TradeRecord tr;
        tr.id           = m_trade_id;
        tr.symbol       = "XAUUSD";
        tr.side         = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice   = pos.entry;
        tr.exitPrice    = exit_px;
        tr.tp           = 0.0; // no fixed TP -- trail only
        tr.sl           = pos.sl;
        tr.size         = pos.size;
        // PnL in raw price points ? size -- handle_closed_trade applies tick_value_multiplier
        // to convert to USD. Do NOT pre-multiply by 100 here (was double-counting).
        tr.pnl          = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                          * pos.size;
        tr.mfe          = pos.mfe * pos.size;
        tr.mae          = 0.0;
        tr.entryTs      = pos.entry_ts;
        tr.exitTs       = now_ms / 1000;
        tr.exitReason   = reason;
        tr.engine       = "GoldFlowEngine";
        tr.regime       = "FLOW";
        tr.spreadAtEntry = m_spread_at_entry;

        const double held_s = (double)(now_ms / 1000 - pos.entry_ts);

        std::cout << "[GOLD-FLOW] EXIT " << (pos.is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " reason=" << reason
                  << " pnl_raw=" << tr.pnl << " pnl_usd=" << (tr.pnl * 100.0)
                  << " mfe=" << pos.mfe
                  << " stage=" << pos.trail_stage
                  << " held=" << held_s << "s\n";
        std::cout.flush();

        // ?? Differentiated cooldown by exit reason ????????????????????????????
        // TRAIL_HIT: trade was profitable and trend gave confirmation ? re-enter fast
        //   10s: the reversal is now in progress, want to catch it quickly
        // BE_HIT: breakeven on remainder after partial -- trend may continue
        //   15s: slight pause to confirm direction, then allow re-entry
        // SL_HIT: full stop hit -- thesis failed, need time to reassess
        //   45s: longer pause, don't re-enter into the same failed setup
        // FORCE_CLOSE / MAX_HOLD_TIMEOUT: stale thesis
        //   30s: standard pause
        // default (other): 30s
        int exit_cooldown_ms = GFE_COOLDOWN_MS;  // 30s default
        if      (std::strcmp(reason, "TRAIL_HIT")         == 0) exit_cooldown_ms = 10000;
        else if (std::strcmp(reason, "BE_HIT")            == 0) exit_cooldown_ms = 15000;
        else if (std::strcmp(reason, "SL_HIT")            == 0) exit_cooldown_ms = 45000;
        else if (std::strcmp(reason, "FORCE_CLOSE")       == 0) exit_cooldown_ms = 30000;
        else if (std::strcmp(reason, "MAX_HOLD_TIMEOUT")  == 0) exit_cooldown_ms = 30000;
        m_exit_cooldown_ms = exit_cooldown_ms;

        // Set continuation mode on profitable close (trail or BE, not SL).
        // Allows faster re-entry when trend is still active.
        // Expires after 3? cooldown to prevent stale mode on ranging days.
        const bool was_profitable = (std::strcmp(reason, "SL_HIT") != 0
                                  && std::strcmp(reason, "MAX_HOLD_TIMEOUT") != 0
                                  && std::strcmp(reason, "FORCE_CLOSE") != 0);
        // Set continuation when:
        //   a) Normal profitable exit (trail/BE with positive PnL), OR
        //   b) BE_HIT on the remaining half after PARTIAL_1R already locked profit.
        //      In this case tr.pnl is ~0 (exited at entry) but the trade was a winner.
        //      Without this, BE_HIT on partial remainder kills continuation mode
        //      and the engine sits in 60s Asia cooldown while the trend continues.
        const bool partial_was_taken = pos.partial_closed;
        const bool set_continuation  = was_profitable
                                     && (tr.pnl > 0.0 || partial_was_taken);
        if (set_continuation) {
            m_continuation_mode       = true;
            // Continuation cooldown: always use normal 30s (not 60s Asia).
            // Trend is established -- 60s Asia cooldown misses the continuation move.
            m_continuation_expires_ms = now_ms + GFE_COOLDOWN_MS * 3; // 90s
        } else {
            m_continuation_mode = false;
        }

        pos             = OpenPos{};
        phase           = Phase::COOLDOWN;
        m_cooldown_start = now_ms;

        // Cancel any pending reload -- position is fully closed, reload is moot
        cancel_reload();

        if (on_close) on_close(tr);
    }
};

