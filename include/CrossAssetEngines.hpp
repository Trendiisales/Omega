#pragma once
// =============================================================================
// CrossAssetEngines.hpp -- Cross-asset and event-driven engines
//
// These engines exploit relationships BETWEEN instruments and SCHEDULED EVENTS.
// All data they need is already flowing in the system -- no new feeds required.
//
// ENGINES (9 co-resident under namespace omega::cross):
//   1. EsNqDivergenceEngine    -- ES/NQ diverge >threshold -> enter laggard
//   2. OilEventFadeEngine      -- EIA inventory spike -> fade 50% of move
//   3. BrentWtiSpreadEngine    -- Brent/WTI spread >$5 -> enter convergence
//   4. FxCascadeEngine         -- EURUSD breaks -> arm GBPUSD + AUDUSD + NZDUSD
//   5. CarryUnwindEngine       -- VIX spike + USDJPY falling -> short USDJPY
//   6. OpeningRangeEngine      -- Time-anchored first-30-min range breakout (5 instances)
//   7. VWAPReversionEngine     -- Price extends from daily VWAP -> enter on reversal tick (4 instances)
//   8. TrendPullbackEngine     -- EMA-9/21/50 trend + pullback to slow EMA with bounce (4 instances)
//   9. NoiseBandMomentumEngine -- ATR-band breakout + session-open anchor (6 instances; gold_london live)
//
// (Engine 10 SilverTurtleTickEngine REMOVED at Batch 5V 2026-04-19 after real-tick
//  backtest showed Sharpe=-16.23 on 42M XAGUSD ticks. See SilverTurtleTickEngine.md
//  wiki tombstone for historical record.)
//
// COST ENFORCEMENT: engine-local on_tick() cost checks have been REMOVED across
// the module. The canonical cost gate now lives in trade_lifecycle.hpp's
// enter_directional() function (spread + slip + commission vs TP distance).
// Each engine signals freely; the close-sink pipeline enforces cost viability
// uniformly. See wiki/entities/trade_lifecycle.md for the canonical gate.
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

// ?????????????????????????????????????????????????????????????????????????????
// Shared helpers
// ?????????????????????????????????????????????????????????????????????????????
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

// ?????????????????????????????????????????????????????????????????????????????
// CrossSignal -- what every cross-asset engine returns
// ?????????????????????????????????????????????????????????????????????????????
struct CrossSignal {
    bool        valid             = false;
    bool        is_long           = true;
    double      entry             = 0.0;
    double      tp                = 0.0;
    double      sl                = 0.0;
    double      size              = 0.01;
    const char* symbol            = "";
    const char* engine            = "";
    const char* reason            = "";
    int         confluence_score  = 1;  // 1-4: how many confirming factors aligned
                                        // main.cpp scales risk multiplier from this
};

// ?????????????????????????????????????????????????????????????????????????????
// ExecutionCostGuard -- enforces cost floors BEFORE any trade is allowed
//
// BlackBull ECN real execution costs (commission + spread + slippage):
//
//   Forex majors:  ~$10/lot  (spread $2, commission $6, slippage $2)
//   Gold:          ~$9/lot   (spread $1.5, commission $6, slippage $1.5)
//   Silver:        ~$12/lot  (spread $3, commission $6, slippage $3)
//   NAS100:        ~$7/lot   (spread $3, slippage $4 -- no commission)
//   US30:          ~$10/lot  (spread $4, slippage $6 -- no commission)
//   GER40:         ~$4.5/lot (spread $2, slippage $2.5 -- no commission)
//   Oil:           ~$4.5/lot (spread $2.5, slippage $2 -- no commission)
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
// All engines call this -- NO trade fires without clearing it.
// ?????????????????????????????????????????????????????????????????????????????
// ExecutionCostGuard is defined in OmegaCostGuard.hpp (included early in main.cpp
// before templated lambdas so MSVC can resolve it at template definition time).
#include "OmegaCostGuard.hpp"

