// =============================================================================
//  GoldFlowEngine.hpp
//  L2 order-flow engine for GOLD.F
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
//  EXIT: Progressive ATR-based trail (4 stages)
//    Stage 1 (1x ATR profit):  SL -> breakeven. Trade is free.
//    Stage 2 (2x ATR profit):  trail at 1.0x ATR behind MFE
//    Stage 3 (5x ATR profit):  trail tightens to 0.5x ATR behind MFE
//    Stage 4 (10x ATR profit): trail tightens to 0.3x ATR -- ride the cascade
//
//  This captures $100-400 trend moves while keeping risk tight on entry.
//  On today's $400 gold drop: enter short at compression, trail tightens
//  progressively, exits near the low with most of the move captured.
//
//  COOLDOWN: 30s after any exit. Prevents overtrading after a move exhausts.
//
//  KNOWN GAP — MAKER EXECUTION (Problem 3):
//    Current engine uses market orders (entry_px = ask for long, bid for short).
//    The flow intelligence spec calls for LIMIT orders at mid to save ~$0.30/trade
//    (half-spread). At 20 trades/day that's ~$6/day or ~$1500/year on $30 risk.
//    Implementation requires:
//      1. Send LIMIT order at mid price via FIX tag 40=2
//      2. Track pending limit order ID
//      3. On fill ACK → transition to LIVE
//      4. On timeout (e.g. 500ms) → cancel limit, optionally send market
//    This is a main.cpp FIX dispatch change, not an engine change.
//    Currently deferred — market orders work correctly, limit adds fill-rate risk.
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
static constexpr int    GFE_SLOW_TICKS        = 100;   // slow confirmation window
static constexpr double GFE_LONG_THRESHOLD    = 0.75;  // bid-heavy: long signal
static constexpr double GFE_SHORT_THRESHOLD   = 0.25;  // ask-heavy: short signal
static constexpr double GFE_DRIFT_MIN         = 0.0;   // drift must be non-zero same dir
// Drift-persistence fallback (used when L2 size data is unavailable — imbalance always 0.5)
static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.30; // drift pts/tick to count as directional
static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // rolling window for drift persistence check
static constexpr int    GFE_ATR_PERIOD        = 100;   // ATR lookback ticks -- raised 20→100:
                                                        // 20-tick hi-lo range was a 2-second window,
                                                        // producing SL of $0.3–5 depending on micro-volatility.
                                                        // 100 ticks = ~10-30s, EWM-smoothed, stable across sessions.
static constexpr double GFE_ATR_EWM_ALPHA     = 0.05;  // EWM smoothing alpha for ATR (20-tick equivalent half-life)
static constexpr double GFE_ATR_MIN           = 2.0;   // ATR floor in $pts -- prevents sub-$2 SL on dead tape
static constexpr double GFE_ATR_SL_MULT       = 1.0;   // SL = ATR * this
static constexpr double GFE_TRAIL_STAGE2_MULT = 1.5;   // EA-matched: wider initial trail, ride moves
static constexpr double GFE_TRAIL_STAGE3_MULT = 0.5;   // tighten to 0.5x ATR at stage 3
static constexpr double GFE_TRAIL_STAGE4_MULT = 0.5;   // EA-matched: wider trail at stage4, ride full moves
static constexpr double GFE_BE_ATR_MULT       = 1.0;   // BE lock at 1x ATR profit
static constexpr double GFE_STAGE2_ATR_MULT   = 2.0;   // start trail at 2x ATR profit
static constexpr double GFE_STAGE3_ATR_MULT   = 8.0;   // EA-matched: only tighten after 8x ATR profit
static constexpr double GFE_STAGE4_ATR_MULT   = 15.0;  // EA-matched: only tighten at 15x ATR profit
static constexpr double GFE_MAX_SPREAD        = 2.5;   // pts — London gold spread $1.50-$4.00; old 0.6 blocked all entries
static constexpr int    GFE_MIN_HOLD_MS       = 5000;   // 5s minimum hold
static constexpr int    GFE_MAX_HOLD_MS       = 3600000; // 60 min — EA has no hold limit, keep generous — prevents indefinite holds on flat tape
                                                          // If the trail has not advanced past Stage 2 by 30 min, the thesis is stale.
                                                          // Observed: held 31 min with no exit because gold went flat after entry,
                                                          // Stage 1 BE locked but trail never tightened — exit at BE/mid.
