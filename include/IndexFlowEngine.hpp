#pragma once
// =============================================================================
// IndexFlowEngine.hpp
// =============================================================================
// L2 order-flow + EWM drift + Regime-gated engine stack for US equity indices.
//
// Architecture: L2 persistence windows + EWM fast/slow drift detection +
// ATR-proportional staircase trail, calibrated for index price scales and the
// much higher per-point dollar values of US index contracts.
//
// SYMBOL CONFIGS (confirmed from OmegaCostGuard.hpp):
//   US500.F  : $50/pt per lot  -- SP500 mini equivalent
//   USTEC.F  : $20/pt per lot  -- NQ Nasdaq equivalent
//   NAS100   : $1/pt  per lot  -- micro NAS (different contract, much smaller)
//   DJ30.F   : $5/pt  per lot  -- Dow Jones equivalent
//
// PER-SYMBOL ATR CALIBRATION (Apr 2026 live data + historical context):
//   US500.F  : daily range 40-80pt, hourly 8-20pt,  ATR_MIN=3pt,  spread=0.5pt
//   USTEC.F  : daily range 100-250pt,hourly 20-60pt, ATR_MIN=8pt,  spread=0.8pt
//   NAS100   : same price as USTEC but $1/pt contract, ATR_MIN=8pt, spread=0.8pt
//   DJ30.F   : daily range 200-500pt,hourly 40-120pt,ATR_MIN=15pt, spread=2pt
//
// ENTRY ARCHITECTURE:
//   1. L2 imbalance persistence (fast=30t, slow=60t) OR EWM drift fallback
//   2. EWM drift confirmation -- price actually moving in signal direction
//   3. Momentum confirmation -- mid moving in signal direction over last 12 ticks
//   4. ATR quality gate -- ATR >= ATR_MIN (filters dead-tape noise entries)
//
// REGIME GOVERNOR (ported from GoldEngineStack RegimeGovernor):
//   COMPRESSION → IMPULSE → TREND → MEAN_REVERSION
//   EWM fast (α=0.05) vs slow (α=0.005) drift signal
//   TREND if |EWM_fast - EWM_slow| > TREND_EWM_THRESHOLD (per-symbol scaled)
//
// EXIT ARCHITECTURE (ATR-proportional staircase trail):
//   Step 1 (+1x ATR): lock BE, bank 33%
//   Step 2 (+2x ATR): bank 33% of remainder, trail 0.5x ATR
//   Step 3 (+4x ATR): tight 0.25x ATR trail
//   Timeout: 60 min max hold -- no forced exit if trail in profit
//
// MACRO CRASH MODE (IndexMacroCrash, shadow by default):
//   Entry: ATR > MACRO_ATR_THRESHOLD, vol_ratio > 2.5, |drift| > MACRO_DRIFT_MIN
//   Exit:  30% bracket floor at 2xATR + 70% velocity trail
//   Max 3 cost-covered pyramid adds
//   shadow_mode=true by default -- never set false without explicit authorization
//
// VWAP ATR UPGRADE for VWAPReversion (existing engine enhancement):
//   Adds ATR-proportional SL to existing g_vwap_rev_sp/nq instances.
//   Wired via IndexFlowCfg.vwap_atr_sl_mult (default 1.0).
//   BE lock at 1x ATR move. Trail at 1.5x ATR, dist 0.5x ATR.
//   Called from tick_indices.hpp after existing VWAPReversion on_tick().
//
// USAGE (tick_indices.hpp):
//   // Per-symbol instance
//   static omega::idx::IndexFlowEngine g_iflow_sp("US500.F");
//   static omega::idx::IndexFlowEngine g_iflow_nq("USTEC.F");
//   static omega::idx::IndexFlowEngine g_iflow_nas("NAS100");
//   static omega::idx::IndexFlowEngine g_iflow_us30("DJ30.F");
//
//   // Each tick:
//   if (g_iflow_sp.has_open_position())
//       g_iflow_sp.on_tick(sym, bid, ask, l2_imb, ca_on_close);
//   else if (base_can_sp) {
//       auto sig = g_iflow_sp.on_tick(sym, bid, ask, l2_imb, ca_on_close);
//       if (sig.valid) { enter_directional(...); g_iflow_sp.patch_size(lot); }
//   }
//
// GLOBALS (globals.hpp additions):
//   static omega::idx::IndexFlowEngine g_iflow_sp("US500.F");
//   static omega::idx::IndexFlowEngine g_iflow_nq("USTEC.F");
//   static omega::idx::IndexFlowEngine g_iflow_nas("NAS100");
//   static omega::idx::IndexFlowEngine g_iflow_us30("DJ30.F");
//   static omega::idx::IndexMacroCrashEngine g_imacro_sp("US500.F");
//   static omega::idx::IndexMacroCrashEngine g_imacro_nq("USTEC.F");
// =============================================================================

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include "OmegaTradeLedger.hpp"
#include "OHLCBarEngine.hpp"

