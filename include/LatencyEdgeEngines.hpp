#pragma once
// =============================================================================
// LatencyEdgeEngines.hpp -- Co-location Speed Advantage Engines
//
// These engines are designed specifically for the 0.3-4ms RTT advantage of a
// co-located VPS. They exploit timing edges unavailable to retail traders on
// home connections (20-100ms RTT).
//
// ENGINES (ACTIVE):
//   2. GoldSpreadDislocation -- Large order hits Gold book ? fade the spike
//   3. GoldEventCompression  -- Tight pre-news compression ? early breakout entry
//
// DELETED:
//   1. GoldSilverLeadLag -- DELETED 2026-03-31. -$66 on 1 trade, held 68min.
//      Silver trading suspended. XAG correlation edge not validated.
//
// NOTE: SpreadDislocation + EventCompression currently run in MANAGE-ONLY mode.
// New entries disabled (RTT ~68ms, latency edge requires <1ms). Existing
// positions drained to TP/SL. Re-enable new entries if RTT drops below 5ms.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>
#include <deque>
#include <algorithm>
#include <functional>
#include "OmegaTradeLedger.hpp"

namespace omega {
namespace latency {

// ?????????????????????????????????????????????????????????????????????????????
// Helpers
// ?????????????????????????????????????????????????????????????????????????????
static inline int64_t le_now_sec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static inline int64_t le_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ?????????????????????????????????????????????????????????????????????????????
// LeSignal -- what these engines return on a new entry
// ?????????????????????????????????????????????????????????????????????????????
struct LeSignal {
    bool        valid     = false;
    bool        is_long   = true;
    double      entry     = 0.0;
    double      tp        = 0.0;
    double      sl        = 0.0;
    double      size      = 0.01;  // fallback min_lot -- overridden by compute_size() in main
    const char* engine    = "";
    const char* reason    = "";
};

// ?????????????????????????????????????????????????????????????????????????????
// LePosition -- open position managed by each engine
// ?????????????????????????????????????????????????????????????????????????????
struct LePosition {
    bool    active          = false;
    bool    is_long         = true;
    double  entry           = 0.0;
    double  tp              = 0.0;
    double  sl              = 0.0;
    double  size            = 0.01;  // fallback min_lot -- overridden by compute_size() in main
    double  mfe             = 0.0;
    double  mae             = 0.0;
    double  spread_at_entry = 0.0;
    int64_t entry_ts        = 0;
    char    symbol[16]      = {};   // explicit symbol string e.g. "XAUUSD" "XAGUSD"
    char    engine[32]      = {};
    char    reason[32]      = {};
};

// ?????????????????????????????????????????????????????????????????????????????
// LePositionManager -- manages one open position with TP/SL/trail/timeout
// ?????????????????????????????????????????????????????????????????????????????
class LePositionManager {
public:
    LePosition pos;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // Returns true if position was closed this tick
    bool manage(double bid, double ask, double latency_ms,
                const char* regime, CloseCb& on_close,
                int max_hold_sec = 120) noexcept {
        if (!pos.active) return false;
        const double mid = (bid + ask) * 0.5;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (-move > pos.mae) pos.mae = -move;

        // Trailing stop -- same 3-stage logic as BreakoutEngine
        const double move_pct = pos.entry > 0.0 ? move / pos.entry * 100.0 : 0.0;
        if (move_pct >= 1.60) {
            const double trail = pos.is_long
                ? mid * (1.0 - 0.25 / 100.0)
                : mid * (1.0 + 0.25 / 100.0);
            if ( pos.is_long && trail > pos.sl) pos.sl = trail;
            if (!pos.is_long && trail < pos.sl) pos.sl = trail;
        } else if (move_pct >= 1.00) {
            const double trail = pos.is_long
                ? mid * (1.0 - 0.40 / 100.0)
                : mid * (1.0 + 0.40 / 100.0);
            if ( pos.is_long && trail > pos.sl) pos.sl = trail;
            if (!pos.is_long && trail < pos.sl) pos.sl = trail;
        } else if (move_pct >= 0.60) {
            const double be = pos.is_long
                ? pos.entry * (1.0 + 0.10 / 100.0)
                : pos.entry * (1.0 - 0.10 / 100.0);
            if ( pos.is_long && be > pos.sl) pos.sl = be;
            if (!pos.is_long && be < pos.sl) pos.sl = be;
        }

        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        const bool timeout = (le_now_sec() - pos.entry_ts) >= max_hold_sec;

        if (tp_hit || sl_hit || timeout) {
            // Cap timeout at SL if price has blown through -- prevents holding to
            // timeout at a price far beyond the intended stop due to sparse ticks.
            double exit_px;
            if (tp_hit)       exit_px = pos.tp;
            else if (sl_hit)  exit_px = pos.sl;
            else {
                // timeout -- use mid, but cap at SL if breached
                const bool sl_breached = pos.is_long ? (mid < pos.sl) : (mid > pos.sl);
                exit_px = sl_breached ? pos.sl : mid;
            }
            const char* reason = tp_hit ? "TP_HIT" : (sl_hit ? "SL_HIT" : "TIMEOUT");
            close(exit_px, reason, latency_ms, regime, on_close);
            return true;
        }
        return false;
    }

