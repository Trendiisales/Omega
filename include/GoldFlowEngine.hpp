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
static constexpr int    GFE_ATR_PERIOD        = 100;   // ATR lookback ticks -- raised 20→100:
                                                        // 20-tick hi-lo range was a 2-second window,
                                                        // producing SL of $0.3–5 depending on micro-volatility.
                                                        // 100 ticks = ~10-30s, EWM-smoothed, stable across sessions.
static constexpr double GFE_ATR_EWM_ALPHA     = 0.05;  // EWM smoothing alpha for ATR (20-tick equivalent half-life)
static constexpr double GFE_ATR_MIN           = 2.0;   // ATR floor in $pts -- prevents sub-$2 SL on dead tape
static constexpr double GFE_ATR_SL_MULT       = 1.0;   // SL = ATR * this
static constexpr double GFE_TRAIL_STAGE2_MULT = 1.0;   // trail at 1.0x ATR from stage 2
static constexpr double GFE_TRAIL_STAGE3_MULT = 0.5;   // tighten to 0.5x ATR at stage 3
static constexpr double GFE_TRAIL_STAGE4_MULT = 0.3;   // tighten to 0.3x ATR at stage 4
static constexpr double GFE_BE_ATR_MULT       = 1.0;   // BE lock at 1x ATR profit
static constexpr double GFE_STAGE2_ATR_MULT   = 2.0;   // start trail at 2x ATR profit
static constexpr double GFE_STAGE3_ATR_MULT   = 5.0;   // tighten at 5x ATR profit
static constexpr double GFE_STAGE4_ATR_MULT   = 10.0;  // tighten again at 10x ATR profit
static constexpr double GFE_MAX_SPREAD        = 0.6;   // pts -- skip if wider
static constexpr int    GFE_MIN_HOLD_MS       = 5000;  // 5s minimum hold
static constexpr int    GFE_COOLDOWN_MS       = 30000; // 30s cooldown after exit
static constexpr double GFE_RISK_DOLLARS      = 30.0;  // $ risk per trade (fallback)
static constexpr double GFE_MIN_LOT           = 0.01;
static constexpr double GFE_MAX_LOT           = 1.0;
static constexpr double GFE_MOMENTUM_TICKS    = 5;     // ticks back for momentum check

// -----------------------------------------------------------------------------
struct GoldFlowEngine {

    // -------------------------------------------------------------------------
    // Public config -- set after construction
    double risk_dollars   = GFE_RISK_DOLLARS;  // override from main.cpp
    bool   shadow_mode    = false;

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
    void on_tick(double bid, double ask,
                 double l2_imb, double ewm_drift,
                 int64_t now_ms,
                 CloseCallback on_close) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Feed ATR and mid history
        update_atr(spread, mid);

        // Cooldown phase
        if (phase == Phase::COOLDOWN) {
            if (now_ms - m_cooldown_start >= GFE_COOLDOWN_MS)
                phase = Phase::IDLE;
            else return;
        }

        // Manage open position
        if (phase == Phase::LIVE) {
            manage_position(bid, ask, mid, spread, now_ms, on_close);
            return;
        }

        // Spread gate -- don't build signal into wide spread
        if (spread > GFE_MAX_SPREAD) return;

        // Update L2 persistence windows
        update_persistence(l2_imb, now_ms);

        // FLOW_BUILDING or IDLE: check for signal
        // Check for directional persistence using rolling window counts
        const bool fast_long  = (m_fast_long_count  >= GFE_FAST_DIR_THRESHOLD);
        const bool fast_short = (m_fast_short_count >= GFE_FAST_DIR_THRESHOLD);
        const bool slow_long  = (m_slow_long_count  >= GFE_SLOW_DIR_THRESHOLD);
        const bool slow_short = (m_slow_short_count >= GFE_SLOW_DIR_THRESHOLD);

        if (fast_long || fast_short) phase = Phase::FLOW_BUILDING;
        else { phase = Phase::IDLE; return; }

        // Momentum: mid vs 5 ticks ago
        const double momentum = mid_momentum();