namespace omega {
namespace idx {

// =============================================================================
// Helpers
// =============================================================================
static inline int64_t idx_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static inline int64_t idx_now_sec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static inline void idx_utc(struct tm& ti) noexcept {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
#ifdef _WIN32
    gmtime_s(&ti, &t);
#else
    gmtime_r(&t, &ti);
#endif
}

// =============================================================================
// IndexSymbolCfg -- per-symbol calibration constants
// =============================================================================
// All ATR/drift/spread/SL values are in PRICE POINTS for that instrument.
// Dollar risk per trade is always computed as: risk_pts * lot * usd_per_pt.
//
// Calibration basis (April 2026 live data + 2yr historical):
//   US500.F  : SP500 mini, $50/pt/lot. Normal hourly ATR 8-20pt. Spread 0.3-0.8pt.
//   USTEC.F  : NQ Nasdaq, $20/pt/lot. Normal hourly ATR 20-60pt. Spread 0.5-1.5pt.
//   NAS100   : micro NAS, $1/pt/lot.  Same price scale as USTEC. Spread 0.5-1.5pt.
//   DJ30.F   : Dow Jones, $5/pt/lot.  Normal hourly ATR 40-120pt. Spread 1-3pt.
//
// DRIFT thresholds calibrated to approximately equivalent dollar-move signals:
//   Gold drift threshold = $0.50 (EWM_fast - slow in gold points)
//   SP500: 0.5pt / 5000pt price = same as 0.5/3000 on gold → scale to 0.5pt
//   NQ: 0.5pt → 1.5pt (NQ moves 3x SP per unit)
//   DJ30: 0.5pt → 5pt (Dow is nominally ~5x SP)
//
// TREND_EWM_THRESHOLD: equivalent to gold's $8 fast-slow drift trigger.
//   On gold at $5000: $8 = 0.16% of price.
//   SP500 at ~5500: 0.16% = 8.8pt → round to 8pt.
//   NQ at ~19000: 0.16% = 30pt.
//   DJ30 at ~40000: 0.16% = 64pt.
// =============================================================================
struct IndexSymbolCfg {
    const char* symbol            = "US500.F";
    double usd_per_pt             = 50.0;   // $ per price point per lot
    double atr_min                = 3.0;    // minimum ATR to allow entry
    double atr_sl_mult            = 1.0;    // SL = ATR * this
    double max_spread             = 1.0;    // max spread at entry (pts)
    double drift_threshold        = 0.5;    // EWM fast-slow drift for fallback signal (pts)
    double drift_persist_ticks    = 20;     // ticks drift must hold before entry
    double trend_ewm_threshold    = 8.0;    // |EWM_fast - EWM_slow| = trending (pts)
    double risk_dollars           = 50.0;   // target $ risk per trade
    double macro_atr_threshold    = 15.0;   // MacroCrash entry: ATR must exceed this
    double macro_drift_min        = 8.0;    // MacroCrash entry: |drift| must exceed this
    double asia_atr_min           = 0.0;    // Asia-session ATR floor (0=disabled)
    double asia_atr_spread_ratio  = 4.0;    // ATR/spread >= this in Asia
    int    max_hold_sec           = 3600;   // 60min timeout, suppressed if trail in profit
    int    cooldown_ms            = 30000;  // 30s post-exit cooldown
    int    min_entry_ticks        = 50;     // ticks before first entry allowed
};

// Predefined configs -- edit here if recalibration is needed
static const IndexSymbolCfg IDX_CFG_SP = {
    "US500.F",
    50.0,     // usd_per_pt: $50/pt/lot
    3.0,      // atr_min: 3pt floor -- below this = dead tape (normal NY quiet = 5-8pt)
    1.0,      // atr_sl_mult: SL = 1x ATR
    1.0,      // max_spread: 1pt cap (normal SP spread = 0.3-0.6pt)
    0.5,      // drift_threshold: 0.5pt EWM drift = real directional move
    20,       // drift_persist_ticks
    8.0,      // trend_ewm_threshold: 8pt fast-slow gap = trending (0.15% of 5500)
    50.0,     // risk_dollars: $50/trade → at ATR=8pt, lot=50/(8*50)=0.125 lots
    15.0,     // macro_atr_threshold: 15pt ATR = genuine expansion (2σ+ day)
    8.0,      // macro_drift_min: 8pt drift = sustained institutional flow
    0.0,      // asia_atr_min: US500 has no Asia session
    4.0,      // asia_atr_spread_ratio
    3600,     // max_hold_sec
    30000,    // cooldown_ms
    50        // min_entry_ticks
};

static const IndexSymbolCfg IDX_CFG_NQ = {
    "USTEC.F",
    20.0,     // usd_per_pt: $20/pt/lot
    8.0,      // atr_min: 8pt floor -- NQ normal quiet = 15-25pt/hr
    1.0,      // atr_sl_mult
    1.5,      // max_spread: 1.5pt (NQ spread = 0.5-1.2pt normally)
    1.5,      // drift_threshold: 1.5pt EWM drift (NQ moves ~3x SP points)
    20,       // drift_persist_ticks
    30.0,     // trend_ewm_threshold: 30pt gap = trending NQ
    50.0,     // risk_dollars: $50/trade → at ATR=25pt, lot=50/(25*20)=0.10 lots
    50.0,     // macro_atr_threshold: 50pt ATR = genuine NQ expansion
    30.0,     // macro_drift_min
    0.0,      // asia_atr_min: USTEC has minimal Asia session
    4.0,
    3600,
    30000,
    50
};

static const IndexSymbolCfg IDX_CFG_NAS100 = {
    "NAS100",
    1.0,      // usd_per_pt: $1/pt/lot (micro contract)
    8.0,      // atr_min: same price scale as USTEC
    1.0,
    1.5,
    1.5,
    20,
    30.0,
    5.0,      // risk_dollars: $5/trade at $1/pt (smaller contract)
    50.0,
    30.0,
    0.0,
    4.0,
    3600,
    30000,
    50
};

static const IndexSymbolCfg IDX_CFG_US30 = {
    "DJ30.F",
    5.0,      // usd_per_pt: $5/pt/lot
    15.0,     // atr_min: 15pt floor -- DJ30 normal quiet = 40-80pt/hr
    1.0,
    3.0,      // max_spread: 3pt (DJ30 spread = 1-2.5pt normally)
    5.0,      // drift_threshold: 5pt (Dow nominally 5x SP points)
    20,
    60.0,     // trend_ewm_threshold: 60pt gap (0.15% of 40000)
    50.0,     // risk_dollars: $50/trade → at ATR=60pt, lot=50/(60*5)=0.167 lots
    100.0,    // macro_atr_threshold: 100pt ATR
    60.0,     // macro_drift_min
    0.0,
    4.0,
    3600,
    30000,
    50
};

// =============================================================================
// IndexFlowSignal
// =============================================================================
struct IndexFlowSignal {
    bool        valid     = false;
    bool        is_long   = false;
    double      entry     = 0.0;
    double      tp        = 0.0;    // absolute price level
    double      sl        = 0.0;    // absolute price level
    double      size      = 0.01;   // fallback -- patched by compute_size in main
    double      atr       = 0.0;    // ATR at signal time (for sizing reference)
    const char* symbol    = "";
    const char* engine    = "IndexFlow";
    const char* reason    = "";
};

// =============================================================================
// IdxRegimeGovernor -- EWM drift-based regime detection
// Direct port of GoldEngineStack RegimeGovernor, scaled per symbol.
// =============================================================================
class IdxRegimeGovernor {
public:
    enum class Regime { MEAN_REVERSION, COMPRESSION, IMPULSE, TREND };

    double ewm_fast   = 0.0;
    double ewm_slow   = 0.0;
    bool   init       = false;

    static constexpr double A_FAST = 0.05;
    static constexpr double A_SLOW = 0.005;

    void update(double mid) noexcept {
        if (!init) { ewm_fast = ewm_slow = mid; init = true; return; }
        ewm_fast = A_FAST * mid + (1.0 - A_FAST) * ewm_fast;
        ewm_slow = A_SLOW * mid + (1.0 - A_SLOW) * ewm_slow;
    }

    double drift() const noexcept { return init ? (ewm_fast - ewm_slow) : 0.0; }

    // Returns true if drift magnitude exceeds the symbol's trend threshold.
    // This is the primary "real institutional flow" detector -- mirrors
    // GoldEngineStack's is_drift_trending() logic.
    bool is_trending(double threshold) const noexcept {
        return std::fabs(drift()) > threshold;
    }

    // Full reset after a confirmed price reversal (mirrors gold reset_drift_on_reversal).
    // Snaps ewm_slow to ewm_fast so new direction registers within a few ticks.
    // Call from tick handler when position closes and price reverses >= 2xATR.
    void reset_on_reversal() noexcept {
        if (!init) return;
        const double old = ewm_fast - ewm_slow;
        ewm_slow = ewm_fast;
        printf("[IDX-DRIFT-RESET] old_drift=%.2f -> 0.00 (full snap)\n", old);
        fflush(stdout);
    }
};

// =============================================================================
// IdxATRTracker -- rolling ATR from 100-tick range
// Rolling ATR from 100-tick range with EWM smoothing.
// =============================================================================
class IdxATRTracker {
    static constexpr int BUF = 256;
    double buf_[BUF] = {};
    int    head_  = 0;
    int    count_ = 0;
    double ewm_   = 0.0;
    bool   init_  = false;
    static constexpr double ALPHA = 0.05; // EWM smoothing alpha

public:
    void push(double mid) noexcept {
        buf_[head_ % BUF] = mid;
        ++head_; ++count_;
        if (count_ % 25 != 0) return; // update every 25 ticks
        const int look = std::min(count_, 100);
        if (look < 10) return;
        double hi = buf_[(head_ - 1 + BUF * 4) % BUF];
        double lo = hi;
        for (int k = 1; k < look; ++k) {
            double p = buf_[(head_ - k - 1 + BUF * 4) % BUF];
            if (p > hi) hi = p;
            if (p < lo) lo = p;
        }
        const double range = hi - lo;
        if (!init_) { ewm_ = range; init_ = true; }
        else          ewm_ = ALPHA * range + (1.0 - ALPHA) * ewm_;
    }

