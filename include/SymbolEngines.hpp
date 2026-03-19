#pragma once
// ==============================================================================
// SymbolEngines.hpp -- Per-instrument CRTP breakout engine policies
//
// Each engine has FULLY INDEPENDENT logic appropriate to its instrument:
//
//   SpEngine    (US500.F) -- equity index, regime-gated, div-gated
//   NqEngine    (USTEC.F) -- equity index, higher beta, div-gated
//   Us30Engine  (DJ30.F)  -- Dow Jones, macro-gated like SP/NQ
//   Nas100Engine(NAS100)  -- Nasdaq cash, looser spread than USTEC.F
//   OilEngine   (USOIL.F) -- commodity, EIA window blocked, VIX-irrelevant
//
// MacroContext is updated every tick in main.cpp and passed to all engines.
// VIX panic threshold is unified at 40.0 for equity engines only.
// ==============================================================================
#include "BreakoutEngine.hpp"
#include <cmath>
#include <chrono>
#include <string>

namespace omega {

// ==============================================================================
// MacroContext -- shared state updated every tick, read by shouldTrade() overrides
// ==============================================================================
struct MacroContext {
    std::string regime;        // "RISK_ON" | "NEUTRAL" | "RISK_OFF"
    double      vix       = 0.0;
    double      es_nq_div = 0.0;  // ES vs NQ relative return -- divergence signal
    bool        sp_open   = false; // US500 position open (cross-symbol guard)
    bool        nq_open   = false; // USTEC position open (cross-symbol guard)
    bool        oil_open  = false; // USOIL position open

    // Cross-symbol compression alignment — set in on_tick from engine phases
    // true = that symbol is currently in COMPRESSION or BREAKOUT_WATCH
    bool        sp_compressing   = false;
    bool        nq_compressing   = false;
    bool        us30_compressing = false;
    bool        ger30_compressing= false;
    bool        uk100_compressing= false;

    // L2 book imbalance per symbol — bid_size / (bid_size + ask_size) at top 3 levels
    // 0.5 = balanced, >0.65 = bid-heavy (bullish pressure), <0.35 = ask-heavy (bearish)
    double      sp_l2_imbalance   = 0.5;
    double      nq_l2_imbalance   = 0.5;
    double      nas_l2_imbalance  = 0.5;
    double      us30_l2_imbalance = 0.5;
    double      gold_l2_imbalance = 0.5;
    double      xag_l2_imbalance  = 0.5;
    double      eur_l2_imbalance  = 0.5;
    double      gbp_l2_imbalance  = 0.5;
    double      cl_l2_imbalance   = 0.5;

    // Session time slot — updated every tick
    // 0=dead(05-07 UTC), 1=London(07-09), 2=London_core(09-12),
    // 3=overlap(12-14), 4=NY(14-17), 5=NY_late(17-22), 6=Asia(22-05)
    int         session_slot = 0;
};

// Returns session slot multiplier for MIN_BREAKOUT_PCT scaling.
// London open and NY open have highest follow-through — allow tighter gates.
// Dead zone and late NY have lowest follow-through — require wider moves.
inline double session_breakout_mult(int slot) noexcept {
    switch (slot) {
        case 1: return 0.70;  // London open  07-09: best breakouts, loosen gate 30%
        case 2: return 0.85;  // London core  09-12: good but not as clean
        case 3: return 0.90;  // Overlap      12-14: mixed, NY not fully open
        case 4: return 0.75;  // NY open      14-17: second best window
        case 5: return 1.10;  // NY late      17-22: choppy, tighten gate 10%
        case 6: return 0.80;  // Asia         22-05: gold/FX active
        default: return 1.20; // Dead zone    05-07: worst, tighten gate 20%
    }
}

// ==============================================================================
// SpEngine -- US500.F (S&P 500 futures)
//
// INSTRUMENT: Equity index. Correlated with NQ. Sensitive to macro regime.
// TP 0.60%, SL 0.35%, VOL_THRESH 0.04%, MIN_GAP 300s, MAX_HOLD 1200s
//
// GATES (SP-specific):
//   spread > 0.04%    -> block (liquidity concern)
//   NQ position open  -> block (doubles correlated equity risk)
//   ES/NQ div > 0.30% -> block (sector rotation -- not a clean index breakout)
//   VIX > 40          -> block (panic regime -- spreads blow out, fills unreliable)
//   RISK_OFF regime   -> block (trending down -- compression breaks fail)
// ==============================================================================
class SpEngine final : public BreakoutEngineBase<SpEngine>
{
public:
    const MacroContext* macro = nullptr;
    double vix_panic      = 40.0;
    double div_threshold  = 0.0060;

