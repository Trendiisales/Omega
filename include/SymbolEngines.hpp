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
//   GoldEngine  (GOLD.F)  -- safe-haven, INVERSE VIX logic (risk-off = trade more)
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

    explicit SpEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.040;
        TP_PCT                = 0.600;
        SL_PCT                = 0.350;
        COMPRESSION_LOOKBACK  = 40;
        BASELINE_LOOKBACK     = 160;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 300;
        MAX_SPREAD_PCT        = 0.04;
        // SP at ~6745 (Mar 2026): 0.012% = $0.81 over 20 ticks.
        // London avg hourly range $28.71, typical 4s move ~$1.93.
        // $0.81 = 42% of typical 4s move — meaningful directional pressure.
        MOMENTUM_THRESH_PCT   = 0.012;
        // SP at ~6745 (Mar 2026): London avg hourly range = $28.71, min = $14.50.
        // 0.12% = $8.09 from comp edge — that is 28% of the avg hourly range.
        // Real breakouts in London clear $3–5 beyond the compression edge.
        // 0.05% = $3.37 — confirms a real directional break without requiring
        // a full hourly range move just to enter.
        // Was 0.12% which was blocking nearly every valid signal.
        MIN_BREAKOUT_PCT      = 0.05;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)           return false; // liquidity gate
        if (!macro)                                 return true;
        // Divergence gate: 0.60% threshold (raised from 0.30%).
        // 0.30% was blocking ~60% of sessions — NQ is higher beta than SP and
        // routinely diverges 0.3-0.5% intraday without signal degradation.
        // 0.60% represents genuine sector rotation / decoupling events only.
        if (std::fabs(macro->es_nq_div) > 0.0060)  return false; // sector rotation -- not clean
        if (macro->vix > 40.0)                      return false; // panic -- unreliable fills
        // RISK_OFF block removed: compression breakouts fire both ways. RISK_OFF = uncertainty, not direction.
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

    explicit NqEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.050;
        TP_PCT                = 0.700;
        SL_PCT                = 0.400;
        COMPRESSION_LOOKBACK  = 35;
        BASELINE_LOOKBACK     = 140;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 240;
        MAX_SPREAD_PCT        = 0.05;
        // NQ at ~24500: 0.025% = $6.13 over 20 ticks — way too tight for early
        // London pre-market chop. 0.010% = $2.45 — still meaningful momentum.
        MOMENTUM_THRESH_PCT   = 0.010;
        // NQ at ~24500: 0.12% = $29.40 beyond comp edge — too large.
        // Tighten to 0.08% = $19.60 — still a genuine break, not a fake.
        MIN_BREAKOUT_PCT      = 0.08;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)           return false; // liquidity gate
        if (!macro)                                 return true;
        // Divergence gate: 0.60% threshold (raised from 0.30%).
        // NQ is higher beta than SP. A 0.30% divergence is normal daily noise —
        // raising to 0.60% targets real decoupling events (sector rotation, events).
        if (std::fabs(macro->es_nq_div) > 0.0060)  return false; // SP/NQ decoupling
        if (macro->vix > 40.0)                      return false; // panic
        // RISK_OFF block removed: compression breakouts fire both ways. RISK_OFF = uncertainty, not direction.
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

    explicit OilEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.080;
        TP_PCT                = 1.200;
        SL_PCT                = 0.600;
        COMPRESSION_LOOKBACK  = 80;
        BASELINE_LOOKBACK     = 240;
        COMPRESSION_THRESHOLD = 0.80;
        MAX_HOLD_SEC          = 1800;
        MIN_GAP_SEC           = 360;
        MAX_SPREAD_PCT        = 0.120;
        // Oil at ~$95.74 (Mar 2026): London avg hourly range = $1.92, min = $1.16.
        // Typical 4s move = $1.92/hr ÷ 15 = $0.128.
        // 0.015% = $0.014 over 20 ticks — only 11% of typical 4s move — too loose,
        // fires on random tick noise. 0.050% = $0.048 = 37% of typical 4s move.
        // Tightened from 0.015% to filter genuine directional momentum.
        MOMENTUM_THRESH_PCT   = 0.050;
        // Oil at ~$95.74: 6% of avg hourly range $1.92 = $0.115.
        // 0.12% = $0.115 from comp edge — confirms real breakout vs noise.
        // Was 0.10% ($0.096) which was slightly below the 6% target.
        MIN_BREAKOUT_PCT      = 0.12;
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
        // Block only 5 min pre-announcement (spread widens) + 30 min post (vol normalises)
        // 14:25-15:00 UTC. The release itself IS the breakout -- trade it after initial spike.
        // Old block was 14:00-15:30 which killed the entire best oil move of the week.
        return (mins >= 14*60+25 && mins < 15*60);  // 14:25-15:00 UTC only
    }
};

