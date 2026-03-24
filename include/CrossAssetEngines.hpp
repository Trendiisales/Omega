#pragma once
// =============================================================================
// CrossAssetEngines.hpp — Cross-asset and event-driven engines
//
// These engines exploit relationships BETWEEN instruments and SCHEDULED EVENTS.
// All data they need is already flowing in the system — no new feeds required.
//
// ENGINES:
//   1. EsNqDivergenceEngine  — ES/NQ diverge >threshold → enter laggard
//   2. OilEventFadeEngine    — EIA inventory spike → fade 50% of move
//   3. BrentWtiSpreadEngine  — Brent/WTI spread >$5 → enter convergence
//   4. FxCascadeEngine       — EURUSD breaks → arm GBPUSD + AUDUSD + NZDUSD
//   5. CarryUnwindEngine     — VIX spike + USDJPY falling → short USDJPY
//   6. OpeningRangeEngine    — Time-anchored first-30-min range breakout
//   7. VWAPReversionEngine   — Price extends from daily VWAP → enter on reversal tick
//   8. TrendPullbackEngine   — EMA-9/21/50 trend + pullback to slow EMA with bounce
//
// COST ENFORCEMENT (ExecutionCostGuard):
//   Every engine's on_tick() checks execution costs before firing any signal.
//   Commission + spread + slippage must be covered by the expected TP move.
//   No trade is allowed to execute if costs are not covered. See struct below.
//
// Architecture: all engines are self-contained, no external deps beyond
// OmegaTradeLedger. Called from on_tick() in main.cpp after existing dispatch.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <deque>
#include <string>
#include <functional>
#include <algorithm>
#include "OmegaTradeLedger.hpp"

namespace omega {
namespace cross {

// ─────────────────────────────────────────────────────────────────────────────
// Shared helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline int64_t ca_now_sec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static inline int64_t ca_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static inline void ca_utc_time(struct tm& ti) noexcept {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
#ifdef _WIN32
    gmtime_s(&ti, &t);
#else
    gmtime_r(&t, &ti);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// CrossSignal — what every cross-asset engine returns
// ─────────────────────────────────────────────────────────────────────────────
struct CrossSignal {
    bool        valid    = false;
    bool        is_long  = true;
    double      entry    = 0.0;
    double      tp       = 0.0;
    double      sl       = 0.0;
    double      size     = 0.01;
    const char* symbol   = "";
    const char* engine   = "";
    const char* reason   = "";
};

// ─────────────────────────────────────────────────────────────────────────────
// ExecutionCostGuard — enforces cost floors BEFORE any trade is allowed
//
// BlackBull ECN real execution costs (commission + spread + slippage):
//
//   Forex majors:  ~$10/lot  (spread $2, commission $6, slippage $2)
//   Gold:          ~$9/lot   (spread $1.5, commission $6, slippage $1.5)
//   Silver:        ~$12/lot  (spread $3, commission $6, slippage $3)
//   NAS100:        ~$7/lot   (spread $3, slippage $4 — no commission)
//   US30:          ~$10/lot  (spread $4, slippage $6 — no commission)
//   GER40:         ~$4.5/lot (spread $2, slippage $2.5 — no commission)
//   Oil:           ~$4.5/lot (spread $2.5, slippage $2 — no commission)
//
// Break-even minimum move (0.01 lot trade, costs already scaled):
//   Forex:  ~1 pip   (~$0.10 at 0.01 lot)
//   Gold:   ~$1.50   at 0.01 lot
//   Silver: ~$3.00   at 0.01 lot
//   NAS100: ~5 pts   at 0.01 lot
//   US30:   ~10 pts  at 0.01 lot
//   GER40:  ~2 pts   at 0.01 lot
//   Oil:    ~$0.08   at 0.01 lot
//
// Usage: before entering any trade, call is_viable(sym, spread, tp_dist, lot).
// Returns false (block trade) if expected_gross < total_cost.
// All engines call this — NO trade fires without clearing it.
// ─────────────────────────────────────────────────────────────────────────────
struct ExecutionCostGuard {
    // Conservative (high-end) cost floors per lot, in instrument price points.
    // Indices and oil have no per-lot commission (included in spread by broker).
    // FX/metals carry $6/lot round-trip commission.

    // Returns total estimated cost in USD for the given instrument and lot size.
    // spread_pts: current live bid-ask spread in price points (ask - bid).
    // lot: position size in lots.
    static double estimated_cost_usd(const char* sym, double spread_pts, double lot) noexcept {
        if (lot <= 0.0) return 9999.0;
        double commission_per_lot = 0.0;
        double slippage_pts       = 0.0;
        double tick_usd_per_lot   = 1.0;  // fallback

        const std::string s(sym);

        // Commission (only Forex and Metals charge per-lot ECN fee)
        if (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" ||
            s == "NZDUSD" || s == "USDJPY") {
            commission_per_lot = 6.0;    // $6 round-trip ECN
            slippage_pts       = 0.0002; // 2 pip typical slippage
            tick_usd_per_lot   = 10.0;   // $10/pip/lot for majors
        } else if (s == "GOLD.F") {
            commission_per_lot = 6.0;
            slippage_pts       = 0.50;   // $0.50 typical slippage
            tick_usd_per_lot   = 1.0;    // $1/pt/lot gold
        } else if (s == "XAGUSD") {
            commission_per_lot = 6.0;
            slippage_pts       = 0.03;   // ~3¢ slippage
            tick_usd_per_lot   = 50.0;   // $50/pt/lot silver (5000oz × $0.01)
        } else if (s == "GER40") {
            commission_per_lot = 0.0;    // included in spread
            slippage_pts       = 2.5;    // 2.5pt typical slippage
            tick_usd_per_lot   = 1.10;   // ~$1.10/pt/lot (EUR-quoted × 1.10)
        } else if (s == "UK100") {
            commission_per_lot = 0.0;
            slippage_pts       = 1.5;
            tick_usd_per_lot   = 1.27;   // GBP-quoted × 1.27
        } else if (s == "ESTX50") {
            commission_per_lot = 0.0;
            slippage_pts       = 2.0;
            tick_usd_per_lot   = 1.10;
        } else if (s == "US500.F") {
            commission_per_lot = 0.0;
            slippage_pts       = 3.0;
            tick_usd_per_lot   = 50.0;   // $50/pt/lot ES
        } else if (s == "USTEC.F") {
            commission_per_lot = 0.0;
            slippage_pts       = 3.0;
            tick_usd_per_lot   = 20.0;
        } else if (s == "NAS100") {
            commission_per_lot = 0.0;
            slippage_pts       = 4.0;
            tick_usd_per_lot   = 1.0;
        } else if (s == "DJ30.F") {
            commission_per_lot = 0.0;
            slippage_pts       = 6.0;
            tick_usd_per_lot   = 5.0;    // $5/pt/lot US30
        } else if (s == "USOIL.F" || s == "BRENT") {
            commission_per_lot = 0.0;
            slippage_pts       = 0.03;
            tick_usd_per_lot   = 1000.0; // $1000/pt/lot crude
        }

        const double spread_cost = spread_pts * tick_usd_per_lot * lot;
        const double slip_cost   = slippage_pts * tick_usd_per_lot * lot;
        const double comm_cost   = commission_per_lot * lot;
        return spread_cost + slip_cost + comm_cost;
    }

