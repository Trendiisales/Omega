#pragma once
//  ADVERSE-PROTECTION: has LOSS_CUT_PCT=0.07 (live via pos_.manage L654) + staircase BE-lock@1xATR -- but CULLED DEAD 2026-05-01 (CULL_LEDGER: PF0.08 n17), protection moot, re-enable blocked (backfill S-2026-06-24n)
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
#include "OpenPositionRegistry.hpp"  // omega::PositionSnapshot (persist_save/restore)
#include "OHLCBarEngine.hpp"
#include "OmegaCostGuard.hpp"     // 2026-05-12 cost gate -- see pos_.open(sig) entry below
#include "IndexRiskGate.hpp"      // S44 portfolio VIX risk-off gate (entry-only)
#include "L2Globals.hpp"          // 2026-05-30: AtomicL2 + g_l2_<sym> globals

namespace omega {
namespace idx {

// =============================================================================
// Helpers
// =============================================================================
// Test-clock override for backtest replay. When > 0, idx_now_ms() and
// idx_now_sec() return this value instead of wall-clock. Production never
// sets this -- behavior is unchanged. Backtest harnesses (e.g.
// backtest/IndexBacktest.cpp) call set_idx_test_clock_ms() per tick from the
// data's tick timestamp so the engines' "now" matches tick-time, not
// wall-clock. Fixes the entry_ts / cooldown / hold-time class of bugs
// that prevent backtest replay from exiting positions.
//
// 2026-05-03: introduced after IndexBacktest revealed that
// pos.entry_ts > now_s (entry_ts wall-clock today, now_s historical April)
// caused trail_arm_ok to never satisfy and BE-lock to never engage,
// leaving positions open for the entire backtest.
inline int64_t s_idx_test_clock_ms = 0;

inline void set_idx_test_clock_ms(int64_t ms) noexcept { s_idx_test_clock_ms = ms; }
inline int64_t get_idx_test_clock_ms() noexcept { return s_idx_test_clock_ms; }

static inline int64_t idx_now_ms() noexcept {
    if (s_idx_test_clock_ms > 0) return s_idx_test_clock_ms;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static inline int64_t idx_now_sec() noexcept {
    if (s_idx_test_clock_ms > 0) return s_idx_test_clock_ms / 1000;
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static inline void idx_utc(struct tm& ti) noexcept {
    // PATH-A-DEBUG-2026-05-08 BUGFIX: honor the test clock for backtest replay.
    // Previously this used std::chrono::system_clock::now() unconditionally,
    // which made the engine's session-window gate use wall-clock-at-run-time
    // instead of the historical tick timestamp -- silently invalidating
    // every session-aware backtest. set_idx_test_clock_ms() is set per-tick
    // by the harness; idx_now_ms() respects it; idx_utc() now does too.
    const time_t t = static_cast<time_t>(idx_now_ms() / 1000);
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
    double drift_persist_ticks    = 12;     // ticks drift must hold before entry (S14 2026-04-24: reduced from 20 — reactive tightening combined with other gates produced 0 signals across 20-day archive)
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
    // S63 2026-05-13: loss_cut_pct optional param -- VWR-pattern cold-loss cut.
    //   Defaults to 0.0 (disabled) for backward compat with any caller that
    //   didn't update. IndexFlowEngine::on_tick passes its LOSS_CUT_PCT.
    bool manage(double bid, double ask, double atr, int max_hold_sec,
                const char* regime, CloseCb& on_close,
                double loss_cut_pct = 0.0) noexcept {
        if (!active) return false;
        const double mid   = (bid + ask) * 0.5;
        const double move  = is_long ? (mid - entry) : (entry - mid);
        if (move > mfe) mfe = move;
        if (-move > mae) mae = -move;

        // S63 2026-05-13 VWR-pattern cold-loss cut. Runs BEFORE staircase
        // trail / SL / timeout so cold-loss outliers are caught early.
        // BE_RATCHET intentionally omitted -- staircase Stage 1 (BE@1xATR)
        // handles giveback prevention already.
        if (loss_cut_pct > 0.0 && entry > 0.0) {
            const double adverse       = -move;
            const double loss_cut_dist = entry * loss_cut_pct / 100.0;
            if (adverse >= loss_cut_dist) {
                const double exit_px = is_long ? bid : ask;
                emit(exit_px, "LOSS_CUT", regime, on_close);
                return true;
            }
        }

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
            // 2026-05-13 (part L): VWR-pattern -- also exempt small winners
            // whose trail SL hasn't yet locked to BE.
            const double cur_move = is_long ? (mid - entry) : (entry - mid);
            if (trail_profit || cur_move > 0.0) {
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

    // S63 2026-05-13 VWR-pattern cold-loss cut (LOSS_CUT only).
    //   The staircase ATR trail (Stage 1 BE @ 1xATR, Stage 2/3 tightened
    //   trail) covers giveback well already, so we add ONLY the cold-loss
    //   phase here. Passed to pos_.manage() as a default-valued parameter
    //   to avoid breaking IdxOpenPosition's signature for other callers.
    //   Default 0.07 = ~5.2pt cut at US500@7400 entry. Set to 0.0 to disable.
    double LOSS_CUT_PCT  = 0.07;

    // S-2026-07-22c CULL toggle (operator live-only order): PF0.15 −$112 live shadow;
    // the audit verdict said DISABLE. Set false in engine_init ⇒ on_tick inert.
    bool enabled = true;

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
    // S66-followup-2 2026-05-14 (part L): read-only accessor for the open
    //   position. Used by engine_init.hpp's GUI position-source registration
    //   to expose IndexFlow trades on /api/v1/omega/positions while a trade
    //   is in flight. IdxOpenPosition (declared above, L362) is already a
    //   public class with public fields, so the accessor doesn't expose any
    //   new mutation surface beyond what `set_shadow_mode()` already does.
    //   Const-ref so callers can't accidentally mutate state.
    const IdxOpenPosition& pos() const noexcept { return pos_; }
    // ISSUE-5: proxy for per-engine shadow_mode control from engine_init.hpp.
    // shadow_mode lives on the private IdxOpenPosition pos_; this setter lets
    // external init code (engine_init.hpp kShadowDefault wiring) flip the flag
    // without breaking encapsulation.
    void   set_shadow_mode(bool b) noexcept { pos_.shadow_mode = b; }

    // ── PATH-A-DEBUG-2026-05-08 ─────────────────────────────────────────────
    // Temporary instrumentation: counts which return path on_tick takes per
    // tick. Added 2026-05-08 to debug 0-trades on NSXUSD HistData (S20 Path A).
    // Purely additive -- no logic changes. Remove or guard with
    // #ifdef OMEGA_PATH_A_DEBUG before merging if desired. Search for
    // "PATH-A-DEBUG-2026-05-08" to find every modification site.
    // ────────────────────────────────────────────────────────────────────────
    struct DebugStats {
        int64_t ret_invalid_data    = 0;  // line 560 bid/ask invalid
        int64_t ret_pos_active      = 0;  // line 592 already in trade (manage path)
        int64_t ret_cooldown        = 0;  // line 597 cfg_.cooldown_ms not yet expired
        int64_t ret_no_can_enter    = 0;  // line 601 cross-engine gate said no
        int64_t ret_atr_not_ready   = 0;  // line 602 atr_tracker count < 50
        int64_t ret_min_ticks       = 0;  // line 603 tick_count < min_entry_ticks
        int64_t ret_atr_below       = 0;  // line 609 atr < atr_min
        int64_t ret_spread_too_wide = 0;  // line 612 spread > max_spread
        int64_t ret_dead_zone       = 0;  // line 622 22-08 UTC dead zone
        int64_t ret_ny_open_noise   = 0;  // line 637 13:30-14:00 UTC
        int64_t ret_sl_cooldown     = 0;  // line 643 90s post-SL block
        int64_t ret_no_signal       = 0;  // line 676 no drift/L2 signal
        int64_t ret_long_mom        = 0;  // line 683 momentum opposes long
        int64_t ret_short_mom       = 0;  // line 684 momentum opposes short
        int64_t ret_chop_guard      = 0;  // line 692 chop guard fired
        int64_t entries             = 0;  // line 740 entry built and returned
    } debug_stats;
    const DebugStats& get_debug_stats() const noexcept { return debug_stats; }

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
        if (!enabled) return {};   // S-2026-07-22c culled via engine_init
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) {
            ++debug_stats.ret_invalid_data;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

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
                                            "IFLOW", cb_wrap,
                                            LOSS_CUT_PCT);
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
            ++debug_stats.ret_pos_active;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

        // Cooldown gate
        if (phase == Phase::COOLDOWN) {
            if (idx_now_ms() < cooldown_until_ms_) {
                ++debug_stats.ret_cooldown;  // PATH-A-DEBUG-2026-05-08
                return {};
            }
            phase = Phase::IDLE;
        }

        if (!can_enter) {
            ++debug_stats.ret_no_can_enter;  // PATH-A-DEBUG-2026-05-08
            return {};
        }
        if (!atr_tracker_.ready()) {
            ++debug_stats.ret_atr_not_ready;  // PATH-A-DEBUG-2026-05-08
            return {};
        }
        if (tick_count_ < cfg_.min_entry_ticks) {
            ++debug_stats.ret_min_ticks;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

        const double atr    = atr_tracker_.atr();
        const double d      = regime_.drift();

        // ATR quality gate -- no entry if market is dead tape
        if (atr < cfg_.atr_min) {
            ++debug_stats.ret_atr_below;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

        // Spread gate
        if (spread > cfg_.max_spread) {
            ++debug_stats.ret_spread_too_wide;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

        // Session gate for US indices: block Asia session (22:00-13:30 UTC)
        // US equity indices have essentially no volume during Asian hours.
        // Only fire during London (08:00-13:30 UTC) and NY (13:30-22:00 UTC).
        {
            struct tm ti{}; idx_utc(ti);
            const int mins = ti.tm_hour * 60 + ti.tm_min;
            // Block 22:00-08:00 UTC for US indices (Asia + dead zone)
            const bool dead = (mins >= 22 * 60) || (mins < 8 * 60);
            if (dead) {
                ++debug_stats.ret_dead_zone;  // PATH-A-DEBUG-2026-05-08
                return {};
            }

            // NY open noise gate: 13:15-14:00 UTC (extended from 13:45)
            // Root cause of 13-trade 26%WR cluster: NY opens at 13:30 UTC with
            // violent bid/ask oscillation. 13 IndexFlow trades in 30 min including
            // direction flips within seconds, 5 zero-gross SL trades.
            // Total damage: -$56.28 in 30 minutes.
            // Extended 2026-04-09: trades at 13:45,13:46,13:47,13:49 all SL_HIT
            // immediately after gate lifted -- NY volatility persists past 13:45.
            // Block 13:15-14:00 UTC (45 min total covers full NY open noise window).
            // S14 2026-04-24: reverted to original 13:30-14:00 (30 min) window.
            // The 13:15-14:00 extension was reactive tightening after Apr 9
            // SL_HIT cluster. Combined with other gates (ATR×1.5, drift_persist=20,
            // SL cooldown) this over-filtered the engine to 0 signals in 20 days.
            const bool ny_open_noise = (mins >= 13 * 60 + 30) && (mins < 14 * 60 + 0);
            if (ny_open_noise) {
                ++debug_stats.ret_ny_open_noise;  // PATH-A-DEBUG-2026-05-08
                return {};
            }
        }

        // SL cooldown gate: 90s after any SL_HIT before re-entering
        // Prevents immediate re-entry after a stop -- the chop pattern
        // that produced rapid direction flips at NY open.
        if (idx_now_ms() < m_sl_cooldown_until_ms_) {
            ++debug_stats.ret_sl_cooldown;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

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
            ++debug_stats.ret_no_signal;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

        // Momentum confirmation: last 12 ticks must show directional move
        // (prevents stale drift from firing on a paused tape)
        const double momentum = mid_buf_[(mid_buf_head_ - 1 + BUF_SZ) % BUF_SZ]
                              - mid_buf_[(mid_buf_head_ - 13 + BUF_SZ * 4) % BUF_SZ];
        if (signal_long  && momentum <= 0.0) {
            ++debug_stats.ret_long_mom;  // PATH-A-DEBUG-2026-05-08
            return {};
        }
        if (signal_short && momentum >= 0.0) {
            ++debug_stats.ret_short_mom;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

        // Chop guard: if drift has oscillated (high range / low net), block entry.
        // Prevents straddle-chop patterns where drift spikes but doesn't sustain.
        // drift_range > 4x drift_threshold with low net = chop.
        if (drift_range_ > cfg_.drift_threshold * 4.0 &&
            std::fabs(d) < cfg_.drift_threshold * 1.5) {
            ++debug_stats.ret_chop_guard;  // PATH-A-DEBUG-2026-05-08
            return {};
        }

        // Min confidence gate: ATR floor already enforced at line 590 (`atr < cfg_.atr_min`).
        // S14 2026-04-24: removed 1.5x multiplier — it stacked 50% extra tightening
        // on top of the already-calibrated atr_min floor. Combined with drift_persist=20
        // and 45-min NY block, the engine fired 0 trades across 20 days of tick data.
        // The hard floor at line 590 is the correct dead-tape filter; this multiplier
        // was reactive tightening that over-filtered real signals.

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

        // 2026-05-12 ExecutionCostGuard belt-and-suspenders gate.
        //   Gates on the initial 3x-SL TP target (sig.tp - sig.entry abs).
        //   On block we plain-return; the engine re-evaluates next tick
        //   when conditions may differ (drift / regime).
        if (!ExecutionCostGuard::is_viable(symbol_, spread, tp_dist, lot, 1.5)) {
            ++debug_stats.ret_chop_guard;  // reuse existing counter -- block is cost-related
            static int64_t s_cost_iflow = 0;
            const int64_t now_ms_iflow = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms_iflow - s_cost_iflow > 60000) {
                s_cost_iflow = now_ms_iflow;
                printf("[IFLOW-%s] BLOCKED cost_gate tp=%.2f spread=%.4f lot=%.3f\n",
                       symbol_, tp_dist, spread, lot);
                fflush(stdout);
            }
            return {};
        }

        if (omega::index_risk_off()) return {};   // S44 portfolio VIX risk-off: no new entry

        pos_.open(sig);
        phase = Phase::LIVE;

        printf("[IFLOW-%s] %s entry=%.2f sl=%.2f tp=%.2f atr=%.2f lot=%.3f "
               "drift=%.2f l2_live=%d\n",
               symbol_, is_long ? "LONG" : "SHORT",
               sig.entry, sl, tp, atr, lot, d, l2_live ? 1 : 0);
        fflush(stdout);

        ++debug_stats.entries;  // PATH-A-DEBUG-2026-05-08
        return sig;
    }

    // ── S-2026-06-03: open-position persistence (resume in-flight trade across
    //   restart/deploy). pos_ (IdxOpenPosition) is private; these two public
    //   methods save/restore full state for the PositionPersistence wiring.
    //   IdxOpenPosition shares the same field names as CrossPosition for these.
    bool persist_save(const char* engine, const char* sym,
                      omega::PositionSnapshot& out) const {
        if (!pos_.active) return false;
        out.engine = engine; out.symbol = sym;
        out.side = pos_.is_long ? "LONG" : "SHORT";
        out.size = pos_.size; out.entry = pos_.entry; out.sl = pos_.sl; out.tp = pos_.tp;
        out.entry_ts = pos_.entry_ts;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos_.active = true; pos_.is_long = (ps.side == "LONG");
        pos_.entry = ps.entry; pos_.sl = ps.sl; pos_.tp = ps.tp; pos_.size = ps.size;
        pos_.entry_ts = ps.entry_ts;
        return true;
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

    // S63 2026-05-13 VWR-pattern in-flight protection (LOSS_CUT + BE_RATCHET).
    //   Macro-crash fade is mean-reversion style with a 60-min timeout
    //   profile (cfg_.max_hold_sec). Defaults use VWR's index-tuned values
    //   (US500 @ 7400, 0.08 -> 5.92pt LOSS_CUT). Override per-instance in
    //   engine_init.hpp if needed.
    double LOSS_CUT_PCT  = 0.08;
    double BE_ARM_PCT    = 0.05;
    double BE_BUFFER_PCT = 0.02;

    // S-2026-07-22c CULL toggle (operator live-only order): no backtest cert exists
    // for the crash-fade. Set false in engine_init ⇒ on_tick inert.
    bool enabled = true;

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

    // ── S66-followup-2 2026-05-14a: read-only GUI accessors ─────────────────
    // Used by engine_init.hpp's GUI position-source registration to expose
    // macro-crash trades on /api/v1/omega/positions while in flight. The
    // engine state lives in scattered private fields (no single "open
    // position" struct like IndexFlowEngine::pos_), so each field gets its
    // own const accessor.
    //
    // Notes:
    //   * is_long / entry / sl / mfe are direct passes through.
    //   * The engine does NOT track MAE (no base_mae_ member). The GUI
    //     position-source synthesises mae = 0 on the snapshot side.
    //   * size() returns the CURRENT open size: after the 30% bracket-floor
    //     leg fires (bracket_fired_ = true), only velocity_size_ remains
    //     open. Before the bracket fires the full base_size_ is in play.
    //     This matches the actual lot the engine would close in
    //     manage_position()'s emit_partial() call.
    //   * All accessors return 0/false sentinels when no position is open;
    //     callers MUST guard via has_open_position() (mirrors the
    //     IndexFlowEngine::pos() contract — the pos struct lingers with
    //     active=false after a close).
    //
    // Pending GUI surface: tp is not exposed because there's no single TP
    //   value — the engine has a bracket target (bracket_target_, 30% of
    //   position) plus a velocity trail (50/70% trailing 2x ATR behind MFE).
    //   Neither maps cleanly to PositionSnapshot::tp. Same shape limitation
    //   as the GoldEngineStack legs_ pending audit (handoff §"What did NOT
    //   land").
    bool   is_long()  const noexcept { return base_is_long_; }
    double entry()    const noexcept { return base_entry_; }
    double sl()       const noexcept { return base_sl_; }
    double mfe()      const noexcept { return base_mfe_; }
    double size()     const noexcept {
        return bracket_fired_ ? velocity_size_ : base_size_;
    }
    // NOTE on shadow_mode: this class deliberately exposes `shadow_mode` as a
    // public member (L964) with a "NEVER set false without authorization"
    // comment. A set_shadow_mode() proxy is intentionally NOT provided here
    // (in contrast to IndexFlowEngine::set_shadow_mode() at L585, where the
    // flag lives on private pos_) — the public member is already directly
    // assignable, and adding a setter would obscure the "do not flip"
    // hardening intent.

    // vol_ratio: recent_range / ewm_baseline (same computation as GoldEngineStack)
    // drift: from IndexFlowEngine::drift()
    // atr: from IndexFlowEngine::atr()
    void on_tick(double bid, double ask, double atr, double drift,
                 double vol_ratio, bool trend_regime, CloseCb on_close,
                 bool can_enter = true) noexcept {
        if (!enabled) return;   // S-2026-07-22c culled via engine_init
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

    // ── S-2026-06-03: open-position persistence. This engine has NO single
    //   pos_ struct — state is in scattered base_*_ fields plus a bracket-floor
    //   leg + velocity trail. We save the core LONG/SHORT/entry/sl/size/entry_ts
    //   and (best-effort) carry bracket_target_ via PositionSnapshot::tp so the
    //   30% floor target survives a restart. On restore we re-derive the
    //   bracket/velocity split (same 30/70 as the entry path); base_atr_ and the
    //   be_locked_/bracket_fired_ trail-stage flags are NOT round-tripped (not in
    //   PositionSnapshot) — restore conservatively resumes pre-bracket/pre-BE so
    //   the next tick re-arms the trail rather than skipping protection.
    bool persist_save(const char* engine, const char* sym,
                      omega::PositionSnapshot& out) const {
        if (!base_active_) return false;
        out.engine = engine; out.symbol = sym;
        out.side = base_is_long_ ? "LONG" : "SHORT";
        out.size = base_size_; out.entry = base_entry_; out.sl = base_sl_;
        out.tp = bracket_target_;
        out.entry_ts = base_entry_ts_;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        base_active_   = true;
        base_is_long_  = (ps.side == "LONG");
        base_entry_    = ps.entry;
        base_sl_       = ps.sl;
        base_size_     = ps.size;
        base_entry_ts_ = ps.entry_ts;
        base_mfe_      = 0.0;
        base_atr_      = 0.0;   // re-learned from live ATR on next tick
        be_locked_     = false;
        bracket_fired_ = false;
        bracket_target_ = ps.tp;
        bracket_size_  = base_size_ * 0.30;
        velocity_size_ = base_size_ * 0.70;
        return true;
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

        // S63 2026-05-13 VWR-pattern in-flight protection. Runs BEFORE the
        // staircase BE/trail so cold-loss / giveback cuts take priority.
        // Phase 1: BE_RATCHET
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && base_entry_ > 0.0) {
            const double arm_pts    = base_entry_ * BE_ARM_PCT    / 100.0;
            const double buffer_pts = base_entry_ * BE_BUFFER_PCT / 100.0;
            if (base_mfe_ >= arm_pts && move <= buffer_pts) {
                const double exit_px = base_is_long_ ? bid : ask;
                const double rem_size = bracket_fired_ ? velocity_size_ : base_size_;
                emit_partial(exit_px, "BE_CUT", on_close, rem_size);
                base_active_ = false;
                cooldown_until_ = idx_now_sec() + cfg_.cooldown_ms / 1000;
                return;
            }
        }
        // Phase 2: LOSS_CUT
        if (LOSS_CUT_PCT > 0.0 && base_entry_ > 0.0) {
            const double adverse       = -move;
            const double loss_cut_dist = base_entry_ * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                const double exit_px = base_is_long_ ? bid : ask;
                const double rem_size = bracket_fired_ ? velocity_size_ : base_size_;
                emit_partial(exit_px, "LOSS_CUT", on_close, rem_size);
                base_active_ = false;
                cooldown_until_ = idx_now_sec() + cfg_.cooldown_ms / 1000;
                return;
            }
        }

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
        // 2026-05-13 (part L): VWR-pattern winner exemption.
        const double cur_move_v = base_is_long_
            ? (mid - base_entry_)
            : (base_entry_ - mid);
        const bool timeout = (held >= cfg_.max_hold_sec) && (cur_move_v <= 0.0);

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
    bool enabled     = true;   // set false in engine_init.hpp to cull (no entries/mgmt)

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
        if (!enabled) return false;   // culled 2026-06-02 (net-neg shadow, see engine_init)
        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return false;
        const double mid = (bid + ask) * 0.5;

        // Push L2 sample into ring buffer on every tick (incl. while in pos).
        // Read from per-symbol L2 global; nullptr means no L2 source.
        {
            const ::AtomicL2* l2 = nullptr;
            if      (strcmp(symbol_, "US500.F") == 0) l2 = &g_l2_sp;
            else if (strcmp(symbol_, "USTEC.F") == 0) l2 = &g_l2_nq;
            else if (strcmp(symbol_, "NAS100")  == 0) l2 = &g_l2_nas;
            if (l2) {
                l2_mic_buf_[l2_buf_head_] = l2->microprice_bias.load(std::memory_order_relaxed);
                l2_imb_buf_[l2_buf_head_] = l2->imbalance.load(std::memory_order_relaxed);
                l2_buf_head_ = (l2_buf_head_ + 1) % L2_BUF_N;
                if (l2_buf_count_ < L2_BUF_N) ++l2_buf_count_;
            }
        }

        // Always manage open position first
        if (active_) {
            _manage(bid, ask, mid, on_close);
            return false;
        }

        // L2-confirmed entry delay -- advance pending state machine.
        if (pending_entry_) {
            _check_pending(bid, ask);
            if (active_) return true;  // confirmed -> opened this tick
            if (pending_entry_) return false;  // still waiting
            // else: cancelled, fall through (cooldown applies)
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

        // EMA separation gate: crossover must not be marginal.
        // 2026-05-30 (S39): the fixed absolute floor (min_ema_sep_ = 0.5 SP /
        // 1.5 NQ) is ~0.006% of price on indices priced in the thousands -- a
        // near-zero band that admits whippy near-marginal crossovers and is the
        // root of the 46% WR / PF~0.96 breakeven on the shadow ledger
        // (ANALYSIS_S39_LIVE_EDGE_AUDIT). Replace with an ATR-relative floor so
        // the gate auto-scales across instrument price AND volatility regime
        // (calibration-drift-proof, per memory:feedback-abs-pt-calibration):
        // require the EMA9-EMA50 gap to be at least EMA_SEP_ATR_FRAC of one H1
        // ATR -- i.e. a real, non-marginal separation -- with the old absolute
        // kept only as a hard floor. SHADOW-VALIDATION CANDIDATE: must show WR
        // improvement over the next shadow window before any promotion.
        const double ema_sep = std::fabs(h1_e9 - h1_e50);
        const double sep_atr = h1bars.ind.atr14.load(std::memory_order_relaxed);
        const double ema_sep_floor = (sep_atr > 0.0)
            ? std::max(min_ema_sep_, EMA_SEP_ATR_FRAC * sep_atr)
            : min_ema_sep_;
        if (ema_sep < ema_sep_floor) return false;

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

        // ── L2 alignment gate (added 2026-05-30 per 10-trade analysis) ──────
        // Sample showed ALIGNED entries WR 67% vs OPPOSED 29%. Block trades
        // where L2 microprice opposes direction over the 30-tick avg PRIOR
        // to the entry tick (smoother than single-tick: replay showed a
        // +$346 winner had mic_e=-0.30 spike but mic_p30=-0.03 clean).
        //   USTEC: avg_mic < -0.10  -> bearish flow -> block LONG
        //   US500: avg_mic < -0.05  -> tighter threshold (less L2 noise on SP)
        // imbalance only used when not stuck at 0.5 (per
        // memory:feedback-l2-data-quality USTEC imb is ~99% stuck at 0.5).
        if (l2_buf_count_ >= 10) {  // need 10+ samples to trust avg
            double mic_block = 0.05;
            if      (strcmp(symbol_, "USTEC.F") == 0) mic_block = 0.10;
            else if (strcmp(symbol_, "NAS100")  == 0) mic_block = 0.10;
            double mic_sum = 0, imb_sum = 0;
            for (int k = 0; k < l2_buf_count_; ++k) {
                mic_sum += l2_mic_buf_[k];
                imb_sum += l2_imb_buf_[k];
            }
            const double mic_avg = mic_sum / l2_buf_count_;
            const double imb_avg = imb_sum / l2_buf_count_;
            if (is_long  && mic_avg < -mic_block) {
                printf("[ISWING-%s] L2-GATE-BLOCK LONG avg_mic=%.4f thr=-%.3f n=%d\n",
                       symbol_, mic_avg, mic_block, l2_buf_count_);
                fflush(stdout);
                return false;
            }
            if (!is_long && mic_avg > +mic_block) {
                printf("[ISWING-%s] L2-GATE-BLOCK SHORT avg_mic=%.4f thr=+%.3f n=%d\n",
                       symbol_, mic_avg, mic_block, l2_buf_count_);
                fflush(stdout);
                return false;
            }
            if (std::fabs(imb_avg - 0.5) > 0.01) {
                if (is_long  && imb_avg < 0.45) {
                    printf("[ISWING-%s] L2-GATE-BLOCK LONG avg_imb=%.3f n=%d\n",
                           symbol_, imb_avg, l2_buf_count_);
                    fflush(stdout);
                    return false;
                }
                if (!is_long && imb_avg > 0.55) {
                    printf("[ISWING-%s] L2-GATE-BLOCK SHORT avg_imb=%.3f n=%d\n",
                           symbol_, imb_avg, l2_buf_count_);
                    fflush(stdout);
                    return false;
                }
            }
        }

        // ── L2-confirmed entry delay (2026-05-30) ──────────────────────────
        // Don't open immediately. Queue pending entry; require N consecutive
        // ticks of confirming microprice alignment before opening. Skips
        // signals where book pressure briefly aligns then immediately
        // reverses (the "false start" pattern that produces 3-5min losers).
        if (!pending_entry_) {
            pending_entry_   = true;
            pending_is_long_ = is_long;
            pending_ticks_   = 0;
            h1_e9_at_entry_  = h1_e9;
            h1_e50_at_entry_ = h1_e50;
            const char* pfx = shadow_mode ? "[ISWING-SHADOW]" : "[ISWING]";
            printf("%s %s SIGNAL-PENDING %s ema_sep=%.2f h1_trend=%d h4_ready=%d "
                   "drift=%.3f waiting=%d ticks\n",
                   pfx, symbol_, is_long ? "LONG" : "SHORT",
                   ema_sep, h1_trend, h4_ready ? 1 : 0, drift,
                   (int)PENDING_CONFIRM_TICKS);
            fflush(stdout);
        }
        return false;  // entry happens via _check_pending in subsequent ticks
    }

    // Called every on_tick before entry/manage to advance pending state.
    // Opens position when L2 confirms or cancels when max ticks exceeded.
    void _check_pending(double bid, double ask) noexcept {
        if (!pending_entry_) return;
        ++pending_ticks_;
        if (l2_buf_count_ < PENDING_CONFIRM_TICKS) return;  // need data
        // Compute mic_avg over the most recent PENDING_CONFIRM_TICKS samples
        // in the ring buffer.
        double mic_sum = 0;
        const int N = std::min(l2_buf_count_, (int)PENDING_CONFIRM_TICKS);
        int idx = (l2_buf_head_ - 1 + L2_BUF_N) % L2_BUF_N;
        for (int k = 0; k < N; ++k) {
            mic_sum += l2_mic_buf_[idx];
            idx = (idx - 1 + L2_BUF_N) % L2_BUF_N;
        }
        const double mic_avg = mic_sum / N;
        const bool confirmed = pending_is_long_
                             ? (mic_avg >= PENDING_CONFIRM_MIC)
                             : (mic_avg <= -PENDING_CONFIRM_MIC);
        if (confirmed) {
            // L2-aware sizing: scale base lot by |mic_avg| * factor, clamped.
            // Strong confirmation -> larger size, weak -> smaller.
            const double size_mult = std::clamp(std::fabs(mic_avg) * 10.0, 0.5, 2.0);
            size_mult_at_entry_ = size_mult;
            // Open
            active_           = true;
            is_long_          = pending_is_long_;
            entry_            = is_long_ ? ask : bid;
            sl_               = is_long_ ? (entry_ - sl_pts_) : (entry_ + sl_pts_);
            trail_sl_         = sl_;
            mfe_              = 0.0;
            be_locked_        = false;
            entry_ts_         = idx_now_sec();
            entry_ms_         = idx_now_ms();
            const char* pfx = shadow_mode ? "[ISWING-SHADOW]" : "[ISWING]";
            printf("%s %s %s entry=%.2f sl=%.2f sl_pts=%.2f mic_avg=%.4f "
                   "size_mult=%.2f confirmed_ticks=%d\n",
                   pfx, symbol_, is_long_ ? "LONG" : "SHORT",
                   entry_, sl_, sl_pts_, mic_avg, size_mult, pending_ticks_);
            fflush(stdout);
            pending_entry_ = false;
            return;
        }
        if (pending_ticks_ >= PENDING_MAX_TICKS) {
            const char* pfx = shadow_mode ? "[ISWING-SHADOW]" : "[ISWING]";
            printf("%s %s PENDING-TIMEOUT %s mic_avg=%.4f thr=%.3f ticks=%d -- SKIP\n",
                   pfx, symbol_, pending_is_long_ ? "LONG" : "SHORT",
                   mic_avg, PENDING_CONFIRM_MIC, pending_ticks_);
            fflush(stdout);
            pending_entry_ = false;
            // 30min cooldown on cancelled pending so we don't churn.
            cooldown_until_ms_ = idx_now_ms() + SWING_COOLDOWN_MS;
        }
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
    // 2026-05-30: exit-side fixes per loss-cluster analysis. 10-trade sample
    // showed winners cut at 0.5R trail (avg $128/130/80) while losers ran
    // full -1R (-$200 SP, -$100 NQ). Net WR 40% R:R ~1:1 -> breakeven.
    // Fix surfaces: stage_trail (no-TP runners ok per memory:omega-stage-
    // trail-result), TP1 50% partial at 1.5R, cold-loss cut at -0.5R if
    // 15min hold with mfe < 0.3R.
    int     stage_              = 0;     // 0 init, 1=2R-armed, 2=5R-armed, 3=10R
    bool    tp1_partial_taken_  = false; // 50% closed at 1.5R MFE
    double  tp1_pnl_pts_        = 0.0;   // partial-close pts (locked)
    // 2026-05-30: rolling-30-tick L2 EMA. Single-tick L2 too noisy -- replay
    // showed +$346 US500 winner blocked by mic_e=-0.30 spike but mic_p30=-0.03
    // (clean). Ring buffer of last 30 ticks gives smoother gate signal.
    static constexpr int L2_BUF_N = 30;
    double  l2_mic_buf_[L2_BUF_N] = {};
    double  l2_imb_buf_[L2_BUF_N] = {};
    int     l2_buf_head_          = 0;
    int     l2_buf_count_         = 0;
    // 2026-05-30 L2-leverage: confirmed-entry delay state.
    bool    pending_entry_        = false;
    bool    pending_is_long_      = false;
    int     pending_ticks_        = 0;
    static constexpr int PENDING_CONFIRM_TICKS = 10;
    static constexpr int PENDING_MAX_TICKS     = 30;
    static constexpr double PENDING_CONFIRM_MIC = 0.03;
    // 2026-05-30 (S39): ATR-relative EMA-separation floor. The EMA9-EMA50 gap
    // at entry must be >= this fraction of one H1 ATR for the crossover to
    // count as non-marginal. 0.25 = a quarter-ATR real separation. Replaces
    // the price-insensitive absolute min_ema_sep_ as the binding gate on
    // indices (see entry-gate comment + ANALYSIS_S39_LIVE_EDGE_AUDIT).
    static constexpr double EMA_SEP_ATR_FRAC = 0.25;
    // 2026-05-30 L2-leverage: lot sizing scaler at entry.
    double  size_mult_at_entry_   = 1.0;
    // 2026-05-30 L2-leverage: L2-trailing exit threshold.
    static constexpr double L2_TRAIL_FLIP_MIC = 0.10;
    // 2026-05-22: escalating SL cooldown. After 2026-05-21 NSX took 2 back-to-back
    // losses ($100 each) because 30min cooldown expired before mean-reversion
    // played out. Escalate cooldown 1h/3h/6h on consec SL_HITs. Reset on win.
    int     consec_sl_         = 0;

    static int s_trade_id_;

    void _manage(double bid, double ask, double mid, CloseCb& on_close) noexcept {
        const double move = is_long_ ? (mid - entry_) : (entry_ - mid);
        if (move > mfe_) mfe_ = move;

        // ── BE lock at 1R MFE ────────────────────────────────────────────────
        if (!be_locked_ && mfe_ >= sl_pts_) {
            be_locked_ = true;
            trail_sl_  = entry_;
            printf("[ISWING-%s] BE-LOCK %s mfe=%.2f sl->%.2f\n",
                   symbol_, is_long_ ? "LONG" : "SHORT", mfe_, trail_sl_);
            fflush(stdout);
        }

        // ── TP1 50% partial at 1.5R MFE ──────────────────────────────────────
        // Lock half position at 1.5R; remainder runs on stage-trail.
        if (!tp1_partial_taken_ && mfe_ >= sl_pts_ * 1.5) {
            const double tp1_px = is_long_ ? (entry_ + sl_pts_ * 1.5)
                                           : (entry_ - sl_pts_ * 1.5);
            tp1_partial_taken_ = true;
            tp1_pnl_pts_       = sl_pts_ * 1.5 * 0.5;  // 50% of 1.5R in pts
            printf("[ISWING-%s] TP1-PARTIAL %s tp1=%.2f mfe=%.2f locked=%.2f pts (50%%)\n",
                   symbol_, is_long_ ? "LONG" : "SHORT", tp1_px, mfe_, tp1_pnl_pts_);
            fflush(stdout);
        }

        // ── Stage trail: looser as MFE grows (no-TP runner) ──────────────────
        // Stage 1 (MFE >= 2R): trail at 1R behind peak
        // Stage 2 (MFE >= 5R): trail at 2R behind peak (loosen for trend room)
        // Stage 3 (MFE >= 10R): trail at 3R behind peak
        int new_stage = stage_;
        if      (mfe_ >= sl_pts_ * 10.0) new_stage = 3;
        else if (mfe_ >= sl_pts_ *  5.0) new_stage = std::max(new_stage, 2);
        else if (mfe_ >= sl_pts_ *  2.0) new_stage = std::max(new_stage, 1);
        if (new_stage > stage_) {
            const char* tag = (new_stage == 1) ? "STAGE1" : (new_stage == 2 ? "STAGE2" : "STAGE3");
            printf("[ISWING-%s] %s %s mfe=%.2f\n",
                   symbol_, tag, is_long_ ? "LONG" : "SHORT", mfe_);
            fflush(stdout);
            stage_ = new_stage;
        }
        double trail_dist = sl_pts_ * 0.5;  // pre-stage: tight 0.5R trail
        if      (stage_ == 1) trail_dist = sl_pts_ * 1.0;
        else if (stage_ == 2) trail_dist = sl_pts_ * 2.0;
        else if (stage_ == 3) trail_dist = sl_pts_ * 3.0;
        if (be_locked_) {
            const double new_sl = is_long_ ? (entry_ + mfe_ - trail_dist)
                                           : (entry_ - mfe_ + trail_dist);
            if (is_long_  && new_sl > trail_sl_) trail_sl_ = new_sl;
            if (!is_long_ && new_sl < trail_sl_) trail_sl_ = new_sl;
        }

        // ── Cold-loss cuts (T1 at 5min, T2 at 15min) ─────────────────────────
        // Layered escape for trades where MFE proves signal failed. Initial SL
        // at -1R stays (gap protection). Cold-cuts trim losses earlier.
        // T1: held >= 5min, adverse >= 0.5R, mfe < 0.2R -> exit at current
        // T2: held >= 15min, adverse >= 0.5R, mfe < 0.3R -> exit at current
        // Replay on 2026-05-29 10-trade sample (iswing_replay.cpp):
        //   T2-only:           -$15 -> +$45  (commit 71aeb07b)
        //   T1+T2:             -$15 -> +$141 (this change, saves $96 on 8min losers)
        //   T0 60s tier added: -$15 -> -$21  (T0 killed legit winners -- REJECTED)
        const int64_t held_sec = idx_now_sec() - entry_ts_;
        const double half_R    = sl_pts_ * 0.5;
        const double cur_adv   = is_long_ ? (entry_ - mid) : (mid - entry_);
        // T1 tightened 2026-05-30 per tier-sweep (backtest/iswing_replay.cpp):
        // adv 0.5R->0.3R + mfe 0.2R->0.15R. Sweep showed this captures -$60
        // losers earlier without killing legit winners (their mfe@5m all
        // > 0.15R threshold on the 2026-05-29 sample). +$43 additional on
        // 9-trade sample vs prior T1 setting.
        const bool cc_t1       = !be_locked_ && held_sec >= 300
                              && cur_adv >= sl_pts_ * 0.3
                              && mfe_ < sl_pts_ * 0.15;
        const bool cc_t2       = !be_locked_ && held_sec >= 900
                              && cur_adv >= half_R
                              && mfe_ < sl_pts_ * 0.3;
        const bool cold_cut    = cc_t1 || cc_t2;
        const bool sl_hit      = is_long_ ? (bid <= trail_sl_) : (ask >= trail_sl_);

        // ── L2 trailing exit (2026-05-30) ──
        // Close position when rolling-10-tick microprice flips against side.
        // 2026-05-30 iter2 per replay sweep: mfe>=1R (not 0.5R) needed. At
        // 0.5R the trail fires too early on winners that are still building
        // (replay: thr=0.10/mfe=0.5R lost $296 vs baseline; thr=0.10/mfe=1R
        // gained $79). Exit at market (current mid).
        bool l2_flip = false;
        if (mfe_ >= sl_pts_ * 1.0 && l2_buf_count_ >= PENDING_CONFIRM_TICKS) {
            double mic_sum = 0;
            const int N = std::min(l2_buf_count_, (int)PENDING_CONFIRM_TICKS);
            int idx = (l2_buf_head_ - 1 + L2_BUF_N) % L2_BUF_N;
            for (int k = 0; k < N; ++k) {
                mic_sum += l2_mic_buf_[idx];
                idx = (idx - 1 + L2_BUF_N) % L2_BUF_N;
            }
            const double mic_avg = mic_sum / N;
            if (is_long_  && mic_avg <= -L2_TRAIL_FLIP_MIC) l2_flip = true;
            if (!is_long_ && mic_avg >=  L2_TRAIL_FLIP_MIC) l2_flip = true;
            if (l2_flip) {
                printf("[ISWING-%s] L2-TRAIL-FLIP %s mic_avg=%.4f mfe=%.2f\n",
                       symbol_, is_long_ ? "LONG" : "SHORT", mic_avg, mfe_);
                fflush(stdout);
            }
        }
        // 2026-05-13 (part L): VWR-pattern winner exemption.
        const double cur_move_s = is_long_ ? (mid - entry_) : (entry_ - mid);
        const bool timeout = (idx_now_sec() - entry_ts_ >= SWING_MAX_HOLD_SEC)
                          && cur_move_s <= 0.0;

        if (!sl_hit && !timeout && !cold_cut && !l2_flip) return;

        const double exit_px = sl_hit ? trail_sl_ : mid;
        const char*  why     = cold_cut ? "COLD_CUT"
                             : sl_hit   ? "SL_HIT"
                             : l2_flip  ? "L2_FLIP"
                             :            "TIMEOUT";
        const double remainder_pts = is_long_ ? (exit_px - entry_) : (entry_ - exit_px);
        // Gross PnL accounting:
        //   no partial:  gross = remainder_pts (full size on close)
        //   partial(50%): gross = 0.75*sl_pts_ (locked at 1.5R on half) + 0.5*remainder_pts
        // Then scale by size_mult_at_entry_ (L2-aware sizing 2026-05-30):
        // L2-strong-confirm trades sized up to 2x, weak ones down to 0.5x.
        const double gross_base = tp1_partial_taken_
                                ? (tp1_pnl_pts_ + 0.5 * remainder_pts)
                                : remainder_pts;
        const double gross   = gross_base * size_mult_at_entry_;

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
        // Reset 2026-05-30 exit-side state for next trade.
        stage_              = 0;
        tp1_partial_taken_  = false;
        tp1_pnl_pts_        = 0.0;
        size_mult_at_entry_ = 1.0;
        // 2026-05-22: escalating cooldown on consec SLs.
        //   1st consec SL -> 1h cooldown
        //   2nd consec SL -> 3h cooldown
        //   3rd+ consec SL -> 6h cooldown
        // Any non-SL exit (timeout-with-gain, BE-trail TP) resets to 30min standard.
        if (sl_hit && gross <= 0.0) {
            ++consec_sl_;
            int64_t cool_ms = SWING_COOLDOWN_MS;  // 30min default
            if (consec_sl_ == 1)      cool_ms =  3600000LL;   // 1h
            else if (consec_sl_ == 2) cool_ms = 10800000LL;   // 3h
            else                      cool_ms = 21600000LL;   // 6h
            cooldown_until_ms_ = idx_now_ms() + cool_ms;
            printf("[ISWING-%s] CONSEC_SL=%d -> cooldown=%lldmin\n",
                   symbol_, consec_sl_, (long long)(cool_ms / 60000LL));
            fflush(stdout);
        } else {
            // Winning or scratched exit -- reset consec counter, standard cooldown.
            consec_sl_ = 0;
            cooldown_until_ms_ = idx_now_ms() + SWING_COOLDOWN_MS;
        }

        if (on_close) on_close(tr);
    }
};

int IndexSwingEngine::s_trade_id_ = 9800;

} // namespace idx
} // namespace omega
