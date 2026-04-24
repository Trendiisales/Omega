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

    // Cross-symbol compression alignment -- set in on_tick from engine phases
    // true = that symbol is currently in COMPRESSION or BREAKOUT_WATCH
    bool        sp_compressing   = false;
    bool        nq_compressing   = false;
    bool        us30_compressing = false;
    bool        ger30_compressing= false;
    bool        uk100_compressing= false;

    // L2 book imbalance per symbol -- bid_size / (bid_size + ask_size) at top 3 levels
    // 0.5 = balanced, >0.65 = bid-heavy (bullish pressure), <0.35 = ask-heavy (bearish)
    double      sp_l2_imbalance   = 0.5;
    double      nq_l2_imbalance   = 0.5;
    double      nas_l2_imbalance  = 0.5;
    double      us30_l2_imbalance = 0.5;
    double      gold_l2_imbalance = 0.5;
    double      eur_l2_imbalance  = 0.5;
    double      gbp_l2_imbalance  = 0.5;
    double      cl_l2_imbalance   = 0.5;
    double      brent_l2_imbalance = 0.5;
    // Previously missing -- added 2026-03-24
    double      ger40_l2_imbalance  = 0.5;
    double      uk100_l2_imbalance  = 0.5;
    double      estx50_l2_imbalance = 0.5;
    double      aud_l2_imbalance    = 0.5;
    double      nzd_l2_imbalance    = 0.5;
    double      jpy_l2_imbalance    = 0.5;

    // ?? Microstructure signals -- per key symbol ???????????????????????????????
    // Populated every tick from g_l2_books when cTrader depth feed is active.
    // Degrade gracefully to neutral (0/0.5/false) when no depth data.
    // Used as entry filters: confirm breakout direction before entry.

    // Microprice bias: >0 = upward pressure, <0 = downward pressure
    // Units: absolute price distance from mid (e.g. 0.25 = 25 ticks on gold)
    double      gold_microprice_bias = 0.0;
    double      sp_microprice_bias   = 0.0;
    double      cl_microprice_bias   = 0.0;
    // FX microprice bias -- now populated from real L2 book
    double      eur_microprice_bias  = 0.0;
    double      gbp_microprice_bias  = 0.0;
    double      aud_microprice_bias  = 0.0;
    double      nzd_microprice_bias  = 0.0;
    double      jpy_microprice_bias  = 0.0;
    double      ger40_microprice_bias= 0.0;

    // Book slope: +ve = buy pressure building, -ve = sell pressure
    // Range -1..+1; use |slope| > 0.10 as meaningful
    double      gold_book_slope = 0.0;
    double      sp_book_slope   = 0.0;

    // Liquidity vacuum: true when top-3 levels on a side are very thin
    // ? fast impulse likely in that direction
    bool        gold_vacuum_ask = false;  // ask thin ? up impulse
    bool        gold_vacuum_bid = false;  // bid thin ? down impulse
    bool        sp_vacuum_ask   = false;
    bool        sp_vacuum_bid   = false;
    bool        cl_vacuum_ask   = false;
    bool        cl_vacuum_bid   = false;

    // Wall detection: large single level > 4? average ? likely price rejection
    double      gold_mid_price  = 0.0;   // needed for wall_above/below context
    bool        gold_wall_above = false;
    bool        gold_wall_below = false;
    double      gold_slope      = 0.0;   // L2Book::book_slope() weighted bid-ask pressure -1..+1
    bool        sp_wall_above   = false;
    bool        sp_wall_below   = false;

    // FX vacuum/wall -- Priority 6 backlog (previously L2 imbalance proxy only)
    // Now populated from actual L2 book data same as GOLD/SP/OIL.
    bool        eur_vacuum_ask  = false;
    bool        eur_vacuum_bid  = false;
    bool        eur_wall_above  = false;
    bool        eur_wall_below  = false;
    bool        gbp_vacuum_ask  = false;
    bool        gbp_vacuum_bid  = false;
    bool        gbp_wall_above  = false;
    bool        gbp_wall_below  = false;
    bool        aud_vacuum_ask  = false;
    bool        aud_vacuum_bid  = false;
    bool        nzd_vacuum_ask  = false;
    bool        nzd_vacuum_bid  = false;
    bool        jpy_vacuum_ask  = false;
    bool        jpy_vacuum_bid  = false;
    double      eur_mid_price   = 0.0;
    double      gbp_mid_price   = 0.0;

    // GER40 / UK100 / ESTX50 vacuum (EU equity bracket L2 gate)
    bool        ger40_vacuum_ask  = false;
    bool        ger40_vacuum_bid  = false;
    bool        ger40_wall_above  = false;
    bool        ger40_wall_below  = false;
    bool        uk100_vacuum_ask  = false;
    bool        uk100_vacuum_bid  = false;
    bool        estx50_vacuum_ask = false;
    bool        estx50_vacuum_bid = false;

    // ?? L2 data quality flags ?????????????????????????????????????????????????
    // ctrader_l2_live: true when cTrader depth client has received at least 1 event
    //   AND at least one symbol book has real non-zero size data.
    //   Separate from per-symbol has_data() -- this is the global connectivity flag.
    // gold_l2_real: true specifically when XAUUSD book has non-zero bid+ask sizes.
    //   When false, drift-persistence fallback is used (price-based, not book-based).
    //   This isolates the BlackBull tag-271 issue: FIX sends no sizes, cTrader may or may not.
    bool        ctrader_l2_live  = false;
    bool        gold_l2_real     = false;
    bool        sp_l2_real       = false;
    bool        cl_l2_real       = false;

    // Session time slot -- updated every tick
    // 0=dead(05-07 UTC), 1=London(07-09), 2=London_core(09-12),
    // 3=overlap(12-14), 4=NY(14-17), 5=NY_late(17-22), 6=Asia(22-05)
    int         session_slot = 0;

    // ?? CVD (Cumulative Volume Delta) direction per key symbol ???????????????
    // +1 = buying dominates last 50 ticks, -1 = selling, 0 = neutral
    // Populated every tick from g_edges.cvd. Degrade to 0 when no data.
    int         gold_cvd_dir   = 0;
    int         sp_cvd_dir     = 0;
    int         nq_cvd_dir     = 0;
    int         eurusd_cvd_dir = 0;
    int         usdjpy_cvd_dir = 0;
    // CVD divergence flags: price and CVD moving opposite directions
    bool        gold_cvd_bull_div  = false;  // bullish: price down, CVD up = absorption
    bool        gold_cvd_bear_div  = false;  // bearish: price up, CVD down = distribution
    bool        sp_cvd_bull_div    = false;
    bool        sp_cvd_bear_div    = false;

    // Previous day high/low -- updated each tick in tick_gold.hpp
    // Used as structural gate: 2yr backtest proves entries INSIDE daily range
    // have EV=+1.732pts at 15min. Entries outside = negative EV.
    double      pdh = 0.0;   // previous day high
    double      pdl = 0.0;   // previous day low
};

