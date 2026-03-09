#pragma once
// ==============================================================================
// SymbolEngines.hpp — Per-instrument CRTP breakout engine policies
//
// Each engine has FULLY INDEPENDENT logic appropriate to its instrument:
//
//   SpEngine   (US500.F) — equity index, regime-gated, cross-NQ guard
//   NqEngine   (USTEC.F) — equity index, regime-gated, cross-SP guard
//   OilEngine  (USOIL.F) — commodity, EIA window blocked, VIX-irrelevant
//   GoldEngine (GOLD.F)  — safe-haven, INVERSE VIX logic (risk-off = trade more)
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
// INSTRUMENT: Equity index. Correlated with NQ. Sensitive to macro regime.
// TP 0.60%, SL 0.35%, VOL_THRESH 0.04%, MIN_GAP 300s, MAX_HOLD 1200s
//
// GATES (SP-specific):
//   spread > 0.04%    -> block (liquidity concern)
//   NQ position open  -> block (doubles correlated equity risk)
//   ES/NQ div > 0.30% -> block (sector rotation — not a clean index breakout)
//   VIX > 40          -> block (panic regime — spreads blow out, fills unreliable)
//   RISK_OFF regime   -> block (trending down — compression breaks fail)
// ==============================================================================
class SpEngine final : public BreakoutEngineBase<SpEngine>
{
public:
    const MacroContext* macro = nullptr;

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
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)           return false; // liquidity gate
        if (!macro)                                 return true;
        if (macro->nq_open)                         return false; // cross-symbol: no double equity risk
        if (std::fabs(macro->es_nq_div) > 0.0030)  return false; // sector rotation — not clean
        if (macro->vix > 40.0)                      return false; // panic — unreliable fills
        if (macro->regime == "RISK_OFF")            return false; // trending down — breakouts fail
        return true;
    }
};

// ==============================================================================
// NqEngine — USTEC.F (Nasdaq-100 futures)
//
// INSTRUMENT: Equity index, higher beta than SP. Tech-heavy, moves faster.
// TP 0.70%, SL 0.40%, VOL_THRESH 0.05%, MIN_GAP 240s, MAX_HOLD 1200s
//
// GATES (NQ-specific):
//   spread > 0.05%    -> block (NQ spread slightly wider than SP)
//   SP position open  -> block (doubles correlated equity risk)
//   ES/NQ div > 0.30% -> block (SP/NQ decoupling — unreliable signal)
//   VIX > 40          -> block (panic regime)
//   RISK_OFF regime   -> block (equity index — no bearish breakouts)
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
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)           return false; // liquidity gate
        if (!macro)                                 return true;
        if (macro->sp_open)                         return false; // cross-symbol: no double equity risk
        if (std::fabs(macro->es_nq_div) > 0.0030)  return false; // SP/NQ decoupling
        if (macro->vix > 40.0)                      return false; // panic
        if (macro->regime == "RISK_OFF")            return false; // equity index — no bearish breakouts
        return true;
    }
};

// ==============================================================================
// OilEngine — USOIL.F (WTI Crude Oil)
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
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)  return false; // oil spread gate
        if (in_inventory_window())         return false; // EIA Wed 14:00-15:30 UTC
        if (macro && macro->vix > 50.0)   return false; // true liquidity crisis only
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

// ==============================================================================
// GoldEngine — GOLD.F (XAU/USD)
//
// INSTRUMENT: Safe-haven asset. INVERSE relationship with equity risk regime.
// TP 0.30%, SL 0.15%, VOL_THRESH 0.04%, MIN_GAP 180s, MAX_HOLD 1500s
//
// GATES (Gold-specific):
//   spread > 0.06%   -> block (gold spread wider than indices)
//   VIX > 60         -> block (true dislocation — all markets illiquid)
//   RISK_OFF         -> ALLOW freely (gold rallies on fear — core edge)
//   RISK_ON          -> allow (gold breakouts still work, just less frequent)
//   NEUTRAL          -> allow normally
//
// NOTE: Gold is a safe-haven. It performs BEST in RISK_OFF.
//   We deliberately do NOT block RISK_OFF for gold — that is when it moves most.
//   The only universal block is extreme VIX > 60 (true dislocation, all illiquid).
// ==============================================================================
class GoldEngine final : public BreakoutEngineBase<GoldEngine>
{
public:
    const MacroContext* macro = nullptr;

    explicit GoldEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.040;
        TP_PCT                = 0.300;
        SL_PCT                = 0.150;
        COMPRESSION_LOOKBACK  = 60;
        BASELINE_LOOKBACK     = 250;
        COMPRESSION_THRESHOLD = 0.75;
        MAX_HOLD_SEC          = 1500;
        MIN_GAP_SEC           = 180;
        MAX_SPREAD_PCT        = 0.06;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)  return false; // gold spread gate
        if (!macro)                        return true;
        if (macro->vix > 60.0)             return false; // true dislocation — all illiquid
        // RISK_OFF = gold's home turf — always allow
        // RISK_ON / NEUTRAL = allow (compression breakouts valid in all regimes for gold)
        return true;
    }
};

} // namespace omega