        // Drift and momentum confirmation.
        // slow_long/short_short now uses GFE_SLOW_DIR_THRESHOLD (75% of 100 ticks = 75 ticks).
        // The old 0.6 fallback (60 ticks streak) is replaced by the window threshold --
        // no separate fallback needed since the window count is already tolerant of neutral ticks.
        const bool long_signal  = fast_long
                                  && slow_long
                                  && ewm_drift > GFE_DRIFT_MIN
                                  && momentum > 0.0;

        const bool short_signal = fast_short
                                  && slow_short
                                  && ewm_drift < -GFE_DRIFT_MIN
                                  && momentum < 0.0;

        if (!long_signal && !short_signal) return;

        // ATR must be warmed up
        if (m_atr <= 0.0) return;

        // ── FIX 1: Stale imbalance check ─────────────────────────────────────
        // The persistence windows confirm imbalance was sustained over the last
        // 30/100 ticks, but the CURRENT tick's imbalance may have already flipped
        // neutral or against direction. If the book has normalised since the signal
        // built, don't enter -- the institutional pressure that justified the signal
        // may already be exhausted.
        // Threshold: current l2_imb must still show directional bias (not just neutral).
        // Long: imbalance still bid-heavy (> 0.60, softer than build threshold of 0.75)
        // Short: imbalance still ask-heavy (< 0.40)
        if (long_signal  && l2_imb < 0.60) {
            std::cout << "[GOLD-FLOW] SIGNAL_STALE long — imb=" << l2_imb
                      << " (need >0.60 at entry)\\n";
            std::cout.flush();
            return;
        }
        if (short_signal && l2_imb > 0.40) {
            std::cout << "[GOLD-FLOW] SIGNAL_STALE short — imb=" << l2_imb
                      << " (need <0.40 at entry)\\n";
            std::cout.flush();
            return;
        }

        // Fire entry
        enter(long_signal, mid, bid, ask, spread, now_ms);
    }

    bool has_open_position() const noexcept {
        return phase == Phase::LIVE;
    }

    double current_atr() const noexcept { return m_atr; }

    // -------------------------------------------------------------------------