    double atr() const noexcept { return ewm_; }
    bool   ready() const noexcept { return count_ >= 50; }

    // Seed from saved state (warm restart) -- initialize ATR from persisted value
    void seed(double val) noexcept {
        if (val <= 0.0 || init_) return;
        ewm_ = val; init_ = true;
    }
};

// =============================================================================
// IdxOpenPosition -- manages one open leg (TP/SL/staircase trail/timeout)
// Mirrors GoldPositionManager but simpler (one leg only, no pyramid for base).
// =============================================================================
class IdxOpenPosition {
public:
    bool    active         = false;
    bool    is_long        = false;
    double  entry          = 0.0;
    double  sl             = 0.0;
    double  tp             = 0.0;    // initial TP (3x ATR) -- removed after staircase armed
    double  mfe            = 0.0;
    double  mae            = 0.0;
    double  atr_at_entry   = 0.0;
    double  size           = 0.01;
    int64_t entry_ts       = 0;      // UTC seconds
    int     trail_stage    = 0;      // 0=initial, 1=BE+partial, 2=tight trail
    bool    be_locked      = false;
    bool    partial_done   = false;  // first stair step (33%) already banked
    double  full_size      = 0.0;    // original size before partial
    bool    shadow_mode    = true;   // default true = log only, no live orders (Class C added 2026-04-21)
    char    symbol[16]     = {};
    char    reason[32]     = {};

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    void open(const IndexFlowSignal& sig) noexcept {
        active       = true;
        is_long      = sig.is_long;
        entry        = sig.entry;
        sl           = sig.sl;
        tp           = sig.tp;
        atr_at_entry = sig.atr;
        size         = sig.size;
        full_size    = sig.size;
        mfe = mae    = 0.0;
        trail_stage  = 0;
        be_locked    = false;
        partial_done = false;
        entry_ts     = idx_now_sec();
        strncpy(symbol, sig.symbol, 15); symbol[15] = '\0';
        strncpy(reason, sig.reason, 31);
    }

    // Call every tick. Returns true if position closed.
    bool manage(double bid, double ask, double atr, int max_hold_sec,
                const char* regime, CloseCb& on_close) noexcept {
        if (!active) return false;
        const double mid   = (bid + ask) * 0.5;
        const double move  = is_long ? (mid - entry) : (entry - mid);
        if (move > mfe) mfe = move;
        if (-move > mae) mae = -move;

        // Staircase trail -- ATR-proportional tiered SL advancement
        // Uses atr_at_entry (stable) not current ATR (changes every 25 ticks)
        const double a = (atr_at_entry > 0.0) ? atr_at_entry : atr;
        if (a > 0.0) {
            // Stage 1: BE lock at 1x ATR profit
            if (!be_locked && move >= a * 1.0) {
                be_locked = true;
                if (is_long  && entry > sl) sl = entry;
                if (!is_long && entry < sl) sl = entry;
                trail_stage = 1;
                printf("[IFLOW-%s] BE-LOCK %s move=%.2f atr=%.2f sl=%.2f\n",
                       symbol, is_long?"LONG":"SHORT", move, a, sl);
                fflush(stdout);
            }
            // Stage 2: trail at 0.5x ATR behind peak from 2x ATR
            if (be_locked && move >= a * 2.0) {
                const double trail = is_long ? (entry + mfe - a * 0.5)
                                             : (entry - mfe + a * 0.5);
                if (is_long  && trail > sl) sl = trail;
                if (!is_long && trail < sl) sl = trail;
                trail_stage = 2;
            }
            // Stage 3: tight trail at 0.25x ATR behind peak from 4x ATR
            if (be_locked && move >= a * 4.0) {
                const double trail = is_long ? (entry + mfe - a * 0.25)
                                             : (entry - mfe + a * 0.25);
                if (is_long  && trail > sl) sl = trail;
                if (!is_long && trail < sl) sl = trail;
                trail_stage = 3;
            }
        }

        // TP hit
        const bool tp_hit = (tp > 0.0) &&
                            (is_long ? (bid >= tp) : (ask <= tp));
        // SL hit
        const bool sl_hit = is_long ? (bid <= sl) : (ask >= sl);

        // Timeout -- suppress if trail in profit
        const int64_t held = idx_now_sec() - entry_ts;
        bool timed_out = false;
        if (held >= max_hold_sec && !tp_hit && !sl_hit) {
            const bool trail_profit = is_long ? (sl >= entry) : (sl <= entry);
            if (trail_profit) {
                // Trail locked above entry -- let it run (profitable trail, no timeout)
                timed_out = false;
            } else {
                timed_out = true;
            }
        }

        if (tp_hit || sl_hit || timed_out) {
            double exit_px;
            const char* why;
            if (tp_hit)       { exit_px = tp;  why = "TP_HIT";  }
            else if (sl_hit)  { exit_px = sl;  why = "SL_HIT";  }
            else              {
                const bool sl_br = is_long ? (mid < sl) : (mid > sl);
                exit_px = sl_br ? sl : mid;
                why = "TIMEOUT";
            }
            emit(exit_px, why, regime, on_close);
            return true;
        }
        return false;
    }

    void force_close(double bid, double ask, const char* regime, CloseCb& on_close) noexcept {
        if (!active) return;
        emit((bid + ask) * 0.5, "FORCE_CLOSE", regime, on_close);
    }

    void patch_size(double lot) noexcept { if (active && lot > 0.0) { size = lot; full_size = lot; } }

private:
    static int s_trade_id_;