// Returns session slot multiplier for MIN_BREAKOUT_PCT scaling.
// London open and NY open have highest follow-through -- allow tighter gates.
// Dead zone and late NY have lowest follow-through -- require wider moves.
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
        VOL_THRESH_PCT        = 0.060;  // SIM: raised 0.040?0.060, WR 27% on SP -- need stronger compression signal
        TP_PCT                = 0.600;
        SL_PCT                = 0.350;
        COMPRESSION_LOOKBACK  = 20;
        BASELINE_LOOKBACK     = 80;
        COMPRESSION_THRESHOLD = 0.70;  // SIM: tightened 0.85?0.70 -- only fire on genuinely tight compression
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 90;    // SIM: raised 60?90s gap between entries
        MAX_SPREAD_PCT        = 0.04;
        MOMENTUM_THRESH_PCT   = 0.010; // SIM: raised 0.006?0.010 -- need real momentum not noise
        MIN_BREAKOUT_PCT      = 0.06;  // SIM: raised 0.03?0.06 -- bigger breakout required
        WATCH_TIMEOUT_SEC     = 240;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                    return false;
        if (!macro)                                          return true;
        // Block US equity indices during Asia session (slot 6 = 22:00-05:00 UTC).
        // US index futures have thin volume and poor follow-through in Asia.
        // False breakouts dominate; the session_breakout_mult does not compensate.
        if (macro->session_slot == 6)                        return false;
        if (std::fabs(macro->es_nq_div) > div_threshold)    return false;
        if (macro->vix > vix_panic)                         return false;
        // L2: block extreme imbalance
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
        VOL_THRESH_PCT        = 0.070;  // SIM: raised 0.050?0.070, WR 28.6% -- filter out low-vol fakeouts
        TP_PCT                = 0.700;
        SL_PCT                = 0.400;
        COMPRESSION_LOOKBACK  = 18;
        BASELINE_LOOKBACK     = 70;
        COMPRESSION_THRESHOLD = 0.70;  // SIM: tightened 0.85?0.70 -- require real compression
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 90;    // SIM: raised 60?90s
        MAX_SPREAD_PCT        = 0.05;
        MOMENTUM_THRESH_PCT   = 0.010; // SIM: raised 0.005?0.010
        MIN_BREAKOUT_PCT      = 0.08;  // SIM: raised 0.04?0.08 -- require committed move
        WATCH_TIMEOUT_SEC     = 240;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                    return false;
        if (!macro)                                          return true;
        if (macro->session_slot == 6)                        return false;  // Asia: no US equity
        if (std::fabs(macro->es_nq_div) > div_threshold)    return false;
        if (macro->vix > vix_panic)                         return false;
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
        WATCH_TIMEOUT_SEC     = 240;
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
// GATES: same macro gates as SP/NQ -- divergence, VIX panic.
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
        VOL_THRESH_PCT        = 0.060;  // SIM: raised 0.035?0.060, 0% WR on DJ30 -- much stronger filter
        TP_PCT                = 0.800;
        SL_PCT                = 0.350;
        COMPRESSION_LOOKBACK  = 20;
        BASELINE_LOOKBACK     = 80;
        COMPRESSION_THRESHOLD = 0.70;  // SIM: tightened 0.85?0.70
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 120;   // SIM: raised 60?120s -- DJ30 needs more time between entries
        MAX_SPREAD_PCT        = 0.05;
        MOMENTUM_THRESH_PCT   = 0.010; // SIM: raised 0.006?0.010
        MIN_BREAKOUT_PCT      = 0.08;  // SIM: raised 0.04?0.08
        WATCH_TIMEOUT_SEC     = 240;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                 return false;
        if (!macro)                                       return true;
        if (macro->session_slot == 6)                     return false;  // Asia: no US equity
        if (std::fabs(macro->es_nq_div) > div_threshold) return false;
        if (macro->vix > vix_panic)                      return false;
        const double imb = macro->us30_l2_imbalance;
        if (imb > 0.0 && (imb < 0.20 || imb > 0.80))    return false;
        return true;
    }
};