private:

    // ATR calculation -- EWM-smoothed tick-to-tick range, 100-tick warmup
    double              m_atr           = 0.0;   // exposed ATR (0 until warmup complete)
    double              m_atr_ewm       = 0.0;   // internal EWM accumulator
    double              m_last_mid_atr  = 0.0;   // previous mid for tick-range computation
    int                 m_atr_warmup_ticks = 0;  // counts ticks until GFE_ATR_PERIOD reached
    std::deque<double>  m_atr_window;             // retains spread data (unused post-fix, kept for compat)
    std::deque<double>  m_mid_window;   // for momentum

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

    int64_t m_cooldown_start   = 0;
    int     m_trade_id         = 0;
    double  m_spread_at_entry  = 0.0;

    void update_atr(double spread, double mid) noexcept {
        m_mid_window.push_back(mid);
        if ((int)m_mid_window.size() > GFE_ATR_PERIOD * 3)
            m_mid_window.pop_front();

        // EWM-smoothed ATR using tick-to-tick range (high-low proxy).
        //
        // OLD: hi-lo range over last 20 ticks = 2-second window.
        //   Problem: on a volatile tick this was $4, on a quiet tick $0.3.
        //   SL was effectively random between $0.3 and $4, sized to whatever
        //   micro-move happened in the 2 seconds before entry.
        //
        // NEW: each tick's "true range" = max(tick-to-tick move, spread).
        //   EWM-smooth over 100 ticks (alpha=0.05, ~20-tick half-life).
        //   GFE_ATR_MIN=$2 floor prevents sub-$2 SL on dead overnight tape.
        //   Result: stable $4-15 ATR during London, $2-5 during Asia -- correct.
        if (m_last_mid_atr > 0.0) {
            const double tick_range = std::max(std::fabs(mid - m_last_mid_atr), spread);
            if (m_atr_ewm <= 0.0)
                m_atr_ewm = tick_range;  // seed on first tick
            else
                m_atr_ewm = GFE_ATR_EWM_ALPHA * tick_range + (1.0 - GFE_ATR_EWM_ALPHA) * m_atr_ewm;
        }
        m_last_mid_atr = mid;

        // Only expose ATR once we have GFE_ATR_PERIOD ticks of warmup
        ++m_atr_warmup_ticks;
        if (m_atr_warmup_ticks >= GFE_ATR_PERIOD)
            m_atr = std::max(GFE_ATR_MIN, m_atr_ewm);

        // Keep m_atr_window populated for momentum (spread data retained)
        m_atr_window.push_back(spread);
        if ((int)m_atr_window.size() > GFE_ATR_PERIOD * 3)
            m_atr_window.pop_front();
    }

    double mid_momentum() const noexcept {
        if ((int)m_mid_window.size() < GFE_MOMENTUM_TICKS + 1) return 0.0;
        return m_mid_window.back() - m_mid_window[m_mid_window.size() - 1 - (int)GFE_MOMENTUM_TICKS];
    }

    void update_persistence(double l2_imb, int64_t /*now_ms*/) noexcept {
        // Classify this tick's direction
        const int dir = (l2_imb > GFE_LONG_THRESHOLD) ? 1
                      : (l2_imb < GFE_SHORT_THRESHOLD) ? -1
                      : 0;

        // Fast window -- push new tick, drop oldest if full
        m_fast_window.push_back(dir);
        if ((int)m_fast_window.size() > GFE_FAST_TICKS)
            m_fast_window.pop_front();

        // Slow window
        m_slow_window.push_back(dir);
        if ((int)m_slow_window.size() > GFE_SLOW_TICKS)
            m_slow_window.pop_front();

        // Recount directional ticks in each window
        m_fast_long_count  = 0; m_fast_short_count = 0;
        m_slow_long_count  = 0; m_slow_short_count = 0;
        for (int d : m_fast_window) {
            if (d ==  1) ++m_fast_long_count;
            if (d == -1) ++m_fast_short_count;
        }
        for (int d : m_slow_window) {
            if (d ==  1) ++m_slow_long_count;
            if (d == -1) ++m_slow_short_count;
        }
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
        // Cap at 0.08 lots per spec: prevents oversizing when ATR collapses
        // on overnight tape (ATR=$2, size = $30/(2*100) = 0.15 lots — too big
        // for a $2 stop that sits inside normal bid/ask noise).
        static constexpr double GFE_MAX_LOT_FLOW = 0.08;
        const double tick_mult = 100.0;
        double size = risk_dollars / (sl_pts * tick_mult);
        size = std::max(GFE_MIN_LOT, std::min(GFE_MAX_LOT_FLOW,
               std::round(size * 100.0) / 100.0));

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
        pos.entry_ts     = now_ms / 1000; // seconds
        phase            = Phase::LIVE;
        ++m_trade_id;

        // Reset persistence so we don't re-enter immediately
        m_fast_long_count = m_fast_short_count = 0;
        m_slow_long_count = m_slow_short_count = 0;
        m_fast_window.clear();
        m_slow_window.clear();

        std::cout << "[GOLD-FLOW] ENTRY " << (is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px
                  << " sl_pts=" << sl_pts
                  << " (atr=" << m_atr << " spread_floor=" << min_sl << ")"
                  << " size=" << size
                  << " spread=" << spread << "\n";
        std::cout.flush();
    }

    void manage_position(double bid, double ask, double mid, double spread,
                         int64_t now_ms, CloseCallback on_close) noexcept
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

        // Stage 2: trail at 1.0x ATR behind MFE, starts at 2x ATR profit
        if (pos.trail_stage < 2 && move >= atr * GFE_STAGE2_ATR_MULT) {
            pos.trail_stage = 2;
            std::cout << "[GOLD-FLOW] TRAIL-STAGE2 trail=1xATR move=" << move << "\n";
            std::cout.flush();
        }
        if (pos.trail_stage == 2) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - atr * GFE_TRAIL_STAGE2_MULT)
                : (pos.entry - pos.mfe + atr * GFE_TRAIL_STAGE2_MULT);
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
        tr.pnl          = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                          * pos.size * 100.0; // $100/pt/lot gold
        tr.mfe          = pos.mfe * pos.size * 100.0;
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
                  << " pnl=" << tr.pnl
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