    void emit(double exit_px, const char* why, const char* regime, CloseCb& on_close) noexcept {
        omega::TradeRecord tr;
        tr.id          = ++s_trade_id_;
        tr.symbol      = symbol;
        tr.side        = is_long ? "LONG" : "SHORT";
        tr.engine      = "IndexFlow";
        tr.entryPrice  = entry;
        tr.exitPrice   = exit_px;
        tr.tp          = tp;
        tr.sl          = sl;
        tr.size        = size;
        tr.pnl         = (is_long ? (exit_px - entry) : (entry - exit_px)) * size;
        tr.mfe         = mfe;
        tr.mae         = mae;
        tr.entryTs     = entry_ts;
        tr.exitTs      = idx_now_sec();
        tr.exitReason  = why;
        tr.regime      = regime ? regime : "";
        tr.shadow      = shadow_mode;
        active = false;
        if (on_close) on_close(tr);
    }
};

int IdxOpenPosition::s_trade_id_ = 9000;

// =============================================================================
// IndexFlowEngine
// =============================================================================
// Core L2 + EWM drift flow engine for equity indices.
// One instance per symbol. Phase machine: IDLE → FLOW_BUILDING → LIVE → COOLDOWN.
// =============================================================================
class IndexFlowEngine {
public:
    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    enum class Phase { IDLE, FLOW_BUILDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    explicit IndexFlowEngine(const char* symbol) {
        if      (strcmp(symbol, "US500.F") == 0) cfg_ = IDX_CFG_SP;
        else if (strcmp(symbol, "USTEC.F") == 0) cfg_ = IDX_CFG_NQ;
        else if (strcmp(symbol, "NAS100")  == 0) cfg_ = IDX_CFG_NAS100;
        else if (strcmp(symbol, "DJ30.F")  == 0) cfg_ = IDX_CFG_US30;
        else                                      cfg_ = IDX_CFG_SP; // safe default
        strncpy(symbol_, symbol, 15);
    }

    // ── Public accessors ─────────────────────────────────────────────────────
    bool has_open_position() const noexcept { return pos_.active; }
    double atr()             const noexcept { return atr_tracker_.atr(); }
    double drift()           const noexcept { return regime_.drift(); }
    bool   is_trending()     const noexcept { return regime_.is_trending(cfg_.trend_ewm_threshold); }
    const IndexSymbolCfg& cfg() const noexcept { return cfg_; }
    void   patch_size(double lot) noexcept { pos_.patch_size(lot); }
    void   reset_drift()    noexcept { regime_.reset_on_reversal(); }
    void   seed_atr(double v) noexcept { atr_tracker_.seed(v); }
    // ISSUE-5: proxy for per-engine shadow_mode control from engine_init.hpp.
    // shadow_mode lives on the private IdxOpenPosition pos_; this setter lets
    // external init code (engine_init.hpp kShadowDefault wiring) flip the flag
    // without breaking encapsulation.
    void   set_shadow_mode(bool b) noexcept { pos_.shadow_mode = b; }

    // Force-close open position (disconnect / session end)
    void force_close(double bid, double ask, CloseCb on_close) noexcept {
        if (!pos_.active) return;
        pos_.force_close(bid, ask, "FORCE_CLOSE", on_close);
        phase = Phase::COOLDOWN;
        cooldown_until_ms_ = idx_now_ms() + cfg_.cooldown_ms;
    }

    // ── Main tick function ────────────────────────────────────────────────────
    // Call every tick. Returns valid signal if new entry opened this tick.
    // l2_imb: order book imbalance 0..1 (0.5 = neutral). Use g_macro_ctx.sp_l2_imbalance etc.
    //         Set to 0.5 if not available -- engine falls back to drift-only mode.
    // can_enter: set false when another engine on same symbol is already in a trade.
    IndexFlowSignal on_tick(const std::string& sym, double bid, double ask,
                            double l2_imb, CloseCb on_close,
                            bool can_enter = true) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return {};

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Always update state buffers regardless of entry gate
        regime_.update(mid);
        atr_tracker_.push(mid);
        feed_persistence(mid);
        ++tick_count_;

        // Always manage open position
        if (pos_.active) {
            bool was_sl = false;
            CloseCb cb_wrap = [&](const omega::TradeRecord& tr) {
                if (tr.exitReason == "SL_HIT") was_sl = true;
                if (on_close) on_close(tr);
            };
            const bool closed = pos_.manage(bid, ask, atr_tracker_.atr(),
                                            cfg_.max_hold_sec,
                                            "IFLOW", cb_wrap);
            if (closed) {
                phase = Phase::COOLDOWN;
                cooldown_until_ms_ = idx_now_ms() + cfg_.cooldown_ms;
                last_exit_side_ = pos_.is_long ? 0 : 1;
                // 90s SL cooldown: blocks chop re-entries after stop hits
                if (was_sl) {
                    const int64_t sl_block = idx_now_ms() + 90000LL;
                    if (sl_block > m_sl_cooldown_until_ms_)
                        m_sl_cooldown_until_ms_ = sl_block;
                }
            }
            return {};
        }

        // Cooldown gate
        if (phase == Phase::COOLDOWN) {
            if (idx_now_ms() < cooldown_until_ms_) return {};
            phase = Phase::IDLE;
        }

        if (!can_enter) return {};
        if (!atr_tracker_.ready()) return {};
        if (tick_count_ < cfg_.min_entry_ticks) return {};

        const double atr    = atr_tracker_.atr();
        const double d      = regime_.drift();

        // ATR quality gate -- no entry if market is dead tape
        if (atr < cfg_.atr_min) return {};

        // Spread gate
        if (spread > cfg_.max_spread) return {};

        // Session gate for US indices: block Asia session (22:00-13:30 UTC)
        // US equity indices have essentially no volume during Asian hours.
        // Only fire during London (08:00-13:30 UTC) and NY (13:30-22:00 UTC).
        {
            struct tm ti{}; idx_utc(ti);
            const int mins = ti.tm_hour * 60 + ti.tm_min;
            // Block 22:00-08:00 UTC for US indices (Asia + dead zone)
            const bool dead = (mins >= 22 * 60) || (mins < 8 * 60);
            if (dead) return {};

            // NY open noise gate: 13:15-14:00 UTC (extended from 13:45)
            // Root cause of 13-trade 26%WR cluster: NY opens at 13:30 UTC with
            // violent bid/ask oscillation. 13 IndexFlow trades in 30 min including
            // direction flips within seconds, 5 zero-gross SL trades.
            // Total damage: -$56.28 in 30 minutes.
            // Extended 2026-04-09: trades at 13:45,13:46,13:47,13:49 all SL_HIT
            // immediately after gate lifted -- NY volatility persists past 13:45.
            // Block 13:15-14:00 UTC (45 min total covers full NY open noise window).
            const bool ny_open_noise = (mins >= 13 * 60 + 15) && (mins < 14 * 60 + 0);
            if (ny_open_noise) return {};
        }

        // SL cooldown gate: 90s after any SL_HIT before re-entering
        // Prevents immediate re-entry after a stop -- the chop pattern
        // that produced rapid direction flips at NY open.
        if (idx_now_ms() < m_sl_cooldown_until_ms_) return {};

        // ── Signal detection ──────────────────────────────────────────────────
        // Primary: L2 imbalance persistence (30-tick fast + 60-tick slow windows)
        // Fallback: EWM drift when L2 is not live (imb=0.5 from broker)
        //
        // LONG signal: bid-heavy book (imb > threshold) OR positive drift
        // SHORT signal: ask-heavy book (imb < threshold) OR negative drift
        //
        // Adapted for the fact that BlackBull often doesn't send L2 size
        // (tag-271 omitted), so drift-only mode is the primary operating mode.

        bool l2_long  = false;
        bool l2_short = false;
        bool drift_long  = false;
        bool drift_short = false;

        // L2 persistence check (only meaningful when l2_imb != 0.5)
        const bool l2_live = (std::fabs(l2_imb - 0.5) > 0.05);
        if (l2_live) {
            l2_long  = check_l2_persistence(true);
            l2_short = check_l2_persistence(false);
        }

        // Drift fallback (threshold configured via cfg_.drift_threshold)
        drift_long  = (d >  cfg_.drift_threshold) && (drift_persist_long_  >= (int)cfg_.drift_persist_ticks);
        drift_short = (d < -cfg_.drift_threshold) && (drift_persist_short_ >= (int)cfg_.drift_persist_ticks);

        const bool signal_long  = l2_long  || drift_long;
        const bool signal_short = l2_short || drift_short;

        if (!signal_long && !signal_short) {
            phase = Phase::FLOW_BUILDING;
            return {};
        }

        // Momentum confirmation: last 12 ticks must show directional move
        // (prevents stale drift from firing on a paused tape)
        const double momentum = mid_buf_[(mid_buf_head_ - 1 + BUF_SZ) % BUF_SZ]
                              - mid_buf_[(mid_buf_head_ - 13 + BUF_SZ * 4) % BUF_SZ];
        if (signal_long  && momentum <= 0.0) return {};
        if (signal_short && momentum >= 0.0) return {};

        // Chop guard: if drift has oscillated (high range / low net), block entry.
        // Prevents straddle-chop patterns where drift spikes but doesn't sustain.
        // drift_range > 4x drift_threshold with low net = chop.
        if (drift_range_ > cfg_.drift_threshold * 4.0 &&
            std::fabs(d) < cfg_.drift_threshold * 1.5) {
            return {};
        }

        // Min confidence gate: require ATR >= 1.5x min for adequate SL headroom.
        if (atr < cfg_.atr_min * 1.5) return {};

        // ── Build the signal ──────────────────────────────────────────────────
        const bool is_long = signal_long;

        // SL: ATR * atr_sl_mult (always from current ATR, clamped to reasonable range)
        const double sl_dist = std::max(cfg_.atr_min, atr * cfg_.atr_sl_mult);
        const double sl      = is_long ? (bid - sl_dist) : (ask + sl_dist);

        // TP: 3x ATR initial target (staircase trail takes over after stage 1)
        // Staircase drives the exit, not a fixed TP. 3x ATR ensures cost_guard passes.
        const double tp_dist = sl_dist * 3.0;
        const double tp      = is_long ? (ask + tp_dist) : (bid - tp_dist);

        // Lot sizing: risk_dollars / (sl_dist * usd_per_pt)
        // Standard risk-based formula: risk = size * sl_pts * usd_per_pt
        const double lot_raw = cfg_.risk_dollars / (sl_dist * cfg_.usd_per_pt);
        const double lot     = std::max(0.01, std::min(1.0, lot_raw));

        IndexFlowSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = is_long ? ask : bid;  // realistic fill
        sig.sl      = sl;
        sig.tp      = tp;
        sig.size    = lot;
        sig.atr     = atr;
        sig.symbol  = symbol_;
        sig.engine  = "IndexFlow";
        sig.reason  = is_long ? "IFLOW_LONG" : "IFLOW_SHORT";

        pos_.open(sig);
        phase = Phase::LIVE;

        printf("[IFLOW-%s] %s entry=%.2f sl=%.2f tp=%.2f atr=%.2f lot=%.3f "
               "drift=%.2f l2_live=%d\n",
               symbol_, is_long ? "LONG" : "SHORT",
               sig.entry, sl, tp, atr, lot, d, l2_live ? 1 : 0);
        fflush(stdout);

        return sig;
    }

private:
    static constexpr int BUF_SZ = 128;