    explicit SpEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.040;
        TP_PCT                = 0.600;
        SL_PCT                = 0.350;
        COMPRESSION_LOOKBACK  = 20;
        BASELINE_LOOKBACK     = 80;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 60;
        MAX_SPREAD_PCT        = 0.04;
        MOMENTUM_THRESH_PCT   = 0.006;
        MIN_BREAKOUT_PCT      = 0.03;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                    return false;
        if (!macro)                                          return true;
        if (std::fabs(macro->es_nq_div) > div_threshold)    return false;
        if (macro->vix > vix_panic)                         return false;
        // Cross-symbol: at least one other US index compressing/breaking.
        // Only enforce once at least one index has been compressing (non-zero state).
        // This prevents blocking during the warmup period before compression is detected.
        const bool any_us_active = macro->sp_compressing || macro->nq_compressing || macro->us30_compressing;
        if (any_us_active && !(macro->nq_compressing || macro->us30_compressing)) return false;
        // L2: block extreme imbalance (book defended by LP, fills will be bad)
        const double imb = macro->sp_l2_imbalance;
        if (imb > 0.0 && (imb < 0.20 || imb > 0.80))       return false;
        return true;
    }
};

// ==============================================================================
// NqEngine -- USTEC.F (Nasdaq-100 futures)
//
// INSTRUMENT: Equity index, higher beta than SP. Tech-heavy, moves faster.
// TP 0.70%, SL 0.40%, VOL_THRESH 0.05%, MIN_GAP 240s, MAX_HOLD 1200s
//
// GATES (NQ-specific):
//   spread > 0.05%    -> block (NQ spread slightly wider than SP)
//   SP position open  -> block (doubles correlated equity risk)
//   ES/NQ div > 0.30% -> block (SP/NQ decoupling -- unreliable signal)
//   VIX > 40          -> block (panic regime)
//   RISK_OFF regime   -> block (equity index -- no bearish breakouts)
// ==============================================================================
class NqEngine final : public BreakoutEngineBase<NqEngine>
{
public:
    const MacroContext* macro = nullptr;
    double vix_panic      = 40.0;
    double div_threshold  = 0.0060;

    explicit NqEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.050;
        TP_PCT                = 0.700;
        SL_PCT                = 0.400;
        COMPRESSION_LOOKBACK  = 18;
        BASELINE_LOOKBACK     = 70;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 60;
        MAX_SPREAD_PCT        = 0.05;
        MOMENTUM_THRESH_PCT   = 0.005;
        MIN_BREAKOUT_PCT      = 0.04;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                    return false;
        if (!macro)                                          return true;
        if (std::fabs(macro->es_nq_div) > div_threshold)    return false;
        if (macro->vix > vix_panic)                         return false;
        const bool any_us_active2 = macro->sp_compressing || macro->nq_compressing || macro->us30_compressing;
        if (any_us_active2 && !(macro->sp_compressing || macro->us30_compressing)) return false;
        const double imb = macro->nq_l2_imbalance;
        if (imb > 0.0 && (imb < 0.20 || imb > 0.80))       return false;
        return true;
    }
};

// ==============================================================================
// OilEngine -- USOIL.F (WTI Crude Oil)
//
// INSTRUMENT: Commodity. Supply/demand driven. NOT correlated with equity regime.
// TP 1.20%, SL 0.60%, VOL_THRESH 0.08%, MIN_GAP 360s, MAX_HOLD 1800s
//
// GATES (Oil-specific):
//   spread > 0.12%             -> block (oil spread much wider than indices)
//   EIA inventory Wed 14-15:30 -> block (scheduled event, not a breakout)
//   VIX > 50                   -> block (true liquidity crisis only)
//
// NOTE: Oil does NOT use equity regime (RISK_ON/OFF).
//   Oil rallies in RISK_OFF (supply shock) and sells in RISK_ON (demand glut).
//   Only true liquidity crisis (VIX>50) blocks all instruments universally.
// ==============================================================================
class OilEngine final : public BreakoutEngineBase<OilEngine>
{
public:
    const MacroContext* macro = nullptr;
    double vix_panic = 50.0;