    void open(const LeSignal& sig, double spread, const char* symbol) noexcept {
        pos.active          = true;
        pos.is_long         = sig.is_long;
        pos.entry           = sig.entry;
        pos.tp              = sig.tp;
        pos.sl              = sig.sl;
        pos.size            = sig.size;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.spread_at_entry = spread;
        pos.entry_ts        = le_now_sec();
        strncpy(pos.symbol, symbol ? symbol : "XAUUSD", 15);
        strncpy(pos.engine, sig.engine, 31);
        strncpy(pos.reason, sig.reason, 31);
    }

    void force_close(double bid, double ask, double latency_ms,
                     const char* regime, CloseCb& on_close) noexcept {
        if (!pos.active) return;
        close((bid + ask) * 0.5, "FORCE_CLOSE", latency_ms, regime, on_close);
    }

private:
    int trade_id_ = 5000;  // separate ID range from other engines

    void close(double exit_px, const char* why, double latency_ms,
               const char* regime, CloseCb& on_close) noexcept {
        omega::TradeRecord tr;
        tr.id          = trade_id_++;
        tr.symbol      = std::string(pos.symbol[0] ? pos.symbol : "XAUUSD");
        tr.side        = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice  = pos.entry;
        tr.exitPrice   = exit_px;
        tr.tp          = pos.tp;
        tr.sl          = pos.sl;
        tr.size        = pos.size;
        tr.pnl         = (pos.is_long ? (exit_px - pos.entry)
                                      : (pos.entry - exit_px)) * pos.size;
        tr.mfe         = pos.mfe;
        tr.mae         = pos.mae;
        tr.entryTs     = pos.entry_ts;
        tr.exitTs      = le_now_sec();
        tr.exitReason  = why;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.latencyMs   = latency_ms;
        tr.engine      = std::string(pos.engine);
        tr.regime      = regime ? regime : "";
        pos.active     = false;
        pos            = LePosition{};
        if (on_close) on_close(tr);
    }
};


// =============================================================================
// ENGINE 1 -- GoldSilverLeadLag -- DELETED
// Deleted 2026-03-31: -$66.48 on 1 trade, held 68min (latency edge held ~70x too long).
// Silver correlation edge not validated. XAG trading suspended.
// =============================================================================


// =============================================================================
// ENGINE 2 -- GoldSpreadDislocation
// =============================================================================
// When a large order hits the gold book, the spread momentarily widens as
// the bid or ask is taken out. The price typically snaps back within 50-200ms.
// With 0.3ms RTT we can fade this dislocation before slower traders react.
//
// MECHANISM:
//   - Tracks a rolling 20-tick median spread on XAUUSD
//   - When instantaneous spread exceeds median ? SPREAD_SPIKE_RATIO,
//     a dislocation is detected
//   - The direction of entry is AGAINST the spike:
//       Ask spike (ask jumped up) ? price was pushed up ? fade SHORT
//       Bid spike (bid dropped)   ? price was pushed down ? fade LONG
//   - This works because market makers reprice immediately -- the spike
//     is a momentary dislocation, not a true directional move
//
// PARAMETERS:
//   SPREAD_SPIKE_RATIO = 2.5   -- spread must be 2.5? its median to qualify
//   MIN_MEDIAN_SPREAD  = $0.30 -- only active when gold is liquid (spread > $0.30)
//   MAX_MEDIAN_SPREAD  = $1.20 -- stop trading if normally wide (illiquid session)
//   TP                 = $0.30 -- fade target: snap back to pre-spike mid
//   SL                 = $0.15 -- tight: if spread stays wide, we're wrong
//   COOLDOWN_SEC       = 15    -- fast cooldown, these happen frequently
//   MAX_HOLD_SEC       = 30    -- snaps back quickly or not at all
//   SESSION GATE       = London/NY only (07:00-20:00 UTC) -- spread dislocations
//                        in Asia are real moves, not noise
// =============================================================================
class GoldSpreadDislocation {
    // Runtime members -- set via configure() from LatencyEdgeCfg.
    // Defaults match calibrated constexpr values prior to config-driven refactor.
    double SPREAD_SPIKE_RATIO = 2.5;
    double MIN_MEDIAN_SPREAD  = 0.30;
    double MAX_MEDIAN_SPREAD  = 1.20;
    double TP                 = 0.30;
    double SL                 = 0.15;
    int    COOLDOWN_SEC       = 60;   // raised from 15s -- 3 SL hits in 45s was possible
    int    MAX_HOLD_SEC       = 30;
    static constexpr int SPREAD_WINDOW = 20;  // structural -- buffer size, not tunable