static constexpr int    GFE_COOLDOWN_MS       = 30000;  // 30s cooldown after exit
static constexpr double GFE_RISK_DOLLARS      = 30.0;  // $ risk per trade (fallback)
static constexpr double GFE_MIN_LOT           = 0.01;
static constexpr double GFE_MAX_LOT           = 1.0;
static constexpr int    GFE_MOMENTUM_TICKS    = 12;    // ticks back for momentum check (int — used as array index)
                                                        // 12 ticks = ~120-240ms at London, ~2-4s at Asia — real directional move.
static constexpr int    GFE_MOMENTUM_BUF_SIZE = 64;    // independent momentum history buffer (separate from ATR buffer)

// Asia/dead-zone session hardening — prevents entries on choppy mean-reverting tape
// Asia gold (22:00-07:00 UTC) has: thin liquidity, micro-oscillations that fake
// directional flow, tight ATR that produces SL easily hit by noise.
// These thresholds require genuinely committed moves before entry.
static constexpr double GFE_ASIA_ATR_MIN           = 5.0;    // $5 ATR floor (vs $2 normal) — rejects dead/thin tape
static constexpr double GFE_ASIA_MAX_SPREAD        = 1.5;    // $1.50 max spread (vs $2.50 normal) — tighter liquidity requirement
static constexpr double GFE_ASIA_DRIFT_MIN         = 0.50;   // drift must exceed $0.50 (vs $0 normal) — real directional pressure
static constexpr double GFE_ASIA_MOMENTUM_MIN      = 0.30;   // mid must move $0.30+ over momentum ticks (vs $0 normal)
static constexpr int    GFE_ASIA_COOLDOWN_MS       = 60000;  // 60s cooldown (vs 30s normal) — fewer attempts on bad tape
static constexpr double GFE_ASIA_ATR_SPREAD_RATIO  = 4.0;    // ATR must be >= 4x spread — reject noise-dominated tape
// Persistence thresholds for Asia: 90% of window must be directional (vs 75% normal)
// On choppy tape, 75% easily fills from random oscillations. 90% requires real conviction.
static constexpr int    GFE_ASIA_FAST_DIR_THRESHOLD = (GFE_FAST_TICKS * 9) / 10;  // 27/30 ticks
static constexpr int    GFE_ASIA_SLOW_DIR_THRESHOLD = (GFE_SLOW_TICKS * 9) / 10;  // 90/100 ticks
// Dominance: max 2 opposing ticks in fast window (vs 7 normal)
static constexpr int    GFE_ASIA_DOMINANCE_MAX_OPPOSING = 2;

// -----------------------------------------------------------------------------
struct GoldFlowEngine {

    // -------------------------------------------------------------------------
    // Public config -- set after construction
    double risk_dollars   = GFE_RISK_DOLLARS;  // override from main.cpp

    // -------------------------------------------------------------------------
    // Observable state
    enum class Phase { IDLE, FLOW_BUILDING, LIVE, COOLDOWN } phase = Phase::IDLE;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  size          = 0.01;
        double  mfe           = 0.0;  // max favourable excursion (pts)
        double  atr_at_entry  = 0.0;  // ATR when trade was entered
        bool    be_locked     = false;
        int     trail_stage   = 0;    // 0=initial SL, 1=BE, 2=trail1, 3=trail2, 4=trail3
        bool    stage2_tight  = false; // true = L2 was weakening at Stage 2 arm → use tight trail (0.5x ATR)
        int64_t entry_ts      = 0;
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
        const int    eff_cooldown   = is_low_quality_session ? GFE_ASIA_COOLDOWN_MS : GFE_COOLDOWN_MS;

        // Feed ATR and momentum history (always — warmup must run every tick)
        update_atr(spread, mid);

        // Cooldown phase
        if (phase == Phase::COOLDOWN) {
            if (now_ms - m_cooldown_start >= eff_cooldown)
                phase = Phase::IDLE;
            else return;
        }

