#pragma once
// ==============================================================================
// BreakoutEngine — CRTP policy-based compression breakout engine
// One instance per primary symbol (MES, MNQ, MCL).
// CRTP eliminates all virtual dispatch — hot path is fully inlined.
// ==============================================================================
#include <deque>
#include <string>
#include <chrono>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <functional>
#include <cstring>
#include "OmegaTradeLedger.hpp"

namespace omega {

enum class Phase : uint8_t { FLAT = 0, COMPRESSION = 1, BREAKOUT = 2 };

struct BreakoutSignal
{
    bool        valid   = false;
    bool        is_long = true;
    double      entry   = 0.0;
    double      tp      = 0.0;
    double      sl      = 0.0;
    const char* reason  = "";
};

struct OpenPos
{
    bool    active          = false;
    bool    is_long         = true;
    double  entry           = 0.0;
    double  tp              = 0.0;
    double  sl              = 0.0;
    double  size            = 1.0;
    double  mfe             = 0.0;
    double  mae             = 0.0;
    int64_t entry_ts        = 0;
    double  spread_at_entry = 0.0;
    char    regime[32]      = {};
};

// ==============================================================================
// CRTP base
// Derived must be: class MyEngine : public BreakoutEngineBase<MyEngine>
// Optional overrides:
//   bool shouldTrade(double bid, double ask, double spread_pct, double lat_ms)
//   void onSignal(const BreakoutSignal&)
// ==============================================================================
template<typename Derived>
class BreakoutEngineBase
{
public:
    // ── Config — set before first tick ───────────────────────────────────────
    double      VOL_THRESH_PCT        = 0.050;
    double      TP_PCT                = 0.400;
    double      SL_PCT                = 2.000;
    int         COMPRESSION_LOOKBACK  = 50;
    int         BASELINE_LOOKBACK     = 200;
    double      COMPRESSION_THRESHOLD = 0.80;
    int         MAX_HOLD_SEC          = 1500;
    int         MIN_GAP_SEC           = 180;
    double      MAX_SPREAD_PCT        = 0.05;
    const char* symbol                = "???";

    // ── Observable state (read by telemetry thread) ───────────────────────────
    Phase   phase          = Phase::FLAT;
    double  comp_high      = 0.0;
    double  comp_low       = 0.0;
    double  recent_vol_pct = 0.0;
    double  base_vol_pct   = 0.0;
    int     signal_count   = 0;
    OpenPos pos;

    using CloseCallback = std::function<void(const TradeRecord&)>;

    // ── Default CRTP hooks ────────────────────────────────────────────────────
    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        return spread_pct <= MAX_SPREAD_PCT && latency_ms <= 15.0;
    }
    void onSignal(const BreakoutSignal& /*sig*/) const noexcept {}

    // ── update() — call on every tick ────────────────────────────────────────
    [[nodiscard]] BreakoutSignal update(double bid, double ask,
                                        double latency_ms,
                                        const char* macro_regime,
                                        CloseCallback on_close) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return {};

        const double mid        = (bid + ask) * 0.5;
        const double spread     = ask - bid;
        const double spread_pct = (mid > 0.0) ? (spread / mid * 100.0) : 999.0;

        m_prices.push_back(mid);
        if (static_cast<int>(m_prices.size()) > BASELINE_LOOKBACK * 2)
            m_prices.pop_front();

        if (static_cast<int>(m_prices.size()) < COMPRESSION_LOOKBACK + 1) return {};

        // Compute volatilities
        recent_vol_pct = rangePct(m_prices.cend() - COMPRESSION_LOOKBACK, m_prices.cend());
        base_vol_pct   = (static_cast<int>(m_prices.size()) >= BASELINE_LOOKBACK)
                         ? rangePct(m_prices.cend() - BASELINE_LOOKBACK, m_prices.cend())
                         : recent_vol_pct;