// ?????????????????????????????????????????????????????????????????????????????
// CrossPosition -- shared open position tracker (simple, one per engine)
// CrossPosition -- shared open position tracker (simple, one per engine)
// ?????????????????????????????????????????????????????????????????????????????
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
    bool    be_locked_      = false; // true once SL moved to breakeven
    bool    tp_extended_    = false; // true once TP has been extended past initial target
    double  init_tp_dist_   = 0.0;  // cached original TP distance for trail calc after extension
    bool    allow_tp_extend = true;  // set false for mean-reversion engines that must close at TP
    bool    shadow_mode     = true;  // default true = log only, no live orders (Class C added 2026-04-21)
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

        // ?? Profit lock-in: BE + trailing stop ???????????????????????????????
        // BE lock: once move >= 50% of initial TP distance, move SL to entry+spread
        // Trail: once move >= 75% of TP distance, trail SL at 40% of TP distance behind peak
        const double tp_dist = tp_extended_ ? init_tp_dist_ : std::fabs(tp - entry);
        if (!tp_extended_) init_tp_dist_ = tp_dist;  // cache original TP dist
        if (tp_dist > 0.0) {
            const double be_threshold    = tp_dist * 0.40;  // lock BE at 40% to TP (was 50%)
            const double trail_threshold = tp_dist * 0.60;  // start trail at 60% to TP (was 75%)
            const double trail_dist      = tp_dist * 0.20;  // trail 20% of TP dist behind peak (was 40% -- too loose)
            if (move >= be_threshold && !be_locked_) {
                // Lock breakeven -- SL moves to entry (+ tiny buffer for spread)
                be_locked_ = true;
                const double be_sl = is_long ? (entry + spread_at_entry)
                                             : (entry - spread_at_entry);
                if (is_long  && be_sl > sl) sl = be_sl;
                if (!is_long && be_sl < sl) sl = be_sl;
            }
            // Mid-lock: at 50% of TP, lock 25% of TP above entry
            if (move >= tp_dist * 0.50 && be_locked_) {
                const double mid_lock = is_long ? (entry + tp_dist * 0.25)
                                                : (entry - tp_dist * 0.25);
                if (is_long  && mid_lock > sl) sl = mid_lock;
                if (!is_long && mid_lock < sl) sl = mid_lock;
            }
            if (move >= trail_threshold) {
                // Trail SL tightly behind MFE -- 20% of TP dist = much tighter lock
                const double trail_sl = is_long ? (entry + mfe - trail_dist)
                                                : (entry - mfe + trail_dist);
                if (is_long  && trail_sl > sl) sl = trail_sl;
                if (!is_long && trail_sl < sl) sl = trail_sl;
            }
        }

        const bool tp_hit = is_long ? (bid >= tp) : (ask <= tp);
        const bool sl_hit = is_long ? (bid <= sl) : (ask >= sl);
        const bool timed_out = (ca_now_sec() - entry_ts) >= max_hold_sec;

        // ?? TP hit: don't close fully -- extend TP and tighten trail ??????????
        // When price reaches initial TP (VWAP), lock BE tightly and extend TP
        // by 1x the original TP distance to capture continuation moves.
        // Trail tightens to 25% of original TP dist behind peak.
        // Only extend once (tp_extended_ flag).
        if (tp_hit && !tp_extended_ && tp_dist > 0.0 && allow_tp_extend) {
            tp_extended_ = true;
            // Extend TP by 1x original dist (ride continuation)
            tp = is_long ? (tp + tp_dist) : (tp - tp_dist);
            // Tighten trail: 15% of original TP dist behind peak MFE
            const double tight_sl = is_long ? (entry + mfe - tp_dist * 0.15)
                                            : (entry - mfe + tp_dist * 0.15);
            if (is_long  && tight_sl > sl) sl = tight_sl;
            if (!is_long && tight_sl < sl) sl = tight_sl;
            return false;  // don't close -- ride continuation
        }

        const char* reason_str = tp_hit ? "TP_HIT" : (sl_hit ? "SL_HIT" : "TIMEOUT");
        double exit_px = tp_hit ? tp : (sl_hit ? sl : mid);

        if (tp_hit || sl_hit || timed_out) {
            if (timed_out && !sl_hit && !tp_hit) {
                const bool sl_breached = is_long ? (mid < sl) : (mid > sl);
                if (sl_breached) {
                    exit_px    = sl;
                    reason_str = "SL_HIT";
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
        mfe = mae = 0.0; be_locked_ = false; tp_extended_ = false; init_tp_dist_ = 0.0;  // allow_tp_extend preserved across trades
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
    // Patch lot size after enter_directional succeeds -- corrects the hardcoded
    // 0.01 fallback with the actual risk-sized lot for accurate shadow P&L.
    void patch_size(double lot) noexcept { if (active && lot > 0.0) size = lot; }

    // Silent reset -- clears internal state without recording a trade.
    // Use this when rolling back a phantom position (enter_directional rejected
    // the trade after pos_.open() had already been called). No broker order
    // was sent, so no trade record should be written.
    void reset() noexcept {
        active = false;
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
        tr.shadow      = shadow_mode;
        active = false;
        if (on_close) on_close(tr);
    }
};

// =============================================================================
// ENGINE 1 -- EsNqDivergenceEngine
// =============================================================================
// When US500 and USTEC diverge significantly (one leads, one lags), the laggard
// catches up. The divergence signal already lives in MacroRegimeDetector but is
// only used as a gate. This engine trades it.
//
// Signal: esNqDivergence() > DIV_ENTRY_THRESH ? enter laggard in leader's direction.
// esNqDivergence() = ES_return - NQ_return over DIV_WINDOW ticks (60 ticks).
// Positive = ES outperforming NQ ? NQ should catch up ? long NQ / short ES.
// Negative = NQ outperforming ES ? ES should catch up ? long ES / short NQ.
//
// Only fires during NY session (13:30-17:00 UTC) when both are liquid.
// Cost check: TP move must cover spread + slippage (no commission for indices).
// =============================================================================
// =============================================================================
// SIGNAL QUALITY FIX (2026-03):
//   Original engine fired on any single tick where div > threshold. This caused
//   excessive entries on normal ES/NQ spread fluctuations -- the divergence value
//   bounces above threshold momentarily then reverses with no actual lag.
//
//   Fix: require divergence to remain above threshold for CONFIRM_TICKS consecutive
//   ticks on the same symbol before firing. This filters single-tick noise while
//   still catching genuine sustained divergences (which persist 5-15+ ticks).
//   CONFIRM_TICKS=3: at 5-15 ticks/sec, 3 ticks = 200-600ms -- enough to confirm
//   a real lag without losing the edge (convergence takes 5-30s).
//
//   Enabled via [cross_asset] esnq_enabled=true in omega_config.ini.
//   Default: disabled until signal quality is validated in shadow.
// =============================================================================
class EsNqDivergenceEngine {
public:
    double  DIV_ENTRY_THRESH   = 0.0008;
    double  DIV_EXIT_THRESH    = 0.0002;
    double  TP_PCT             = 0.08;
    double  SL_PCT             = 0.05;
    int     MAX_HOLD_SEC       = 300;
    int     COOLDOWN_SEC       = 120;
    int     CONFIRM_TICKS      = 3;       // consecutive ticks above threshold required
    bool    enabled            = false;   // disabled until shadow validates signal quality

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        double div, CloseCb on_close) noexcept {
        if (bid <= 0 || ask <= 0) return {};
        // Always manage open position even when disabled -- drain existing pos.
        if (pos_.active) {
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }
        if (!enabled) return {};

        // Session gate: NY only (13:30-17:00 UTC)
        struct tm ti{}; ca_utc_time(ti);
        const int mins = ti.tm_hour * 60 + ti.tm_min;
        if (mins < 13*60+30 || mins >= 17*60) {
            confirm_count_ = 0;
            return {};
        }
        if (ca_now_sec() < cooldown_until_) return {};

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Determine if this tick qualifies and in which direction.
        // Laggard-long only -- shorting the leader is unreliable.
        bool qualifies = false;
        bool is_long   = true;
        if      (div >  DIV_ENTRY_THRESH && sym == "USTEC.F") { qualifies = true; is_long = true; }
        else if (div < -DIV_ENTRY_THRESH && sym == "US500.F") { qualifies = true; is_long = true; }

        if (!qualifies || confirm_is_long_ != is_long) {
            confirm_count_   = qualifies ? 1 : 0;
            confirm_is_long_ = is_long;
            return {};
        }

        ++confirm_count_;
        if (confirm_count_ < CONFIRM_TICKS) return {};  // not yet confirmed

        confirm_count_ = 0;  // reset -- don't fire every subsequent tick

        const double tp      = mid * (1.0 + (is_long ? 1 : -1) * TP_PCT / 100.0);
        const double sl      = mid * (1.0 - (is_long ? 1 : -1) * SL_PCT / 100.0);
        // tp_dist removed -- cost check now in enter_directional with real lot size

        // Cost check removed: enter_directional() performs the definitive
        // cost check with the actual computed lot size. Checking here with
        // hardcoded 0.01 lots caused phantom trades: engine opened pos_ at
        // 0.01 (passes), real lot failed enter_directional, force_close fired.

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
        pos_.allow_tp_extend = false;  // EsNqDiv: mean-reversion, close at target
        last_div_at_entry_ = div;
        cooldown_until_    = ca_now_sec() + COOLDOWN_SEC;
        printf("[ESNQ-DIV] %s %s div=%.5f confirmed=%dticks entry=%.2f tp=%.2f sl=%.2f\n",
               sym.c_str(), is_long?"LONG":"SHORT", div, CONFIRM_TICKS, mid, tp, sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    int64_t open_entry_ts()     const { return pos_.entry_ts;  }  // UTC seconds -- stale-position detection
    double open_entry()   const { return pos_.entry; }
    bool   open_is_long() const { return pos_.is_long; }
    double open_size()    const { return pos_.size; }
    void cancel() noexcept { pos_.reset(); }  // phantom rollback -- no trade recorded
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void rollback() noexcept { pos_.reset(); }

private:
    CrossPosition pos_;
    int64_t cooldown_until_    = 0;
    double  last_div_at_entry_ = 0.0;
    int     confirm_count_     = 0;
    bool    confirm_is_long_   = false;
};

// =============================================================================
// ENGINE 2 -- OilEventFadeEngine
// =============================================================================
// EIA crude inventory release: Wednesday 14:30 UTC.
// Pattern: initial spike in one direction (0.3-0.8%), then 65-70% probability
// of fading 50% of the spike within 5 minutes.
//
// Logic:
//   1. Start monitoring at 14:30 UTC on Wednesdays
//   2. Capture price at 14:30:00 (pre-release baseline)
//   3. If price spikes >SPIKE_THRESH in first 15s ? arm fade in opposite direction
//   4. Enter fade at 14:30:15 (15s after release -- initial spike absorbed)
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
    int     MAX_HOLD_SEC     = 900;   // 15 min -- fade should resolve quickly
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

        // Spike confirmed -- fade it
        const bool spike_up = (spike_pct > 0.0);
        const double spike_dist = std::fabs(mid - baseline_);
        const bool is_long = !spike_up;  // fade: if spiked up, go short

        const double tp = is_long ? mid + spike_dist * TP_RATIO : mid - spike_dist * TP_RATIO;
        const double sl = is_long ? mid - spike_dist * SL_RATIO : mid + spike_dist * SL_RATIO;
        // tp_dist removed -- cost check now in enter_directional with real lot size

        // Cost gate
        // Cost check removed: enter_directional() performs the definitive
        // cost check with the actual computed lot size. Checking here with
        // hardcoded 0.01 lots caused phantom trades: engine opened pos_ at
        // 0.01 (passes), real lot failed enter_directional, force_close fired.

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
        pos_.allow_tp_extend = false;  // OilFade: mean-reversion, close at target
        armed_ = true;
        printf("[OIL-EIA-FADE] %s spike=%.3f%% entry=%.4f tp=%.4f sl=%.4f\n",
               is_long?"LONG":"SHORT", spike_pct, mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    int64_t open_entry_ts()     const { return pos_.entry_ts;  }  // UTC seconds -- stale-position detection
    double open_entry()   const { return pos_.entry; }
    bool   open_is_long() const { return pos_.is_long; }
    double open_size()    const { return pos_.size; }
    void cancel() noexcept { pos_.reset(); }  // phantom rollback -- no trade recorded
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void rollback() noexcept { pos_.reset(); }

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
// ENGINE 3 -- BrentWtiSpreadEngine
// =============================================================================
// Brent and WTI crude normally trade within $2-4 of each other.
// When the spread widens significantly (>SPREAD_THRESH), one converges back.
// Historically: Brent typically converges DOWN to WTI (Brent is the premium leg).
//
// Signal: |brent_mid - wti_mid| > SPREAD_THRESH ? enter the laggard.
// If Brent > WTI by more than threshold: short Brent OR long WTI.
// We prefer entering the cheaper leg (WTI long) -- smaller spread cost.
//
// Session: London/NY only (07:00-22:00 UTC).
// Cost check: TP convergence distance must exceed oil execution floor.
// =============================================================================
class BrentWtiSpreadEngine {
public:
    double  SPREAD_THRESH  = 5.00;   // $5 Brent/WTI spread triggers entry
    double  TP_DIST        = 1.50;   // $1.50 convergence target
    double  SL_DIST        = 2.00;   // $2.00 SL -- if spread widens further
    int     MAX_HOLD_SEC   = 3600;   // 1 hour -- spread can take time to converge
    int     COOLDOWN_SEC   = 600;    // 10 min between entries
    bool    enabled        = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // Call on every USOIL.F tick -- pass current Brent price from book
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
        // Cost check removed: enter_directional() performs the definitive
        // cost check with the actual computed lot size. Checking here with
        // hardcoded 0.01 lots caused phantom trades: engine opened pos_ at
        // 0.01 (passes), real lot failed enter_directional, force_close fired.

        // Brent premium too high ? WTI should catch up (long WTI)
        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = true;  // long WTI -- cheaper leg catches up
        sig.entry   = wti_mid;
        sig.tp      = wti_mid + TP_DIST;
        sig.sl      = wti_mid - SL_DIST;
        sig.size    = 0.01;
        sig.symbol  = "USOIL.F";
        sig.engine  = "BrentWtiSpread";
        sig.reason  = "WTI_DISCOUNT_CATCH";

        pos_.open(sig, spread);
        pos_.allow_tp_extend = false;  // BrentWti: mean-reversion, close at target
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[BRENT-WTI] Spread=%.2f WTI LONG entry=%.4f tp=%.4f sl=%.4f\n",
               brent_wti_spread, wti_mid, sig.tp, sig.sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    int64_t open_entry_ts()     const { return pos_.entry_ts;  }  // UTC seconds -- stale-position detection
    double open_entry()   const { return pos_.entry; }
    bool   open_is_long() const { return pos_.is_long; }
    double open_size()    const { return pos_.size; }
    void cancel() noexcept { pos_.reset(); }  // phantom rollback -- no trade recorded
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void rollback() noexcept { pos_.reset(); }

private:
    CrossPosition pos_;
    int64_t       cooldown_until_ = 0;
};

// =============================================================================
// ENGINE 4 -- FxCascadeEngine
// =============================================================================
// When EURUSD fires a breakout (phase transitions to FLAT after signal),
// correlated FX pairs follow within 100-500ms. With 0.3ms RTT we arm a
// bracket on GBPUSD/AUDUSD/NZDUSD in the same direction before they react.
//
// Correlation structure (USD as base):
//   EURUSD ? ? GBPUSD ? (0.88 corr), AUDUSD ? (0.73), NZDUSD ? (0.69)
//   EURUSD ? ? USDJPY ? (?0.65, inverse because JPY is quote)
//
// Signal: EURUSD breakout fires (external flag set by main.cpp) ?
//         arm bracket on GBPUSD/AUDUSD/NZDUSD in same direction within CASCADE_WINDOW_MS.
//
// Each pair gets its own independent CrossPosition -- GBPUSD/AUDUSD/NZDUSD
// can all enter simultaneously on the same EURUSD signal (different instruments,
// not duplicate exposure). Each has its own cooldown.
//
// Cost check: per-pair TP must cover $10/lot Forex ECN cost at entry lot.
// =============================================================================
class FxCascadeEngine {
public:
    int64_t CASCADE_WINDOW_MS  = 200;   // tightened 500?200ms: live RTT is 3-9ms, 200ms is ample
    double  TP_PCT             = 0.08;  // raised 0.06?0.08: 8pip TP clears cost guard at ratio?1.5?
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
        printf("[FX-CASCADE] EURUSD %s fired -- cascade armed (GBPUSD+AUDUSD+NZDUSD)\n",
               is_long ? "LONG" : "SHORT");
        fflush(stdout);
    }

    // ?? GBPUSD tick ??????????????????????????????????????????????????????????
    CrossSignal on_tick_gbpusd(double bid, double ask, CloseCb on_close) noexcept {
        return tick_pair(bid, ask, on_close, pos_gbp_, cooldown_gbp_, "GBPUSD");
    }

    // ?? AUDUSD tick ??????????????????????????????????????????????????????????
    CrossSignal on_tick_audusd(double bid, double ask, CloseCb on_close) noexcept {
        return tick_pair(bid, ask, on_close, pos_aud_, cooldown_aud_, "AUDUSD");
    }

    // ?? NZDUSD tick ??????????????????????????????????????????????????????????
    CrossSignal on_tick_nzdusd(double bid, double ask, CloseCb on_close) noexcept {
        return tick_pair(bid, ask, on_close, pos_nzd_, cooldown_nzd_, "NZDUSD");
    }

    bool has_open_position() const {
        return pos_gbp_.active || pos_aud_.active || pos_nzd_.active;
    }

    // Per-leg open checks -- used by main.cpp to gate each cascade leg independently.
    // This is critical: the cascade is designed to enter ALL three legs on a EURUSD signal.
    // Using the aggregate has_open_position() blocks AUD/NZD once GBP fires, defeating
    // the multi-leg purpose entirely. Per-leg gates allow concurrent cascade positions.
    bool has_open_gbpusd() const { return pos_gbp_.active; }
    bool has_open_audusd() const { return pos_aud_.active; }
    bool has_open_nzdusd() const { return pos_nzd_.active; }
    // Unrealised PnL helpers -- return first active leg (GBP > AUD > NZD)
    double open_entry()   const {
        if (pos_gbp_.active) return pos_gbp_.entry;
        if (pos_aud_.active) return pos_aud_.entry;
        if (pos_nzd_.active) return pos_nzd_.entry;
        return 0.0;
    }
    bool   open_is_long() const {
        if (pos_gbp_.active) return pos_gbp_.is_long;
        if (pos_aud_.active) return pos_aud_.is_long;
        return pos_nzd_.is_long;
    }
    double open_size()    const {
        if (pos_gbp_.active) return pos_gbp_.size;
        if (pos_aud_.active) return pos_aud_.size;
        if (pos_nzd_.active) return pos_nzd_.size;
        return 0.0;
    }

    // Force-close all legs -- each pair uses its own current price.
    // Called on disconnect; gbpusd_bid/ask used as fallback for AUD/NZD if zeroed.
    void rollback_gbp() noexcept { pos_gbp_.reset(); }
    void rollback_aud() noexcept { pos_aud_.reset(); }
    void rollback_nzd() noexcept { pos_nzd_.reset(); }
    void force_close(double gbp_bid, double gbp_ask, CloseCb on_close) {
        pos_gbp_.force_close(gbp_bid, gbp_ask, on_close);
        // AUD/NZD legs use their own price when available, else GBPUSD as proxy
        pos_aud_.force_close(gbp_bid, gbp_ask, on_close);
        pos_nzd_.force_close(gbp_bid, gbp_ask, on_close);
    }

    // Per-pair force-close when caller has the exact price
    void cancel_gbpusd() noexcept { pos_gbp_.reset(); }
    void cancel_audusd() noexcept { pos_aud_.reset(); }
    void cancel_nzdusd() noexcept { pos_nzd_.reset(); }
    void force_close_audusd(double bid, double ask, CloseCb on_close) { pos_aud_.force_close(bid, ask, on_close); }
    void force_close_nzdusd(double bid, double ask, CloseCb on_close) { pos_nzd_.force_close(bid, ask, on_close); }
    void patch_size_gbp(double lot) noexcept { pos_gbp_.patch_size(lot); }
    void patch_size_aud(double lot) noexcept { pos_aud_.patch_size(lot); }
    void patch_size_nzd(double lot) noexcept { pos_nzd_.patch_size(lot); }

private:
    // Shared armed state -- set by notify_eurusd_signal(), consumed per-pair independently
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
            armed_ = false;  // window expired -- disarm entirely
            return {};
        }
        if (ca_now_sec() < cooldown) return {};

        const double tp = armed_long_ ? mid * (1.0 + TP_PCT/100.0) : mid * (1.0 - TP_PCT/100.0);
        const double sl = armed_long_ ? mid * (1.0 - SL_PCT/100.0) : mid * (1.0 + SL_PCT/100.0);
        // tp_dist removed -- cost check now in enter_directional with real lot size

        // Cost gate -- Forex pairs carry commission + spread + slippage ~$10/lot
        // Cost check removed: enter_directional() performs the definitive
        // cost check with the actual computed lot size. Checking here with
        // hardcoded 0.01 lots caused phantom trades: engine opened pos_ at
        // 0.01 (passes), real lot failed enter_directional, force_close fired.

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
// ENGINE 5 -- CarryUnwindEngine (improved: 4-factor confirmation)
// =============================================================================
// USDJPY carry trade unwind. Original fired on VIX tick + any USDJPY dip,
// including dips within bull-market uptrends -- a lagging signal problem.
//
// Improved signal requires ALL FOUR to be true:
//   1. VIX > VIX_SPIKE_THRESH AND surged > VIX_SURGE_PCT (risk-off confirmed)
//   2. USDJPY down > USDJPY_MOVE_PCT in short 60-tick window (momentum)
//   3. fast EMA < slow EMA -- medium-term downtrend established
//      Prevents shorting a dip in an ongoing uptrend
//   4. Realized vol > REALVOL_MIN_PCT over 30-tick window (options skew proxy)
//      Low realized vol = dead tape noise, not a real unwind
// =============================================================================
class CarryUnwindEngine {
public:
    double  VIX_SPIKE_THRESH  = 20.0; // VIX above this triggers monitoring
    double  VIX_SURGE_PCT     = 5.0;  // VIX up 5% in window = spike confirmed
    double  USDJPY_MOVE_PCT   = 0.10; // USDJPY must be down 0.10% in short window
    double  TP_PCT            = 0.20; // 20 pip TP
    double  SL_PCT            = 0.12; // 12 pip SL
    int     MAX_HOLD_SEC      = 600;
    int     COOLDOWN_SEC      = 300;
    // Medium-term trend EMAs (~20-tick fast, ~60-tick slow)
    double  TREND_FAST_ALPHA  = 2.0 / (20.0 + 1.0);
    double  TREND_SLOW_ALPHA  = 2.0 / (60.0 + 1.0);
    // Realized vol filter -- min 30-tick hi-lo range / mid
    double  REALVOL_MIN_PCT   = 0.04; // 4bp min realized vol
    bool    enabled           = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(double usdjpy_bid, double usdjpy_ask,
                        double vix_now, CloseCb on_close) noexcept {
        if (!enabled || usdjpy_bid <= 0 || vix_now <= 0) return {};
        const double mid    = (usdjpy_bid + usdjpy_ask) * 0.5;
        const double spread = usdjpy_ask - usdjpy_bid;

        if (pos_.active) {
            pos_.manage(usdjpy_bid, usdjpy_ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        // Update all rolling state every tick
        vix_window_.push_back(vix_now);
        price_window_.push_back(mid);
        if ((int)vix_window_.size()   > WINDOW_TICKS) vix_window_.pop_front();
        if ((int)price_window_.size() > WINDOW_TICKS) price_window_.pop_front();

        // Medium-term EMAs
        if (ema_fast_ <= 0.0) { ema_fast_ = mid; ema_slow_ = mid; }
        else {
            ema_fast_ = TREND_FAST_ALPHA * mid + (1.0 - TREND_FAST_ALPHA) * ema_fast_;
            ema_slow_ = TREND_SLOW_ALPHA * mid + (1.0 - TREND_SLOW_ALPHA) * ema_slow_;
        }

        // Realized vol ring buffer
        realvol_window_.push_back(mid);
        if ((int)realvol_window_.size() > REALVOL_TICKS) realvol_window_.pop_front();

        if ((int)vix_window_.size()    < WINDOW_TICKS)  return {};
        if ((int)realvol_window_.size() < REALVOL_TICKS) return {};

        // ?? Filter 1: VIX spike ???????????????????????????????????????????????
        const double vix_start = vix_window_.front();
        const double vix_surge = (vix_now - vix_start) / vix_start * 100.0;
        if (vix_now  < VIX_SPIKE_THRESH) return {};
        if (vix_surge < VIX_SURGE_PCT)   return {};

        // ?? Filter 2: Short-term USDJPY momentum (must be falling) ???????????
        const double price_start = price_window_.front();
        const double price_move  = (mid - price_start) / price_start * 100.0;
        if (price_move > -USDJPY_MOVE_PCT) return {};

        // ?? Filter 3: Medium-term downtrend (fast EMA < slow EMA) ????????????
        // Prevents shorting a dip within an established uptrend
        if (ema_fast_ >= ema_slow_) return {};

        // ?? Filter 4: Realized vol confirms energy (options skew proxy) ???????
        // Low realized vol = noise, not a real risk-off unwind
        const double rv_hi  = *std::max_element(realvol_window_.begin(), realvol_window_.end());
        const double rv_lo  = *std::min_element(realvol_window_.begin(), realvol_window_.end());
        const double rv_pct = (rv_hi - rv_lo) / mid * 100.0;
        if (rv_pct < REALVOL_MIN_PCT) return {};

        if (ca_now_sec() < cooldown_until_) return {};

        const double tp      = mid * (1.0 - TP_PCT/100.0);
        const double sl      = mid * (1.0 + SL_PCT/100.0);
        // tp_dist removed -- cost check now in enter_directional with real lot size

        // Cost check removed: enter_directional() performs the definitive
        // cost check with the actual computed lot size. Checking here with
        // hardcoded 0.01 lots caused phantom trades: engine opened pos_ at
        // 0.01 (passes), real lot failed enter_directional, force_close fired.

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = false;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = "USDJPY";
        sig.engine  = "CarryUnwind";
        sig.reason  = "VIX_SPIKE_JPY_BID";

        pos_.open(sig, spread);
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[CARRY-UNWIND] SHORT USDJPY entry=%.3f vix=%.1f surge=%.1f%% "
               "move=%.3f%% ema_f=%.3f ema_s=%.3f rv=%.4f%%\n",
               mid, vix_now, vix_surge, price_move, ema_fast_, ema_slow_, rv_pct);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    int64_t open_entry_ts()     const { return pos_.entry_ts;  }  // UTC seconds -- stale-position detection
    double open_entry()   const { return pos_.entry; }
    bool   open_is_long() const { return pos_.is_long; }
    double open_size()    const { return pos_.size; }
    void cancel() noexcept { pos_.reset(); }  // phantom rollback -- no trade recorded
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void rollback() noexcept { pos_.reset(); }

private:
    static constexpr int WINDOW_TICKS  = 60;
    static constexpr int REALVOL_TICKS = 30;
    CrossPosition      pos_;
    std::deque<double> vix_window_;
    std::deque<double> price_window_;
    std::deque<double> realvol_window_;
    double             ema_fast_       = 0.0;
    double             ema_slow_       = 0.0;
    int64_t            cooldown_until_ = 0;
};

// =============================================================================
// ENGINE 6 -- OpeningRangeEngine
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
    double  SL_PCT             = 0.06; // SL = 0.06% -- inside range
    int     MAX_HOLD_SEC       = 1800; // 30 min -- ORB resolves quickly
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
        // tp_dist removed -- cost check now in enter_directional with real lot size
            // Cost gate
            // Cost check removed: enter_directional() performs the definitive
            // cost check with the actual computed lot size. Checking here with
            // hardcoded 0.01 lots caused phantom trades: engine opened pos_ at
            // 0.01 (passes), real lot failed enter_directional, force_close fired.

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
        // tp_dist removed -- cost check now in enter_directional with real lot size
            // Cost gate
            // Cost check removed: enter_directional() performs the definitive
            // cost check with the actual computed lot size. Checking here with
            // hardcoded 0.01 lots caused phantom trades: engine opened pos_ at
            // 0.01 (passes), real lot failed enter_directional, force_close fired.

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
    int64_t open_entry_ts()     const { return pos_.entry_ts;  }  // UTC seconds -- stale-position detection
    void cancel() noexcept { pos_.reset(); }  // phantom rollback -- no trade recorded
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void rollback() noexcept { pos_.reset(); }

    // Expose range for telemetry
    double range_high() const { return range_high_; }
    double range_low()  const { return range_low_;  }

    // Reset range state -- call on reconnect so a partial pre-disconnect range
    // (e.g. 10 minutes into the 30-minute window) does not trigger on stale data.
    // The daily reset in on_tick() handles normal day boundaries.
    void reset_range() noexcept {
        range_high_ = 0.0;
        range_low_  = 0.0;
        armed_      = false;
        // Do NOT reset last_day_ -- daily reset logic still uses it
    }

private:
    CrossPosition pos_;
    double  range_high_ = 0.0;
    double  range_low_  = 0.0;
    bool    armed_      = false;
    int     last_day_   = -1;
};

// =============================================================================
// ENGINE 7 -- VWAPReversionEngine
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
//   5. SL = extension ? EXTENSION_SL_RATIO beyond current price (away from VWAP)
//
// Suited for: GER40, US500.F, USTEC.F, EURUSD (liquid, mean-reverting).
// Not suited for: strongly trending sessions -- gated by extension threshold.
//
// Session: London open to NY close (08:00-22:00 UTC).
//
// Cost check: TP distance to VWAP must exceed execution floor before entry.
// =============================================================================
class VWAPReversionEngine {
public:
    double  EXTENSION_THRESH_PCT = 0.20; // price must be >0.20% from EWM-VWAP to qualify
    double  EXTENSION_SL_RATIO   = 0.60; // SL = extension ? 0.60 past entry (further from VWAP)
    double  MAE_EXIT_RATIO       = 0.50; // exit early if adverse move > 0.50 ? TP dist
    double  MAX_EXTENSION_PCT    = 0.80; // block entry if extension > 0.80% -- stale VWAP proxy
    int     MAX_HOLD_SEC         = 900;  // 15 min base timeout
    int     COOLDOWN_SEC         = 180;  // 3 min between normal entries
    int     MAE_COOLDOWN_SEC     = 600;  // 10 min after MAE exit -- thesis was wrong, don't retry
    int     CONSEC_FC_BLOCK_SEC  = 1800; // 30 min block after 2 consecutive MAE exits same direction
    int     TP_FLIP_COOLDOWN_SEC = 1200; // 20 min after TP: block OPPOSITE direction
                                          // TP = price crossed VWAP; trend may continue same way
    int     MIN_SESSION_MIN      = 120;  // only enter after 2hrs of session data (10:00 UTC)
    bool    enabled              = true;
    // Confluence thresholds
    double  CONF_VIX_THRESH      = 18.0;
    double  CONF_L2_THRESH       = 0.12;
    // Rolling EWM VWAP alpha -- TIME-BASED not tick-count-based.
    // Target: ~2hr half-life regardless of tick frequency.
    // Each tick we compute decay = 1 - exp(-dt / HALF_LIFE_SEC)
    // At dt=1s: decay?0.0096% per second. After 7200s (2hr): ~50% decayed.
    // This is invariant to whether NQ ticks 10?/sec or 1?/min.
    static constexpr double EWM_VWAP_HALF_LIFE_SEC = 7200.0;  // 2hr

    // Reset the EWM VWAP anchor -- called by main.cpp at session open.
    void reset_ewm_vwap(double anchor) noexcept {
        ewm_vwap_      = anchor;
        last_tick_sec_ = 0;  // will be set on first tick after reset
        printf("[VWAP-REV] EWM VWAP reset to %.4f (session anchor)\n", anchor);
        fflush(stdout);
    }

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    // sym:      instrument symbol string
    // bid/ask:  current quotes
    // vwap:     daily open (seed value -- engine maintains rolling EWM VWAP internally)
    // on_close: trade close callback
    // vix:      current VIX level (0 = unknown, skip confluence factor)
    // l2_imb:   L2 order book imbalance 0..1 (0.5 = neutral)
    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        double vwap_seed, CloseCb on_close,
                        double vix = 0.0, double l2_imb = 0.5) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // ?? Rolling EWM VWAP -- time-based decay, 2hr half-life ???????????????
        // Uses wall-clock elapsed seconds so decay is invariant to tick frequency.
        // NQ ticks ~10x/sec in NY session. Tick-count alpha would decay in minutes.
        // Time-based: ? = 1 - exp(-dt/T?) where T?=7200s. After 2hrs: 50% decayed.
        // Seed on first tick of session with the anchor price from main.cpp.
        if (ewm_vwap_ <= 0.0 && vwap_seed > 0.0) {
            ewm_vwap_      = vwap_seed;
            last_tick_sec_ = ca_now_sec();
        }
        if (ewm_vwap_ > 0.0) {
            const int64_t now_sec = ca_now_sec();
            const double dt = (last_tick_sec_ > 0)
                ? static_cast<double>(now_sec - last_tick_sec_)
                : 0.0;
            last_tick_sec_ = now_sec;
            if (dt > 0.0 && dt < 3600.0) {  // sanity: ignore gaps > 1hr (reconnect)
                // ? = 1 - e^(-dt/T?). For dt=1s, T?=7200s: ??0.000139
                const double alpha = 1.0 - std::exp(-dt / EWM_VWAP_HALF_LIFE_SEC);
                ewm_vwap_ += alpha * (mid - ewm_vwap_);
            }
        }
        const double vwap = (ewm_vwap_ > 0.0) ? ewm_vwap_ : vwap_seed;
        if (vwap <= 0) return {};

        if (pos_.active) {
            // ?? Progressive timeout: extend if still moving toward VWAP ??????
            // Standard 15min timeout cuts positions that are slowly reverting.
            // If price is still trending toward TP at timeout, extend by 5min.
            // This lets slow-moving reversions complete instead of timing out.
            const double move_toward = pos_.is_long ? (mid - pos_.entry)
                                                    : (pos_.entry - mid);
            const double tp_dist_pos = std::fabs(pos_.tp - pos_.entry);
            const int64_t held = ca_now_sec() - pos_.entry_ts;
            if (held >= MAX_HOLD_SEC) {
                const double progress = tp_dist_pos > 0 ? move_toward / tp_dist_pos : 0.0;
                if (progress > 0.30) {
                    // Still 30%+ toward TP and moving right way -- extend 5min, once
                    if (!timeout_extended_) {
                        timeout_extended_ = true;
                        printf("[VWAP-REV] %s timeout extended -- progress=%.0f%% still trending toward TP\n",
                               sym.c_str(), progress * 100.0);
                        fflush(stdout);
                        // managed by MAX_HOLD_SEC in manage() -- bump entry_ts to extend
                        pos_.entry_ts = ca_now_sec() - MAX_HOLD_SEC + 300; // 5min extension
                    }
                }
            }

            // ?? MAE early exit -- mean-reversion thesis invalidation ????????????
            const double adverse = pos_.is_long ? (pos_.entry - mid)
                                                : (mid - pos_.entry);
            if (adverse > tp_dist_pos * MAE_EXIT_RATIO && tp_dist_pos > 0.0) {
                printf("[VWAP-REV] %s MAE exit -- adverse=%.2f > %.2f (%.0f%% of TP dist) -- thesis dead\n",
                       sym.c_str(), adverse, tp_dist_pos * MAE_EXIT_RATIO,
                       MAE_EXIT_RATIO * 100.0);
                fflush(stdout);
                const bool this_long = pos_.is_long;
                pos_.force_close(bid, ask, on_close);
                timeout_extended_ = false;
                cooldown_until_ = ca_now_sec() + MAE_COOLDOWN_SEC;
                if (last_fc_long_ == this_long) {
                    ++consec_fc_same_dir_;
                } else {
                    consec_fc_same_dir_ = 1;
                    last_fc_long_       = this_long;
                }
                if (consec_fc_same_dir_ >= 2) {
                    fc_block_long_  = this_long;
                    fc_block_until_ = ca_now_sec() + CONSEC_FC_BLOCK_SEC;
                    printf("[VWAP-REV] %s %d consecutive MAE in %s direction -- blocking 30min\n",
                           sym.c_str(), consec_fc_same_dir_, this_long ? "LONG" : "SHORT");
                    fflush(stdout);
                }
                return {};
            }
            pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
            return {};
        }

        // Reset extension flag on new trade cycle
        timeout_extended_ = false;

        // Session gate: London/NY (08:00-22:00 UTC) -- entry only, not exit
        struct tm ti{}; ca_utc_time(ti);
        const int h = ti.tm_hour;
        if (h < 8 || h >= 22) return {};

        // Minimum session time gate (10:00 UTC minimum)
        const int session_min_elapsed = (h - 8) * 60 + ti.tm_min;
        if (session_min_elapsed < MIN_SESSION_MIN) return {};

        if (ca_now_sec() < cooldown_until_) return {};

        // ?? TP flip cooldown -- block opposite direction after TP hit ??????????
        // After a TP hit, block the OPPOSITE direction for TP_FLIP_COOLDOWN_SEC.
        // LONG TP ? block SHORT. SHORT TP ? block LONG.
        if (tp_flip_until_ > ca_now_sec()) {
            const bool would_be_long = (mid < vwap);
            if (would_be_long == tp_flip_block_long_) return {};  // blocked direction
        }

        // ?? Consecutive FC direction block ????????????????????????????????????
        if (fc_block_until_ > ca_now_sec()) {
            const bool is_long_signal = (mid < vwap);
            if (is_long_signal == fc_block_long_) return {};
        }

        // Compute deviation from VWAP
        const double deviation_pct = (mid - vwap) / vwap * 100.0;
        const bool   above_vwap    = (deviation_pct > 0.0);
        const double abs_dev_pct   = std::fabs(deviation_pct);

        if (abs_dev_pct < EXTENSION_THRESH_PCT) {
            prev_mid_ = mid;
            return {};
        }

        // ?? Maximum extension cap ?????????????????????????????????????????????
        // If price is MORE than MAX_EXTENSION_PCT from VWAP, the "dislocation"
        // is the day's accumulated trend, not a mean-reversion setup.
        // The daily-open VWAP proxy becomes unreliable when price has moved >0.5%
        // -- at that point it's a trend day and fading it consistently loses.
        // Evidence: USTEC at 13:30 was 0.58% above daily open -- every SHORT FC'd.
        if (abs_dev_pct > MAX_EXTENSION_PCT) {
            static thread_local int64_t s_ext_log = 0;
            if (ca_now_sec() - s_ext_log >= 300) {
                s_ext_log = ca_now_sec();
                printf("[VWAP-REV] %s extension %.3f%% > max %.3f%% -- stale VWAP, skip\n",
                       sym.c_str(), abs_dev_pct, MAX_EXTENSION_PCT);
                fflush(stdout);
            }
            return {};
        }

        // Update momentum window
        mid_window_[mid_window_pos_] = mid;
        mid_window_pos_ = (mid_window_pos_ + 1) % TREND_WINDOW;
        if (mid_window_count_ < TREND_WINDOW) ++mid_window_count_;

        // Reversal tick: price ticking back toward VWAP
        if (prev_mid_ <= 0.0) { prev_mid_ = mid; return {}; }
        const bool reversal_tick = above_vwap ? (mid < prev_mid_) : (mid > prev_mid_);
        prev_mid_ = mid;
        if (!reversal_tick) return {};

        // Trend momentum filter: block if still trending away from VWAP
        if (mid_window_count_ >= TREND_WINDOW) {
            const double oldest      = mid_window_[mid_window_pos_];
            const double trend_move  = mid - oldest;
            const double block_thresh = vwap * EXTENSION_THRESH_PCT * 0.5 / 100.0;
            const bool trending_away = above_vwap ? (trend_move > block_thresh)
                                                  : (trend_move < -block_thresh);
            if (trending_away) return {};
        }

        const bool is_long = !above_vwap;

        // ?? Confluence scoring ????????????????????????????????????????????????
        // Score 1-4: each factor adds 1 point to the signal quality.
        // main.cpp uses the score to scale risk: 1=1?, 2=1.5?, 3=2?, 4=3?
        //
        // Factor 1 (always):  base signal -- price extended beyond threshold + reversal tick
        // Factor 2:           NY/London overlap session (13:30-17:00 UTC)
        //                     Historically the strongest mean-reversion window.
        // Factor 3:           Elevated VIX (> CONF_VIX_THRESH)
        //                     High vol = larger extensions, larger snapbacks.
        // Factor 4:           L2 order book confirms direction
        //                     Bid-heavy (imb > 0.5+thresh) = bullish pressure ? long
        //                     Ask-heavy (imb < 0.5-thresh) = bearish pressure ? short
        int score = 1;
        // Session overlap bonus: NY/London overlap 13:30-17:00 UTC = 5.5-9hrs into session
        // session_min_elapsed is minutes since 08:00 UTC
        if (session_min_elapsed >= (5*60+30) && session_min_elapsed < (9*60)) ++score;
        // VIX bonus
        if (vix > CONF_VIX_THRESH) ++score;
        // L2 directional confirmation
        const double l2_dev = l2_imb - 0.5;
        const bool l2_confirms = is_long  ? (l2_dev >  CONF_L2_THRESH)   // bid-heavy ? long
                                           : (l2_dev < -CONF_L2_THRESH);  // ask-heavy ? short
        if (l2_confirms) ++score;

        // TP = VWAP level (full reversion)
        const double tp      = vwap;
        const double tp_dist = std::fabs(tp - mid);

        // SL = extension ? SL_RATIO past entry (further from VWAP)
        const double extension_abs = std::fabs(mid - vwap);
        const double sl_offset     = extension_abs * EXTENSION_SL_RATIO;
        const double sl = above_vwap ? (mid + sl_offset) : (mid - sl_offset);

        if (tp_dist <= 0.0 || tp_dist < sl_offset * 0.5) return {};

        CrossSignal sig;
        sig.valid            = true;
        sig.is_long          = is_long;
        sig.entry            = mid;
        sig.tp               = tp;
        sig.sl               = sl;
        sig.size             = 0.01;
        sig.symbol           = sym.c_str();
        sig.engine           = "VWAPReversion";
        sig.reason           = is_long ? "VWAP_REV_LONG" : "VWAP_REV_SHORT";
        sig.confluence_score = score;

        pos_.open(sig, spread);
        pos_.allow_tp_extend = false;  // VWAP reversion: close AT VWAP, do not extend past it
                                       // Mean-reversion edge ends when price returns to VWAP
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[VWAP-REV] %s %s vwap=%.4f mid=%.4f dev=%.3f%% tp=%.4f sl=%.4f "
               "score=%d vix=%.1f l2=%.3f\n",
               sym.c_str(), is_long?"LONG":"SHORT",
               vwap, mid, deviation_pct, sig.tp, sig.sl,
               score, vix, l2_imb);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    int64_t open_entry_ts()     const { return pos_.entry_ts;  }  // UTC seconds -- stale-position detection
    void cancel()  noexcept { pos_.reset(); timeout_extended_ = false; }
    void force_close(double bid, double ask, CloseCb on_close) {
        pos_.force_close(bid, ask, on_close);
        timeout_extended_ = false;
    }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void rollback() noexcept { pos_.reset(); timeout_extended_ = false; }

    // Called by main.cpp when a TP_HIT close is confirmed for this engine.
    // Sets a flip cooldown blocking the opposite direction for TP_FLIP_COOLDOWN_SEC.
    // Prevents LONG TP ? immediate SHORT into continued uptrend.
    void notify_tp_hit(bool was_long) noexcept {
        tp_flip_block_long_ = !was_long;  // block the OPPOSITE direction
        tp_flip_until_      = ca_now_sec() + TP_FLIP_COOLDOWN_SEC;
        consec_fc_same_dir_ = 0;          // TP hit = trend day over, reset FC counter
        printf("[VWAP-REV] TP hit (%s) -- blocking %s entries for %ds\n",
               was_long ? "LONG" : "SHORT",
               was_long ? "SHORT" : "LONG",
               TP_FLIP_COOLDOWN_SEC);
        fflush(stdout);
    }

private:
    CrossPosition pos_;
    double  prev_mid_           = 0.0;
    int64_t cooldown_until_     = 0;
    double  ewm_vwap_           = 0.0;  // rolling time-based EWM VWAP
    int64_t last_tick_sec_      = 0;    // wall-clock seconds of last tick (for time-decay)
    bool    timeout_extended_   = false; // true once timeout extension has been used
    // TP flip cooldown -- blocks opposite direction after TP hit
    bool    tp_flip_block_long_ = false;
    int64_t tp_flip_until_      = 0;
    // Consecutive MAE exit tracking -- blocks direction after 2 FCs in a row
    int     consec_fc_same_dir_ = 0;
    bool    last_fc_long_       = false;
    bool    fc_block_long_      = false;
    int64_t fc_block_until_     = 0;
    static constexpr int TREND_WINDOW = 20;
    double  mid_window_[TREND_WINDOW] = {};
    int     mid_window_pos_           = 0;
    int     mid_window_count_         = 0;
};

// =============================================================================
// ENGINE 8 -- TrendPullbackEngine
// =============================================================================
// EMA-9/21/50 trend detection: when all three EMAs are stacked in order
// (EMA9 > EMA21 > EMA50 for uptrend, or reverse for downtrend), the dominant
// trend is confirmed. Pullbacks to the slow EMA (EMA50) with a bounce
// confirmation tick provide high-probability continuation entries.
//
// Signal logic:
//   1. EMA9 > EMA21 > EMA50 (uptrend) OR EMA9 < EMA21 < EMA50 (downtrend)
//   2. Price pulls back to EMA50 ? PULLBACK_BAND_PCT
//   3. Price then bounces back toward trend direction (one confirmation tick)
//   4. Enter in trend direction
//   5. TP = EMA9 level (trend continuation target -- price already was there)
//   6. SL = EMA50 (if price breaches EMA50 on close, trend is invalidated)
//
// EMA update: computed in on_tick() from each new mid price.
// Warm-up: requires EMA_WARMUP_TICKS before any signal (EMA must be stable).
//
// Session: London/NY (08:00-22:00 UTC) -- no trend pullbacks in dead Asian hours.
//
// Cost check: TP distance (EMA9 - entry) must cover execution floor.
// =============================================================================
class TrendPullbackEngine {
public:
    double  PULLBACK_BAND_PCT = 0.15;    // price within 0.15% of EMA50 = "at EMA50"
                                          // widened from 0.05%: tick EMA50 drifts faster than bar EMA50
    double  MIN_EMA_SEP       = 10.0;   // minimum EMA9-EMA50 separation to confirm real trend
                                         // Gold: 5pt, SP500: 15pt, USTEC: 25pt -- set per-symbol in main.cpp
    // H4 directional gate: +1=uptrend (longs only), -1=downtrend (shorts only), 0=flat (both)
    // Eliminates counter-trend M15 entries -- primary mechanism for reducing losses.
    // Set via seed_h4_trend() each tick when g_bars_gold.h4 is ready.
    bool    H4_GATE_ENABLED   = false;  // enable per-symbol in main.cpp (gold only initially)
    // ATR-scaled SL floor: replaces fixed 8pt. SL = max(EMA50_dist, ATR_SL_MULT * ATR).
    // Adapts to volatility: quiet (ATR=4pt)->floor=4.8pt, volatile (ATR=12pt)->floor=14.4pt.
    double  ATR_SL_MULT       = 1.2;   // 1.2x ATR SL floor -- set per-symbol in main.cpp

    // ── Improvement 1: Volatility regime sizing scalar ────────────────────────
    // When M15 ATR > VOL_SCALE_HIGH_MULT * avg_atr20 → cut size by VOL_SCALE_CUT
    // When M15 ATR < VOL_SCALE_LOW_MULT  * avg_atr20 → boost size by VOL_SCALE_BOOST
    // Disabled when VOL_SCALE_HIGH_MULT=0.
    double  VOL_SCALE_HIGH_MULT = 1.5;   // ATR > 1.5x avg → volatile regime
    double  VOL_SCALE_LOW_MULT  = 0.7;   // ATR < 0.7x avg → quiet regime
    double  VOL_SCALE_CUT       = 0.60;  // size multiplier in volatile regime
    double  VOL_SCALE_BOOST     = 1.20;  // size multiplier in quiet regime

    // ── Improvement 2: Per-engine daily loss cap ──────────────────────────────
    double  DAILY_LOSS_CAP      = 0.0;   // max daily loss in dollars; 0=disabled
                                          // set per-symbol in main.cpp

    // ── Improvement 4: Time-of-day weighting ─────────────────────────────────
    // Best gold M15 windows: London open (07:00-09:30 UTC) and NY open (13:30-15:00 UTC)
    // Mid-session (10:00-13:30 UTC): false pullbacks -- half size
    // Outside preferred windows: full size allowed (session gate already limits hours)
    bool    TOD_WEIGHT_ENABLED  = false; // enable per-symbol; gold=true in main.cpp

    // ── Improvement 5: CVD confirmation gate ─────────────────────────────────
    // Require CVD direction agrees with entry direction.
    // cvd_dir: +1=buy pressure, -1=sell pressure, 0=neutral (permissive)
    // Set via seed_cvd() each tick.
    bool    CVD_GATE_ENABLED    = false; // enable per-symbol; gold=true in main.cpp

    // ── Improvement 7: News proximity SL widening ─────────────────────────────
    // When a news event is <NEWS_WARN_SECS away, widen the SL by NEWS_SL_MULT.
    // Prevents being stopped out by the pre-event spike before the real move.
    int64_t NEWS_WARN_SECS      = 900;  // 15 minutes before event
    double  NEWS_SL_MULT        = 1.5;  // widen SL 50% when news imminent

    // ── Improvement 8: Pyramiding on second pullback ──────────────────────────
    bool    PYRAMID_ENABLED     = false; // enable per-symbol; gold=true in main.cpp
    double  PYRAMID_SIZE_MULT   = 0.5;  // add-on = 50% of original size
    int     PYRAMID_MAX_ADDS    = 1;    // max 1 pyramid add-on

    bool    shadow_mode         = true; // default true = log only, no live orders (Class C added 2026-04-21)
    // EMA alphas calibrated for ~10 ticks/sec (London gold rate).
    // Using tick-count periods directly produced EMAs with half-life <1s
    // (EMA9 at alpha=0.2 = 3-tick half-life = 0.3s) -- flipping trend
    // direction every few ticks and firing opposite-direction entries.
    // Now calibrated to time-equivalent periods:
    //   EMA9  ? 9s  (N=90  ticks @ 10/s)
    //   EMA21 ? 21s (N=210 ticks @ 10/s)
    //   EMA50 ? 50s (N=500 ticks @ 10/s)
    double  EMA9_ALPHA        = 2.0 / (90.0  + 1.0);
    double  EMA21_ALPHA       = 2.0 / (210.0 + 1.0);
    double  EMA50_ALPHA       = 2.0 / (500.0 + 1.0);
    double  ATR_ALPHA         = 2.0 / (140.0 + 1.0); // ATR-14 equivalent in ticks
    int     EMA_WARMUP_TICKS  = 500;    // ticks before EMAs are trusted (EMA50=500tick period)
    int     MAX_HOLD_SEC      = 86400;  // no timeout -- ATR trail manages exit
    int     COOLDOWN_SEC      = 300;  // raised 120?300: 2-min re-entries on thin tape caused direction-flip losses
    bool    enabled           = true;
    // ATR trail params (data-validated on gold: 2x ATR arm, 1x ATR trail)
    double  TRAIL_ARM_ATR_MULT  = 2.0;  // arm trail after price moves 2x ATR from entry
    double  TRAIL_DIST_ATR_MULT = 1.0;  // trail SL 1x ATR behind peak MFE
    double  BE_ATR_MULT         = 1.0;  // lock BE after 1x ATR profit

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        CloseCb on_close) noexcept {
        if (!enabled || bid <= 0 || ask <= 0) return {};
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // EMA + ATR update -- always runs regardless of position state
        if (ema9_  <= 0.0) { ema9_ = ema21_ = ema50_ = mid; prev_mid_ = mid; }
        const double tick_move = std::fabs(mid - prev_mid_);
        // When bar EMAs are injected (m_using_bar_emas_=true), skip tick EMA update.
        // Bar EMAs from real M1 closes are far more accurate -- don't pollute with ticks.
        // ATR still updates from ticks for intra-bar SL sizing precision.
        if (!m_using_bar_emas_) {
            if (atr_ <= 0.0) atr_ = tick_move > 0 ? tick_move : 0.5;
            else             atr_ += ATR_ALPHA * (tick_move - atr_);
            ema9_  += EMA9_ALPHA  * (mid - ema9_);
            ema21_ += EMA21_ALPHA * (mid - ema21_);
            ema50_ += EMA50_ALPHA * (mid - ema50_);
        } else {
            // Bar EMA mode: only update ATR from ticks (intra-bar precision)
            if (atr_ > 0.0) atr_ += ATR_ALPHA * (tick_move - atr_);
        }
        prev_mid_ = mid;
        ++tick_count_;

        if (pos_.active) {
            // ?? ATR-based trail -- replaces old TP-extension trail ?????????????
            // BE lock: after 1x ATR profit, move SL to entry
            // Trail arm: after 2x ATR profit, trail SL at 1x ATR behind peak MFE
            // EMA50 floor: SL never falls below EMA50 (trend invalidation level)
            const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
            if (pos_.mfe < move) pos_.mfe = move;  // track MFE manually
            const double atr = atr_ > 0.01 ? atr_ : 0.5; // safety floor

            // 1) BE lock at 1x ATR (floor 3pt -- ATR=0 must not lock BE instantly)
            const double be_arm_thresh = std::max(atr * BE_ATR_MULT, 3.0);
            if (!be_locked_ && move >= be_arm_thresh) {
                be_locked_ = true;
                const double be_sl = pos_.is_long ? pos_.entry + spread
                                                  : pos_.entry - spread;
                if (pos_.is_long  && be_sl > pos_.sl) pos_.sl = be_sl;
                if (!pos_.is_long && be_sl < pos_.sl) pos_.sl = be_sl;
                printf("[TREND-PB] %s BE locked atr=%.3f entry=%.3f sl=%.3f\n",
                       sym.c_str(), atr, pos_.entry, pos_.sl);
                fflush(stdout);
            }

            // ── Improvement 6: Partial exit at 1R ────────────────────────────────
            // Take 50% off when price reaches 1x TP distance (1R).
            // Locks profit on half; lets remaining 50% ride the trail.
            // Only fires once per trade (partial_done_ flag).
            if (!partial_done_ && move >= std::fabs(pos_.tp - pos_.entry) * 0.95) {
                const double exit_px = pos_.is_long ? bid : ask;
                partial_exit(exit_px, 0.5, sym, "PARTIAL_1R");
                // Tighten trail after partial -- protect the locked portion
                // Move SL to breakeven + 25% of initial TP dist immediately
                const double be_plus = pos_.is_long
                    ? pos_.entry + std::fabs(pos_.tp - pos_.entry) * 0.25
                    : pos_.entry - std::fabs(pos_.tp - pos_.entry) * 0.25;
                if ((pos_.is_long && be_plus > pos_.sl) || (!pos_.is_long && be_plus < pos_.sl))
                    pos_.sl = be_plus;
            }

            // 2) ATR trail arm at 2x ATR -- trail SL at 1x ATR behind peak
            // FLOOR: minimum arm threshold and trail distance so ATR=0 (unseeded)
            // doesn't cause immediate arm + zero-buffer exit at MFE peak.
            // On cold start (ATR not seeded from bars): atr_≈0 -> trail arms
            // on first tick of profit and SL placed exactly at MFE price.
            // Any reversal tick exits immediately = misses the full move.
            // Fix: floor arm at 5pt, trail dist at 3pt (gold spread noise floor).
            const double trail_arm_thresh = std::max(atr * TRAIL_ARM_ATR_MULT, 5.0);
            const double trail_dist_pts   = std::max(atr * TRAIL_DIST_ATR_MULT, 3.0);
            if (move >= trail_arm_thresh) {
                const double trail_sl = pos_.is_long
                    ? (pos_.entry + pos_.mfe - trail_dist_pts)
                    : (pos_.entry - pos_.mfe + trail_dist_pts);
                if (pos_.is_long  && trail_sl > pos_.sl) pos_.sl = trail_sl;
                if (!pos_.is_long && trail_sl < pos_.sl) pos_.sl = trail_sl;
            }

            // 3) EMA50 floor -- only apply when NOT using bar EMAs
            // With bar EMAs: ema50_ is the 50-min average = approximately the entry level.
            // Using it as SL floor causes instant stops (SL=entry or above).
            // With tick EMAs (fallback): ema50 updates fast enough to be a valid floor.
            if (!m_using_bar_emas_) {
                if (pos_.is_long  && ema50_ > pos_.sl) pos_.sl = ema50_;
                if (!pos_.is_long && ema50_ < pos_.sl) pos_.sl = ema50_;
            }

            // ?? Immediate reversal guard (M15 scale) ??????????????????????????
            // If within first 60s price moves >3pts adverse with zero MFE:
            // entry was wrong from the start -- close before the full SL fires.
            // Scaled larger than GoldFlow (2pt/15s) because M15 EMAs need more
            // breathing room -- spread noise on gold is ~0.35pt.
            {
                const int64_t held_s  = ca_now_sec() - pos_.entry_ts;
                const double  adverse = pos_.is_long ? (pos_.entry - mid)
                                                     : (mid - pos_.entry);
                if (held_s <= 60 && adverse > 3.0 && pos_.mfe < 0.20) {
                    printf("[TREND-PB] %s IMM-REVERSAL adverse=%.2f in %llds mfe=%.2f -- bail\n",
                           sym.c_str(), adverse, (long long)held_s, pos_.mfe);
                    fflush(stdout);
                    const double exit_px = pos_.is_long ? bid : ask;
                    omega::TradeRecord tr;
                    tr.symbol = sym; tr.side = pos_.is_long?"LONG":"SHORT";
                    tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
                    tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.size;
                    tr.mfe=pos_.mfe; tr.mae=adverse;
                    tr.entryTs=pos_.entry_ts; tr.exitTs=ca_now_sec();
                    tr.exitReason="IMM_REVERSAL"; tr.engine="TrendPullback";
                    tr.spreadAtEntry=pos_.spread_at_entry;
                    tr.shadow=shadow_mode;
                    pos_.reset(); be_locked_=false; prev_at_ema50_=false;
                    cooldown_until_=ca_now_sec()+COOLDOWN_SEC;
                    if (on_close) on_close(tr);
                    return {};
                }
            }

            // ?? Time-stop-in-loss (M15 scale) ?????????????????????????????????????????
            // If losing >2pts after 5 minutes with MFE <1pt: thesis is dead.
            // M15 swing should show SOME movement in 5 minutes. If it hasn't,
            // the EMA50 level is not holding and the trend has reversed.
            // Only fires before BE is locked -- once BE locked the trail manages exit.
            {
                const int64_t held_s  = ca_now_sec() - pos_.entry_ts;
                const double  adverse = pos_.is_long ? (pos_.entry - mid)
                                                     : (mid - pos_.entry);
                if (!be_locked_
                    && held_s > 300        // 5 minutes
                    && adverse > 2.0       // losing >2pts
                    && pos_.mfe < 1.0) {   // never went 1pt in our favour
                    printf("[TREND-PB] %s TIME-STOP adverse=%.2f held=%llds mfe=%.2f -- dead thesis\n",
                           sym.c_str(), adverse, (long long)held_s, pos_.mfe);
                    fflush(stdout);
                    const double exit_px = pos_.is_long ? bid : ask;
                    omega::TradeRecord tr;
                    tr.symbol = sym; tr.side = pos_.is_long?"LONG":"SHORT";
                    tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
                    tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.size;
                    tr.mfe=pos_.mfe; tr.mae=adverse;
                    tr.entryTs=pos_.entry_ts; tr.exitTs=ca_now_sec();
                    tr.exitReason="TIME_STOP"; tr.engine="TrendPullback";
                    tr.spreadAtEntry=pos_.spread_at_entry;
                    tr.shadow=shadow_mode;
                    pos_.reset(); be_locked_=false; prev_at_ema50_=false;
                    cooldown_until_=ca_now_sec()+COOLDOWN_SEC;
                    if (on_close) on_close(tr);
                    return {};
                }
            }

            // 15-minute hard stop: if still losing after 15 min regardless of MFE,
            // the thesis is dead. The 5-min stop only fires when mfe<1pt.
            // This catches trades that showed brief profit early then reversed.
            // Only fires before BE is locked -- trail manages exit once BE locked.
            {
                const int64_t held_s2 = ca_now_sec() - pos_.entry_ts;
                const double adverse2 = pos_.is_long ? (pos_.entry - mid)
                                                     : (mid - pos_.entry);
                if (!be_locked_
                    && held_s2 > 600       // 10 minutes (was 15 -- too long to hold loser)
                    && adverse2 > pos_.spread_at_entry * 2.0) {  // meaningful loss beyond spread noise
                    printf("[TREND-PB] %s HARD-TIME-STOP held=%llds adverse=%.2f mfe=%.2f -- 10min no BE\n",
                           sym.c_str(), (long long)held_s2, adverse2, pos_.mfe);
                    fflush(stdout);
                    const double exit_px = pos_.is_long ? bid : ask;
                    omega::TradeRecord tr;
                    tr.symbol = sym; tr.side = pos_.is_long?"LONG":"SHORT";
                    tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
                    tr.tp=pos_.tp; tr.sl=pos_.sl; tr.size=pos_.size;
                    tr.mfe=pos_.mfe; tr.mae=adverse2;
                    tr.entryTs=pos_.entry_ts; tr.exitTs=ca_now_sec();
                    tr.exitReason="TIME_STOP"; tr.engine="TrendPullback";
                    tr.spreadAtEntry=pos_.spread_at_entry;
                    tr.shadow=shadow_mode;
                    pos_.reset(); be_locked_=false; prev_at_ema50_=false;
                    cooldown_until_=ca_now_sec()+COOLDOWN_SEC;
                    if (on_close) on_close(tr);
                    return {};
                }
            }

            // Check SL / timeout
            // Minimum hold: don't check SL for first 2s -- prevents entry-tick exits
            // caused by spread noise or stale EMA50 floor firing immediately
            const int64_t held_s = ca_now_sec() - pos_.entry_ts;
            const bool sl_hit    = (held_s >= 2) &&
                                   (pos_.is_long ? (bid <= pos_.sl) : (ask >= pos_.sl));
            const bool timed_out = held_s >= MAX_HOLD_SEC;
            if (sl_hit || timed_out) {
                const double exit_px = sl_hit ? pos_.sl : mid;
                const char*  reason  = sl_hit ? "SL_HIT" : "TIMEOUT";
                omega::TradeRecord tr;
                tr.symbol     = sym;
                tr.side       = pos_.is_long ? "LONG" : "SHORT";
                tr.entryPrice = pos_.entry;
                tr.exitPrice  = exit_px;
                tr.tp         = pos_.tp;
                tr.sl         = pos_.sl;
                tr.size       = pos_.size;
                tr.mfe        = pos_.mfe;
                tr.mae        = pos_.mae;
                tr.entryTs    = pos_.entry_ts;
                tr.exitTs     = ca_now_sec();
                tr.exitReason = reason;
                tr.engine     = "TrendPullback";
                tr.spreadAtEntry = pos_.spread_at_entry;
                tr.shadow     = shadow_mode;
                record_daily_pnl(tr.pnl);  // track for daily cap
                // Full symbol?tick_value table -- must match tick_value_multiplier() in main.cpp
                // XAUUSD=100, US500.F=50, USTEC.F=20, DJ30.F=5, GER40=1.10, UK100=1.33,
                // ESTX50=1.10, NAS100=1, EURUSD/GBPUSD/etc=100000, others=1
                // P&L in raw price-points * size only.
                // handle_closed_trade() applies tick_value_multiplier() once to convert
                // to USD. Do NOT multiply by tick_val here -- double-counting caused
                // 100x inflation ($8.44 displayed as $844 for XAUUSD).
                tr.pnl = (pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px))
                         * pos_.size;
                tr.net_pnl = tr.pnl;
                printf("[TREND-PB] %s %s CLOSE @%.3f reason=%s pnl=%.2f atr=%.3f trail_sl=%.3f\n",
                       sym.c_str(), tr.side.c_str(), exit_px, reason, tr.pnl, atr, pos_.sl);
                fflush(stdout);
                // Track consecutive SL hits for direction block
                if (std::strcmp(reason, "SL_HIT") == 0) {
                    if (pos_.is_long) {
                        ++m_consec_sl_long_;
                        m_consec_sl_short_ = 0;
                        if (m_consec_sl_long_ >= 2) {
                            m_long_blocked_until_ = ca_now_sec() + 600; // 10 min block
                            m_consec_sl_long_ = 0;
                            printf("[TREND-PB] %s LONG blocked 10min after 2 consec SL hits\n", sym.c_str());
                            fflush(stdout);
                        }
                    } else {
                        ++m_consec_sl_short_;
                        m_consec_sl_long_ = 0;
                        if (m_consec_sl_short_ >= 2) {
                            m_short_blocked_until_ = ca_now_sec() + 600;
                            m_consec_sl_short_ = 0;
                            printf("[TREND-PB] %s SHORT blocked 10min after 2 consec SL hits\n", sym.c_str());
                            fflush(stdout);
                        }
                    }
                } else {
                    // Profitable exit -- reset consecutive SL counters
                    m_consec_sl_long_ = 0;
                    m_consec_sl_short_ = 0;
                }
                pos_.reset();
                be_locked_    = false;
                partial_done_ = false;
                pyramid_adds_ = 0;
                prev_at_ema50_ = false;  // reset confirmation state on close
                cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
                if (on_close) on_close(tr);
            }
            return {};
        }

        if (tick_count_ < EMA_WARMUP_TICKS) return {};

        // Session gate: indices only (thin tape post-London)
        // Gold trades 23h/day -- trend is a trend regardless of session.
        // H4 gate, CVD gate, EMA separation and MIN_EMA_SEP already filter noise.
        // Removing the 08:00 gate means Asian trends like the 4580->4800 move are caught.
        struct tm ti{}; ca_utc_time(ti);
        const int h = ti.tm_hour;
        const bool is_gold = (sym.find("XAU") != std::string::npos);
        if (!is_gold) {
            // Indices: London open to post-NY close only (08:00-20:00 UTC)
            if (h < 8 || h >= 20) return {};
        }
        // Gold: no session gate -- 24h trend following

        if (ca_now_sec() < cooldown_until_) return {};

        // Direction block: after 2 consecutive SL hits in same direction,
        // block that direction for 10 minutes -- market is not trending that way
        const int64_t now_cs = ca_now_sec();
        const bool long_dir_blocked  = (now_cs < m_long_blocked_until_);
        const bool short_dir_blocked = (now_cs < m_short_blocked_until_);

        // Trend detection -- all three EMAs must be stacked
        const bool uptrend   = (ema9_ > ema21_) && (ema21_ > ema50_);
        const bool downtrend = (ema9_ < ema21_) && (ema21_ < ema50_);
        if (!uptrend && !downtrend) return {};

        // Minimum EMA separation -- stack must be meaningfully separated.
        // EMA9-EMA50 < min_sep means EMAs are flat/noise, not a real trend.
        // Gold: require 5pt separation (typical clean trend = 15-40pt).
        // Indices: require 10pt separation.
        // Without this, a shallow stack from stale/misaligned seed data fires bad trades.
        const double ema_span = std::fabs(ema9_ - ema50_);
        if (ema_span < MIN_EMA_SEP) return {};
        if (!uptrend && !downtrend) return {};

        // M5 structural trend gate -- when bar EMAs active, require M5 agrees
        // Prevents LONG during confirmed M5 downtrend and vice versa
        // m5_trend_state_=0 (flat/unknown) allows signals from EMA stack alone
        if (m_using_bar_emas_ && m5_trend_state_ != 0) {
            if (uptrend   && m5_trend_state_ < 0) return {};
            if (downtrend && m5_trend_state_ > 0) return {};
        }

        // Pullback detection: price within band of EMA50
        // Widened from 0.05% to 0.15% -- tick EMA50 drifts faster than bar EMA50
        // so price appears "always near" EMA50 with a tight band.
        const double band = mid * PULLBACK_BAND_PCT / 100.0;
        const bool at_ema50 = std::fabs(mid - ema50_) < band;
        if (!at_ema50) {
            prev_at_ema50_ = false;
            return {};
        }

        // Bounce confirmation: prev tick was also at EMA50, now ticking away in trend direction
        if (!prev_at_ema50_) {
            prev_at_ema50_ = true;
            return {};
        }
        const bool bounce_up   = uptrend   && (mid > ema50_);
        const bool bounce_down = downtrend && (mid < ema50_);
        if (!bounce_up && !bounce_down) return {};

        const bool is_long = uptrend;

        // Apply direction block (fixed: was checking wrong direction)
        if (is_long  && long_dir_blocked)  return {};
        if (!is_long && short_dir_blocked) return {};

        // H4 directional gate: only trade in HTF trend direction
        // h4_trend_state_: +1=uptrend, -1=downtrend, 0=flat/unknown
        // When H4_GATE_ENABLED and H4 trend is known, block counter-trend entries.
        // 0 (flat/not enough bars) = permissive -- both directions allowed.
        if (H4_GATE_ENABLED && h4_trend_state_ != 0) {
            if (is_long  && h4_trend_state_ < 0) {
                printf("[TREND-PB] %s LONG blocked by H4 downtrend\n", sym.c_str());
                fflush(stdout);
                return {};
            }
            if (!is_long && h4_trend_state_ > 0) {
                printf("[TREND-PB] %s SHORT blocked by H4 uptrend\n", sym.c_str());
                fflush(stdout);
                return {};
            }
        }

        // ── Improvement 2: Daily loss cap ────────────────────────────────────
        // Reset at midnight. If today's P&L is worse than -DAILY_LOSS_CAP, stop.
        if (DAILY_LOSS_CAP > 0.0) {
            const int64_t today = static_cast<int64_t>(std::time(nullptr)) / 86400;
            if (today != daily_pnl_day_) { daily_pnl_ = 0.0; daily_pnl_day_ = today; }
            if (daily_pnl_ <= -DAILY_LOSS_CAP) {
                printf("[TREND-PB] %s DAILY_CAP hit pnl=%.2f cap=%.2f -- no more entries today\n",
                       sym.c_str(), daily_pnl_, DAILY_LOSS_CAP);
                fflush(stdout);
                return {};
            }
        }

        // ── Improvement 4: Time-of-day weighting ─────────────────────────────
        // Gold 24h: Asian trending hours get full size when trend is clear.
        // Prime: London open 07-09:30, NY open 13:30-15:00, Asian trend 00:00-07:00
        // Reduced: Mid-session 10:00-13:30 (positioning noise)
        double tod_size_mult = 1.0;
        if (TOD_WEIGHT_ENABLED) {
            struct tm tod_ti{}; ca_utc_time(tod_ti);
            const int hmins = tod_ti.tm_hour * 60 + tod_ti.tm_min;
            const bool prime    = (hmins >= 420 && hmins < 570)   // 07:00-09:30 London open
                               || (hmins >= 810 && hmins < 900)   // 13:30-15:00 NY open
                               || (hmins <  420);                  // 00:00-07:00 Asian trend
            const bool mid_sess = (hmins >= 600 && hmins < 810);  // 10:00-13:30 thin tape
            tod_size_mult = prime ? 1.0 : mid_sess ? 0.5 : 0.8;
        }

        // ── Improvement 5: CVD confirmation gate ─────────────────────────────
        // CVD must agree with entry direction. Neutral (0) = permissive.
        if (CVD_GATE_ENABLED && cvd_dir_ != 0) {
            if (is_long  && cvd_dir_ < 0) {
                printf("[TREND-PB] %s LONG blocked by CVD sell pressure\n", sym.c_str());
                fflush(stdout);
                return {};
            }
            if (!is_long && cvd_dir_ > 0) {
                printf("[TREND-PB] %s SHORT blocked by CVD buy pressure\n", sym.c_str());
                fflush(stdout);
                return {};
            }
        }

        // ── Improvement 1: Volatility regime size scalar ──────────────────────
        double vol_size_mult = 1.0;
        if (VOL_SCALE_HIGH_MULT > 0.0 && avg_atr20_ > 0.0 && atr_ > 0.0) {
            if      (atr_ > avg_atr20_ * VOL_SCALE_HIGH_MULT) vol_size_mult = VOL_SCALE_CUT;
            else if (atr_ < avg_atr20_ * VOL_SCALE_LOW_MULT)  vol_size_mult = VOL_SCALE_BOOST;
        }
        // Combined size multiplier (applied after compute_size in main.cpp via patch_size)
        const double size_mult = tod_size_mult * vol_size_mult;

        // SL placed PAST EMA50 with ATR buffer -- not AT the EMA50 level.
        //
        // BUG (fixed): previous code set sl_dist = max(|mid-ema50|, atr_floor).
        // When price is AT EMA50 (which is the entry trigger), |mid-ema50| is tiny.
        // If atr_ is also small (tick ATR during flat tape), sl_dist = tiny -> instant SL hit.
        // Example: entry=4778.62 SHORT, ema50=4783.20 -> sl_dist=4.58pt -> SL=4783.20
        // Gold wicks +4.58pt (normal noise), SL hit. Then continues -134pt. Wasted.
        //
        // FIX: sl_dist = EMA50_distance + ATR_buffer
        // SL is placed a buffer PAST EMA50, not at EMA50.
        // Price can touch and briefly pierce EMA50 without stopping out.
        //   ATR=4pt:  buffer=4.8pt -> sl_dist = EMA50_dist + 4.8pt
        //   ATR=8pt:  buffer=9.6pt -> sl_dist = EMA50_dist + 9.6pt
        //   ATR=53pt: buffer=63pt  -> sl_dist = EMA50_dist + 63pt (crash day)
        // Minimum buffer: max(atr*ATR_SL_MULT, spread*3, 5.0)
        const double sl_raw      = ema50_;
        const double atr_floor   = (atr_ > 0.5) ? atr_ * ATR_SL_MULT : 5.0;
        const double spread_floor = spread * 3.0;
        const double sl_buffer   = std::max({atr_floor, spread_floor, 5.0});
        const double sl_raw_dist = std::fabs(mid - sl_raw);
        const double sl_dist     = sl_raw_dist + sl_buffer;  // PAST EMA50, not AT it

        // TP = ATR-based fixed distance: 2.5x ATR for gold (~20-25pts typical)
        const double atr_safe  = atr_ > 2.0 ? atr_ : 10.0;  // floor at 10pts
        const double tp_dist   = std::max(atr_safe * 2.5, sl_dist * 2.0);  // min 2:1 R:R
        const double tp = is_long ? (mid + tp_dist) : (mid - tp_dist);

        if (tp_dist <= 0.0 || sl_dist <= 0.0) return {};
        if (sl_dist < spread * 1.5) return {};

        // ── Improvement 7: News proximity SL widening ─────────────────────────
        // Widen SL when a major event is <NEWS_WARN_SECS away.
        // Avoids being stopped out by pre-event spike before the real move.
        double news_sl_mult = 1.0;
        if (news_secs_until_ < NEWS_WARN_SECS) {
            news_sl_mult = NEWS_SL_MULT;
            printf("[TREND-PB] %s NEWS imminent in %llds -- SL widened x%.1f\n",
                   sym.c_str(), (long long)news_secs_until_, NEWS_SL_MULT);
            fflush(stdout);
        }
        const double final_sl_dist = sl_dist * news_sl_mult;
        const double final_sl = is_long ? (mid - final_sl_dist) : (mid + final_sl_dist);

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = final_sl;
        sig.size    = 0.01 * size_mult;  // vol+TOD scalar baked in; main.cpp may override
        sig.symbol  = sym.c_str();
        sig.engine  = "TrendPullback";
        sig.reason  = is_long ? "TREND_PB_LONG" : "TREND_PB_SHORT";

        pos_.open(sig, spread);
        be_locked_      = false;
        partial_done_   = false;
        pyramid_adds_   = 0;
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;
        printf("[TREND-PB] %s %s ema9=%.4f ema21=%.4f ema50=%.4f entry=%.4f sl=%.4f atr=%.3f"
               " size_mult=%.2f news_in=%llds\n",
               sym.c_str(), is_long?"LONG":"SHORT",
               ema9_, ema21_, ema50_, mid, final_sl, atr_,
               size_mult, (long long)news_secs_until_);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const { return pos_.active; }
    int     pyramid_adds_   = 0;     // public: read/written by main.cpp pyramid dispatch
    void cancel() noexcept { pos_.reset(); be_locked_ = false; }
    void force_close(double bid, double ask, CloseCb on_close) { pos_.force_close(bid, ask, on_close); be_locked_ = false; }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void rollback() noexcept { pos_.reset(); be_locked_ = false; }
    // Reset cooldown immediately -- used by crash override in main.cpp
    // to bypass re-entry delay when gold is in a strong directional move.
    void force_cooldown_expire() noexcept { cooldown_until_ = 0; }
    // Live position accessors for GUI telemetry
    bool    open_is_long()   const { return pos_.is_long;   }
    double  open_entry()     const { return pos_.entry;     }
    double  open_sl()        const { return pos_.sl;        }
    double  open_tp()        const { return pos_.tp;        }
    double  open_size()      const { return pos_.size;      }
    int64_t open_entry_ts()  const { return pos_.entry_ts;  }  // UTC seconds -- used for stale-position detection
    double  current_atr()    const { return atr_;           }

    // ?? Bar EMA injection -- replaces tick EMAs with real M1 bar closes ???????
    // Called each tick from main.cpp when g_bars_sp/nq/ger are ready.
    // Bar EMAs (computed from 200 M1 OHLC closes) are far more accurate than
    // tick-based EMAs which had half-life of 0.3s at 10 ticks/sec.
    // Also seeds ATR from bar ATR14 -- true range is better than tick range.
    void seed_bar_emas(double e9, double e21, double e50, double bar_atr) noexcept {
        if (e9 <= 0.0 || e50 <= 0.0) return;
        ema9_  = e9;
        ema21_ = e21;
        ema50_ = e50;
        if (bar_atr > 0.0) atr_ = bar_atr;
        // Mark as warmed -- bar data is always valid after m1_ready
        if (tick_count_ < EMA_WARMUP_TICKS) tick_count_ = EMA_WARMUP_TICKS;
        m_using_bar_emas_ = true;
    }

    // Seed M5 structural trend -- gates signal direction
    void seed_m5_trend(int trend_state) noexcept { m5_trend_state_ = trend_state; }
    void seed_h4_trend(int trend_state) noexcept { h4_trend_state_ = trend_state; }
    void seed_cvd(int cvd_dir) noexcept { cvd_dir_ = cvd_dir; }
    void seed_vol_atr_avg(double avg_atr) noexcept { avg_atr20_ = avg_atr; }
    void seed_news_secs(int64_t secs_until) noexcept { news_secs_until_ = secs_until; }

    // Record a closed trade P&L for daily cap tracking
    void record_daily_pnl(double pnl) noexcept {
        daily_pnl_ += pnl;
        // Reset at UTC midnight
        const int64_t today = static_cast<int64_t>(std::time(nullptr)) / 86400;
        if (today != daily_pnl_day_) { daily_pnl_ = pnl; daily_pnl_day_ = today; }
    }
    double daily_pnl() const noexcept { return daily_pnl_; }

    // Partial exit: close fraction of position, reduce size
    // Returns PnL of the partial close
    double partial_exit(double exit_px, double fraction, const std::string& sym,
                        const char* reason) noexcept {
        if (!pos_.active || fraction <= 0.0 || fraction >= 1.0) return 0.0;
        const double close_size = pos_.size * fraction;
        const double rem_size   = pos_.size - close_size;
        // No tick_val here -- handle_closed_trade applies tick_value_multiplier() once.
        const double pnl = (pos_.is_long ? (exit_px - pos_.entry) : (pos_.entry - exit_px))
                           * close_size;
        printf("[TREND-PB] %s PARTIAL EXIT %.0f%% @%.3f reason=%s pnl=%.2f remaining=%.4f\n",
               sym.c_str(), fraction*100, exit_px, reason, pnl, rem_size);
        fflush(stdout);
        record_daily_pnl(pnl);
        partial_done_ = true;
        pos_.size = rem_size;  // shrink live position size
        return pnl;
    }

    bool using_bar_emas() const { return m_using_bar_emas_; }

    // Expose EMAs for telemetry / external inspection
    double ema9()  const { return ema9_;  }
    double ema21() const { return ema21_; }
    double ema50() const { return ema50_; }

    // ?? Warm-restart persistence ?????????????????????????????????????????????
    // Saves EMA9/21/50 + ATR so next restart skips EMA_WARMUP_TICKS cold period.
    // State is discarded if older than 4 hours (overnight gap / weekend).
    void save_state(const std::string& path) const noexcept {
        if (tick_count_ < EMA_WARMUP_TICKS) return;  // don't save cold state
        FILE* fp = fopen(path.c_str(), "w");
        if (!fp) return;
        fprintf(fp, "ema9=%.6f\n",    ema9_);
        fprintf(fp, "ema21=%.6f\n",   ema21_);
        fprintf(fp, "ema50=%.6f\n",   ema50_);
        fprintf(fp, "atr=%.6f\n",     atr_);
        fprintf(fp, "saved_ts=%lld\n", (long long)std::time(nullptr));
        fclose(fp);
    }

    void load_state(const std::string& path) noexcept {
        if (has_open_position()) return;  // never overwrite live position state
        FILE* fp = fopen(path.c_str(), "r");
        if (!fp) return;
        char line[128];
        double e9=0, e21=0, e50=0, atr=0;
        int64_t saved_ts = 0;
        while (fgets(line, sizeof(line), fp)) {
            char key[32]; double val = 0.0;
            if (sscanf(line, "%31[^=]=%lf", key, &val) != 2) continue;
            const std::string k(key);
            if      (k == "ema9")     e9       = val;
            else if (k == "ema21")    e21      = val;
            else if (k == "ema50")    e50      = val;
            else if (k == "atr")      atr      = val;
            else if (k == "saved_ts") saved_ts = static_cast<int64_t>(val);
        }
        fclose(fp);
        const int64_t now_ts = static_cast<int64_t>(std::time(nullptr));
        const int64_t age    = now_ts - saved_ts;
        // Delete and reject if: timestamp invalid, too stale (>4hr), or bad EMAs
        if (saved_ts <= 0 || saved_ts > now_ts || age > 4 * 3600 || age < 0
            || e9 <= 0.0 || e50 <= 0.0) {
            remove(path.c_str());  // delete so it doesn't keep reloading corrupt data
            return;
        }
        ema9_       = e9;
        ema21_      = e21;
        ema50_      = e50;
        atr_        = (atr > 0.0) ? atr : atr_;
        tick_count_ = EMA_WARMUP_TICKS;  // mark as warmed -- skip blind zone
    }

private:
    CrossPosition pos_;
    double  ema9_          = 0.0;
    double  ema21_         = 0.0;
    double  ema50_         = 0.0;
    double  atr_           = 0.0;  // EWM ATR-14 for trail sizing
    bool    m_using_bar_emas_ = false;
    int     m5_trend_state_   = 0;     // +1=uptrend, -1=downtrend, 0=flat (from M5 bars)
    int     h4_trend_state_   = 0;     // +1=uptrend, -1=downtrend, 0=flat (from H4 bars -- HTF gate)
    int     cvd_dir_          = 0;     // +1=buy pressure, -1=sell, 0=neutral (from seed_cvd)
    double  avg_atr20_        = 0.0;   // 20-bar rolling ATR average for vol regime scaling
    int64_t news_secs_until_  = INT64_MAX; // seconds until next news event (from seed_news_secs)
    double  daily_pnl_        = 0.0;   // today's net P&L for this engine instance
    int64_t daily_pnl_day_    = 0;     // UTC day of daily_pnl_ (for reset)
    bool    partial_done_     = false; // true after first partial exit this trade
    // Consecutive SL tracker -- block direction after 2 consecutive SL hits
    int     m_consec_sl_long_  = 0;   // consecutive long SL hits
    int     m_consec_sl_short_ = 0;   // consecutive short SL hits
    int64_t m_long_blocked_until_  = 0;  // epoch sec, long blocked after 2 consec SL
    int64_t m_short_blocked_until_ = 0;  // epoch sec, short blocked after 2 consec SL  // true when bar EMAs injected via seed_bar_emas()
    int     tick_count_    = 0;
    bool    prev_at_ema50_ = false;
    double  prev_mid_      = 0.0;
    bool    be_locked_     = false;
    int64_t cooldown_until_ = 0;
};


// =============================================================================
// ENGINE 9 -- NoiseBandMomentumEngine
// =============================================================================
// Highest-Sharpe documented indices strategy in peer-reviewed research.
//
// BASIS: Zarattini, Aziz, Barbon (2024) "Beat the Market: An Effective
//   Intraday Momentum Strategy for S&P500 ETF (SPY)" improved by
//   Maroy (2025) with VWAP+Ladder exit: Sharpe > 3.0, annual > 50%.
//   MQL5 independent implementation: Sharpe 5.9 on USTEC over 5yr.
//
// MECHANISM:
//   Price breaking out of the intraday noise band (rolling ATR from session
//   open, widened by ATR_MULT) signals genuine institutional momentum.
//   The band naturally widens through the day, matching the time-of-day
//   adjustment in the original paper. Entry on band breach. Primary stop:
//   VWAP crossing with tolerance buffer. Secondary: fixed SL_PCT.
//
// REGIME: ALL. Sharpe improves to ~3.5 in VIX>20 environments per research.
// SESSION: 13:30-21:30 UTC (NY session). Supports EU override for GER40.
// COOLDOWN: 600s. Max 1 position per instance.
// INSTRUMENTS: US500.F, USTEC.F, NAS100, DJ30.F (one instance each).
// =============================================================================
class NoiseBandMomentumEngine {
public:
    // Configurable parameters
    int    LOOKBACK_TICKS    = 300;   // ticks for rolling ATR computation
    double ATR_MULT          = 1.5;   // band = session_open +/- ATR_MULT * atr
    double VWAP_STOP_MULT    = 0.5;   // VWAP stop tolerance: 0.5 x band_half
    double TP_PCT            = 0.60;  // TP: 0.60% from entry
    double SL_PCT            = 0.25;  // fallback SL: 0.25% from entry
    int    MAX_HOLD_SEC      = 2700;  // 45 min max hold
    int    COOLDOWN_SEC      = 600;   // 10 min cooldown
    double MAX_SPREAD_PCT    = 0.05;  // block if spread/mid > 0.05%
    double MIN_BAND_PCT      = 0.08;  // min band width (price %) before firing
    double MAX_BAND_PCT      = 2.00;  // cap band (news spike guard)
    int    SESSION_OPEN_UTC  = 13;    // session open hour UTC
    int    SESSION_OPEN_MIN  = 30;    // session open minute UTC
    int    SESSION_CLOSE_UTC = 21;    // force-flat hour UTC
    int    SESSION_CLOSE_MIN = 30;    // force-flat minute UTC
    int    WARMUP_TICKS      = 120;   // ticks before first signal (ATR warmup)
    bool   enabled           = true;

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    CrossSignal on_tick(const std::string& sym, double bid, double ask,
                        CloseCb on_close) noexcept {
        if (!enabled || bid <= 0.0 || ask <= 0.0) return {};
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        if (pos_.active) {
            _manage_position(bid, ask, on_close);
            return {};
        }

        struct tm ti{}; ca_utc_time(ti);
        const int mins = ti.tm_hour * 60 + ti.tm_min;
        const int open_mins  = SESSION_OPEN_UTC  * 60 + SESSION_OPEN_MIN;
        const int close_mins = SESSION_CLOSE_UTC * 60 + SESSION_CLOSE_MIN;

        if (ti.tm_yday != last_day_) {
            _reset_daily(mid);
            last_day_ = ti.tm_yday;
        }

        if (mins < open_mins || mins >= close_mins) return {};

        _update_atr(mid);
        _update_vwap(mid);
        tick_count_++;

        if (tick_count_ < WARMUP_TICKS) return {};
        if (mid > 0.0 && spread / mid * 100.0 > MAX_SPREAD_PCT) return {};
        if (ca_now_sec() < cooldown_until_) return {};
        if (session_open_ <= 0.0) return {};

        const double atr       = _rolling_atr();
        if (atr <= 0.0) return {};
        const double band_half = atr * ATR_MULT;
        const double band_pct  = (mid > 0.0) ? (band_half / mid * 100.0) : 0.0;

        if (band_pct < MIN_BAND_PCT || band_pct > MAX_BAND_PCT) return {};

        const double upper_band = session_open_ + band_half;
        const double lower_band = session_open_ - band_half;

        const bool go_long  = (mid > upper_band);
        const bool go_short = (mid < lower_band);
        if (!go_long && !go_short) return {};

        // VWAP alignment filter: skip if VWAP opposes direction
        if (vwap_ > 0.0 && tick_count_ > WARMUP_TICKS + 60) {
            const double vwap_vs_open = vwap_ - session_open_;
            if (go_long  && vwap_vs_open < -band_half * 0.3) return {};
            if (go_short && vwap_vs_open >  band_half * 0.3) return {};
        }

        const double tp_dist = mid * TP_PCT / 100.0;
        const double sl_dist = mid * SL_PCT / 100.0;
        const double tp = go_long ? mid + tp_dist : mid - tp_dist;
        const double sl = go_long ? mid - sl_dist : mid + sl_dist;

        vwap_at_entry_   = vwap_ > 0.0 ? vwap_ : mid;
        band_half_entry_ = band_half;
        is_long_entry_   = go_long;
        entry_time_      = ca_now_sec();

        CrossSignal sig;
        sig.valid   = true;
        sig.is_long = go_long;
        sig.entry   = go_long ? ask : bid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.size    = 0.01;
        sig.symbol  = sym.c_str();
        sig.engine  = "NoiseBandMomentum";
        sig.reason  = go_long ? "NBM_LONG" : "NBM_SHORT";

        pos_.open(sig, spread);
        cooldown_until_ = ca_now_sec() + COOLDOWN_SEC;

        printf("[NBM-%s] %s open=%.4f band=[%.4f,%.4f] atr=%.4f vwap=%.4f tp=%.4f sl=%.4f\n",
               sym.c_str(), go_long ? "LONG" : "SHORT",
               session_open_, lower_band, upper_band, atr, vwap_, tp, sl);
        fflush(stdout);
        return sig;
    }

    bool has_open_position() const  { return pos_.active;   }
    int64_t open_entry_ts()     const { return pos_.entry_ts;  }  // UTC seconds -- stale-position detection
    void cancel()   noexcept        { pos_.reset();          }
    void rollback() noexcept        { pos_.reset();          }
    void patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void force_close(double bid, double ask, CloseCb on_close) {
        pos_.force_close(bid, ask, on_close);
    }
    // Live position accessors for GUI telemetry
    bool   open_is_long() const { return pos_.is_long; }
    double open_entry()   const { return pos_.entry;   }
    double open_sl()      const { return pos_.sl;      }
    double open_tp()      const { return pos_.tp;      }
    double open_size()    const { return pos_.size;    }

    double session_open_price() const { return session_open_; }
    double current_vwap()       const { return vwap_;         }
    double current_band_half()  const { return band_half_entry_; }

private:
    CrossPosition pos_;
    double   session_open_   = 0.0;
    int      last_day_       = -1;
    int      tick_count_     = 0;

    static constexpr int ATR_BUF_SZ = 600;
    double   atr_buf_[ATR_BUF_SZ]  = {};
    int      atr_head_  = 0;
    int      atr_count_ = 0;
    double   prev_mid_  = 0.0;

    double   vwap_cum_pv_  = 0.0;
    double   vwap_cum_vol_ = 0.0;
    double   vwap_         = 0.0;

    double   vwap_at_entry_   = 0.0;
    double   band_half_entry_ = 0.0;
    bool     is_long_entry_   = true;
    int64_t  entry_time_      = 0;
    int64_t  cooldown_until_  = 0;

    void _reset_daily(double mid) noexcept {
        session_open_   = mid;
        tick_count_     = 0;
        atr_head_       = 0;
        atr_count_      = 0;
        prev_mid_       = 0.0;
        vwap_cum_pv_    = 0.0;
        vwap_cum_vol_   = 0.0;
        vwap_           = mid;
        vwap_at_entry_  = 0.0;
        band_half_entry_= 0.0;
    }

    void _update_atr(double mid) noexcept {
        if (prev_mid_ > 0.0) {
            const double tr = std::fabs(mid - prev_mid_);
            atr_buf_[atr_head_ % ATR_BUF_SZ] = tr;
            atr_head_++;
            if (atr_count_ < LOOKBACK_TICKS) atr_count_++;
        }
        prev_mid_ = mid;
    }

    void _update_vwap(double mid) noexcept {
        vwap_cum_pv_  += mid;
        vwap_cum_vol_ += 1.0;
        if (vwap_cum_vol_ > 0.0)
            vwap_ = vwap_cum_pv_ / vwap_cum_vol_;
    }

    double _rolling_atr() const noexcept {
        if (atr_count_ < 2) return 0.0;
        double sum = 0.0;
        const int n = std::min(atr_count_, LOOKBACK_TICKS);
        for (int i = 0; i < n; ++i)
            sum += atr_buf_[i % ATR_BUF_SZ];
        return sum / n;
    }

    void _manage_position(double bid, double ask, CloseCb on_close) noexcept {
        const double mid = (bid + ask) * 0.5;
        _update_vwap(mid);

        const int64_t age = ca_now_sec() - entry_time_;
        if (age > 60 && vwap_ > 0.0 && band_half_entry_ > 0.0) {
            const double tol = band_half_entry_ * VWAP_STOP_MULT;
            const bool vwap_stop = is_long_entry_
                ? (mid < vwap_ - tol)
                : (mid > vwap_ + tol);
            if (vwap_stop) {
                printf("[NBM] VWAP stop: mid=%.4f vwap=%.4f tol=%.4f\n", mid, vwap_, tol);
                fflush(stdout);
                pos_.force_close(bid, ask, on_close);
                return;
            }
        }
        pos_.manage(bid, ask, MAX_HOLD_SEC, on_close);
    }
};

// -- SilverTurtleTickEngine REMOVED at Batch 5V (2026-04-19) --
// Real-tick backtest on 42M XAGUSD ticks (Jan 2023 - Jan 2025): Sharpe=-16.23,
// MaxDD=$18,381, 0/24 positive months. Root cause: 65% timeout rate; TP=$0.30
// requires 49x the actual 45-min average XAGUSD move.
// See wiki/entities/SilverTurtleTickEngine.md (tombstone) for full historical
// record; wiki/issues.md ISSUE-124 for the header doc-drift closure that
// bundled with this removal. XAGUSD remains hard-blocked at on_tick.hpp:1794.

} // namespace cross
} // namespace omega