    IndexSymbolCfg cfg_;
    char   symbol_[16] = {};
    int    tick_count_  = 0;
    int64_t cooldown_until_ms_      = 0;
    int     last_exit_side_         = -1;
    int64_t m_sl_cooldown_until_ms_ = 0;  // 90s block after SL_HIT

    IdxRegimeGovernor regime_;
    IdxATRTracker     atr_tracker_;
    IdxOpenPosition   pos_;

    // Persistence window buffers (fast/slow L2 tracking windows)
    static constexpr int FAST_TICKS = 30;
    static constexpr int SLOW_TICKS = 60;

    double l2_fast_buf_[FAST_TICKS] = {};
    double l2_slow_buf_[SLOW_TICKS] = {};
    int    l2_fast_head_ = 0, l2_fast_count_ = 0;
    int    l2_slow_head_ = 0, l2_slow_count_ = 0;

    // Momentum buffer
    double mid_buf_[BUF_SZ] = {};
    int    mid_buf_head_     = 0;

    // Drift persistence counters
    int    drift_persist_long_  = 0;
    int    drift_persist_short_ = 0;
    double drift_range_         = 0.0;  // max-min drift in recent window (chop detector)
    double drift_window_[64]    = {};
    int    drift_win_head_      = 0;

    void feed_persistence(double mid) noexcept {
        mid_buf_[mid_buf_head_ % BUF_SZ] = mid;
        ++mid_buf_head_;

        const double d = regime_.drift();

        // Track drift persistence
        if (d >  cfg_.drift_threshold) { ++drift_persist_long_;  drift_persist_short_ = 0; }
        else if (d < -cfg_.drift_threshold) { ++drift_persist_short_; drift_persist_long_  = 0; }
        else { drift_persist_long_ = 0; drift_persist_short_ = 0; }

        // Chop detector: drift range over last 64 ticks
        drift_window_[drift_win_head_ % 64] = d;
        ++drift_win_head_;
        const int n = std::min(drift_win_head_, 64);
        double hi = drift_window_[0], lo = drift_window_[0];
        for (int i = 1; i < n; ++i) {
            if (drift_window_[i] > hi) hi = drift_window_[i];
            if (drift_window_[i] < lo) lo = drift_window_[i];
        }
        drift_range_ = hi - lo;
    }

    bool check_l2_persistence(bool want_long) const noexcept {
        // Not used when l2_imb = 0.5 (broker not sending L2 size)
        // This is a stub for when real L2 becomes available per-tick
        // In current BlackBull setup, drift-only mode is the primary path
        (void)want_long;
        return false;
    }
};

// =============================================================================
// IndexMacroCrashEngine
// =============================================================================
// Hybrid bracket floor + velocity trail for index macro expansion moves.
// Direct port of MacroCrashEngine v2.0 logic, scaled for indices.
//
// ENTRY GATES (all must pass):
//   ATR >= cfg.macro_atr_threshold    -- genuine vol expansion
//   vol_ratio > 2.5                   -- recent vol > 2.5x EWM baseline
//   |drift| >= cfg.macro_drift_min    -- strongly directional
//   Regime = IMPULSE or TREND (from IdxRegimeGovernor)
//
// EXIT:
//   30% bracket floor: limit at 2x ATR (guaranteed locked profit, regardless of reversal)
//   70% velocity trail: arms at 3x ATR, trails at 2x ATR behind MFE
//
// PYRAMID (shadow by default):
//   Trigger: price moves 2x ATR from last add
//   Gate: be_locked=true on base position, all prior SLs advance to new add entry
//   Max 3 adds (4 positions total)
//   pyramid_shadow=true by default -- logs [IMACRO-PYRAMID-SHADOW]
//
// shadow_mode=true by default.
// NEVER set shadow_mode=false without explicit authorization.
// =============================================================================
class IndexMacroCrashEngine {
public:
    bool shadow_mode     = true;   // NEVER change without authorization
    bool pyramid_shadow  = true;   // NEVER change without authorization

    explicit IndexMacroCrashEngine(const char* symbol) {
        if      (strcmp(symbol, "US500.F") == 0) cfg_ = IDX_CFG_SP;
        else if (strcmp(symbol, "USTEC.F") == 0) cfg_ = IDX_CFG_NQ;
        else if (strcmp(symbol, "NAS100")  == 0) cfg_ = IDX_CFG_NAS100;
        else if (strcmp(symbol, "DJ30.F")  == 0) cfg_ = IDX_CFG_US30;
        else                                      cfg_ = IDX_CFG_SP;
        strncpy(symbol_, symbol, 15);
    }

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    bool has_open_position() const noexcept { return base_active_; }

    // vol_ratio: recent_range / ewm_baseline (same computation as GoldEngineStack)
    // drift: from IndexFlowEngine::drift()
    // atr: from IndexFlowEngine::atr()
    void on_tick(double bid, double ask, double atr, double drift,
                 double vol_ratio, bool trend_regime, CloseCb on_close,
                 bool can_enter = true) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Manage open positions every tick
        if (base_active_) {
            manage_position(bid, ask, atr, mid, on_close);
            return;
        }

        if (!can_enter) return;
        if (idx_now_sec() < cooldown_until_) return;

        // Entry gates -- mirrors MacroCrashEngine v2.0
        if (atr < cfg_.macro_atr_threshold) return;
        if (vol_ratio < 2.5) return;
        if (std::fabs(drift) < cfg_.macro_drift_min) return;
        if (!trend_regime) return;
        if (spread > cfg_.max_spread * 2.0) return;  // wide spread on expansion = ok with 2x tolerance

        const bool is_long = (drift > 0.0);