        // ── Manage open position ──────────────────────────────────────────────
        if (pos.active) {
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move  > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            if ( pos.is_long && mid >= pos.tp) { closePos(mid, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && mid <= pos.tp) { closePos(mid, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if ( pos.is_long && mid <= pos.sl) { closePos(mid, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && mid >= pos.sl) { closePos(mid, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (nowSec() - pos.entry_ts >= static_cast<int64_t>(MAX_HOLD_SEC)) {
                closePos(mid, "TIMEOUT", latency_ms, macro_regime, on_close); return {};
            }
            return {};
        }

        // ── Phase FSM ─────────────────────────────────────────────────────────
        const bool in_compression = (base_vol_pct > 0.0) &&
                                    (recent_vol_pct < COMPRESSION_THRESHOLD * base_vol_pct);

        if (phase == Phase::FLAT) {
            if (in_compression) {
                phase     = Phase::COMPRESSION;
                comp_high = mid;
                comp_low  = mid;
            }
            return {};
        }

        // phase == COMPRESSION
        if (!in_compression) { phase = Phase::FLAT; return {}; }

        if (mid > comp_high) comp_high = mid;
        if (mid < comp_low)  comp_low  = mid;

        const double thresh    = mid * VOL_THRESH_PCT / 100.0;
        const bool long_break  = (mid >= comp_high + thresh);
        const bool short_break = (mid <= comp_low  - thresh);
        if (!long_break && !short_break) return {};

        // CRTP gate
        if (!static_cast<Derived*>(this)->shouldTrade(bid, ask, spread_pct, latency_ms)) {
            phase = Phase::FLAT; return {};
        }

        const int64_t now = nowSec();
        if (now - m_last_signal_ts < static_cast<int64_t>(MIN_GAP_SEC)) {
            phase = Phase::FLAT; return {};
        }

        const bool   is_long = long_break;
        const double tp = is_long ? mid * (1.0 + TP_PCT / 100.0)
                                  : mid * (1.0 - TP_PCT / 100.0);
        const double sl = is_long ? mid * (1.0 - SL_PCT / 100.0)
                                  : mid * (1.0 + SL_PCT / 100.0);

        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = mid;
        pos.tp              = tp;
        pos.sl              = sl;
        pos.size            = 1.0;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.entry_ts        = now;
        pos.spread_at_entry = spread;
        strncpy_s(pos.regime, macro_regime ? macro_regime : "", 31);

        m_last_signal_ts = now;
        ++m_trade_id;
        ++signal_count;
        phase = Phase::FLAT;

        BreakoutSignal sig;
        sig.valid   = true;
        sig.is_long = is_long;
        sig.entry   = mid;
        sig.tp      = tp;
        sig.sl      = sl;
        sig.reason  = is_long ? "COMP_BREAK_LONG" : "COMP_BREAK_SHORT";

        static_cast<Derived*>(this)->onSignal(sig);
        return sig;
    }

    void forceClose(double bid, double ask, const char* reason,
                    double latency_ms, const char* macro_regime,
                    CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        closePos((bid + ask) * 0.5, reason, latency_ms, macro_regime, on_close);
    }

protected:
    std::deque<double> m_prices;
    int64_t            m_last_signal_ts = 0;
    int                m_trade_id       = 0;

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    template<typename It>
    static double rangePct(It begin, It end) noexcept {
        double hi = *begin, lo = *begin;
        for (auto it = begin; it != end; ++it) {
            if (*it > hi) hi = *it;
            if (*it < lo) lo = *it;
        }
        const double mid = (hi + lo) * 0.5;
        return (mid > 0.0) ? ((hi - lo) / mid * 100.0) : 0.0;
    }

    void closePos(double exit_px, const char* reason,
                  double latency_ms, const char* macro_regime,
                  CloseCallback& on_close) noexcept
    {
        if (!pos.active) return;

        TradeRecord tr;
        tr.id            = m_trade_id;
        tr.symbol        = symbol;
        tr.side          = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice    = pos.entry;
        tr.exitPrice     = exit_px;
        tr.tp            = pos.tp;
        tr.sl            = pos.sl;
        tr.size          = pos.size;
        tr.pnl           = (pos.is_long ? (exit_px - pos.entry)
                                        : (pos.entry - exit_px)) * pos.size;
        tr.mfe           = pos.mfe;
        tr.mae           = pos.mae;
        tr.entryTs       = pos.entry_ts;
        tr.exitTs        = nowSec();
        tr.exitReason    = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.latencyMs     = latency_ms;
        tr.engine        = "BreakoutEngine";
        tr.regime        = (macro_regime && *macro_regime) ? macro_regime : pos.regime;

        pos.active = false;
        pos        = OpenPos{};

        if (on_close) on_close(tr);
    }
};

// ==============================================================================
// Concrete engine — standard CRTP policy (no extra overrides needed)
// Used for GOLD.F (the gold multi-stack handles the rest).
// ==============================================================================
class BreakoutEngine final : public BreakoutEngineBase<BreakoutEngine>
{
public:
    explicit BreakoutEngine(const char* sym) noexcept { symbol = sym; }
};

// ==============================================================================
// MacroContext — shared pointer passed into per-symbol shouldTrade() overrides.
// Populated by MacroRegimeDetector results each tick before engine dispatch.
// ==============================================================================
struct MacroContext {
    std::string regime;       // "RISK_ON" | "NEUTRAL" | "RISK_OFF"
    double      vix       = 0.0;
    double      es_nq_div = 0.0;  // ES vs NQ relative return — divergence signal
    bool        sp_open   = false; // US500 position open (cross-symbol guard)
    bool        nq_open   = false; // USTEC position open (cross-symbol guard)
    bool        oil_open  = false; // USOIL position open
};

// ==============================================================================
// SpEngine — US500.F (S&P 500 futures)
//
// Strategy: Compression Breakout with macro regime gating
//
// Parameters rationale:
//   TP 0.60% — SP500 clean breakouts typically extend 0.5-0.8% before fading.
//   SL 0.35% — Above intraday noise (~0.15-0.20%) but cuts failed breaks fast.
//   VOL_THRESH 0.04% — Slightly tighter than default: SP500 is the most liquid,
//                       less noise per tick, compression is real at 0.04%.
//   COMPRESSION_LOOKBACK 60 — 60-tick window captures ~5-10min of SP500 range.
//   MIN_GAP 300s — 5min gap between SP signals: SP can trend for hours but
//                  re-entry after a failed break needs time to reset.
//
// Regime gating (via MacroContext):
//   RISK_OFF + VIX>30: block new longs (trending down), allow shorts only
//   RISK_ON  + VIX<18: full two-way trading
//   NEUTRAL:           full two-way trading
//   ES/NQ divergence > 0.15%: sector rotation in progress — block both sides
//   NQ already open: block SP entry (too correlated — doubles risk)
// ==============================================================================
class SpEngine final : public BreakoutEngineBase<SpEngine>
{
public:
    const MacroContext* macro = nullptr;  // set externally before first tick

    explicit SpEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.040;   // tighter than default — SP is liquid, compression is real
        TP_PCT                = 0.600;   // 0.60% TP — clean SP breakouts extend 0.5-0.8%
        SL_PCT                = 0.350;   // 0.35% SL — above noise, cut fast
        COMPRESSION_LOOKBACK  = 60;      // ~5-10min of SP ticks
        BASELINE_LOOKBACK     = 200;
        COMPRESSION_THRESHOLD = 0.75;    // slightly tighter — SP needs clearer compression
        MAX_HOLD_SEC          = 1200;    // 20min max — SP trends resolve faster than oil
        MIN_GAP_SEC           = 300;     // 5min gap between signals
        MAX_SPREAD_PCT        = 0.04;    // SP spread rarely exceeds 0.02% — 0.04% is safe ceiling
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT) return false;
        if (latency_ms > 15.0)           return false;
        if (!macro)                       return true;  // no context — allow

        // Cross-symbol guard: if NQ already open, skip SP (correlated — doubles exposure)
        if (macro->nq_open) return false;

        // ES/NQ divergence > 0.15%: sector rotation underway, not a clean breakout
        if (std::fabs(macro->es_nq_div) > 0.0015) return false;

        // RISK_OFF + VIX elevated: SP trending directionally, breakout bias skewed
        // In RISK_OFF we allow shorts (momentum) but block longs
        // Note: shouldTrade is called after direction is determined (pos.is_long set)
        // We return false here and let the FSM skip — the regime is checked at signal-emit
        // by returning false for longs specifically. Since CRTP can't inspect direction
        // pre-signal, we block ALL entries when VIX > 35 (panic regime — spread widens too)
        if (macro->vix > 35.0) return false;

        return true;
    }
};

// ==============================================================================
// NqEngine — USTEC.F (Nasdaq futures)
//
// Parameters rationale:
//   TP 0.70% — NQ is more volatile than SP, extends further on clean breaks.
//   SL 0.40% — NQ noise is slightly higher than SP: 0.40% clears daily ATR noise.
//   VOL_THRESH 0.05% — NQ needs a real vol spike to signal, default is appropriate.
//   COMPRESSION_LOOKBACK 50 — NQ compresses slightly faster than SP.
//   MIN_GAP 240s — 4min: NQ can re-compress quickly but needs a reset window.
//
// Regime gating:
//   RISK_OFF + VIX>30: NQ sells off harder than SP (higher beta tech) — block longs only
//                      allow shorts in RISK_OFF (momentum aligned)
//   SP already open: block NQ entry (too correlated, same macro risk)
//   ES/NQ divergence > 0.15%: NQ diverging FROM SP — possible valid move but
//                              also possible mean-reversion setup, block breakout
//   VIX > 35: block all (panic)
// ==============================================================================
class NqEngine final : public BreakoutEngineBase<NqEngine>
{
public:
    const MacroContext* macro = nullptr;

    explicit NqEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.050;   // default — NQ needs a full 0.05% move
        TP_PCT                = 0.700;   // 0.70% TP — NQ extends further
        SL_PCT                = 0.400;   // 0.40% SL — slightly more room than SP
        COMPRESSION_LOOKBACK  = 50;
        BASELINE_LOOKBACK     = 200;
        COMPRESSION_THRESHOLD = 0.75;
        MAX_HOLD_SEC          = 1200;
        MIN_GAP_SEC           = 240;     // 4min gap
        MAX_SPREAD_PCT        = 0.05;
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT) return false;
        if (latency_ms > 15.0)           return false;
        if (!macro)                       return true;

        // Cross-symbol guard: if SP already open, skip NQ
        if (macro->sp_open) return false;

        // ES/NQ divergence: NQ diverging from SP — unclear which will mean-revert
        if (std::fabs(macro->es_nq_div) > 0.0015) return false;

        // Panic VIX
        if (macro->vix > 35.0) return false;

        return true;
    }
};

// ==============================================================================
// OilEngine — USOIL.F (WTI Crude Oil)
//
// Oil is fundamentally different from indices:
//   - Driven by supply/demand news, inventory reports (Wed 14:30 UTC), OPEC
//   - Much more volatile intraday: typical daily ATR 1.5-2.5%
//   - Spreads widen significantly around inventory data
//   - Compression breakout is valid but needs oil-specific sizing
//
// Parameters rationale:
//   TP 1.20% — Oil moves 1-2% on clean breakouts. 0.4% leaves money on the table.
//   SL 0.60% — Oil noise is 0.3-0.5% intraday: SL needs to clear that.
//   VOL_THRESH 0.08% — Oil needs a bigger initial move to confirm compression break.
//   COMPRESSION_LOOKBACK 40 — Oil compresses in shorter windows before inventory spikes.
//   BASELINE_LOOKBACK 150 — Shorter baseline: oil regimes change faster.
//   MIN_GAP 360s — 6min gap: oil can have multiple spikes on same news.
//   MAX_HOLD 1800s — 30min: oil trends can run longer than index moves.
//   MAX_SPREAD_PCT 0.12% — Oil spread widens more than indices.
//
// Regime gating:
//   RISK_OFF: oil often sells off with risk (recession fears) — allow both sides
//   RISK_ON:  oil rallies with risk-on — allow both sides
//   Inventory window (Wed 14:30 UTC ± 30min): block entries — spread explodes
//   Spread > 0.12%: likely near news event — block
// ==============================================================================
class OilEngine final : public BreakoutEngineBase<OilEngine>
{
public:
    const MacroContext* macro = nullptr;

