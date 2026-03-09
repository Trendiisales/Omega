#pragma once
// ==============================================================================
// SymbolEngines.hpp — Per-instrument CRTP breakout engine policies
//
// Depends on BreakoutEngine.hpp (must be included before this file).
// MacroContext, SpEngine, NqEngine, OilEngine all live in namespace omega.
//
// Separated from BreakoutEngine.hpp to avoid MSVC namespace-resolution issues
// when GoldEngineStack.hpp (which also opens namespace omega) is in scope.
// ==============================================================================
#include "BreakoutEngine.hpp"
#include <cmath>
#include <chrono>
#include <string>

namespace omega {

// ==============================================================================
// MacroContext — shared state updated every tick, read by shouldTrade() overrides
// ==============================================================================
struct MacroContext {
    std::string regime;        // "RISK_ON" | "NEUTRAL" | "RISK_OFF"
    double      vix       = 0.0;
    double      es_nq_div = 0.0;  // ES vs NQ relative return — divergence signal
    bool        sp_open   = false; // US500 position open (cross-symbol guard)
    bool        nq_open   = false; // USTEC position open (cross-symbol guard)
    bool        oil_open  = false; // USOIL position open
};

// ==============================================================================
// SpEngine — US500.F (S&P 500 futures)
//
// TP 0.60%, SL 0.35%, VOL_THRESH 0.04%, MIN_GAP 300s, MAX_HOLD 1200s
// Regime gating:
//   NQ open → block (correlated, doubles risk)
//   ES/NQ divergence > 0.30% → block (sector rotation, not clean breakout)
//   VIX > 35 → block all (panic, spreads blow out)
// ==============================================================================
class SpEngine final : public BreakoutEngineBase<SpEngine>
{
public:
    const MacroContext* macro = nullptr;  // set by apply_engine_config()

    explicit SpEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.040;
        TP_PCT                = 0.600;
        SL_PCT                = 0.350;
        COMPRESSION_LOOKBACK  = 60;
        BASELINE_LOOKBACK     = 200;
        COMPRESSION_THRESHOLD = 0.75;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 300;
        MAX_SPREAD_PCT        = 0.04;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT) return false;
        if (latency_ms > 2000.0)           return false;
        if (!macro)                       return true;
        if (macro->nq_open)               return false;
        if (std::fabs(macro->es_nq_div) > 0.0030) return false;
        if (macro->vix > 35.0)            return false;
        return true;
    }
};

// ==============================================================================
// NqEngine — USTEC.F (Nasdaq-100 futures)
//
// TP 0.70%, SL 0.40%, VOL_THRESH 0.05%, MIN_GAP 240s, MAX_HOLD 1200s
// Regime gating:
//   SP open → block (correlated, doubles risk)
//   ES/NQ divergence > 0.30% → block
//   VIX > 35 → block all
// ==============================================================================
class NqEngine final : public BreakoutEngineBase<NqEngine>
{
public:
    const MacroContext* macro = nullptr;

    explicit NqEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.050;
        TP_PCT                = 0.700;
        SL_PCT                = 0.400;
        COMPRESSION_LOOKBACK  = 50;
        BASELINE_LOOKBACK     = 200;
        COMPRESSION_THRESHOLD = 0.75;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 240;
        MAX_SPREAD_PCT        = 0.05;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT) return false;
        if (latency_ms > 2000.0)           return false;
        if (!macro)                       return true;
        if (macro->sp_open)               return false;
        if (std::fabs(macro->es_nq_div) > 0.0030) return false;
        if (macro->vix > 35.0)            return false;
        return true;
    }
};

// ==============================================================================
// OilEngine — USOIL.F (WTI Crude Oil)
//
// TP 1.20%, SL 0.60%, VOL_THRESH 0.08%, MIN_GAP 360s, MAX_HOLD 1800s
// EIA inventory window (Wed 14:00-15:30 UTC) automatically blocked.
// ==============================================================================
class OilEngine final : public BreakoutEngineBase<OilEngine>
{
public:
    const MacroContext* macro = nullptr;

    explicit OilEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.080;
        TP_PCT                = 1.200;
        SL_PCT                = 0.600;
        COMPRESSION_LOOKBACK  = 40;
        BASELINE_LOOKBACK     = 150;
        COMPRESSION_THRESHOLD = 0.70;
        MAX_HOLD_SEC          = 1800;
        MIN_GAP_SEC           = 360;
        MAX_SPREAD_PCT        = 0.120;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT) return false;
        if (latency_ms > 2000.0)           return false;
        if (in_inventory_window())        return false;
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
        return (mins >= 14*60 && mins < 15*60+30);  // 14:00-15:30 UTC
    }
};

} // namespace omega
