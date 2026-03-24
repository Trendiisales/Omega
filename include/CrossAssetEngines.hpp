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
//   4. FxCascadeEngine       — EURUSD breaks → arm correlated pairs
//   5. CarryUnwindEngine     — VIX spike + USDJPY falling → short USDJPY
//   6. OpeningRangeEngine    — Time-anchored first-30-min range breakout
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

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = is_long ? mid * (1.0 + TP_PCT/100.0) : mid * (1.0 - TP_PCT/100.0);
        sig.sl      = is_long ? mid * (1.0 - SL_PCT/100.0) : mid * (1.0 + SL_PCT/100.0);
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

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = is_long
            ? mid + spike_dist * TP_RATIO
            : mid - spike_dist * TP_RATIO;
        sig.sl      = is_long
            ? mid - spike_dist * SL_RATIO
            : mid + spike_dist * SL_RATIO;
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
// bracket on GBPUSD in the same direction before it reacts.
//
// Correlation structure (USD as base):
//   EURUSD ↑ → GBPUSD ↑ (0.88 corr), AUDUSD ↑ (0.73), NZDUSD ↑ (0.69)
//   EURUSD ↑ → USDJPY ↓ (−0.65, inverse because JPY is quote)
//
// Signal: EURUSD breakout fires (external flag set by main.cpp) →
//         arm bracket on GBPUSD in same direction within CASCADE_WINDOW_MS.
// =============================================================================
class FxCascadeEngine {
public:
    int64_t CASCADE_WINDOW_MS  = 500;   // must enter within 500ms of EURUSD signal
    double  TP_PCT             = 0.06;  // 6pip TP on GBPUSD
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
        printf("[FX-CASCADE] EURUSD %s fired — cascade armed\n",
               is_long ? "LONG" : "SHORT");
        fflush(stdout);
    }

    // Call on every GBPUSD tick
    CrossSignal on_tick_gbpusd(double bid, double ask, CloseCb on_close) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;

        if (pos_.active) {
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        if (!armed_) return {};
        if (ca_now_ms() - armed_ts_ms_ > CASCADE_WINDOW_MS) {
            armed_ = false;  // window expired
            return {};
        }
        if (ca_now_sec() < cooldown_until_) return {};

        armed_ = false;  // consume the signal

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = armed_long_;
        sig.entry   = mid;
        sig.tp      = armed_long_ ? mid * (1.0 + TP_PCT/100.0) : mid * (1.0 - TP_PCT/100.0);
        sig.sl      = armed_long_ ? mid * (1.0 - SL_PCT/100.0) : mid * (1.0 + SL_PCT/100.0);
        sig.size    = 0.01;
        sig.symbol  = "GBPUSD";
        sig.engine  = "FxCascade";
        sig.reason  = armed_long_ ? "EUR_CASCADE_LONG" : "EUR_CASCADE_SHORT";

        pos_.open(sig, spread);
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[FX-CASCADE] GBPUSD %s entry=%.5f tp=%.5f sl=%.5f\n",
               armed_long_?"LONG":"SHORT", mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }

private:
    CrossPosition pos_;
    bool    armed_       = false;
    bool    armed_long_  = true;
    int64_t armed_ts_ms_ = 0;
    int64_t cooldown_until_ = 0;
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

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = false;  // carry unwind = short USDJPY
        sig.entry   = mid;
        sig.tp      = mid * (1.0 - TP_PCT/100.0);
        sig.sl      = mid * (1.0 + SL_PCT/100.0);
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
//
// Different from compression breakout: the range is time-fixed (always the
// first 30 minutes), not volatility-ratio based. Fires once per session.
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
            armed_ = true;
            CrossSignal sig;
            sig.valid   = true;
            sig.is_long = true;
            sig.entry   = mid;
            sig.tp      = mid * (1.0 + TP_PCT/100.0);
            sig.sl      = mid * (1.0 - SL_PCT/100.0);
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
            armed_ = true;
            CrossSignal sig;
            sig.valid   = true;
            sig.is_long = false;
            sig.entry   = mid;
            sig.tp      = mid * (1.0 - TP_PCT/100.0);
            sig.sl      = mid * (1.0 + SL_PCT/100.0);
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

} // namespace cross
} // namespace omega
