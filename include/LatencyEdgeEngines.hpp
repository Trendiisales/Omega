#pragma once
// =============================================================================
// LatencyEdgeEngines.hpp — Co-location Speed Advantage Engines
//
// These engines are designed specifically for the 0.3-4ms RTT advantage of a
// co-located VPS. They exploit timing edges unavailable to retail traders on
// home connections (20-100ms RTT).
//
// ENGINES:
//   1. GoldSilverLeadLag     — Gold fires signal → enter Silver before it reacts
//   2. GoldSpreadDislocation — Large order hits Gold book → fade the spike
//   3. GoldEventCompression  — Tight pre-news compression → early breakout entry
//
// All engines are fully independent from GoldEngineStack and CRTP engines.
// Each maintains its own position, P&L tracking, and cooldown state.
// All positions are paper-tracked in SHADOW mode, real orders in LIVE mode.
//
// HOW THEY WORK:
//   on_tick_gold(bid, ask, latency_ms) — call on every GOLD.F tick
//   on_tick_silver(bid, ask)           — call on every XAGUSD tick
//   has_open_position()                — position management query
//   signal_log()                       — last signal detail string
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

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline int64_t le_now_sec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static inline int64_t le_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// LeSignal — what these engines return on a new entry
// ─────────────────────────────────────────────────────────────────────────────
struct LeSignal {
    bool        valid     = false;
    bool        is_long   = true;
    double      entry     = 0.0;
    double      tp        = 0.0;
    double      sl        = 0.0;
    double      size      = 1.0;
    const char* engine    = "";
    const char* reason    = "";
};

// ─────────────────────────────────────────────────────────────────────────────
// LePosition — open position managed by each engine
// ─────────────────────────────────────────────────────────────────────────────
struct LePosition {
    bool    active          = false;
    bool    is_long         = true;
    double  entry           = 0.0;
    double  tp              = 0.0;
    double  sl              = 0.0;
    double  size            = 1.0;
    double  mfe             = 0.0;
    double  mae             = 0.0;
    double  spread_at_entry = 0.0;
    int64_t entry_ts        = 0;
    char    symbol[16]      = {};   // explicit symbol string e.g. "GOLD.F" "XAGUSD"
    char    engine[32]      = {};
    char    reason[32]      = {};
};

// ─────────────────────────────────────────────────────────────────────────────
// LePositionManager — manages one open position with TP/SL/trail/timeout
// ─────────────────────────────────────────────────────────────────────────────
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

        // Trailing stop — same 3-stage logic as BreakoutEngine
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
            const double exit_px = tp_hit ? pos.tp : (sl_hit ? pos.sl : mid);
            const char*  reason  = tp_hit ? "TP_HIT" : (sl_hit ? "SL_HIT" : "TIMEOUT");
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
        strncpy(pos.symbol, symbol ? symbol : "GOLD.F", 15);
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
        tr.symbol      = std::string(pos.symbol[0] ? pos.symbol : "GOLD.F");
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
// ENGINE 1 — GoldSilverLeadLag
// =============================================================================
// Gold and silver are correlated ~0.85. When gold fires a directional signal,
// silver typically follows within 50-500ms. With 0.3ms RTT we can enter silver
// before its price adjusts.
//
// MECHANISM:
//   - Maintains a rolling 30-tick gold price window to detect gold direction
//   - When gold moves >= GOLD_SIGNAL_MOVE in one direction within 10 ticks,
//     a lead signal is armed
//   - Next silver tick: if silver hasn't moved by >= SILVER_MIN_REACTION,
//     enter silver in gold's direction
//   - Silver TP/SL scaled from gold's move magnitude
//
// PARAMETERS — calibrated from live market observation:
//   Silver bid-ask spread: ~$0.10. SL must be >= 2.5x spread = $0.25 min.
//   Gold noise at $4990: $0.50 in 10 ticks is normal noise. Need $2.00+ for signal.
//   Silver min reaction: $0.008 < spread noise. Use $0.05 = half spread.
//   Cooldown: 30s caused re-entry after SL, creating churn. Use 300s (5 min).
//   Gold window: 10 ticks at 5-15t/s = <2s. Use 20 ticks for real direction.
//
//   GOLD_SIGNAL_MOVE   = $2.00  — gold must move $2.00 in 20 ticks (genuine move)
//   SILVER_MIN_REACTION= $0.05  — silver must not have moved $0.05 yet (half spread)
//   SILVER_TP          = $0.50  — $0.50 target: 2:1 R:R vs $0.25 SL
//   SILVER_SL          = $0.25  — 2.5x spread: above noise, below genuine reversal
//   SIGNAL_EXPIRY_MS   = 500    — tighter expiry: if 500ms elapsed edge is gone
//   MAX_SPREAD_GOLD    = $1.50  — unchanged
//   MAX_SPREAD_SILVER  = $0.15  — slightly wider to allow for spreads
//   COOLDOWN_SEC       = 300    — 5 min between entries: genuine signals are rare
//   MAX_HOLD_SEC       = 60     — if silver hasn't followed in 60s it won't
//   GOLD_WINDOW        = 20     — ticks to measure gold move (was 10 — too noisy)
// =============================================================================
class GoldSilverLeadLag {
    static constexpr double GOLD_SIGNAL_MOVE    = 2.00;  // $2.00 gold move in 20 ticks
    static constexpr double SILVER_MIN_REACTION = 0.05;  // silver must not have moved $0.05
    static constexpr double SILVER_TP           = 0.50;  // $0.50 target
    static constexpr double SILVER_SL           = 0.25;  // $0.25 stop — 2.5x spread
    static constexpr int64_t SIGNAL_EXPIRY_MS   = 500;   // 500ms expiry
    static constexpr double MAX_SPREAD_GOLD     = 1.50;
    static constexpr double MAX_SPREAD_SILVER   = 0.15;
    static constexpr int    COOLDOWN_SEC        = 300;   // 5 min cooldown
    static constexpr int    MAX_HOLD_SEC        = 60;    // 60s max hold
    static constexpr int    GOLD_WINDOW         = 20;    // 20 ticks for direction