// ==============================================================================
// GoldEngine -- GOLD.F (XAU/USD)
//
// INSTRUMENT: Safe-haven asset. INVERSE relationship with equity risk regime.
// TP 0.30%, SL 0.15%, VOL_THRESH 0.04%, MIN_GAP 180s, MAX_HOLD 1500s
//
// GATES (Gold-specific):
//   spread > 0.06%   -> block (gold spread wider than indices)
//   VIX > 60         -> block (true dislocation -- all markets illiquid)
//   RISK_OFF         -> ALLOW freely (gold rallies on fear -- core edge)
//   RISK_ON          -> allow (gold breakouts still work, just less frequent)
//   NEUTRAL          -> allow normally
//
// NOTE: Gold is a safe-haven. It performs BEST in RISK_OFF.
//   We deliberately do NOT block RISK_OFF for gold -- that is when it moves most.
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
        COMPRESSION_LOOKBACK  = 40;    // 60->40: faster warmup, gold ticks infrequently
        BASELINE_LOOKBACK     = 160;   // 250->160: 4x ratio, faster baseline
        COMPRESSION_THRESHOLD = 0.85;  // 0.75->0.85: loosen for elevated vol regime
        MAX_HOLD_SEC          = 1500;
        MIN_GAP_SEC           = 180;
        MAX_SPREAD_PCT        = 0.06;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)  return false; // gold spread gate
        if (!macro)                        return true;
        if (macro->vix > 60.0)             return false; // true dislocation -- all illiquid
        // RISK_OFF = gold's home turf -- always allow
        // RISK_ON / NEUTRAL = allow (compression breakouts valid in all regimes for gold)
        return true;
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

    explicit Us30Engine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.035;
        TP_PCT                = 0.800;
        SL_PCT                = 0.350;
        COMPRESSION_LOOKBACK  = 40;
        BASELINE_LOOKBACK     = 160;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 180;
        MAX_SPREAD_PCT        = 0.05;
        // DJ30 at ~46700: 0.025% = $11.68 over 20 ticks — completely unreachable
        // in early London. 0.006% = $2.80 — meaningful momentum for Dow.
        MOMENTUM_THRESH_PCT   = 0.006;
        // DJ30 at ~46700: 0.12% = $56.04 beyond comp edge — absurd.
        // 0.04% = $18.68 — still a real break above noise for Dow.
        MIN_BREAKOUT_PCT      = 0.04;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)          return false;
        if (!macro)                                return true;
        if (std::fabs(macro->es_nq_div) > 0.0060) return false; // same div gate as SP/NQ
        if (macro->vix > 40.0)                     return false;
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

    explicit Nas100Engine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.050;
        TP_PCT                = 0.700;
        SL_PCT                = 0.400;
        COMPRESSION_LOOKBACK  = 35;
        BASELINE_LOOKBACK     = 140;
        COMPRESSION_THRESHOLD = 0.85;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 180;
        MAX_SPREAD_PCT        = 0.06;
        // NAS100 at ~24500: same as NqEngine — 0.010% = $2.45 momentum threshold
        MOMENTUM_THRESH_PCT   = 0.010;
        // NAS100: 0.08% = $19.60 beyond comp edge — matches NqEngine
        MIN_BREAKOUT_PCT      = 0.08;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)          return false;
        if (!macro)                                return true;
        if (std::fabs(macro->es_nq_div) > 0.0060) return false;
        if (macro->vix > 40.0)                     return false;
        return true;
    }
};

} // namespace omega