    std::deque<double> spread_history_;  // last 20 spreads
    double prev_mid_   = 0.0;
    int64_t last_entry_= 0;
    int     trade_count_ = 0;

    LePositionManager pos_mgr_;

    double median_spread() const {
        if (spread_history_.empty()) return 0.0;
        std::vector<double> v(spread_history_.begin(), spread_history_.end());
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    static bool in_active_session() noexcept {
        const auto t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        const int h = ti.tm_hour;
        // London + NY: 07:00-20:00 UTC
        // Asia window: 22:00-05:00 UTC (Tokyo open, NZ/AU morning)
        // Dead zone 05:00-07:00 and 20:00-22:00 remain blocked -- thin liquidity.
        // Spread dislocations in Asia are real moves (comment confirmed this was wrong to block).
        return (h >= 7 && h < 20) || h >= 22 || h < 5;
    }

public:
    bool has_open_position() const { return pos_mgr_.pos.active; }
    int  trade_count()       const { return trade_count_; }

    void configure(double spike_ratio, double min_med, double max_med,
                   double tp, double sl, int cooldown_sec, int max_hold_sec) {
        SPREAD_SPIKE_RATIO = spike_ratio;
        MIN_MEDIAN_SPREAD  = min_med;
        MAX_MEDIAN_SPREAD  = max_med;
        TP                 = tp;
        SL                 = sl;
        COOLDOWN_SEC       = cooldown_sec;
        MAX_HOLD_SEC       = max_hold_sec;
    }

    using CloseCb = LePositionManager::CloseCb;

    // can_enter=false: manage existing position to closure but block all new entries.
    // This is the simultaneous-position guard -- called with gold_can_enter from main.
    LeSignal on_tick(double bid, double ask, double latency_ms,
                     CloseCb on_close, bool can_enter = true) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return {};

        const double spread = ask - bid;
        const double mid    = (bid + ask) * 0.5;

        // Always manage open position -- TP/SL/timeout must run regardless of can_enter
        if (pos_mgr_.pos.active) {
            pos_mgr_.manage(bid, ask, latency_ms, "SPREAD_DISLOC", on_close, MAX_HOLD_SEC);
        }

        // New-entry gate: blocked when another XAUUSD engine already has a position
        if (!can_enter) { prev_mid_ = mid; return {}; }

        // Update spread history
        spread_history_.push_back(spread);
        if ((int)spread_history_.size() > SPREAD_WINDOW)
            spread_history_.pop_front();

        // Need full window before trading
        if ((int)spread_history_.size() < SPREAD_WINDOW) {
            prev_mid_ = mid;
            return {};
        }

        const double med = median_spread();

        // Session gate: London+NY only (07:00-20:00 UTC).
        // Spread dislocations in Asia are real moves, not noise.
        if (!in_active_session()) { prev_mid_ = mid; return {}; }

        // Median must be in liquid range
        if (med < MIN_MEDIAN_SPREAD || med > MAX_MEDIAN_SPREAD) {
            prev_mid_ = mid;
            return {};
        }

        // Cooldown gate
        if (le_now_sec() - last_entry_ < COOLDOWN_SEC) {
            prev_mid_ = mid;
            return {};
        }

        // Already in position
        if (pos_mgr_.pos.active) { prev_mid_ = mid; return {}; }

        // Spread spike detected?
        if (spread < med * SPREAD_SPIKE_RATIO) { prev_mid_ = mid; return {}; }

        // Determine direction of spike
        // If prev_mid is valid, compare ask and bid movement to prev state
        if (prev_mid_ <= 0.0) { prev_mid_ = mid; return {}; }

        // If mid jumped up (ask was taken) ? fade SHORT back to prev level
        // If mid jumped down (bid was taken) ? fade LONG back to prev level
        const double mid_jump = mid - prev_mid_;
        const bool   is_long  = (mid_jump < -0.10);  // mid dropped ? fade LONG
        const bool   is_short = (mid_jump >  0.10);  // mid jumped  ? fade SHORT

        if (!is_long && !is_short) { prev_mid_ = mid; return {}; }

        last_entry_ = le_now_sec();
        ++trade_count_;

        LeSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = is_long ? mid + TP  : mid - TP;
        sig.sl      = is_long ? mid - SL  : mid + SL;
        sig.size    = 0.01;  // fallback min_lot -- compute_size() in main overrides this
        sig.reason  = is_long ? "SPREAD_SPIKE_FADE_LONG" : "SPREAD_SPIKE_FADE_SHORT";

        pos_mgr_.open(sig, spread, "XAUUSD");

        printf("[SPREAD-DISLOC-ENTRY] Gold %s entry=%.2f tp=%.2f sl=%.2f "
               "spread=%.2f median=%.2f ratio=%.1f\n",
               sig.is_long ? "LONG" : "SHORT",
               sig.entry, sig.tp, sig.sl,
               spread, med, spread / med);
        fflush(stdout);

        prev_mid_ = mid;
        return sig;
    }