    // Gold price window — last 10 mids
    std::deque<double> gold_window_;
    double gold_prev_mid_ = 0.0;
    double silver_prev_mid_ = 0.0;

    // Armed signal state
    bool    armed_          = false;
    bool    armed_long_     = false;
    double  gold_arm_mid_   = 0.0;   // gold mid when signal armed
    int64_t arm_time_ms_    = 0;     // arm timestamp in ms

    int64_t last_entry_sec_ = 0;
    int     trade_count_    = 0;

    LePositionManager pos_mgr_;

public:
    bool has_open_position() const { return pos_mgr_.pos.active; }
    int  trade_count()       const { return trade_count_; }

    using CloseCb = LePositionManager::CloseCb;

    // Call on every GOLD.F tick — arms the lead signal
    void on_tick_gold(double bid, double ask) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return;
        const double spread = ask - bid;
        if (spread > MAX_SPREAD_GOLD) {
            armed_ = false;  // spread blown — cancel any arm
            return;
        }
        const double mid = (bid + ask) * 0.5;

        gold_window_.push_back(mid);
        if ((int)gold_window_.size() > GOLD_WINDOW)
            gold_window_.pop_front();

        gold_prev_mid_ = mid;

        if ((int)gold_window_.size() < GOLD_WINDOW) return;

        const double oldest = gold_window_.front();
        const double move   = mid - oldest;  // signed: positive = up

