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
//  Usage in main.cpp:
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
static constexpr int    GFE_ATR_PERIOD        = 20;    // ATR lookback ticks
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
                 CloseCallback& on_close) noexcept
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
        const bool fast_long  = (m_fast_long_count  >= GFE_FAST_TICKS);
        const bool fast_short = (m_fast_short_count >= GFE_FAST_TICKS);
        const bool slow_long  = (m_slow_long_count  >= GFE_SLOW_TICKS);
        const bool slow_short = (m_slow_short_count >= GFE_SLOW_TICKS);

        if (fast_long || fast_short) phase = Phase::FLOW_BUILDING;
        else { phase = Phase::IDLE; return; }

        // Momentum: mid vs 5 ticks ago
        const double momentum = mid_momentum();

        // Drift and momentum confirmation
        const bool long_signal  = fast_long
                                  && (slow_long || m_slow_long_count >= GFE_SLOW_TICKS * 0.6)
                                  && ewm_drift > GFE_DRIFT_MIN
                                  && momentum > 0.0;

        const bool short_signal = fast_short
                                  && (slow_short || m_slow_short_count >= GFE_SLOW_TICKS * 0.6)
                                  && ewm_drift < -GFE_DRIFT_MIN
                                  && momentum < 0.0;

        if (!long_signal && !short_signal) return;

        // ATR must be warmed up
        if (m_atr <= 0.0) return;

        // Fire entry
        enter(long_signal, mid, bid, ask, spread, now_ms);
    }

    bool has_open_position() const noexcept {
        return phase == Phase::LIVE;
    }

    double current_atr() const noexcept { return m_atr; }

    // -------------------------------------------------------------------------
private:

    // ATR calculation (20-tick rolling spread/range proxy)
    double              m_atr           = 0.0;
    std::deque<double>  m_atr_window;
    std::deque<double>  m_mid_window;   // for momentum

    // Persistence counters -- count consecutive ticks above/below threshold
    int     m_fast_long_count  = 0;
    int     m_fast_short_count = 0;
    int     m_slow_long_count  = 0;
    int     m_slow_short_count = 0;

    int64_t m_cooldown_start   = 0;
    int     m_trade_id         = 0;
    double  m_spread_at_entry  = 0.0;

    void update_atr(double spread, double mid) noexcept {
        m_mid_window.push_back(mid);
        if ((int)m_mid_window.size() > GFE_ATR_PERIOD * 3)
            m_mid_window.pop_front();

        // Use high-low range proxy: spread + mid movement tick-to-tick
        m_atr_window.push_back(spread);
        if ((int)m_atr_window.size() > GFE_ATR_PERIOD * 3)
            m_atr_window.pop_front();

        if ((int)m_atr_window.size() >= GFE_ATR_PERIOD) {
            double sum = 0.0;
            const int n = (int)m_atr_window.size();
            for (int i = n - GFE_ATR_PERIOD; i < n; ++i)
                sum += m_atr_window[i];
            // ATR from spread is too tight -- use mid range instead
            // Range ATR: max-min of mid over lookback
            const auto mb = m_mid_window.end() - std::min((int)m_mid_window.size(), GFE_ATR_PERIOD);
            const double range_hi = *std::max_element(mb, m_mid_window.end());
            const double range_lo = *std::min_element(mb, m_mid_window.end());
            m_atr = range_hi - range_lo;
        }
    }

    double mid_momentum() const noexcept {
        if ((int)m_mid_window.size() < GFE_MOMENTUM_TICKS + 1) return 0.0;
        return m_mid_window.back() - m_mid_window[m_mid_window.size() - 1 - (int)GFE_MOMENTUM_TICKS];
    }

    void update_persistence(double l2_imb, int64_t /*now_ms*/) noexcept {
        // Fast window: consecutive ticks
        if (l2_imb > GFE_LONG_THRESHOLD) {
            ++m_fast_long_count;
            m_fast_short_count = 0;
        } else if (l2_imb < GFE_SHORT_THRESHOLD) {
            ++m_fast_short_count;
            m_fast_long_count = 0;
        } else {
            // Neutral tick -- decay slowly (don't reset instantly, allows small retracements)
            if (m_fast_long_count  > 0) --m_fast_long_count;
            if (m_fast_short_count > 0) --m_fast_short_count;
        }
        // Slow window: same logic, separate counter
        if (l2_imb > GFE_LONG_THRESHOLD) {
            ++m_slow_long_count;
            m_slow_short_count = 0;
        } else if (l2_imb < GFE_SHORT_THRESHOLD) {
            ++m_slow_short_count;
            m_slow_long_count = 0;
        } else {
            if (m_slow_long_count  > 0) --m_slow_long_count;
            if (m_slow_short_count > 0) --m_slow_short_count;
        }
    }

    void enter(bool is_long, double mid, double bid, double ask,
               double spread, int64_t now_ms) noexcept
    {
        const double sl_pts  = m_atr * GFE_ATR_SL_MULT;
        if (sl_pts <= 0.0) return;

        // Size: fixed dollar risk / SL_pts
        // 1 lot gold = $100/pt at BlackBull. Adjust tick_mult if different.
        const double tick_mult = 100.0; // $100 per pt per lot
        double size = risk_dollars / (sl_pts * tick_mult);
        size = std::max(GFE_MIN_LOT, std::min(GFE_MAX_LOT, std::round(size * 100.0) / 100.0));

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

        std::cout << "[GOLD-FLOW] ENTRY " << (is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px
                  << " sl_pts=" << sl_pts
                  << " atr=" << m_atr
                  << " size=" << size
                  << " spread=" << spread << "\n";
        std::cout.flush();
    }

    void manage_position(double bid, double ask, double mid, double spread,
                         int64_t now_ms, CloseCallback& on_close) noexcept
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

        // ---- Minimum hold time ------------------------------------------
        const int64_t held_ms = now_ms - (pos.entry_ts * 1000);
        if (held_ms < GFE_MIN_HOLD_MS && pos.trail_stage == 0) {
            // Still in initial SL zone and not held long enough -- let it run
            // (avoids exiting on the very first tick's spread bounce)
            return;
        }

        // ---- Exit -------------------------------------------------------
        const double exit_px = pos.is_long ? bid : ask;
        const char*  reason  = pos.be_locked
            ? (pos.sl > pos.entry + 0.01 || pos.sl < pos.entry - 0.01 ? "TRAIL_HIT" : "BE_HIT")
            : "SL_HIT";

        close_position(exit_px, reason, now_ms, on_close);
    }

    void close_position(double exit_px, const char* reason,
                        int64_t now_ms, CloseCallback& on_close) noexcept
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