    void force_close(double bid, double ask, double latency_ms, CloseCb on_close) {
        pos_mgr_.force_close(bid, ask, latency_ms, "SPREAD_DISLOC", on_close);
    }
};

// =============================================================================
// ENGINE 3 -- GoldEventCompression
// =============================================================================
// Before major scheduled economic releases, gold compresses extremely tightly
// as traders flatten positions. The moment the number hits, gold explodes.
// With 0.3ms RTT, we can enter the breakout before slower traders confirm it.
//
// MECHANISM:
//   - Hardcoded schedule of weekly high-impact events in UTC
//   - In the 90 seconds before a release, monitors gold for tight compression
//   - If compression detected, arms an early entry with tighter trigger ($0.15
//     vs normal $0.35) because co-location means we don't need confirmation
//   - After the release, monitors the first directional tick and enters
//
// SCHEDULED EVENTS (UTC):
//   Monday    N/A (market still settling)
//   Tuesday   12:30 -- US CPI (second Tuesday of month -- approximated as weekly)
//   Wednesday 14:30 -- EIA Oil (handled by OilEngine separately)
//   Wednesday 18:00 -- FOMC (8x/year -- approximated as weekly check)
//   Thursday  12:30 -- US Initial Jobless Claims (every Thursday)
//   Friday    12:30 -- US NFP (first Friday of month -- approximated as weekly)
//
// PARAMETERS:
//   PRE_EVENT_WINDOW_SEC = 90   -- start watching 90s before event
//   EVENT_COMP_RANGE     = $0.40 -- gold must compress into $0.40 range (tight)
//   EVENT_TRIGGER        = $0.15 -- enter at $0.15 beyond range (vs normal $0.35)
//   TP                   = $3.00 -- event moves are large -- $3 target
//   SL                   = $0.80 -- event stop -- bigger than normal to survive spike
//   COMP_WINDOW          = 20    -- ticks to measure compression
//   MAX_HOLD_SEC         = 300   -- 5 min -- event moves need time to develop
// =============================================================================
class GoldEventCompression {
    // Runtime members -- set via configure() from LatencyEdgeCfg.
    // Defaults match calibrated constexpr values prior to config-driven refactor.
    double EVENT_COMP_RANGE = 0.40;
    double EVENT_TRIGGER    = 0.15;
    double TP               = 3.00;
    double SL               = 0.80;
    int    MAX_HOLD_SEC     = 300;
    double MAX_SPREAD       = 2.00;  // spreads widen pre-event
    int    COOLDOWN_SEC     = 600;   // 10 min between event trades
    // Fixed structural constants -- not tunable via config
    static constexpr int PRE_EVENT_WINDOW_SEC = 90;
    static constexpr int COMP_WINDOW          = 20;
    static constexpr int MAX_DAILY_TRADES     = 4;