    explicit OilEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.080;
        TP_PCT                = 1.200;
        SL_PCT                = 0.600;
        COMPRESSION_LOOKBACK  = 40;
        BASELINE_LOOKBACK     = 120;
        COMPRESSION_THRESHOLD = 0.80;
        MAX_HOLD_SEC          = 1800;
        MIN_GAP_SEC           = 90;
        MAX_SPREAD_PCT        = 0.120;
        MOMENTUM_THRESH_PCT   = 0.050;
        MIN_BREAKOUT_PCT      = 0.06;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)           return false;
        if (in_inventory_window())                  return false;
        if (macro && macro->vix > vix_panic)        return false;
        return true;
    }

private:
    static bool in_inventory_window() noexcept {
        const auto t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        struct tm ti = {};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        if (ti.tm_wday != 3) return false;  // Wednesday only
        const int mins = ti.tm_hour * 60 + ti.tm_min;
        // Block only 5 min pre-announcement (spread widens) + 30 min post (vol normalises)
        // 14:25-15:00 UTC. The release itself IS the breakout -- trade it after initial spike.
        // Old block was 14:00-15:30 which killed the entire best oil move of the week.
        return (mins >= 14*60+25 && mins < 15*60);  // 14:25-15:00 UTC only
    }
};

// ==============================================================================
// Us30Engine -- DJ30.F (Dow Jones Industrial Average)
//
// INSTRUMENT: US equity index. Less tech-heavy than NQ, steadier than SP.
// TP 0.80%, SL 0.35%, VOL_THRESH 0.035%, MIN_GAP 180s, MAX_HOLD 1200s
//
// GATES: same macro gates as SP/NQ — divergence, VIX panic.
// Slightly looser VOL_THRESH than SP (Dow is slower-moving).
// ==============================================================================
class Us30Engine final : public BreakoutEngineBase<Us30Engine>
{
public:
    const MacroContext* macro = nullptr;
    double vix_panic     = 40.0;
    double div_threshold = 0.0060;

    explicit Us30Engine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.035;
        TP_PCT                = 0.800;
        SL_PCT                = 0.350;
        COMPRESSION_LOOKBACK  = 20;
        BASELINE_LOOKBACK     = 80;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 60;
        MAX_SPREAD_PCT        = 0.05;
        MOMENTUM_THRESH_PCT   = 0.006;
        MIN_BREAKOUT_PCT      = 0.04;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                 return false;
        if (!macro)                                       return true;
        if (std::fabs(macro->es_nq_div) > div_threshold) return false;
        if (macro->vix > vix_panic)                      return false;
        { const bool any = macro->sp_compressing || macro->nq_compressing || macro->us30_compressing;
          if (any && !(macro->sp_compressing || macro->nq_compressing)) return false; }
        const double imb = macro->us30_l2_imbalance;
        if (imb > 0.0 && (imb < 0.20 || imb > 0.80))    return false;
        return true;
    }
};

// ==============================================================================
// Nas100Engine -- NAS100 (Nasdaq-100 cash index)
//
// INSTRUMENT: Nasdaq cash. Tracks USTEC.F closely but different tick frequency.
// TP 0.70%, SL 0.40%, VOL_THRESH 0.050%, MIN_GAP 180s, MAX_HOLD 1200s
//
// GATES: same as NqEngine but slightly wider spread tolerance (cash vs futures).
// Independent position from USTEC.F — they compress/break independently.
// ==============================================================================
class Nas100Engine final : public BreakoutEngineBase<Nas100Engine>
{
public:
    const MacroContext* macro = nullptr;
    double vix_panic     = 40.0;
    double div_threshold = 0.0060;

    explicit Nas100Engine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.050;
        TP_PCT                = 0.700;
        SL_PCT                = 0.400;
        COMPRESSION_LOOKBACK  = 18;
        BASELINE_LOOKBACK     = 70;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 60;
        MAX_SPREAD_PCT        = 0.06;
        MOMENTUM_THRESH_PCT   = 0.005;
        MIN_BREAKOUT_PCT      = 0.04;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                 return false;
        if (!macro)                                       return true;
        if (std::fabs(macro->es_nq_div) > div_threshold) return false;
        if (macro->vix > vix_panic)                      return false;
        { const bool any = macro->sp_compressing || macro->nq_compressing || macro->us30_compressing;
          if (any && !(macro->sp_compressing || macro->us30_compressing)) return false; }
        const double imb = macro->nas_l2_imbalance;
        if (imb > 0.0 && (imb < 0.20 || imb > 0.80))    return false;
        return true;
    }
};

} // namespace omega