        if (std::fabs(move) >= GOLD_SIGNAL_MOVE) {
            const bool is_long = move > 0.0;
            // Only arm if not already armed in same direction
            if (!armed_ || armed_long_ != is_long) {
                armed_       = true;
                armed_long_  = is_long;
                gold_arm_mid_= mid;
                arm_time_ms_ = le_now_ms();
                printf("[LEAD-LAG-ARM] Gold moved $%.2f in %d ticks → %s | arm_mid=%.2f\n",
                       move, GOLD_WINDOW, is_long ? "LONG" : "SHORT", mid);
                fflush(stdout);
            }
        }
    }

    // Call on every XAGUSD tick — fires entry if silver hasn't reacted yet
    LeSignal on_tick_silver(double bid, double ask, double latency_ms,
                            CloseCb on_close) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return {};
        const double spread_silver = ask - bid;
        const double mid           = (bid + ask) * 0.5;

        // Always manage open position first
        if (pos_mgr_.pos.active) {
            pos_mgr_.manage(bid, ask, latency_ms, "LEAD_LAG", on_close, MAX_HOLD_SEC);
        }

        // Check signal arm is valid
        if (!armed_) { silver_prev_mid_ = mid; return {}; }

        // Check expiry
        const int64_t age_ms = le_now_ms() - arm_time_ms_;
        if (age_ms > SIGNAL_EXPIRY_MS) {
            armed_ = false;
            silver_prev_mid_ = mid;
            return {};
        }

        // Check spread gate
        if (spread_silver > MAX_SPREAD_SILVER) {
            silver_prev_mid_ = mid;
            return {};
        }

        // Check cooldown
        if (le_now_sec() - last_entry_sec_ < COOLDOWN_SEC) {
            silver_prev_mid_ = mid;
            return {};
        }

        // Already in position
        if (pos_mgr_.pos.active) { silver_prev_mid_ = mid; return {}; }

        // Core check: has silver already moved in gold's direction?
        // If silver_prev_mid_ > 0 and silver has already moved >= SILVER_MIN_REACTION,
        // the edge is gone — silver already repriced.
        if (silver_prev_mid_ > 0.0) {
            const double silver_move = mid - silver_prev_mid_;
            const bool already_long  = (silver_move >= SILVER_MIN_REACTION);
            const bool already_short = (silver_move <= -SILVER_MIN_REACTION);
            if ((armed_long_ && already_long) || (!armed_long_ && already_short)) {
                // Silver already reacted — edge gone
                armed_ = false;
                silver_prev_mid_ = mid;
                return {};
            }
        }

        // Edge confirmed — silver hasn't moved yet, gold has. Enter silver.
        armed_ = false;
        last_entry_sec_ = le_now_sec();
        ++trade_count_;

        LeSignal sig;
        sig.valid    = true;
        sig.is_long  = armed_long_;
        sig.entry    = mid;
        sig.tp       = armed_long_ ? mid + SILVER_TP : mid - SILVER_TP;
        sig.sl       = armed_long_ ? mid - SILVER_SL : mid + SILVER_SL;
        sig.size     = 1.0;
        sig.engine   = "GoldSilverLeadLag";
        sig.reason   = armed_long_ ? "GOLD_LEADS_LONG" : "GOLD_LEADS_SHORT";

        pos_mgr_.open(sig, spread_silver, "XAGUSD");

        printf("[LEAD-LAG-ENTRY] Silver %s entry=%.4f tp=%.4f sl=%.4f "
               "gold_moved=%.2f age_ms=%lld\n",
               sig.is_long ? "LONG" : "SHORT",
               sig.entry, sig.tp, sig.sl,
               (gold_prev_mid_ - gold_arm_mid_),
               (long long)age_ms);
        fflush(stdout);

        silver_prev_mid_ = mid;
        return sig;
    }

    void force_close(double bid, double ask, double latency_ms, CloseCb on_close) {
        pos_mgr_.force_close(bid, ask, latency_ms, "LEAD_LAG", on_close);
    }
};

// =============================================================================
// ENGINE 2 — GoldSpreadDislocation
// =============================================================================
// When a large order hits the gold book, the spread momentarily widens as
// the bid or ask is taken out. The price typically snaps back within 50-200ms.
// With 0.3ms RTT we can fade this dislocation before slower traders react.
//
// MECHANISM:
//   - Tracks a rolling 20-tick median spread on GOLD.F
//   - When instantaneous spread exceeds median × SPREAD_SPIKE_RATIO,
//     a dislocation is detected
//   - The direction of entry is AGAINST the spike:
//       Ask spike (ask jumped up) → price was pushed up → fade SHORT
//       Bid spike (bid dropped)   → price was pushed down → fade LONG
//   - This works because market makers reprice immediately — the spike
//     is a momentary dislocation, not a true directional move
//
// PARAMETERS:
//   SPREAD_SPIKE_RATIO = 2.5   — spread must be 2.5× its median to qualify
//   MIN_MEDIAN_SPREAD  = $0.30 — only active when gold is liquid (spread > $0.30)
//   MAX_MEDIAN_SPREAD  = $1.20 — stop trading if normally wide (illiquid session)
//   TP                 = $0.30 — fade target: snap back to pre-spike mid
//   SL                 = $0.15 — tight: if spread stays wide, we're wrong
//   COOLDOWN_SEC       = 15    — fast cooldown, these happen frequently
//   MAX_HOLD_SEC       = 30    — snaps back quickly or not at all
//   SESSION GATE       = London/NY only (07:00-20:00 UTC) — spread dislocations
//                        in Asia are real moves, not noise
// =============================================================================
class GoldSpreadDislocation {
    static constexpr double SPREAD_SPIKE_RATIO = 2.5;
    static constexpr double MIN_MEDIAN_SPREAD  = 0.30;
    static constexpr double MAX_MEDIAN_SPREAD  = 1.20;
    static constexpr double TP                 = 0.30;
    static constexpr double SL                 = 0.15;
    static constexpr int    COOLDOWN_SEC       = 60;   // raised from 15s — 3 SL hits in 45s was possible
    static constexpr int    MAX_HOLD_SEC       = 30;
    static constexpr int    SPREAD_WINDOW      = 20;

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
        return (h >= 7 && h < 20);  // London + NY: 07:00-20:00 UTC
    }