    std::deque<double> comp_window_;
    int64_t last_entry_ = 0;
    int     trade_count_ = 0;
    bool    armed_       = false;
    int     daily_trades_ = 0;      // reset at UTC midnight
    int     last_reset_day_ = -1;   // UTC day-of-year for reset tracking

    LePositionManager pos_mgr_;

    void check_daily_reset() noexcept {
        const auto t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        if (ti.tm_yday != last_reset_day_) {
            last_reset_day_ = ti.tm_yday;
            daily_trades_ = 0;
        }
    }

    // Returns seconds until next high-impact event, or -1 if not within window
    static int secs_to_next_event() noexcept {
        const auto t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        const int wday = ti.tm_wday;  // 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat
        const int mins = ti.tm_hour * 60 + ti.tm_min;
        const int secs = mins * 60 + ti.tm_sec;

        // Event times in seconds from midnight UTC
        // Thursday 12:30 UTC -- US Jobless Claims (every week, reliable)
        // Tuesday  12:30 UTC -- US CPI/PPI (approximate -- treat as weekly)
        // Friday   12:30 UTC -- NFP (approximate -- treat as weekly)
        // Wednesday 18:00 UTC -- FOMC approximate
        struct EventDef { int wday; int event_sec; };
        static const EventDef EVENTS[] = {
            {2, 12*3600+30*60},  // Tuesday  12:30
            {4, 12*3600+30*60},  // Thursday 12:30 (Jobless Claims -- every week)
            {5, 12*3600+30*60},  // Friday   12:30 (NFP week)
            {3, 18*3600+0*60},   // Wednesday 18:00 (FOMC)
        };

        for (const auto& ev : EVENTS) {
            if (wday != ev.wday) continue;
            const int diff = ev.event_sec - secs;
            if (diff >= 0 && diff <= PRE_EVENT_WINDOW_SEC) {
                return diff;  // seconds until event
            }
        }
        return -1;
    }

public:
    bool has_open_position() const { return pos_mgr_.pos.active; }
    int  trade_count()       const { return trade_count_; }

    void configure(double comp_range, double trigger, double tp, double sl,
                   int max_hold_sec, int cooldown_sec, double max_spread) {
        EVENT_COMP_RANGE = comp_range;
        EVENT_TRIGGER    = trigger;
        TP               = tp;
        SL               = sl;
        MAX_HOLD_SEC     = max_hold_sec;
        COOLDOWN_SEC     = cooldown_sec;
        MAX_SPREAD       = max_spread;
    }