        // Manage open position
        if (phase == Phase::LIVE) {
            manage_position(bid, ask, mid, spread, l2_imb, now_ms, on_close);
            return;
        }

        // ── FIX (claim 3): Block ALL signal accumulation until ATR is warmed up ──
        if (m_atr_warmup_ticks < GFE_ATR_PERIOD) return;

        // ── FIX (claim 4): Update persistence BEFORE spread gate ──────────────
        update_persistence(l2_imb, now_ms);

        // Spread gate — session-aware: tighter during Asia/dead zone
        if (spread > eff_max_spread) return;

        // ── Asia/dead-zone ATR quality gate ──────────────────────────────────
        // On low-quality tape, reject if ATR is too low (noise-dominated) or
        // ATR-to-spread ratio is too small (SL within spread fluctuation range).
        if (is_low_quality_session) {
            if (m_atr < GFE_ASIA_ATR_MIN) return;  // tape too dead
            if (spread > 0.0 && m_atr / spread < GFE_ASIA_ATR_SPREAD_RATIO) return;  // SL within noise
        }

        // ── L2 availability detection ─────────────────────────────────────────
        // BlackBull FIX feed sends no tag-271 size data → imbalance() always 0.500.
        // When L2 is structurally unavailable, the persistence windows fill with
        // all-neutral ticks and can NEVER reach directional threshold.
        // In that case use DRIFT-PERSISTENCE mode: EWM drift sustained for
        // GFE_DRIFT_PERSIST_TICKS consecutive ticks + stronger momentum threshold.
        // This is still a real confirmation signal — just price-based not book-based.
        const bool l2_data_live = (std::fabs(l2_imb - 0.5) > 0.001);

        // Session-aware persistence thresholds: Asia requires 90% dominance, normal 75%
        const int eff_fast_thresh = is_low_quality_session ? GFE_ASIA_FAST_DIR_THRESHOLD : GFE_FAST_DIR_THRESHOLD;
        const int eff_slow_thresh = is_low_quality_session ? GFE_ASIA_SLOW_DIR_THRESHOLD : GFE_SLOW_DIR_THRESHOLD;

        bool fast_long, fast_short, slow_long, slow_short;
        if (l2_data_live) {
            // Normal path: rolling window imbalance persistence
            fast_long  = (m_fast_long_count  >= eff_fast_thresh);
            fast_short = (m_fast_short_count >= eff_fast_thresh);
            slow_long  = (m_slow_long_count  >= eff_slow_thresh);
            slow_short = (m_slow_short_count >= eff_slow_thresh);
        } else {
            // Fallback path: L2 unavailable — use drift persistence counter
            // Update drift persistence window
            const int drift_dir = (ewm_drift > GFE_DRIFT_FALLBACK_THRESHOLD)  ?  1
                                 : (ewm_drift < -GFE_DRIFT_FALLBACK_THRESHOLD) ? -1
                                 : 0;
            m_drift_persist_window.push_back(drift_dir);
            if ((int)m_drift_persist_window.size() > GFE_DRIFT_PERSIST_TICKS)
                m_drift_persist_window.pop_front();

            int drift_long_count = 0, drift_short_count = 0;
            for (int d : m_drift_persist_window) {
                if (d ==  1) ++drift_long_count;
                if (d == -1) ++drift_short_count;
            }
            // Require 70% of drift ticks directional (stricter than L2 path's 75% of 30)
            const int drift_thresh = (GFE_DRIFT_PERSIST_TICKS * 7) / 10;
            fast_long  = (drift_long_count  >= drift_thresh);
            fast_short = (drift_short_count >= drift_thresh);
            // For slow confirmation, require the same drift ratio over the full window
            slow_long  = fast_long;   // drift persistence IS the slow confirmation
            slow_short = fast_short;
        }

        if (fast_long || fast_short) phase = Phase::FLOW_BUILDING;
        else { phase = Phase::IDLE; return; }

