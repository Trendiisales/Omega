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

enum class Phase : uint8_t { FLAT = 0, COMPRESSION = 1, BREAKOUT_WATCH = 2, IN_TRADE = 3 };

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
    int     watch_ticks    = 0;
    double  recent_vol_pct = 0.0;
    double  base_vol_pct   = 0.0;
    int     signal_count   = 0;
    OpenPos pos;

    using CloseCallback = std::function<void(const TradeRecord&)>;

    // ── Default CRTP hooks ────────────────────────────────────────────────────
    bool shouldTrade(double /*bid*/, double /*ask*/,
                     double spread_pct, double latency_ms) const noexcept
    {
        return spread_pct <= MAX_SPREAD_PCT;
    }
    void onSignal(const BreakoutSignal& /*sig*/) const noexcept {}

    // ── update() — call on every tick ────────────────────────────────────────
    // can_enter=true  → full processing including new entries
    // can_enter=false → warmup + position management only, no new entries
    [[nodiscard]] BreakoutSignal update(double bid, double ask,
                                        double latency_ms,
                                        const char* macro_regime,
                                        CloseCallback on_close,
                                        bool can_enter = true) noexcept
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
        // During warmup (< BASELINE_LOOKBACK ticks): use the longest window we have
        // so compression detection is meaningful from the start.
        // Without this, base_vol = recent_vol → in_compression always false → FLAT forever.
        recent_vol_pct = rangePct(m_prices.cend() - COMPRESSION_LOOKBACK, m_prices.cend());
        if (static_cast<int>(m_prices.size()) >= BASELINE_LOOKBACK) {
            base_vol_pct = rangePct(m_prices.cend() - BASELINE_LOOKBACK, m_prices.cend());
        } else {
            // Warmup: use all available ticks as baseline.
            // This lets compression detection work from tick COMPRESSION_LOOKBACK+1 onwards.
            base_vol_pct = rangePct(m_prices.cbegin(), m_prices.cend());
        }

        // ── Manage open position ──────────────────────────────────────────────
        if (pos.active) {
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move  > pos.mfe) pos.mfe =  move;
            if (-move > pos.mae) pos.mae = -move;

            if ( pos.is_long && mid >= pos.tp) { closePos(mid, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && mid <= pos.tp) { closePos(mid, "TP_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if ( pos.is_long && mid <= pos.sl) { closePos(mid, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }
            if (!pos.is_long && mid >= pos.sl) { closePos(mid, "SL_HIT",  latency_ms, macro_regime, on_close); return {}; }

            // ── BREAKOUT FAILURE SCRATCH ──────────────────────────────────────
            // If within first 120s the price moves against us by > 0.08% of entry,
            // the breakout is confirmed false — cut immediately. Do NOT hold a
            // wrong-direction break for 20 minutes hoping for reversal.
            // Data: USTEC SHORT held 20min at -$20 vs SL $100 away. Scratch saves
            // ~$10-15 by exiting at first confirmation of failure (~60-90s in).
            // 0.08% chosen: above spread noise (0.02-0.04%) but tight enough to
            // catch a genuine false break within the first few minutes.
            {
                const int64_t held_sec = nowSec() - pos.entry_ts;
                if (held_sec <= 120) {
                    const double adverse_pct = pos.is_long
                        ? (pos.entry - mid) / pos.entry * 100.0
                        : (mid - pos.entry) / pos.entry * 100.0;
                    if (adverse_pct > 0.08) {
                        std::cout << "[SCRATCH] " << symbol
                                  << (pos.is_long ? " LONG" : " SHORT")
                                  << " false breakout — adverse=" << adverse_pct
                                  << "% in " << held_sec << "s\n";
                        closePos(mid, "SCRATCH", latency_ms, macro_regime, on_close);
                        return {};
                    }
                }
            }

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
                std::cout << "[ENG-" << symbol << "] COMPRESSION entered"
                          << " recent=" << recent_vol_pct << "% base=" << base_vol_pct
                          << "% ratio=" << (base_vol_pct>0?recent_vol_pct/base_vol_pct:0)
                          << " mid=" << mid << "\n";
                std::cout.flush();
            }
            return {};
        }

        // ── COMPRESSION phase: track the range while vol is compressed ────────
        if (phase == Phase::COMPRESSION) {
            if (in_compression) {
                if (mid > comp_high) comp_high = mid;
                if (mid < comp_low)  comp_low  = mid;
                return {};  // still compressing — keep tracking
            }
            // Compression just ended — transition to BREAKOUT_WATCH.
            // comp_high/comp_low are now frozen. Watch up to 15 ticks for price exit.
            phase       = Phase::BREAKOUT_WATCH;
            watch_ticks = 15;
            std::cout << "[ENG-" << symbol << "] BREAKOUT_WATCH started"
                      << " hi=" << comp_high << " lo=" << comp_low << "\n";
            std::cout.flush();
            // Fall through to BREAKOUT_WATCH check on this same tick
        }

        // ── BREAKOUT_WATCH phase: wait for price to exit the frozen range ─────
        if (phase == Phase::BREAKOUT_WATCH) {
            // Buffer = 0.5x spread (tight — just needs to clear the range edge)
            const double min_exit    = spread * 0.5;
            const bool   long_break  = (mid > comp_high + min_exit);
            const bool   short_break = (mid < comp_low  - min_exit);

            if (!long_break && !short_break) {
                watch_ticks--;
                if (watch_ticks <= 0) {
                    std::cout << "[ENG-" << symbol << "] BREAKOUT_WATCH expired (no exit), resetting"
                              << " mid=" << mid << " hi=" << comp_high << " lo=" << comp_low << "\n";
                    std::cout.flush();
                    phase = Phase::FLAT;
                }
                return {};  // still watching
            }

            // Price has exited the compression range — confirmed breakout
            std::cout << "[ENG-" << symbol << "] BREAKOUT attempt"
                      << (long_break?" LONG":" SHORT")
                      << " mid=" << mid << " hi=" << comp_high << " lo=" << comp_low
                      << " can_enter=" << can_enter << "\n";
            std::cout.flush();

            // Session/latency gate
            if (!can_enter) {
                std::cout << "[ENG-" << symbol << "] BLOCKED: can_enter=false\n"; std::cout.flush();
                phase = Phase::FLAT; return {};
            }

            // CRTP gate — instrument-specific filters (spread, regime, EIA etc)
            if (!static_cast<Derived*>(this)->shouldTrade(bid, ask, spread_pct, latency_ms)) {
                std::cout << "[ENG-" << symbol << "] BLOCKED: shouldTrade=false"
                          << " spread=" << spread_pct << "%\n"; std::cout.flush();
                phase = Phase::FLAT; return {};
            }

            const int64_t now = nowSec();
            if (now - m_last_signal_ts < static_cast<int64_t>(MIN_GAP_SEC)) {
                std::cout << "[ENG-" << symbol << "] BLOCKED: min_gap not met"
                          << " gap=" << (now-m_last_signal_ts) << "s min=" << MIN_GAP_SEC << "\n";
                std::cout.flush();
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
        }  // end BREAKOUT_WATCH

        return {};  // phase not handled (shouldn't reach here)
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
// Per-instrument typed engines (SpEngine, NqEngine, OilEngine) and MacroContext
// live in SymbolEngines.hpp which includes this file.
// ==============================================================================
class BreakoutEngine final : public BreakoutEngineBase<BreakoutEngine>
{
public:
    explicit BreakoutEngine(const char* sym) noexcept { symbol = sym; }
};

} // namespace omega