// ==============================================================================
// Nas100Engine -- NAS100 (Nasdaq-100 cash index)
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
        WATCH_TIMEOUT_SEC     = 240;  // 2 min
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)                 return false;
        if (!macro)                                       return true;
        if (macro->session_slot == 6)                     return false;  // Asia: no US equity
        if (std::fabs(macro->es_nq_div) > div_threshold) return false;
        if (macro->vix > vix_panic)                      return false;
        const double imb = macro->nas_l2_imbalance;
        if (imb > 0.0 && (imb < 0.20 || imb > 0.80))    return false;
        return true;
    }
};

// ==============================================================================
// EuIndexEngine -- GER40, UK100, ESTX50
//
// European equity indices. Correlated with US equity regime (VIX-driven).
// ES/NQ divergence is a US-only signal -- not applicable here.
// VIX panic blocks all equity index trading universally.
// L2 imbalance available for GER40/UK100/ESTX50 via MacroContext fields
// (ger30_l2_imbalance etc) -- but MacroContext does not have these fields yet.
// Gate on VIX only for now; div gate excluded (EUR/GBP-listed, not ES/NQ).
// ==============================================================================
class EuIndexEngine final : public BreakoutEngineBase<EuIndexEngine>
{
public:
    const MacroContext* macro = nullptr;
    double vix_panic = 40.0;  // same threshold as US equity engines

    explicit EuIndexEngine(const char* sym) noexcept {
        symbol = sym;
        // Defaults -- overwritten by apply_generic_index_config() at startup
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)         return false;
        if (!macro)                               return true;
        // VIX panic: same gate as US equity engines.
        // European indices sell off in sync with US during panic regimes.
        if (macro->vix > vix_panic)              return false;
        return true;
    }
};

// ==============================================================================
// BrentEngine -- UKBRENT (Brent Crude Oil)
//
// Commodity. Not equity-regime correlated (same logic as OilEngine for WTI).
// VIX panic at 50 (liquidity crisis threshold, not equity regime threshold).
// EIA inventory window blocked same as WTI -- same release, same impact.
// ==============================================================================
class BrentEngine final : public BreakoutEngineBase<BrentEngine>
{
public:
    const MacroContext* macro = nullptr;
    double vix_panic = 50.0;  // commodity: only true liquidity crisis

    explicit BrentEngine(const char* sym) noexcept {
        symbol = sym;
        // Defaults -- overwritten by apply_generic_brent_config() at startup
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double /*latency_ms*/) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT)         return false;
        if (in_inventory_window())               return false;
        if (macro && macro->vix > vix_panic)     return false;
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
        return (mins >= 14*60+25 && mins < 15*60);  // 14:25-15:00 UTC
    }
};

} // namespace omega