    using CloseCb = LePositionManager::CloseCb;

    // can_enter=false: manage existing position to closure but block all new entries.
    // This is the simultaneous-position guard -- called with gold_can_enter from main.
    LeSignal on_tick(double bid, double ask, double latency_ms,
                     CloseCb on_close, bool can_enter = true) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return {};

        const double spread = ask - bid;
        const double mid    = (bid + ask) * 0.5;

        // Always manage open position -- TP/SL/timeout must run regardless of can_enter
        if (pos_mgr_.pos.active) {
            pos_mgr_.manage(bid, ask, latency_ms, "EVENT_COMP", on_close, MAX_HOLD_SEC);
        }

        // New-entry gate: blocked when another XAUUSD engine already has a position
        if (!can_enter) return {};

        // Update compression window
        comp_window_.push_back(mid);
        if ((int)comp_window_.size() > COMP_WINDOW)
            comp_window_.pop_front();

        // Cooldown gate
        if (le_now_sec() - last_entry_ < COOLDOWN_SEC) return {};
        if (pos_mgr_.pos.active) return {};
        if (spread > MAX_SPREAD) return {};

        // Daily reset and max trades gate
        check_daily_reset();
        if (daily_trades_ >= MAX_DAILY_TRADES) return {};

        // Check if we're in pre-event window
        const int secs_to_ev = secs_to_next_event();
        if (secs_to_ev < 0) {
            armed_ = false;
            return {};
        }

        // Need full compression window
        if ((int)comp_window_.size() < COMP_WINDOW) return {};

        // Measure compression
        double hi = *std::max_element(comp_window_.begin(), comp_window_.end());
        double lo = *std::min_element(comp_window_.begin(), comp_window_.end());
        const double range = hi - lo;

        // If compression tight enough, arm the engine
        if (range <= EVENT_COMP_RANGE && !armed_) {
            armed_ = true;
            printf("[EVENT-COMP-ARMED] Gold pre-event compression range=$%.2f "
                   "secs_to_event=%d hi=%.2f lo=%.2f\n",
                   range, secs_to_ev, hi, lo);
            fflush(stdout);
        }

        if (!armed_) return {};

        // With tighter trigger ($0.15 vs $0.35), enter on first directional break
        const bool long_break  = mid > hi + EVENT_TRIGGER;
        const bool short_break = mid < lo - EVENT_TRIGGER;

        if (!long_break && !short_break) return {};

        // Disarm and enter
        armed_ = false;
        last_entry_ = le_now_sec();
        ++trade_count_;
        ++daily_trades_;

        LeSignal sig;
        sig.valid   = true;
        sig.is_long = long_break;
        sig.entry   = mid;
        sig.tp      = long_break ? mid + TP  : mid - TP;
        sig.sl      = long_break ? mid - SL  : mid + SL;
        sig.size    = 0.01;  // fallback min_lot -- compute_size() in main overrides this
        sig.reason  = long_break ? "EVENT_BREAK_LONG" : "EVENT_BREAK_SHORT";

        pos_mgr_.open(sig, spread, "XAUUSD");

        printf("[EVENT-COMP-ENTRY] Gold %s entry=%.2f tp=%.2f sl=%.2f "
               "range=$%.2f trigger=$%.2f secs_to_ev=%d\n",
               sig.is_long ? "LONG" : "SHORT",
               sig.entry, sig.tp, sig.sl,
               range, EVENT_TRIGGER, secs_to_ev);
        fflush(stdout);

        return sig;
    }