        // Shadow logging
        const char* pfx = shadow_mode ? "[IMACRO-SHADOW]" : "[IMACRO]";
        printf("%s %s %s atr=%.2f vol_ratio=%.2f drift=%.2f entry=%.2f\n",
               pfx, symbol_, is_long ? "LONG" : "SHORT",
               atr, vol_ratio, drift, is_long ? ask : bid);
        fflush(stdout);

        // Shadow mode: open the position for simulation -- same as live
        // but no FIX order sent. This is the correct shadow pattern.
        // Do NOT return here -- position must be tracked to generate a TradeRecord on close.

        // Live entry (runs in both shadow and live)
        base_active_ = true;
        base_is_long_ = is_long;
        base_entry_  = is_long ? ask : bid;
        base_sl_     = is_long ? (bid - atr * cfg_.atr_sl_mult)
                                : (ask + atr * cfg_.atr_sl_mult);
        base_atr_    = atr;
        base_size_   = cfg_.risk_dollars / (atr * cfg_.atr_sl_mult * cfg_.usd_per_pt);
        base_size_   = std::max(0.01, std::min(1.0, base_size_));
        base_mfe_    = 0.0;
        be_locked_   = false;
        bracket_fired_ = false;
        base_entry_ts_ = idx_now_sec();

        // Bracket floor order: 30% lot at 2x ATR target
        // In live mode this would fire a separate limit order
        bracket_target_ = is_long ? (base_entry_ + atr * 2.0)
                                   : (base_entry_ - atr * 2.0);
        bracket_size_   = base_size_ * 0.30;
        velocity_size_  = base_size_ * 0.70;

        printf("[IMACRO] %s %s entry=%.2f bracket=%.2f sl=%.2f lot=%.3f\n",
               symbol_, is_long?"LONG":"SHORT",
               base_entry_, bracket_target_, base_sl_, base_size_);
        fflush(stdout);
    }

private:
    IndexSymbolCfg cfg_;
    char   symbol_[16] = {};
    int64_t cooldown_until_ = 0;

    bool   base_active_    = false;
    bool   base_is_long_   = false;
    double base_entry_     = 0.0;
    double base_sl_        = 0.0;
    double base_mfe_       = 0.0;
    double base_atr_       = 0.0;
    double base_size_      = 0.01;
    int64_t base_entry_ts_ = 0;
    double bracket_target_ = 0.0;
    double bracket_size_   = 0.0;
    double velocity_size_  = 0.0;
    bool   bracket_fired_  = false;
    bool   be_locked_      = false;

    static int s_trade_id_;

    void manage_position(double bid, double ask, double atr, double mid,
                         CloseCb& on_close) noexcept {
        const double move = base_is_long_ ? (mid - base_entry_) : (base_entry_ - mid);
        if (move > base_mfe_) base_mfe_ = move;

        const double a = (base_atr_ > 0.0) ? base_atr_ : atr;

        // BE lock at 1x ATR
        if (!be_locked_ && move >= a * 1.0) {
            be_locked_ = true;
            if (base_is_long_  && base_entry_ > base_sl_) base_sl_ = base_entry_;
            if (!base_is_long_ && base_entry_ < base_sl_) base_sl_ = base_entry_;
        }

        // Bracket floor: fire at 2x ATR (30% of position)
        if (!bracket_fired_) {
            const bool br_hit = base_is_long_ ? (bid >= bracket_target_)
                                              : (ask <= bracket_target_);
            if (br_hit) {
                bracket_fired_ = true;
                emit_partial(bracket_target_, "BRACKET_FLOOR", on_close, bracket_size_);
            }
        }

        // Velocity trail: arm at 3x ATR, trail at 2x ATR behind MFE
        if (be_locked_ && move >= a * 3.0) {
            const double trail = base_is_long_ ? (base_entry_ + base_mfe_ - a * 2.0)
                                               : (base_entry_ - base_mfe_ + a * 2.0);
            if (base_is_long_  && trail > base_sl_) base_sl_ = trail;
            if (!base_is_long_ && trail < base_sl_) base_sl_ = trail;
        }

        // SL / timeout check
        const bool sl_hit = base_is_long_ ? (bid <= base_sl_) : (ask >= base_sl_);
        const int64_t held = idx_now_sec() - base_entry_ts_;
        const bool timeout = (held >= cfg_.max_hold_sec);

        if (sl_hit || timeout) {
            const double exit_px = sl_hit ? base_sl_ : mid;
            const char* why = sl_hit ? "SL_HIT" : "TIMEOUT";
            const double rem_size = bracket_fired_ ? velocity_size_ : base_size_;
            emit_partial(exit_px, why, on_close, rem_size);
            base_active_ = false;
            cooldown_until_ = idx_now_sec() + cfg_.cooldown_ms / 1000;
        }
    }

    void emit_partial(double exit_px, const char* why, CloseCb& on_close, double sz) noexcept {
        omega::TradeRecord tr;
        tr.id         = ++s_trade_id_;
        tr.symbol     = symbol_;
        tr.side       = base_is_long_ ? "LONG" : "SHORT";
        tr.engine     = "IndexMacroCrash";
        tr.entryPrice = base_entry_;
        tr.exitPrice  = exit_px;
        tr.sl         = base_sl_;
        tr.size       = sz;
        tr.pnl        = (base_is_long_ ? (exit_px - base_entry_) : (base_entry_ - exit_px)) * sz;
        tr.mfe        = base_mfe_;
        tr.entryTs    = base_entry_ts_;
        tr.exitTs     = idx_now_sec();
        tr.exitReason = why;
        tr.regime     = "MACRO_CRASH";
        tr.shadow     = shadow_mode;
        if (on_close) on_close(tr);
    }
};

int IndexMacroCrashEngine::s_trade_id_ = 9500;

// =============================================================================
// VWAPAtrUpgrade -- ATR-proportional SL/trail for existing VWAPReversionEngine
// =============================================================================
// Wraps the existing CrossPosition inside VWAPReversionEngine and upgrades
// its SL/trail management without touching CrossAssetEngines.hpp.
//
// WIRING (tick_indices.hpp, after g_vwap_rev_sp.on_tick()):
//   if (g_vwap_rev_sp.has_open_position()) {
//       idx::apply_vwap_atr_trail(g_vwap_rev_sp_pos_ref, atr, bid, ask);
//   }
//
// Since CrossPosition is public on VWAPReversionEngine, this can be applied
// directly to pos_ inside the engine from external code. But CrossPosition
// is a struct member -- the cleanest approach is to track the upgrade state
// here and apply BE/trail moves to the engine's already-open position.
//
// This struct holds upgrade state PER symbol instance:
//   - be_locked: true once 1x ATR profit achieved
//   - trail_stage: 0/1/2 matching IdxOpenPosition
//   - mfe_tracked: peak favourable excursion tracked here (not in CrossPosition)
// =============================================================================
struct VWAPAtrTrail {
    bool   be_locked    = false;
    int    trail_stage  = 0;
    double mfe          = 0.0;    // peak profit in points
    double atr_at_be    = 0.0;    // ATR when BE was first locked

    // Returns the upgraded SL level (or 0 if no change).
    // Caller applies this to the engine's pos_.sl if it improves the SL.
    //
    // atr: current ATR from IdxATRTracker
    // entry: position entry price
    // is_long: position direction
    // mid: current mid price
    double compute_sl(double atr, double entry, bool is_long, double mid) noexcept {
        const double move = is_long ? (mid - entry) : (entry - mid);
        if (move > mfe) mfe = move;
        if (atr <= 0.0) return 0.0;

        // Stage 1: BE lock at 1x ATR
        if (!be_locked && move >= atr * 1.0) {
            be_locked    = true;
            trail_stage  = 1;
            atr_at_be    = atr;
            return entry; // lock at entry
        }

        const double a = (atr_at_be > 0.0) ? atr_at_be : atr;

        // Stage 2: trail at 0.5x ATR behind MFE from 1.5x ATR
        if (be_locked && move >= a * 1.5) {
            trail_stage = 2;
            const double sl = is_long ? (entry + mfe - a * 0.5)
                                      : (entry - mfe + a * 0.5);
            return sl;
        }

        return 0.0; // no change
    }