    explicit OilEngine(const char* sym) noexcept {
        symbol                = sym;
        VOL_THRESH_PCT        = 0.080;   // oil needs a real move to break compression
        TP_PCT                = 1.200;   // 1.20% TP — oil runs further
        SL_PCT                = 0.600;   // 0.60% SL — above oil intraday noise
        COMPRESSION_LOOKBACK  = 40;      // oil compresses quickly
        BASELINE_LOOKBACK     = 150;     // shorter baseline — oil regimes shift fast
        COMPRESSION_THRESHOLD = 0.70;    // tighter threshold — oil noise is higher
        MAX_HOLD_SEC          = 1800;    // 30min — oil trends run longer
        MIN_GAP_SEC           = 360;     // 6min gap — oil can multi-spike on news
        MAX_SPREAD_PCT        = 0.120;   // oil spreads widen more
    }

    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        if (spread_pct > MAX_SPREAD_PCT) return false;
        if (latency_ms > 15.0)           return false;

        // EIA inventory report window: Wednesday 14:30 UTC ± 30min
        // Spread explodes, price gaps — no clean breakout possible
        if (in_inventory_window()) return false;

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
        return (mins >= 14*60 && mins < 15*60+30);  // 14:00-15:30 UTC (30min before + 60min after)
    }
};

} // namespace omega