    void force_close(double bid, double ask, double latency_ms, CloseCb on_close) {
        pos_mgr_.force_close(bid, ask, latency_ms, "EVENT_COMP", on_close);
    }
};

// =============================================================================
// LatencyEdgeCfg -- all tunable parameters for LatencyEdgeStack in one struct.
// Populated from [latency_edge] ini section by main.cpp, then passed to
// LatencyEdgeStack::configure(). Default values match prior constexpr calibration.
// =============================================================================
struct LatencyEdgeCfg {
    // GoldSilverLeadLag DELETED -- fields removed
    // GoldSpreadDislocation
    double  spread_disloc_spike_ratio    = 2.5;
    double  spread_disloc_min_median     = 0.30;
    double  spread_disloc_max_median     = 1.20;
    double  spread_disloc_tp             = 0.30;
    double  spread_disloc_sl             = 0.15;
    int     spread_disloc_cooldown_sec   = 60;
    int     spread_disloc_max_hold_sec   = 30;
    // GoldEventCompression
    double  event_comp_range             = 0.40;
    double  event_comp_trigger           = 0.15;
    double  event_comp_tp                = 3.00;
    double  event_comp_sl                = 0.80;
    int     event_comp_max_hold_sec      = 300;
    int     event_comp_cooldown_sec      = 600;
    double  event_comp_max_spread        = 2.00;
};

// =============================================================================
// LatencyEdgeStack -- public interface wired into Omega's on_tick
// =============================================================================
// Single object per process. Call:
//   on_tick_gold(bid, ask, latency_ms, on_close)   -- every XAUUSD tick
//   has_open_position()                             -- any position open
//   force_close_all(...)                            -- disconnect/session end
//
// GoldSilverLeadLag DELETED 2026-03-31 -- see tombstone above.
// Remaining: SpreadDislocation + EventCompression (manage-only, drain existing).
// =============================================================================
class LatencyEdgeStack {
public:
    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    void configure(const LatencyEdgeCfg& c) {
        spread_disloc_.configure(
            c.spread_disloc_spike_ratio, c.spread_disloc_min_median,
            c.spread_disloc_max_median,  c.spread_disloc_tp,
            c.spread_disloc_sl,          c.spread_disloc_cooldown_sec,
            c.spread_disloc_max_hold_sec);
        event_comp_.configure(
            c.event_comp_range,    c.event_comp_trigger,
            c.event_comp_tp,       c.event_comp_sl,
            c.event_comp_max_hold_sec, c.event_comp_cooldown_sec,
            c.event_comp_max_spread);
        printf("[LE-CFG] SpreadDisloc tp=%.2f sl=%.2f cooldown=%ds "
               "EventComp tp=%.2f sl=%.2f trigger=%.2f max_hold=%ds\n",
               c.spread_disloc_tp,   c.spread_disloc_sl,   c.spread_disloc_cooldown_sec,
               c.event_comp_tp,      c.event_comp_sl,
               c.event_comp_trigger, c.event_comp_max_hold_sec);
        fflush(stdout);
    }

    // SpreadDislocation + EventCompression manage-only (no new entries -- latency
    // edge requires <1ms RTT, VPS RTT is ~68ms). Drain existing positions only.
    LeSignal on_tick_gold(double bid, double ask, double latency_ms,
                          CloseCb on_close, bool /*can_enter*/ = true) noexcept {
        spread_disloc_.on_tick(bid, ask, latency_ms, on_close, false);
        event_comp_.on_tick(bid, ask, latency_ms, on_close, false);
        return {};
    }

    bool has_open_position() const noexcept {
        return spread_disloc_.has_open_position() ||
               event_comp_.has_open_position();
    }

    void force_close_all(double gold_bid, double gold_ask,
                         double /*silver_bid*/, double /*silver_ask*/,
                         double latency_ms, CloseCb on_close) noexcept {
        spread_disloc_.force_close(gold_bid, gold_ask, latency_ms, on_close);
        event_comp_.force_close(gold_bid, gold_ask, latency_ms, on_close);
    }

    void print_stats() const noexcept {
        printf("[LATENCY-EDGE] SpreadDisloc trades=%d  EventComp trades=%d\n",
               spread_disloc_.trade_count(),
               event_comp_.trade_count());
        fflush(stdout);
    }

private:
    GoldSpreadDislocation spread_disloc_;
    GoldEventCompression  event_comp_;
};

} // namespace latency
} // namespace omega