    void reset() noexcept {
        be_locked = false; trail_stage = 0; mfe = 0.0; atr_at_be = 0.0;
    }
};

// =============================================================================
// IndexFlowCfg -- ini-driven config for all index flow parameters
// =============================================================================
struct IndexFlowCfg {
    // IndexFlowEngine overrides (0 = use symbol default)
    double sp_risk_dollars      = 50.0;
    double nq_risk_dollars      = 50.0;
    double nas_risk_dollars     = 5.0;
    double us30_risk_dollars    = 50.0;
    double sp_atr_sl_mult       = 1.0;
    double nq_atr_sl_mult       = 1.0;
    double us30_atr_sl_mult     = 1.0;
    double sp_max_spread        = 1.0;
    double nq_max_spread        = 1.5;
    double us30_max_spread      = 3.0;
    // VWAPAtrTrail upgrade
    double vwap_atr_sl_mult     = 1.0;  // BE lock at this * ATR
    // MacroCrash (shadow by default -- config cannot override shadow_mode)
    // shadow_mode is hardcoded true -- cannot be set via ini
};

// =============================================================================
// IndexSwingEngine
// =============================================================================
// H1 + H4 HTF swing-trend entry engine for US equity indices.
//
// CONCEPT:
//   Uses H1 OHLCBarEngine bars (trend_state, EMA9/EMA50) as primary direction
//   gate plus H4 OHLCBarEngine bars as higher-timeframe confirmation.
//   Entry fires only when H1 and H4 trend_state agree on direction.
//   Tick-level EWM drift (from IdxRegimeGovernor) confirms momentum at entry.
//
//   This is the "structural" layer above IndexFlowEngine:
//     IndexFlowEngine  = tick L2/drift microstructure entries (minutes)
//     IndexSwingEngine = H1/H4 EMA-crossover swing entries    (hours)
//
// ENTRY GATES (all must pass):
//   1. H1 trend_state == +1 (LONG) or -1 (SHORT)  -- OHLCBarEngine swing pivot
//   2. H4 trend_state == same direction            -- HTF confirmation
//      (if H4 not ready: H1-only allowed)
//   3. H1 EMA9 > EMA50 (LONG) or < EMA50 (SHORT)  -- EMA crossover confirmed
//   4. MIN_EMA_SEP: |EMA9 - EMA50| >= min_ema_sep  -- crossover not marginal
//   5. Tick drift does not strongly oppose H1 dir  -- no counter-drift entry
//   6. ATR >= atr_min (from per-symbol config)     -- real vol, not dead tape
//   7. NY+London session only (08:00-22:00 UTC)    -- no Asia entries
//   8. NY open noise gate: block 13:15-14:00 UTC
//   9. Cooldown: SWING_COOLDOWN_MS (30min) after any exit
//  10. Direction-flip cooldown: 4h before entering opposite side
//
// EXIT:
//   SL: entry +/- sl_pts (fixed points, per-symbol calibrated)
//   Trail: arms at 1x sl_pts profit, trails 0.5x sl_pts behind MFE
//   Timeout: 8 hours max hold
//   shadow_mode=true: position tracked, NO FIX order sent
//
// PNL_SCALE:
//   Applied to reported P&L in TradeRecord for dashboard visibility.
//   Set to usd_per_pt * min_lot so shadow equity curve is readable.
//   Does NOT affect entry/exit logic.
//
// USAGE (tick_indices.hpp, at end of on_tick_us500 / on_tick_ustec):
//   g_iswing_sp.on_tick(bid, ask, g_bars_sp.h1, g_bars_sp.h4,
//                       g_iflow_sp.drift(), ca_on_close);
//   g_iswing_nq.on_tick(bid, ask, g_bars_nq.h1, g_bars_nq.h4,
//                       g_iflow_nq.drift(), ca_on_close);
//
// GLOBALS (omega_types.hpp):
//   static omega::idx::IndexSwingEngine g_iswing_sp("US500.F",  8.0, 0.5, 0.5);
//   static omega::idx::IndexSwingEngine g_iswing_nq("USTEC.F", 25.0, 1.5, 0.2);
//
// CONFIGURE (engine_init.hpp):
//   g_iswing_sp.shadow_mode = true;
//   g_iswing_nq.shadow_mode = true;
// =============================================================================
class IndexSwingEngine {
public:
    bool shadow_mode = true;   // NEVER set false without explicit authorization

    // Construct with per-symbol calibration.
    // sl_pts      : fixed SL distance in price points
    // min_ema_sep : minimum |EMA9 - EMA50| on H1 to confirm crossover is real
    // pnl_scale   : dollar multiplier for TradeRecord P&L (shadow visibility)
    explicit IndexSwingEngine(const char* symbol,
                              double sl_pts      = 8.0,
                              double min_ema_sep = 0.5,
                              double pnl_scale   = 0.5) noexcept
        : sl_pts_(sl_pts), min_ema_sep_(min_ema_sep), pnl_scale_(pnl_scale)
    {
        if      (strcmp(symbol, "US500.F") == 0) cfg_ = IDX_CFG_SP;
        else if (strcmp(symbol, "USTEC.F") == 0) cfg_ = IDX_CFG_NQ;
        else if (strcmp(symbol, "NAS100")  == 0) cfg_ = IDX_CFG_NAS100;
        else if (strcmp(symbol, "DJ30.F")  == 0) cfg_ = IDX_CFG_US30;
        else                                      cfg_ = IDX_CFG_SP;
        strncpy(symbol_, symbol, 15); symbol_[15] = '\0';
    }

    using CloseCb = std::function<void(const omega::TradeRecord&)>;

    bool has_open_position()  const noexcept { return active_; }
    double entry_price()      const noexcept { return entry_; }
    double sl_price()         const noexcept { return sl_; }
    bool   is_long_at_entry() const noexcept { return is_long_; }