    // Returns expected gross profit in USD if TP is hit.
    // tp_dist_pts: |tp - entry| in price points.
    static double expected_gross_usd(const char* sym, double tp_dist_pts, double lot) noexcept {
        if (lot <= 0.0 || tp_dist_pts <= 0.0) return 0.0;
        double tick_usd_per_lot = 1.0;
        const std::string s(sym);
        if      (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" || s == "NZDUSD") tick_usd_per_lot = 10.0;
        else if (s == "USDJPY")    tick_usd_per_lot = 10.0;   // approx at 150
        else if (s == "GOLD.F")    tick_usd_per_lot = 1.0;
        else if (s == "XAGUSD")    tick_usd_per_lot = 50.0;
        else if (s == "GER40")     tick_usd_per_lot = 1.10;
        else if (s == "UK100")     tick_usd_per_lot = 1.27;
        else if (s == "ESTX50")    tick_usd_per_lot = 1.10;
        else if (s == "US500.F")   tick_usd_per_lot = 50.0;
        else if (s == "USTEC.F")   tick_usd_per_lot = 20.0;
        else if (s == "NAS100")    tick_usd_per_lot = 1.0;
        else if (s == "DJ30.F")    tick_usd_per_lot = 5.0;
        else if (s == "USOIL.F" || s == "BRENT") tick_usd_per_lot = 1000.0;
        return tp_dist_pts * tick_usd_per_lot * lot;
    }

    // Gate: returns true if the trade is viable (expected gross > total cost).
    // COST_RATIO_MIN: gross must be at least this multiple of cost (default 1.5×
    // — ensures we're not trading for a coin-flip breakeven with spread noise).
    static bool is_viable(const char* sym, double spread_pts, double tp_dist_pts,
                           double lot, double cost_ratio_min = 1.5) noexcept {
        const double cost  = estimated_cost_usd(sym, spread_pts, lot);
        const double gross = expected_gross_usd(sym, tp_dist_pts, lot);
        if (gross < cost * cost_ratio_min) {
            printf("[COST-GUARD] BLOCKED %s spread=%.5f tp_dist=%.5f lot=%.2f"
                   " cost=$%.2f gross=$%.2f ratio=%.2f < %.1fx\n",
                   sym, spread_pts, tp_dist_pts, lot, cost, gross, gross/cost, cost_ratio_min);
            fflush(stdout);
            return false;
        }
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// CrossPosition — shared open position tracker (simple, one per engine)
// ─────────────────────────────────────────────────────────────────────────────
struct CrossPosition {
    bool    active          = false;
    bool    is_long         = true;
    double  entry           = 0.0;
    double  tp              = 0.0;
    double  sl              = 0.0;
    double  size            = 0.01;
    double  mfe             = 0.0;
    double  mae             = 0.0;
    double  spread_at_entry = 0.0;
    int64_t entry_ts        = 0;
    char    symbol[16]      = {};
    char    engine[32]      = {};
    char    reason[32]      = {};

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    bool manage(double bid, double ask, int max_hold_sec,
                CloseCb on_close) noexcept {
        if (!active) return false;
        const double mid  = (bid + ask) * 0.5;
        const double move = is_long ? (mid - entry) : (entry - mid);
        if (move >  mfe) mfe =  move;
        if (-move > mae) mae = -move;

        const bool tp_hit = is_long ? (bid >= tp) : (ask <= tp);
        const bool sl_hit = is_long ? (bid <= sl) : (ask >= sl);
        const bool timed_out = (ca_now_sec() - entry_ts) >= max_hold_sec;

        const char* reason_str = tp_hit ? "TP_HIT" : (sl_hit ? "SL_HIT" : "TIMEOUT");
        double exit_px = tp_hit ? tp : (sl_hit ? sl : mid);

        if (tp_hit || sl_hit || timed_out) {
            // On timeout: cap exit at SL if price has blown through.
            // A sparse-tick reconnect can let price drift past SL without triggering
            // the explicit sl_hit check. Without this, timeout records a loss larger
            // than the intended stop — same fix applied in GoldPositionManager and BracketEngine.
            if (timed_out && !sl_hit && !tp_hit) {
                const bool sl_breached = is_long ? (mid < sl) : (mid > sl);
                if (sl_breached) {
                    exit_px    = sl;
                    reason_str = "SL_HIT";  // price was past SL at timeout — report as SL, not TIMEOUT
                }
            }
            emit(exit_px, reason_str, on_close);
            return true;
        }
        return false;
    }

    void open(const CrossSignal& sig, double spread) noexcept {
        active          = true;
        is_long         = sig.is_long;
        entry           = sig.entry;
        tp              = sig.tp;
        sl              = sig.sl;
        size            = sig.size;
        spread_at_entry = spread;
        entry_ts        = ca_now_sec();
        mfe = mae = 0.0;
#ifdef _WIN32
        strncpy_s(symbol, sig.symbol, 15);
        strncpy_s(engine, sig.engine, 31);
        strncpy_s(reason, sig.reason, 31);
#else
        strncpy(symbol, sig.symbol, 15); symbol[15] = '\0';
        strncpy(engine, sig.engine, 31); engine[31] = '\0';
        strncpy(reason, sig.reason, 31); reason[31] = '\0';
#endif
    }

    void force_close(double bid, double ask, CloseCb on_close) noexcept {
        if (!active) return;
        const double mid = (bid + ask) * 0.5;
        emit(mid, "FORCE_CLOSE", on_close);
    }

private:
    void emit(double exit_px, const char* exit_reason, CloseCb on_close) {
        omega::TradeRecord tr;
        tr.symbol      = symbol;
        tr.side        = is_long ? "LONG" : "SHORT";
        tr.entryPrice  = entry;
        tr.exitPrice   = exit_px;
        tr.tp          = tp;
        tr.sl          = sl;
        tr.size        = size;
        tr.pnl         = (is_long ? (exit_px - entry) : (entry - exit_px)) * size;
        tr.mfe         = mfe;
        tr.mae         = mae;
        tr.entryTs     = entry_ts;
        tr.exitTs      = ca_now_sec();
        tr.exitReason  = exit_reason;
        tr.spreadAtEntry = spread_at_entry;
        tr.engine      = engine;
        active = false;
        if (on_close) on_close(tr);
    }
};

// =============================================================================
// ENGINE 1 — EsNqDivergenceEngine
// =============================================================================
// When US500 and USTEC diverge significantly (one leads, one lags), the laggard
// catches up. The divergence signal already lives in MacroRegimeDetector but is
// only used as a gate. This engine trades it.
//
// Signal: esNqDivergence() > DIV_ENTRY_THRESH → enter laggard in leader's direction.
// esNqDivergence() = ES_return - NQ_return over DIV_WINDOW ticks (60 ticks).
// Positive = ES outperforming NQ → NQ should catch up → long NQ / short ES.
// Negative = NQ outperforming ES → ES should catch up → long ES / short NQ.
//
// Only fires during NY session (13:30-17:00 UTC) when both are liquid.
// Cost check: TP move must cover spread + slippage (no commission for indices).
// =============================================================================
class EsNqDivergenceEngine {
public:
    // Configurable
    double  DIV_ENTRY_THRESH   = 0.0008;  // 0.08% return divergence = meaningful lag
    double  DIV_EXIT_THRESH    = 0.0002;  // exit when divergence closes to 0.02%
    double  TP_PCT             = 0.08;    // 0.08% TP — convergence target
    double  SL_PCT             = 0.05;    // 0.05% SL
    int     MAX_HOLD_SEC       = 300;     // 5 min — convergence should happen fast
    int     COOLDOWN_SEC       = 120;
    bool    enabled            = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // Call every tick with current prices and the MacroRegimeDetector divergence
    // sym: "US500.F" or "USTEC.F" depending on which instrument this tick is for
    // div: g_macro_ctx.es_nq_div (pre-computed)
    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        double div, CloseCb on_close) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        if (pos_.active) {
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }
        // Session gate: NY only (13:30-17:00 UTC)
        struct tm ti{}; ca_utc_time(ti);
        const int mins = ti.tm_hour * 60 + ti.tm_min;
        if (mins < 13*60+30 || mins >= 17*60) return {};

        if (ca_now_sec() < cooldown_until_) return {};
        const double mid  = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // div > threshold: ES leading, NQ lagging → long NQ on NQ tick
        // div < -threshold: NQ leading, ES lagging → long ES on ES tick
        // Note: we only enter the laggard LONG. Shorting the leader is not done
        // here — we cannot reliably predict whether the leader corrects down or
        // the laggard catches up (asymmetric convergence).
        bool fire = false;
        bool is_long = true;

        if (div > DIV_ENTRY_THRESH && sym == "USTEC.F") {
            fire = true; is_long = true;  // NQ lags ES upward → long NQ
        } else if (div < -DIV_ENTRY_THRESH && sym == "US500.F") {
            fire = true; is_long = true;  // ES lags NQ upward → long ES
        }
        // ES short path (div>thresh on US500.F tick) intentionally excluded:
        // entering ES short when ES leads NQ is not reliable convergence —
        // the leader may simply be right and NQ is slow. Laggard-long only.
        if (!fire) return {};

        const double tp = is_long ? mid * (1.0 + TP_PCT/100.0) : mid * (1.0 - TP_PCT/100.0);
        const double sl = is_long ? mid * (1.0 - SL_PCT/100.0) : mid * (1.0 + SL_PCT/100.0);
        const double tp_dist = std::fabs(tp - mid);

        // Cost gate: gross TP must cover all execution costs
        if (!ExecutionCostGuard::is_viable(sym.c_str(), spread, tp_dist, 0.01)) return {};

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = sym.c_str();
        sig.engine  = "EsNqDivergence";
        sig.reason  = is_long ? "NQ_LAGS_ES" : "ES_LAGS_NQ";

        pos_.open(sig, spread);
        last_div_at_entry_ = div;
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[ESNQ-DIV] %s %s div=%.5f entry=%.2f tp=%.2f sl=%.2f\n",
               sym.c_str(), is_long?"LONG":"SHORT", div, mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

private:
    CrossPosition pos_;
    int64_t cooldown_until_ = 0;
    double  last_div_at_entry_ = 0.0;
};

// =============================================================================
// ENGINE 2 — OilEventFadeEngine
// =============================================================================
// EIA crude inventory release: Wednesday 14:30 UTC.
// Pattern: initial spike in one direction (0.3-0.8%), then 65-70% probability
// of fading 50% of the spike within 5 minutes.
//
// Logic:
//   1. Start monitoring at 14:30 UTC on Wednesdays
//   2. Capture price at 14:30:00 (pre-release baseline)
//   3. If price spikes >SPIKE_THRESH in first 15s → arm fade in opposite direction
//   4. Enter fade at 14:30:15 (15s after release — initial spike absorbed)
//   5. TP = 50% of spike distance, SL = full spike distance
//   6. Exit by 14:45 at latest
// Cost check: TP move (50% spike) must exceed oil execution floor.
// =============================================================================
class OilEventFadeEngine {
public:
    double  SPIKE_THRESH_PCT = 0.30;  // minimum spike % to qualify for fade
    double  TP_RATIO         = 0.50;  // TP = 50% of spike distance
    double  SL_RATIO         = 1.20;  // SL = 120% of spike (slightly past spike high)
    int     ENTRY_DELAY_SEC  = 15;    // wait 15s after release for spike to complete
    int     MAX_HOLD_SEC     = 900;   // 15 min — fade should resolve quickly
    bool    enabled          = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        CloseCb on_close) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;

        if (pos_.active) {
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        struct tm ti{}; ca_utc_time(ti);
        // Only Wednesday 14:29-14:45 UTC
        if (ti.tm_wday != 3) { reset(); return {}; }
        const int mins = ti.tm_hour * 60 + ti.tm_min;
        if (mins < 14*60+29 || mins >= 14*60+45) { reset(); return {}; }

        const int64_t now = ca_now_sec();

        // Capture baseline at 14:29:50-14:30:00
        if (baseline_ <= 0.0 && mins == 14*60+29 && ti.tm_sec >= 50) {
            baseline_ = mid;
            baseline_ts_ = now;
            printf("[OIL-EIA] Baseline captured %.4f at %02d:%02d:%02d UTC\n",
                   mid, ti.tm_hour, ti.tm_min, ti.tm_sec);
            fflush(stdout);
        }
        if (baseline_ <= 0.0) return {};

        // After ENTRY_DELAY_SEC, check if spike happened and arm fade
        if (armed_) return {};  // already armed or fired this week
        if (now - baseline_ts_ < ENTRY_DELAY_SEC) return {};

        const double spike_pct = (mid - baseline_) / baseline_ * 100.0;
        if (std::fabs(spike_pct) < SPIKE_THRESH_PCT) return {}; // no spike

        // Spike confirmed — fade it
        const bool spike_up = (spike_pct > 0.0);
        const double spike_dist = std::fabs(mid - baseline_);
        const bool is_long = !spike_up;  // fade: if spiked up, go short

        const double tp = is_long ? mid + spike_dist * TP_RATIO : mid - spike_dist * TP_RATIO;
        const double sl = is_long ? mid - spike_dist * SL_RATIO : mid + spike_dist * SL_RATIO;
        const double tp_dist = std::fabs(tp - mid);

        // Cost gate
        if (!ExecutionCostGuard::is_viable(sym.c_str(), spread, tp_dist, 0.01)) return {};

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = sym.c_str();
        sig.engine  = "OilEventFade";
        sig.reason  = spike_up ? "EIA_FADE_SHORT" : "EIA_FADE_LONG";

        pos_.open(sig, spread);
        armed_ = true;
        printf("[OIL-EIA-FADE] %s spike=%.3f%% entry=%.4f tp=%.4f sl=%.4f\n",
               is_long?"LONG":"SHORT", spike_pct, mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

private:
    CrossPosition pos_;
    double  baseline_    = 0.0;
    int64_t baseline_ts_ = 0;
    bool    armed_       = false;  // one trade per EIA release

    void reset() {
        if (baseline_ > 0.0) baseline_ = 0.0;
        // Reset weekly on Monday
        struct tm ti{}; ca_utc_time(ti);
        if (ti.tm_wday == 1) armed_ = false;
    }
};

// =============================================================================
// ENGINE 3 — BrentWtiSpreadEngine
// =============================================================================
// Brent and WTI crude normally trade within $2-4 of each other.
// When the spread widens significantly (>SPREAD_THRESH), one converges back.
// Historically: Brent typically converges DOWN to WTI (Brent is the premium leg).
//
// Signal: |brent_mid - wti_mid| > SPREAD_THRESH → enter the laggard.
// If Brent > WTI by more than threshold: short Brent OR long WTI.
// We prefer entering the cheaper leg (WTI long) — smaller spread cost.
//
// Session: London/NY only (07:00-22:00 UTC).
// Cost check: TP convergence distance must exceed oil execution floor.
// =============================================================================
class BrentWtiSpreadEngine {
public:
    double  SPREAD_THRESH  = 5.00;   // $5 Brent/WTI spread triggers entry
    double  TP_DIST        = 1.50;   // $1.50 convergence target
    double  SL_DIST        = 2.00;   // $2.00 SL — if spread widens further
    int     MAX_HOLD_SEC   = 3600;   // 1 hour — spread can take time to converge
    int     COOLDOWN_SEC   = 600;    // 10 min between entries
    bool    enabled        = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // Call on every USOIL.F tick — pass current Brent price from book
    CrossSignal on_tick_wti(double wti_bid, double wti_ask, double brent_mid,
                            CloseCb on_close) noexcept {
        if (!enabled || wti_bid <= 0 || brent_mid <= 0) return {};
        const double wti_mid = (wti_bid + wti_ask) * 0.5;
        const double spread  = wti_ask - wti_bid;

        if (pos_.active) {
            pos_.manage(wti_bid, wti_ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        struct tm ti{}; ca_utc_time(ti);
        const int h = ti.tm_hour;
        if (h < 7 || h >= 22) return {};  // London/NY only

        if (ca_now_sec() < cooldown_until_) return {};

        const double brent_wti_spread = brent_mid - wti_mid;
        if (brent_wti_spread <= SPREAD_THRESH) return {};

        // Cost gate: TP_DIST must exceed execution floor
        if (!ExecutionCostGuard::is_viable("USOIL.F", spread, TP_DIST, 0.01)) return {};

        // Brent premium too high → WTI should catch up (long WTI)
        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = true;  // long WTI — cheaper leg catches up
        sig.entry   = wti_mid;
        sig.tp      = wti_mid + TP_DIST;
        sig.sl      = wti_mid - SL_DIST;
        sig.size    = 0.01;
        sig.symbol  = "USOIL.F";
        sig.engine  = "BrentWtiSpread";
        sig.reason  = "WTI_DISCOUNT_CATCH";

        pos_.open(sig, spread);
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[BRENT-WTI] Spread=%.2f WTI LONG entry=%.4f tp=%.4f sl=%.4f\n",
               brent_wti_spread, wti_mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

private:
    CrossPosition pos_;
    int64_t       cooldown_until_ = 0;
};

// =============================================================================
// ENGINE 4 — FxCascadeEngine
// =============================================================================
// When EURUSD fires a breakout (phase transitions to FLAT after signal),
// correlated FX pairs follow within 100-500ms. With 0.3ms RTT we arm a
// bracket on GBPUSD/AUDUSD/NZDUSD in the same direction before they react.
//
// Correlation structure (USD as base):
//   EURUSD ↑ → GBPUSD ↑ (0.88 corr), AUDUSD ↑ (0.73), NZDUSD ↑ (0.69)
//   EURUSD ↑ → USDJPY ↓ (−0.65, inverse because JPY is quote)
//
// Signal: EURUSD breakout fires (external flag set by main.cpp) →
//         arm bracket on GBPUSD/AUDUSD/NZDUSD in same direction within CASCADE_WINDOW_MS.
//
// Each pair gets its own independent CrossPosition — GBPUSD/AUDUSD/NZDUSD
// can all enter simultaneously on the same EURUSD signal (different instruments,
// not duplicate exposure). Each has its own cooldown.
//
// Cost check: per-pair TP must cover $10/lot Forex ECN cost at entry lot.
// =============================================================================
class FxCascadeEngine {
public:
    int64_t CASCADE_WINDOW_MS  = 500;   // must enter within 500ms of EURUSD signal
    double  TP_PCT             = 0.06;  // 6pip TP on GBPUSD/AUDUSD/NZDUSD
    double  SL_PCT             = 0.04;  // 4pip SL
    int     MAX_HOLD_SEC       = 120;
    int     COOLDOWN_SEC       = 60;
    bool    enabled            = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // Call from main when EURUSD fires a signal
    void notify_eurusd_signal(bool is_long) noexcept {
        if (!enabled) return;
        armed_long_    = is_long;
        armed_ts_ms_   = ca_now_ms();
        armed_         = true;
        printf("[FX-CASCADE] EURUSD %s fired — cascade armed (GBPUSD+AUDUSD+NZDUSD)\n",
               is_long ? "LONG" : "SHORT");
        fflush(stdout);
    }

    // ── GBPUSD tick ──────────────────────────────────────────────────────────
    CrossSignal on_tick_gbpusd(double bid, double ask, CloseCb on_close) noexcept {
        return tick_pair(bid, ask, on_close, pos_gbp_, cooldown_gbp_, "GBPUSD");
    }

    // ── AUDUSD tick ──────────────────────────────────────────────────────────
    CrossSignal on_tick_audusd(double bid, double ask, CloseCb on_close) noexcept {
        return tick_pair(bid, ask, on_close, pos_aud_, cooldown_aud_, "AUDUSD");
    }

    // ── NZDUSD tick ──────────────────────────────────────────────────────────
    CrossSignal on_tick_nzdusd(double bid, double ask, CloseCb on_close) noexcept {
        return tick_pair(bid, ask, on_close, pos_nzd_, cooldown_nzd_, "NZDUSD");
    }

    bool has_open_position() const {
        return pos_gbp_.active || pos_aud_.active || pos_nzd_.active;
    }

    // Force-close all legs — each pair uses its own current price.
    // Called on disconnect; gbpusd_bid/ask used as fallback for AUD/NZD if zeroed.
    void force_close(double gbp_bid, double gbp_ask, CloseCb on_close) {
        pos_gbp_.force_close(gbp_bid, gbp_ask, on_close);
        // AUD/NZD legs use their own price when available, else GBPUSD as proxy
        pos_aud_.force_close(gbp_bid, gbp_ask, on_close);
        pos_nzd_.force_close(gbp_bid, gbp_ask, on_close);
    }

    // Per-pair force-close when caller has the exact price
    void force_close_audusd(double bid, double ask, CloseCb on_close) { pos_aud_.force_close(bid, ask, on_close); }
    void force_close_nzdusd(double bid, double ask, CloseCb on_close) { pos_nzd_.force_close(bid, ask, on_close); }

private:
    // Shared armed state — set by notify_eurusd_signal(), consumed per-pair independently
    bool    armed_       = false;
    bool    armed_long_  = true;
    int64_t armed_ts_ms_ = 0;

    // Per-pair positions and cooldowns
    CrossPosition pos_gbp_;
    CrossPosition pos_aud_;
    CrossPosition pos_nzd_;
    int64_t cooldown_gbp_ = 0;
    int64_t cooldown_aud_ = 0;
    int64_t cooldown_nzd_ = 0;

    CrossSignal tick_pair(double bid, double ask, CloseCb on_close,
                          CrossPosition& pos, int64_t& cooldown,
                          const char* pair_sym) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        if (pos.active) {
            pos.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        if (!armed_) return {};
        if (ca_now_ms() - armed_ts_ms_ > CASCADE_WINDOW_MS) {
            armed_ = false;  // window expired — disarm entirely
            return {};
        }
        if (ca_now_sec() < cooldown) return {};

        const double tp = armed_long_ ? mid * (1.0 + TP_PCT/100.0) : mid * (1.0 - TP_PCT/100.0);
        const double sl = armed_long_ ? mid * (1.0 - SL_PCT/100.0) : mid * (1.0 + SL_PCT/100.0);
        const double tp_dist = std::fabs(tp - mid);

        // Cost gate — Forex pairs carry commission + spread + slippage ~$10/lot
        if (!ExecutionCostGuard::is_viable(pair_sym, spread, tp_dist, 0.01)) return {};

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = armed_long_;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = pair_sym;
        sig.engine  = "FxCascade";
        sig.reason  = armed_long_ ? "EUR_CASCADE_LONG" : "EUR_CASCADE_SHORT";

        pos.open(sig, spread);
        cooldown = ca_now_sec() + COOLDOWN_SEC;
        printf("[FX-CASCADE] %s %s entry=%.5f tp=%.5f sl=%.5f\n",
               pair_sym, armed_long_?"LONG":"SHORT", mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }
};

// =============================================================================
// ENGINE 5 — CarryUnwindEngine
// =============================================================================
// USDJPY is the classic carry trade pair. When risk-off triggers, JPY
// repatriation causes fast USDJPY drops. VIX spike confirms the risk-off signal.
//
// Signal:
//   1. VIX spikes >VIX_SPIKE_PCT in last 60 ticks (rapid vol expansion)
//   2. USDJPY falling (momentum negative)
//   → Short USDJPY aggressively
//
// This is a macro-driven momentum entry, not compression breakout.
// Only fires when VIX is spiking AND USDJPY is already moving down.
// Cost check: TP pip distance must exceed Forex ECN cost (~1 pip at 0.01 lot).
// =============================================================================
class CarryUnwindEngine {
public:
    double  VIX_SPIKE_THRESH  = 20.0; // VIX above this triggers monitoring
    double  VIX_SURGE_PCT     = 5.0;  // VIX up 5% in window = spike confirmed
    double  USDJPY_MOVE_PCT   = 0.10; // USDJPY must be down 0.10% in window
    double  TP_PCT            = 0.20; // 20 pip TP
    double  SL_PCT            = 0.12; // 12 pip SL
    int     MAX_HOLD_SEC      = 600;
    int     COOLDOWN_SEC      = 300;
    bool    enabled           = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(double usdjpy_bid, double usdjpy_ask,
                        double vix_now, CloseCb on_close) noexcept {
        if (!enabled || usdjpy_bid <= 0 || vix_now <= 0) return {};
        const double mid = (usdjpy_bid + usdjpy_ask) * 0.5;
        const double spread = usdjpy_ask - usdjpy_bid;

        if (pos_.active) {
            pos_.manage(usdjpy_bid, usdjpy_ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        // Track VIX and price history
        vix_window_.push_back(vix_now);
        price_window_.push_back(mid);
        if ((int)vix_window_.size()   > WINDOW_TICKS) vix_window_.pop_front();
        if ((int)price_window_.size() > WINDOW_TICKS) price_window_.pop_front();
        if ((int)vix_window_.size() < WINDOW_TICKS) return {};

        const double vix_start   = vix_window_.front();
        const double vix_surge   = (vix_now - vix_start) / vix_start * 100.0;
        const double price_start = price_window_.front();
        const double price_move  = (mid - price_start) / price_start * 100.0;

        if (vix_now < VIX_SPIKE_THRESH) return {};
        if (vix_surge < VIX_SURGE_PCT) return {};
        if (price_move > -USDJPY_MOVE_PCT) return {};  // must be falling

        if (ca_now_sec() < cooldown_until_) return {};

        const double tp = mid * (1.0 - TP_PCT/100.0);
        const double sl = mid * (1.0 + SL_PCT/100.0);
        const double tp_dist = std::fabs(tp - mid);

        // Cost gate
        if (!ExecutionCostGuard::is_viable("USDJPY", spread, tp_dist, 0.01)) return {};

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = false;  // carry unwind = short USDJPY
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = "USDJPY";
        sig.engine  = "CarryUnwind";
        sig.reason  = "VIX_SPIKE_JPY_BID";

        pos_.open(sig, spread);
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[CARRY-UNWIND] SHORT USDJPY entry=%.3f vix=%.1f surge=%.1f%% usd_move=%.3f%%\n",
               mid, vix_now, vix_surge, price_move);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

private:
    static constexpr int WINDOW_TICKS = 60;
    CrossPosition  pos_;
    std::deque<double> vix_window_;
    std::deque<double> price_window_;
    int64_t cooldown_until_ = 0;
};

// =============================================================================
// ENGINE 6 — OpeningRangeEngine
// =============================================================================
// Time-anchored opening range breakout. Tracks the high/low of the first
// RANGE_WINDOW_MIN minutes after a session open, then brackets those levels.
//
// Used for:
//   US equities: NY open 13:30 UTC, track 13:30-14:00, bracket at 14:00
//   GER40:       Xetra open 08:00 UTC, track 08:00-08:30, bracket at 08:30
//   Silver:      COMEX open 13:30 UTC, track 13:30-14:00, bracket at 14:00
//   UK100:       LSE open 08:00 UTC, track 08:00-08:15, bracket at 08:15
//   ESTX50:      Euronext open 09:00 UTC, track 09:00-09:15, bracket at 09:15
//
// Different from compression breakout: the range is time-fixed (always the
// first N minutes), not volatility-ratio based. Fires once per session.
// Cost check: TP_PCT move must exceed per-instrument execution floor.
// =============================================================================
class OpeningRangeEngine {
public:
    int     RANGE_WINDOW_MIN   = 30;   // build range over first 30 minutes
    double  BUFFER_PCT         = 0.03; // enter 0.03% beyond range edge
    double  TP_PCT             = 0.10; // TP = 0.10% beyond entry
    double  SL_PCT             = 0.06; // SL = 0.06% — inside range
    int     MAX_HOLD_SEC       = 1800; // 30 min — ORB resolves quickly
    int     OPEN_HOUR          = 13;   // session open hour UTC
    int     OPEN_MIN           = 30;   // session open minute UTC
    bool    enabled            = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        CloseCb on_close) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;

        if (pos_.active) {
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        struct tm ti{}; ca_utc_time(ti);
        const int mins_since_midnight = ti.tm_hour * 60 + ti.tm_min;
        const int open_mins           = OPEN_HOUR * 60 + OPEN_MIN;
        const int range_end_mins      = open_mins + RANGE_WINDOW_MIN;

        // Daily reset at open
        if (ti.tm_yday != last_day_) {
            range_high_ = 0.0;
            range_low_  = 0.0;
            armed_       = false;
            last_day_    = ti.tm_yday;
        }

        // Build range during first RANGE_WINDOW_MIN minutes
        if (mins_since_midnight >= open_mins && mins_since_midnight < range_end_mins) {
            if (range_high_ <= 0.0) {
                range_high_ = mid; range_low_ = mid;
                printf("[ORB-%s] Range build started at %02d:%02d UTC\n",
                       sym.c_str(), ti.tm_hour, ti.tm_min);
                fflush(stdout);
            }
            if (mid > range_high_) range_high_ = mid;
            if (mid < range_low_)  range_low_  = mid;
            return {};
        }

        // After range window: bracket the range (once per session)
        if (mins_since_midnight < range_end_mins) return {};
        if (range_high_ <= 0.0 || armed_) return {};

        const double buffer = mid * BUFFER_PCT / 100.0;

        // Breakout above range
        if (mid > range_high_ + buffer) {
            const double tp = mid * (1.0 + TP_PCT/100.0);
            const double sl = mid * (1.0 - SL_PCT/100.0);
            const double tp_dist = std::fabs(tp - mid);
            // Cost gate
            if (!ExecutionCostGuard::is_viable(sym.c_str(), spread, tp_dist, 0.01)) return {};

            armed_ = true;
            CrossSignal sig;
            sig.valid   = true;
            sig.is_long = true;
            sig.entry   = mid;
            sig.tp      = tp;
            sig.sl      = sl;
            sig.size    = 0.01;
            sig.symbol  = sym.c_str();
            sig.engine  = "OpeningRange";
            sig.reason  = "ORB_LONG";
            pos_.open(sig, spread);
            printf("[ORB-%s] LONG rh=%.4f entry=%.4f tp=%.4f sl=%.4f\n",
                   sym.c_str(), range_high_, mid, sig.tp, sig.sl);
            fflush(stdout);
            return sig;
        }
        // Breakout below range
        if (mid < range_low_ - buffer) {
            const double tp = mid * (1.0 - TP_PCT/100.0);
            const double sl = mid * (1.0 + SL_PCT/100.0);
            const double tp_dist = std::fabs(tp - mid);
            // Cost gate
            if (!ExecutionCostGuard::is_viable(sym.c_str(), spread, tp_dist, 0.01)) return {};

            armed_ = true;
            CrossSignal sig;
            sig.valid   = true;
            sig.is_long = false;
            sig.entry   = mid;
            sig.tp      = tp;
            sig.sl      = sl;
            sig.size    = 0.01;
            sig.symbol  = sym.c_str();
            sig.engine  = "OpeningRange";
            sig.reason  = "ORB_SHORT";
            pos_.open(sig, spread);
            printf("[ORB-%s] SHORT rl=%.4f entry=%.4f tp=%.4f sl=%.4f\n",
                   sym.c_str(), range_low_, mid, sig.tp, sig.sl);
            fflush(stdout);
            return sig;
        }
        return {};
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

    // Expose range for telemetry
    double range_high() const { return range_high_; }
    double range_low()  const { return range_low_;  }

private:
    CrossPosition pos_;
    double  range_high_ = 0.0;
    double  range_low_  = 0.0;
    bool    armed_      = false;
    int     last_day_   = -1;
};

// =============================================================================
// ENGINE 7 — VWAPReversionEngine
// =============================================================================
// Price that extends significantly from the daily VWAP has a strong mean-
// reversion tendency, particularly in indices and liquid FX pairs. The entry
// fires when price begins to tick back toward VWAP after over-extension.
//
// Signal logic:
//   1. Price deviates from VWAP by > EXTENSION_THRESH_PCT (e.g., 0.20%)
//   2. Price then ticks back toward VWAP (reversal tick confirmed)
//   3. Enter in the direction of VWAP (long if price below VWAP, short above)
//   4. TP = VWAP level (full reversion)
//   5. SL = extension × EXTENSION_SL_RATIO beyond current price (away from VWAP)
//
// Suited for: GER40, US500.F, USTEC.F, EURUSD (liquid, mean-reverting).
// Not suited for: strongly trending sessions — gated by extension threshold.
//
// Session: London open to NY close (08:00-22:00 UTC).
//
// Cost check: TP distance to VWAP must exceed execution floor before entry.
// =============================================================================
class VWAPReversionEngine {
public:
    double  EXTENSION_THRESH_PCT = 0.20; // price must be >0.20% from VWAP to qualify
    double  EXTENSION_SL_RATIO   = 0.60; // SL = extension × 0.60 past entry (away from VWAP)
    int     MAX_HOLD_SEC         = 900;  // 15 min — VWAP pull should happen fast
    int     COOLDOWN_SEC         = 180;  // 3 min between entries
    bool    enabled              = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // sym:      instrument symbol string
    // bid/ask:  current quotes
    // vwap:     daily VWAP (caller computes — GoldStack exposes vwap(), others use rolling)
    // on_close: trade close callback
    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        double vwap, CloseCb on_close) noexcept {
        if (!enabled || bid <= 0 || ask <= 0 || vwap <= 0) return {};
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        if (pos_.active) {
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        // Session gate: London/NY (08:00-22:00 UTC)
        struct tm ti{}; ca_utc_time(ti);
        const int h = ti.tm_hour;
        if (h < 8 || h >= 22) return {};

        if (ca_now_sec() < cooldown_until_) return {};

        // Compute deviation from VWAP
        const double deviation_pct = (mid - vwap) / vwap * 100.0;
        const bool   above_vwap    = (deviation_pct > 0.0);
        const double abs_dev_pct   = std::fabs(deviation_pct);

        // Must be extended beyond threshold
        if (abs_dev_pct < EXTENSION_THRESH_PCT) {
            prev_mid_ = mid;
            return {};
        }

        // Reversal tick: previous price was further from VWAP, now ticking back
        // above_vwap: price was above → reversal means prev > mid (tick down toward VWAP)
        // below_vwap: price was below → reversal means prev < mid (tick up toward VWAP)
        if (prev_mid_ <= 0.0) {
            prev_mid_ = mid;
            return {};
        }
        const bool reversal_tick = above_vwap ? (mid < prev_mid_) : (mid > prev_mid_);
        prev_mid_ = mid;

        if (!reversal_tick) return {};

        // Entry: toward VWAP
        const bool is_long = !above_vwap;  // if above VWAP → short; below → long

        // TP = VWAP level (full reversion)
        const double tp      = vwap;
        const double tp_dist = std::fabs(tp - mid);

        // SL = extension distance × SL_RATIO, placed past the current price
        // (further from VWAP than entry). If above VWAP (short): SL is above entry.
        const double extension_abs = std::fabs(mid - vwap);
        const double sl_offset     = extension_abs * EXTENSION_SL_RATIO;
        const double sl = above_vwap ? (mid + sl_offset) : (mid - sl_offset);

        // Sanity: TP must be closer to mid than SL (valid R:R geometry)
        if (tp_dist <= 0.0 || tp_dist < sl_offset * 0.5) return {};

        // Cost gate: TP to VWAP distance must cover execution costs
        if (!ExecutionCostGuard::is_viable(sym.c_str(), spread, tp_dist, 0.01)) return {};

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = sym.c_str();
        sig.engine  = "VWAPReversion";
        sig.reason  = is_long ? "VWAP_REV_LONG" : "VWAP_REV_SHORT";

        pos_.open(sig, spread);
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[VWAP-REV] %s %s vwap=%.4f mid=%.4f dev=%.3f%% tp=%.4f sl=%.4f\n",
               sym.c_str(), is_long?"LONG":"SHORT",
               vwap, mid, deviation_pct, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

private:
    CrossPosition pos_;
    double  prev_mid_       = 0.0;
    int64_t cooldown_until_ = 0;
};

// =============================================================================
// ENGINE 8 — TrendPullbackEngine
// =============================================================================
// EMA-9/21/50 trend detection: when all three EMAs are stacked in order
// (EMA9 > EMA21 > EMA50 for uptrend, or reverse for downtrend), the dominant
// trend is confirmed. Pullbacks to the slow EMA (EMA50) with a bounce
// confirmation tick provide high-probability continuation entries.
//
// Signal logic:
//   1. EMA9 > EMA21 > EMA50 (uptrend) OR EMA9 < EMA21 < EMA50 (downtrend)
//   2. Price pulls back to EMA50 ± PULLBACK_BAND_PCT
//   3. Price then bounces back toward trend direction (one confirmation tick)
//   4. Enter in trend direction
//   5. TP = EMA9 level (trend continuation target — price already was there)
//   6. SL = EMA50 (if price breaches EMA50 on close, trend is invalidated)
//
// EMA update: computed in on_tick() from each new mid price.
// Warm-up: requires EMA_WARMUP_TICKS before any signal (EMA must be stable).
//
// Session: London/NY (08:00-22:00 UTC) — no trend pullbacks in dead Asian hours.
//
// Cost check: TP distance (EMA9 - entry) must cover execution floor.
// =============================================================================
class TrendPullbackEngine {
public:
    double  PULLBACK_BAND_PCT = 0.05;    // price within 0.05% of EMA50 = "at EMA50"
    double  EMA9_ALPHA        = 2.0 / (9.0  + 1.0);
    double  EMA21_ALPHA       = 2.0 / (21.0 + 1.0);
    double  EMA50_ALPHA       = 2.0 / (50.0 + 1.0);
    int     EMA_WARMUP_TICKS  = 60;     // ticks before EMAs are trusted
    int     MAX_HOLD_SEC      = 600;    // 10 min — pullback entries should resolve fast
    int     COOLDOWN_SEC      = 120;
    bool    enabled           = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        CloseCb on_close) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // EMA update — always runs regardless of position state
        if (ema9_  <= 0.0) { ema9_ = ema21_ = ema50_ = mid; }  // seed on first tick
        ema9_  += EMA9_ALPHA  * (mid - ema9_);
        ema21_ += EMA21_ALPHA * (mid - ema21_);
        ema50_ += EMA50_ALPHA * (mid - ema50_);
        ++tick_count_;

        if (pos_.active) {
            // Update SL to EMA50 (trailing the slow EMA in trend direction)
            // Only tighten — never move SL against us
            if (pos_.is_long  && ema50_ > pos_.sl) pos_.sl = ema50_;
            if (!pos_.is_long && ema50_ < pos_.sl) pos_.sl = ema50_;
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        if (tick_count_ < EMA_WARMUP_TICKS) return {};

        // Session gate: London/NY (08:00-22:00 UTC)
        struct tm ti{}; ca_utc_time(ti);
        const int h = ti.tm_hour;
        if (h < 8 || h >= 22) return {};

        if (ca_now_sec() < cooldown_until_) return {};

        // Trend detection — all three EMAs must be stacked
        const bool uptrend   = (ema9_ > ema21_) && (ema21_ > ema50_);
        const bool downtrend = (ema9_ < ema21_) && (ema21_ < ema50_);
        if (!uptrend && !downtrend) return {};

        // Pullback detection: price at EMA50 band
        const double band = mid * PULLBACK_BAND_PCT / 100.0;
        const bool at_ema50 = std::fabs(mid - ema50_) < band;
        if (!at_ema50) {
            prev_at_ema50_ = false;
            prev_mid_      = mid;
            return {};
        }

        // Bounce confirmation: prev tick was also at EMA50, now ticking away in trend direction
        if (!prev_at_ema50_) {
            prev_at_ema50_ = true;
            prev_mid_      = mid;
            return {};
        }
        const bool bounce_up   = uptrend   && (mid > prev_mid_);
        const bool bounce_down = downtrend && (mid < prev_mid_);
        prev_mid_ = mid;
        if (!bounce_up && !bounce_down) return {};

        const bool is_long = uptrend;

        // TP = EMA9 level (price was there before pullback)
        const double tp      = ema9_;
        const double tp_dist = std::fabs(tp - mid);
        // SL = EMA50 (trend invalidated if EMA50 breached)
        const double sl      = ema50_;

        // Sanity: TP must be in the right direction
        if (is_long  && tp <= mid) return {};
        if (!is_long && tp >= mid) return {};
        if (tp_dist <= 0.0)        return {};

        // Cost gate
        if (!ExecutionCostGuard::is_viable(sym.c_str(), spread, tp_dist, 0.01)) return {};

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = sym.c_str();
        sig.engine  = "TrendPullback";
        sig.reason  = is_long ? "TREND_PB_LONG" : "TREND_PB_SHORT";

        pos_.open(sig, spread);
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[TREND-PB] %s %s ema9=%.4f ema21=%.4f ema50=%.4f entry=%.4f tp=%.4f sl=%.4f\n",
               sym.c_str(), is_long?"LONG":"SHORT",
               ema9_, ema21_, ema50_, mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

    // Expose EMAs for telemetry / external inspection
    double ema9()  const { return ema9_;  }
    double ema21() const { return ema21_; }
    double ema50() const { return ema50_; }

private:
    CrossPosition pos_;
    double  ema9_          = 0.0;
    double  ema21_         = 0.0;
    double  ema50_         = 0.0;
    int     tick_count_    = 0;
    bool    prev_at_ema50_ = false;
    double  prev_mid_      = 0.0;
    int64_t cooldown_until_ = 0;
};

} // namespace cross
} // namespace omega