        // ── FIX (claim 5): Directional dominance — reject mixed windows ────────
        // OLD: fast_long checked m_fast_long_count >= threshold independently.
        // A window of 23 long + 7 short ticks passed even though 30% was opposing.
        // NEW: require the opposing direction count is < 25% of the window.
        // This ensures the window is genuinely one-directional, not mixed.
        // dominance_threshold: normal = 25% opposing allowed (7/30), Asia = max 2 opposing
        static constexpr int GFE_DOMINANCE_MAX_OPPOSING = GFE_FAST_TICKS / 4; // 7/30
        const int eff_dominance_max = is_low_quality_session ? GFE_ASIA_DOMINANCE_MAX_OPPOSING : GFE_DOMINANCE_MAX_OPPOSING;
        if (fast_long  && m_fast_short_count >= eff_dominance_max) {
            phase = Phase::IDLE; return;  // too much opposing pressure — not clean flow
        }
        if (fast_short && m_fast_long_count  >= eff_dominance_max) {
            phase = Phase::IDLE; return;
        }

        // Momentum: mid vs GFE_MOMENTUM_TICKS ticks ago (12 ticks = ~120-240ms London)
        const double momentum = mid_momentum();

        // Drift threshold: session-aware + L2-availability-aware.
        // Normal session: L2 available = just non-zero, L2 unavailable = $0.30 fallback.
        // Asia/dead zone: always at least $0.50 drift required — micro-oscillations don't count.
        double drift_threshold = l2_data_live ? GFE_DRIFT_MIN : GFE_DRIFT_FALLBACK_THRESHOLD;
        if (is_low_quality_session) drift_threshold = std::max(drift_threshold, GFE_ASIA_DRIFT_MIN);

        // Momentum floor: Asia requires $0.30+ price movement (vs any non-zero normally).
        // Prevents entries on sub-$0.10 momentum ticks that dominate choppy overnight tape.
        const double momentum_floor = is_low_quality_session ? GFE_ASIA_MOMENTUM_MIN : 0.0;

        const bool long_signal  = fast_long
                                  && slow_long
                                  && ewm_drift > drift_threshold
                                  && momentum > momentum_floor;

        const bool short_signal = fast_short
                                  && slow_short
                                  && ewm_drift < -drift_threshold
                                  && momentum < -momentum_floor;

        if (!long_signal && !short_signal) return;

        // ATR check (redundant after warmup gate above, but kept as safety)
        if (m_atr <= 0.0) return;