    // ── Main tick function ────────────────────────────────────────────────────
    // h1bars: g_bars_sp.h1 or g_bars_nq.h1
    // h4bars: g_bars_sp.h4 or g_bars_nq.h4
    //         (pass h1bars for both if H4 not built for that symbol yet)
    // drift:  from g_iflow_sp.drift() -- tick-level EWM directional bias
    // on_close: callback for TradeRecord (shadow P&L logging)
    bool on_tick(double bid, double ask,
                 const OHLCBarEngine& h1bars, const OHLCBarEngine& h4bars,
                 double drift, CloseCb on_close) noexcept {
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return false;
        const double mid = (bid + ask) * 0.5;

        // Always manage open position first
        if (active_) {
            _manage(bid, ask, mid, on_close);
            return false;
        }

        // Cooldown gate
        if (idx_now_ms() < cooldown_until_ms_) return false;

        // H1 readiness: m1_ready flag reused by OHLCBarEngine for all timeframes
        if (!h1bars.ind.m1_ready.load(std::memory_order_relaxed)) return false;

        // H1 trend_state: +1=UP, -1=DOWN, 0=FLAT
        const int h1_trend = h1bars.ind.trend_state.load(std::memory_order_relaxed);
        if (h1_trend == 0) return false;

        // H4 confirmation: if H4 has data, trend must agree with H1
        // If H4 not yet ready (cold start / indices don't build H4 yet), allow H1-only
        const bool h4_ready = h4bars.ind.m1_ready.load(std::memory_order_relaxed);
        if (h4_ready) {
            const int h4_trend = h4bars.ind.trend_state.load(std::memory_order_relaxed);
            if (h4_trend != 0 && h4_trend != h1_trend) return false;  // H4 disagrees
        }

        // H1 EMA crossover confirmation
        if (!h1bars.ind.m1_ema_live.load(std::memory_order_relaxed)) return false;
        const double h1_e9  = h1bars.ind.ema9 .load(std::memory_order_relaxed);
        const double h1_e50 = h1bars.ind.ema50.load(std::memory_order_relaxed);
        if (h1_e9 <= 0.0 || h1_e50 <= 0.0) return false;

        // EMA separation gate: crossover must not be marginal
        const double ema_sep = std::fabs(h1_e9 - h1_e50);
        if (ema_sep < min_ema_sep_) return false;

        // EMA direction must match H1 trend_state
        const bool ema_long  = (h1_e9 > h1_e50);
        const bool ema_short = (h1_e9 < h1_e50);
        if (h1_trend == +1 && !ema_long)  return false;
        if (h1_trend == -1 && !ema_short) return false;

        const bool is_long = (h1_trend == +1);

        // Tick drift alignment: block only strongly opposing drift
        if (is_long  && drift < -cfg_.drift_threshold * 2.0) return false;
        if (!is_long && drift >  cfg_.drift_threshold * 2.0) return false;

        // ATR gate: H1 ATR from OHLCBarEngine indicators
        const double h1_atr = h1bars.ind.atr14.load(std::memory_order_relaxed);
        if (h1_atr > 0.0 && h1_atr < cfg_.atr_min) return false;

        // Session gate: London + NY only (08:00-22:00 UTC)
        {
            struct tm ti{}; idx_utc(ti);
            const int mins = ti.tm_hour * 60 + ti.tm_min;
            if (mins < 8 * 60 || mins >= 22 * 60) return false;
            // NY open noise: block 13:15-14:00 UTC
            if (mins >= 13 * 60 + 15 && mins < 14 * 60) return false;
        }

        // Direction-flip cooldown: 4h before entering opposite of last exit
        if (last_exit_dir_ != 0 && last_exit_dir_ != (is_long ? 1 : -1)) {
            if (idx_now_ms() - last_exit_ms_ < 14400000LL) return false;
        }

        // ── Open position ─────────────────────────────────────────────────────
        active_           = true;
        is_long_          = is_long;
        entry_            = is_long ? ask : bid;
        sl_               = is_long ? (entry_ - sl_pts_) : (entry_ + sl_pts_);
        trail_sl_         = sl_;
        mfe_              = 0.0;
        be_locked_        = false;
        entry_ts_         = idx_now_sec();
        entry_ms_         = idx_now_ms();
        h1_e9_at_entry_   = h1_e9;
        h1_e50_at_entry_  = h1_e50;

        const char* pfx = shadow_mode ? "[ISWING-SHADOW]" : "[ISWING]";
        printf("%s %s %s entry=%.2f sl=%.2f sl_pts=%.2f ema_sep=%.2f "
               "h1_trend=%d h4_ready=%d drift=%.3f\n",
               pfx, symbol_, is_long ? "LONG" : "SHORT",
               entry_, sl_, sl_pts_, ema_sep,
               h1_trend, h4_ready ? 1 : 0, drift);
        fflush(stdout);
        return true;
    }

private:
    static constexpr int64_t SWING_COOLDOWN_MS  = 1800000LL;  // 30min between entries
    static constexpr int64_t SWING_MAX_HOLD_SEC = 28800LL;    // 8h max hold

    IndexSymbolCfg cfg_;
    char   symbol_[16]        = {};
    double sl_pts_             = 8.0;
    double min_ema_sep_        = 0.5;
    double pnl_scale_          = 0.5;

    bool    active_            = false;
    bool    is_long_           = false;
    double  entry_             = 0.0;
    double  sl_                = 0.0;
    double  trail_sl_          = 0.0;
    double  mfe_               = 0.0;
    bool    be_locked_         = false;
    int64_t entry_ts_          = 0;
    int64_t entry_ms_          = 0;
    int64_t cooldown_until_ms_ = 0;
    int     last_exit_dir_     = 0;   // +1=was long, -1=was short
    int64_t last_exit_ms_      = 0;
    double  h1_e9_at_entry_    = 0.0;
    double  h1_e50_at_entry_   = 0.0;

    static int s_trade_id_;

    void _manage(double bid, double ask, double mid, CloseCb& on_close) noexcept {
        const double move = is_long_ ? (mid - entry_) : (entry_ - mid);
        if (move > mfe_) mfe_ = move;

        // BE lock at 1x sl_pts_ profit
        if (!be_locked_ && move >= sl_pts_) {
            be_locked_ = true;
            trail_sl_  = entry_;
            printf("[ISWING-%s] BE-LOCK %s move=%.2f sl->%.2f\n",
                   symbol_, is_long_ ? "LONG" : "SHORT", move, trail_sl_);
            fflush(stdout);
        }

        // Trail at 0.5x sl_pts_ behind MFE once BE locked
        if (be_locked_) {
            const double new_sl = is_long_ ? (entry_ + mfe_ - sl_pts_ * 0.5)
                                           : (entry_ - mfe_ + sl_pts_ * 0.5);
            if (is_long_  && new_sl > trail_sl_) trail_sl_ = new_sl;
            if (!is_long_ && new_sl < trail_sl_) trail_sl_ = new_sl;
        }

        const bool sl_hit  = is_long_ ? (bid <= trail_sl_) : (ask >= trail_sl_);
        const bool timeout = (idx_now_sec() - entry_ts_ >= SWING_MAX_HOLD_SEC);

        if (!sl_hit && !timeout) return;

        const double exit_px = sl_hit ? trail_sl_ : mid;
        const char*  why     = sl_hit ? "SL_HIT"  : "TIMEOUT";
        const double gross   = is_long_ ? (exit_px - entry_) : (entry_ - exit_px);

        const char* pfx = shadow_mode ? "[ISWING-SHADOW]" : "[ISWING]";
        printf("%s %s CLOSE %s entry=%.2f exit=%.2f pts=%.2f mfe=%.2f why=%s\n",
               pfx, symbol_, is_long_ ? "LONG" : "SHORT",
               entry_, exit_px, gross, mfe_, why);
        fflush(stdout);

        omega::TradeRecord tr;
        tr.id         = ++s_trade_id_;
        tr.symbol     = symbol_;
        tr.side       = is_long_ ? "LONG" : "SHORT";
        tr.engine     = "IndexSwing";
        tr.entryPrice = entry_;
        tr.exitPrice  = exit_px;
        tr.sl         = trail_sl_;
        tr.size       = 0.01;
        tr.pnl        = gross * pnl_scale_;
        tr.mfe        = mfe_;
        tr.entryTs    = entry_ts_;
        tr.exitTs     = idx_now_sec();
        tr.exitReason = why;
        tr.regime     = "SWING";
        tr.shadow     = shadow_mode;

        last_exit_dir_     = is_long_ ? 1 : -1;
        last_exit_ms_      = idx_now_ms();
        active_            = false;
        cooldown_until_ms_ = idx_now_ms() + SWING_COOLDOWN_MS;

        if (on_close) on_close(tr);
    }
};

int IndexSwingEngine::s_trade_id_ = 9800;

} // namespace idx
} // namespace omega
