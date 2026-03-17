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
};

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
        return true;
    }
};

} // namespace omega