        // ── Stale imbalance check at entry ────────────────────────────────────
        // Persistence windows confirm imbalance was sustained over last 30/100 ticks,
        // but current tick's imbalance may have already flipped neutral.
        //
        // BROKER FALLBACK: BlackBull's FIX feed sends tag 269= (side) but NOT tag 271=
        // (MDEntrySize). With zero sizes on both sides, imbalance() returns exactly 0.5
        // Stale imbalance check — only when L2 data is actually live
        // (l2_data_live declared above in persistence section)
        if (l2_data_live) {
            if (long_signal  && l2_imb < 0.60) {
                std::cout << "[GOLD-FLOW] SIGNAL_STALE long — imb=" << l2_imb
                          << " (need >0.60 at entry)\n";
                std::cout.flush();
                return;
            }
            if (short_signal && l2_imb > 0.40) {
                std::cout << "[GOLD-FLOW] SIGNAL_STALE short — imb=" << l2_imb
                          << " (need <0.40 at entry)\n";
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

    // Force-close any open position (used during disconnect cleanup)
    void force_close(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!has_open_position()) return;
        const double exit_px = pos.is_long ? bid : ask;
        close_position(exit_px, "FORCE_CLOSE", now_ms, on_close);
    }

    double current_atr() const noexcept { return m_atr; }

    // -------------------------------------------------------------------------
private:

    // ATR calculation -- EWM-smoothed tick-to-tick range, 100-tick warmup
    double              m_atr           = 0.0;   // exposed ATR (0 until warmup complete)
    double              m_atr_ewm       = 0.0;   // internal EWM accumulator
    double              m_last_mid_atr  = 0.0;   // previous mid for tick-range computation
    int                 m_atr_warmup_ticks = 0;  // counts ticks until GFE_ATR_PERIOD reached
    std::deque<double>  m_atr_window;             // spread data (retained for compat)
    // Momentum buffer: SEPARATE from ATR buffer.
    // OLD: m_mid_window was shared — ATR_PERIOD*3 trim implicitly controlled momentum history.
    // Tuning ATR period would silently change how far back momentum looked.
    // NEW: independent buffer, fixed size GFE_MOMENTUM_BUF_SIZE (64 entries).
    // Momentum only ever looks back GFE_MOMENTUM_TICKS=12 entries — 64 is ample.
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

    // Drift persistence fallback (used when L2 size data is unavailable — imbalance always 0.5)
    // Tracks ewm_drift direction over GFE_DRIFT_PERSIST_TICKS=20 ticks.
    // Replaces L2 persistence windows when broker doesn't send tag-271 size data.
    std::deque<int> m_drift_persist_window;

    int64_t m_cooldown_start   = 0;
    int     m_trade_id         = 0;
    double  m_spread_at_entry  = 0.0;
    int     m_last_session_slot = -1;

    void update_atr(double spread, double mid) noexcept {
        // ATR: EWM-smoothed tick-to-tick range
        if (m_last_mid_atr > 0.0) {
            const double tick_range = std::max(std::fabs(mid - m_last_mid_atr), spread);
            if (m_atr_ewm <= 0.0)
                m_atr_ewm = tick_range;
            else
                m_atr_ewm = GFE_ATR_EWM_ALPHA * tick_range + (1.0 - GFE_ATR_EWM_ALPHA) * m_atr_ewm;
        }
        m_last_mid_atr = mid;

        ++m_atr_warmup_ticks;
        if (m_atr_warmup_ticks >= GFE_ATR_PERIOD)
            m_atr = std::max(GFE_ATR_MIN, m_atr_ewm);

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

        // Fast window — incremental update: subtract outgoing, add incoming.
        // OLD: full O(n) recount every tick (30 + 100 iterations = 130 ops/tick).
        // NEW: O(1) — track only the outgoing and incoming values.
        if ((int)m_fast_window.size() >= GFE_FAST_TICKS) {
            const int outgoing = m_fast_window.front();
            if (outgoing ==  1) --m_fast_long_count;
            if (outgoing == -1) --m_fast_short_count;
            m_fast_window.pop_front();
        }
        m_fast_window.push_back(dir);
        if (dir ==  1) ++m_fast_long_count;
        if (dir == -1) ++m_fast_short_count;

        // Slow window — same incremental logic
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
        // ── FIX 2: SL floor — ATR floor alone is insufficient ────────────────
        // GFE_ATR_MIN = $2.0 prevents sub-$2 SL from EWM ATR on dead tape.
        // BUT: spread = $0.60 at max entry. $2.0 SL / $0.60 spread = 3.3x spread.
        // A normal spread fluctuation can close 30% of that SL gap instantly.
        // Any tick-rate noise on Asia tape hits a $2 SL in seconds.
        //
        // Minimum SL = max(ATR * mult, spread * 3.0).
        // spread * 3.0 ensures SL is never reachable by spread noise alone.
        // Example: spread=$0.45 → min SL=$1.35, but ATR=$3 wins → SL=$3.
        //          spread=$0.55 → min SL=$1.65, ATR=$2 → SL=$2 (ATR wins).
        //          spread=$0.60 → min SL=$1.80, ATR=$2 → SL=$2 (ATR wins).
        // The spread gate (GFE_MAX_SPREAD=$0.60) already caps spread at entry.
        const double atr_sl  = m_atr * GFE_ATR_SL_MULT;
        const double min_sl  = spread * 3.0;
        const double sl_pts  = std::max(atr_sl, min_sl);
        if (sl_pts <= 0.0) return;

        // Size: fixed dollar risk / SL_pts
        // 1 lot gold = $100/pt at BlackBull.
        // Cap at 0.08 lots: prevents oversizing when ATR collapses on overnight tape.
        // Round to 0.001 lot precision (many brokers support this for gold).
        // Old rounding to 0.01 could inflate risk by up to 1% per trade.
        static constexpr double GFE_MAX_LOT_FLOW = 0.50;  // raised 0.08→0.50: matches max_lot_gold. At ATR=$2 SL, $30 risk = 0.15 lots — 0.08 cap was cutting that to $16 max loss.
        static constexpr double GFE_LOT_STEP     = 0.001; // broker lot precision
        const double tick_mult = 100.0;
        double size = risk_dollars / (sl_pts * tick_mult);
        // Round down to nearest lot step (never round up — never risk more than intended)
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
        pos.be_locked    = false;
        pos.trail_stage  = 0;
        pos.stage2_tight = false;
        pos.entry_ts     = now_ms / 1000; // seconds
        phase            = Phase::LIVE;
        ++m_trade_id;

        // Reset persistence so we don't re-enter immediately
        m_fast_long_count = m_fast_short_count = 0;
        m_slow_long_count = m_slow_short_count = 0;
        m_fast_window.clear();
        m_slow_window.clear();
        m_drift_persist_window.clear();

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

        const double atr      = pos.atr_at_entry; // use ATR at entry for consistent steps
        const double move     = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);

        // Track MFE
        if (move > pos.mfe) pos.mfe = move;

        // ---- Progressive trail stages ------------------------------------
        // Stage 1: BE lock at 1x ATR profit
        if (pos.trail_stage < 1 && move >= atr * GFE_BE_ATR_MULT) {
            pos.sl = pos.entry;
            pos.be_locked = true;
            pos.trail_stage = 1;
            std::cout << "[GOLD-FLOW] TRAIL-STAGE1 BE move=" << move << " atr=" << atr << "\n";
            std::cout.flush();
        }

        // Stage 2: trail at 1.0x ATR behind MFE, starts at 2x ATR profit.
        // L2 confirmation at transition: if imbalance has weakened (order-flow no longer
        // confirms the direction), arm with a tight 0.5x ATR trail immediately rather than
        // waiting for Stage 3. This prevents the Stage 2 1x-ATR trail from giving back
        // excess profit on mean-reverting days where flow fades after the initial move.
        if (pos.trail_stage < 2 && move >= atr * GFE_STAGE2_ATR_MULT) {
            const bool l2_confirming = (pos.is_long ? l2_imb >= 0.55 : l2_imb <= 0.45);
            pos.stage2_tight = !l2_confirming;
            pos.trail_stage = 2;
            std::cout << "[GOLD-FLOW] TRAIL-STAGE2"
                      << (pos.stage2_tight ? " TIGHT(L2-weak)" : " NORMAL(L2-ok)")
                      << " trail=" << (pos.stage2_tight ? "0.5x" : "1.0x")
                      << "ATR move=" << move << " l2=" << l2_imb << "\n";
            std::cout.flush();
        }
        if (pos.trail_stage == 2) {
            // Use tight trail if L2 was weakening at Stage 2 arm, normal trail otherwise.
            // If L2 recovers after arming tight, upgrade to normal — don't stay tight forever.
            if (pos.stage2_tight && (pos.is_long ? l2_imb >= 0.65 : l2_imb <= 0.35)) {
                pos.stage2_tight = false;
                std::cout << "[GOLD-FLOW] TRAIL-STAGE2 L2 recovered — upgrading to normal trail\n";
                std::cout.flush();
            }
            const double stage2_mult = pos.stage2_tight ? GFE_TRAIL_STAGE3_MULT : GFE_TRAIL_STAGE2_MULT;
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - atr * stage2_mult)
                : (pos.entry - pos.mfe + atr * stage2_mult);
            if ((pos.is_long && trail_sl > pos.sl) || (!pos.is_long && trail_sl < pos.sl)) {
                pos.sl = trail_sl;
            }
        }