public:
    bool has_open_position() const { return pos_mgr_.pos.active; }
    int  trade_count()       const { return trade_count_; }

    using CloseCb = LePositionManager::CloseCb;

    LeSignal on_tick(double bid, double ask, double latency_ms,
                     CloseCb on_close) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return {};

        const double spread = ask - bid;
        const double mid    = (bid + ask) * 0.5;

        // Always manage open position
        if (pos_mgr_.pos.active) {
            pos_mgr_.manage(bid, ask, latency_ms, "SPREAD_DISLOC", on_close, MAX_HOLD_SEC);
        }

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

        // If mid jumped up (ask was taken) → fade SHORT back to prev level
        // If mid jumped down (bid was taken) → fade LONG back to prev level
        const double mid_jump = mid - prev_mid_;
        const bool   is_long  = (mid_jump < -0.10);  // mid dropped → fade LONG
        const bool   is_short = (mid_jump >  0.10);  // mid jumped  → fade SHORT

        if (!is_long && !is_short) { prev_mid_ = mid; return {}; }

        last_entry_ = le_now_sec();
        ++trade_count_;

        LeSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = is_long ? mid + TP  : mid - TP;
        sig.sl      = is_long ? mid - SL  : mid + SL;
        sig.size    = 1.0;
        sig.engine  = "GoldSpreadDisloc";
        sig.reason  = is_long ? "SPREAD_SPIKE_FADE_LONG" : "SPREAD_SPIKE_FADE_SHORT";

        pos_mgr_.open(sig, spread, "GOLD.F");

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
// ENGINE 3 — GoldEventCompression
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
//   Tuesday   12:30 — US CPI (second Tuesday of month — approximated as weekly)
//   Wednesday 14:30 — EIA Oil (handled by OilEngine separately)
//   Wednesday 18:00 — FOMC (8x/year — approximated as weekly check)
//   Thursday  12:30 — US Initial Jobless Claims (every Thursday)
//   Friday    12:30 — US NFP (first Friday of month — approximated as weekly)
//
// PARAMETERS:
//   PRE_EVENT_WINDOW_SEC = 90   — start watching 90s before event
//   EVENT_COMP_RANGE     = $0.40 — gold must compress into $0.40 range (tight)
//   EVENT_TRIGGER        = $0.15 — enter at $0.15 beyond range (vs normal $0.35)
//   TP                   = $3.00 — event moves are large — $3 target
//   SL                   = $0.80 — event stop — bigger than normal to survive spike
//   COMP_WINDOW          = 20    — ticks to measure compression
//   MAX_HOLD_SEC         = 300   — 5 min — event moves need time to develop
// =============================================================================
class GoldEventCompression {
    static constexpr int    PRE_EVENT_WINDOW_SEC = 90;
    static constexpr double EVENT_COMP_RANGE     = 0.40;
    static constexpr double EVENT_TRIGGER        = 0.15;
    static constexpr double TP                   = 3.00;
    static constexpr double SL                   = 0.80;
    static constexpr int    COMP_WINDOW          = 20;
    static constexpr int    MAX_HOLD_SEC         = 300;
    static constexpr double MAX_SPREAD           = 2.00;  // spreads widen pre-event
    static constexpr int    COOLDOWN_SEC         = 600;   // 10 min between event trades