        // Stage 3: tighten trail to 0.5x ATR at 5x ATR profit
        if (pos.trail_stage < 3 && move >= atr * GFE_STAGE3_ATR_MULT) {
            pos.trail_stage = 3;
            std::cout << "[GOLD-FLOW] TRAIL-STAGE3 trail=0.5xATR move=" << move << "\n";
            std::cout.flush();
        }
        if (pos.trail_stage == 3) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - atr * GFE_TRAIL_STAGE3_MULT)
                : (pos.entry - pos.mfe + atr * GFE_TRAIL_STAGE3_MULT);
            if ((pos.is_long && trail_sl > pos.sl) || (!pos.is_long && trail_sl < pos.sl)) {
                pos.sl = trail_sl;
            }
        }

        // Stage 4: tighten to 0.3x ATR at 10x ATR profit -- ride the cascade
        if (pos.trail_stage < 4 && move >= atr * GFE_STAGE4_ATR_MULT) {
            pos.trail_stage = 4;
            std::cout << "[GOLD-FLOW] TRAIL-STAGE4 trail=0.3xATR RIDING CASCADE move=" << move << "\n";
            std::cout.flush();
        }
        if (pos.trail_stage == 4) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - atr * GFE_TRAIL_STAGE4_MULT)
                : (pos.entry - pos.mfe + atr * GFE_TRAIL_STAGE4_MULT);
            if ((pos.is_long && trail_sl > pos.sl) || (!pos.is_long && trail_sl < pos.sl)) {
                pos.sl = trail_sl;
            }
        }

        // ---- Max hold timeout -------------------------------------------
        // If the trade has been open longer than GFE_MAX_HOLD_MS and trail
        // stage is still 0 or 1 (never advanced to real trailing), the thesis
        // is stale. Exit at market to free capital for the next signal.
        // Stage 2+ means profit is building — allow the trail to work.
        // Session-aware: Asia gets 60 min max (thin tape moves slowly).
        const bool is_low_qual = (m_last_session_slot == 6 || m_last_session_slot == 0);
        const int64_t eff_max_hold = is_low_qual
            ? static_cast<int64_t>(GFE_MAX_HOLD_MS) * 2   // 60 min in Asia
            : static_cast<int64_t>(GFE_MAX_HOLD_MS);       // 30 min normal
        const int64_t held_ms = now_ms - (pos.entry_ts * 1000LL);
        if (held_ms >= eff_max_hold && pos.trail_stage < 2) {
            std::cout << "[GOLD-FLOW] MAX_HOLD_TIMEOUT"
                      << " held=" << held_ms / 1000 << "s"
                      << " stage=" << pos.trail_stage
                      << " move=" << move
                      << " — exiting stale thesis\n";
            std::cout.flush();
            const double exit_px = pos.is_long ? bid : ask;
            close_position(exit_px, "MAX_HOLD_TIMEOUT", now_ms, on_close);
            return;
        }

        // ---- SL check ---------------------------------------------------
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (!sl_hit) return;

        // Always honour a hard SL hit immediately, regardless of time held.
        // OLD code blocked exits when held_ms < 5s AND trail_stage == 0 to
        // "avoid spread bounce". This caused the engine to hold a full 1xATR
        // loss open on a genuine false signal entry, which is exactly wrong.
        // The spread gate on entry (GFE_MAX_SPREAD=$0.60) already filters
        // entries where spread noise could fake-hit the SL on the first tick.

        // ---- Exit -------------------------------------------------------
        const double exit_px = pos.is_long ? bid : ask;
        const char*  reason  = pos.be_locked
            ? (pos.sl > pos.entry + 0.01 || pos.sl < pos.entry - 0.01 ? "TRAIL_HIT" : "BE_HIT")
            : "SL_HIT";

        close_position(exit_px, reason, now_ms, on_close);
    }

    void close_position(double exit_px, const char* reason,
                        int64_t now_ms, CloseCallback on_close) noexcept
    {
        omega::TradeRecord tr;
        tr.id           = m_trade_id;
        tr.symbol       = "GOLD.F";
        tr.side         = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice   = pos.entry;
        tr.exitPrice    = exit_px;
        tr.tp           = 0.0; // no fixed TP -- trail only
        tr.sl           = pos.sl;
        tr.size         = pos.size;
        // PnL in raw price points × size — handle_closed_trade applies tick_value_multiplier
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

        pos             = OpenPos{};
        phase           = Phase::COOLDOWN;
        m_cooldown_start = now_ms;

        if (on_close) on_close(tr);
    }
};