    std::deque<double> comp_window_;
    int64_t last_entry_ = 0;
    int     trade_count_ = 0;
    bool    armed_       = false;
    int     daily_trades_ = 0;      // reset at UTC midnight
    int     last_reset_day_ = -1;   // UTC day-of-year for reset tracking
    static constexpr int MAX_DAILY_TRADES = 4;  // max 4 event trades per day

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
        // Thursday 12:30 UTC — US Jobless Claims (every week, reliable)
        // Tuesday  12:30 UTC — US CPI/PPI (approximate — treat as weekly)
        // Friday   12:30 UTC — NFP (approximate — treat as weekly)
        // Wednesday 18:00 UTC — FOMC approximate
        struct EventDef { int wday; int event_sec; };
        static const EventDef EVENTS[] = {
            {2, 12*3600+30*60},  // Tuesday  12:30
            {4, 12*3600+30*60},  // Thursday 12:30 (Jobless Claims — every week)
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

    using CloseCb = LePositionManager::CloseCb;

    LeSignal on_tick(double bid, double ask, double latency_ms,
                     CloseCb on_close) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return {};

        const double spread = ask - bid;
        const double mid    = (bid + ask) * 0.5;

        // Always manage open position
        if (pos_mgr_.pos.active) {
            pos_mgr_.manage(bid, ask, latency_ms, "EVENT_COMP", on_close, MAX_HOLD_SEC);
        }

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
        sig.size    = 1.0;
        sig.engine  = "GoldEventComp";
        sig.reason  = long_break ? "EVENT_BREAK_LONG" : "EVENT_BREAK_SHORT";

        pos_mgr_.open(sig, spread, "GOLD.F");

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
// LatencyEdgeStack — public interface wired into Omega's on_tick
// =============================================================================
// Single object per process. Call:
//   on_tick_gold(bid, ask, latency_ms, on_close)   — every GOLD.F tick
//   on_tick_silver(bid, ask, latency_ms, on_close)  — every XAGUSD tick
//   has_open_position()                             — any position open
//   force_close_all(...)                            — disconnect/session end
// Returns LeSignal from each call so main.cpp can dispatch live orders
// and update telemetry without the stack needing to know about those systems.
// =============================================================================
class LatencyEdgeStack {
public:
    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // Returns valid signal if SpreadDislocation or EventCompression fired this tick
    LeSignal on_tick_gold(double bid, double ask, double latency_ms,
                          CloseCb on_close) noexcept {
        // Lead-lag disabled — was arming on every $0.50 gold wiggle creating noise
        // lead_lag_.on_tick_gold(bid, ask);  // DISABLED pending redesign

        // Spread dislocation engine — returns signal if entry fired
        const auto sd_sig = spread_disloc_.on_tick(bid, ask, latency_ms, on_close);
        if (sd_sig.valid) {
            log_entry(sd_sig, "GOLD.F");
            last_gold_signal_ = sd_sig;
            return sd_sig;
        }

        // Event compression engine — returns signal if entry fired
        const auto ev_sig = event_comp_.on_tick(bid, ask, latency_ms, on_close);
        if (ev_sig.valid) {
            log_entry(ev_sig, "GOLD.F");
            last_gold_signal_ = ev_sig;
            return ev_sig;
        }

        return {};
    }

    // Returns valid signal if lead-lag fired on this silver tick.
    // DISABLED: was firing on every gold wiggle — fundamental design issue.
    // Existing open positions are still managed to closure.
    LeSignal on_tick_silver(double bid, double ask, double latency_ms,
                            CloseCb on_close) noexcept {
        if (lead_lag_.has_open_position()) {
            // Drain any hanging position — call the actual manage path
            lead_lag_.on_tick_silver(bid, ask, latency_ms, on_close);
        }
        return {};  // no new entries — DISABLED
    }

    bool has_open_position() const noexcept {
        return lead_lag_.has_open_position() ||
               spread_disloc_.has_open_position() ||
               event_comp_.has_open_position();
    }

    void force_close_all(double gold_bid, double gold_ask,
                         double silver_bid, double silver_ask,
                         double latency_ms, CloseCb on_close) noexcept {
        spread_disloc_.force_close(gold_bid, gold_ask, latency_ms, on_close);
        event_comp_.force_close(gold_bid, gold_ask, latency_ms, on_close);
        lead_lag_.force_close(silver_bid, silver_ask, latency_ms, on_close);
    }

    void print_stats() const noexcept {
        printf("[LATENCY-EDGE] LeadLag trades=%d  SpreadDisloc trades=%d  EventComp trades=%d\n",
               lead_lag_.trade_count(),
               spread_disloc_.trade_count(),
               event_comp_.trade_count());
        fflush(stdout);
    }

private:
    GoldSilverLeadLag     lead_lag_;
    GoldSpreadDislocation spread_disloc_;
    GoldEventCompression  event_comp_;
    LeSignal              last_gold_signal_;
    LeSignal              last_silver_signal_;

    static void log_entry(const LeSignal& sig, const char* sym) noexcept {
        printf("[LE-ENTRY] %s %s entry=%.4f tp=%.4f sl=%.4f eng=%s reason=%s\n",
               sym,
               sig.is_long ? "LONG" : "SHORT",
               sig.entry, sig.tp, sig.sl,
               sig.engine, sig.reason);
        fflush(stdout);
    }
};

} // namespace latency
} // namespace omega
