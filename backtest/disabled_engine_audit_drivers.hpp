#pragma once
// =============================================================================
// disabled_engine_audit_drivers.hpp -- CRTP-based engine driver tuple
//                                      for re-validating audit-disabled
//                                      XAUUSD engines against the 2yr corpus.
//
// Built 2026-05-28 (S37-Z / task #9) per operator directive
// "use CRTP, arrays, c++ to manage the backtesting please".
//
// DESIGN
//   1. EngineDriver<Derived> -- CRTP base; uniform feed_tick / boot / stats.
//   2. Per-engine Driver structs : EngineDriver<Self>. Each wraps a
//      production engine class with default config + a TradeRecord sink.
//   3. std::tuple<Driver1, ..., DriverN> -- compile-time array. Fed by a
//      single fold-expression dispatch in the main loop.
//   4. Per-driver Stats object (n / WR / PF / gross / MaxDD / session
//      breakdown / monthly buckets) tracked independently.
//
// SCOPE (Tier 1 -- clean (bid, ask, ts_ms, on_close)-style interface):
//   * GoldBracketEngine  (already audited 2026-05-28; included for parity)
//   * XauusdFvgEngine    (g_disable_xauusd_fvg = true; S46 lookahead bias
//                         in inline-reimpl harness; this is the real class)
//   * DonchianEngine     (TombstoneD by S37 Phase H trail audit -- predicted
//                         negative; default-config baseline measurement)
//
// DEFERRED (Tier 2 -- complex driving):
//   * CandleFlowEngine: needs BarSnap + DOMSnap + ATR pipeline. Out of scope
//     until a minimal BarSnap synthesizer ships.
//   * GoldStack sub-engines (SessionMomentum / IntradaySeasonality /
//     AsianRange / VWAPSnapback / VWAPStretchReversion / DXYDivergence):
//     consume GoldSnapshot. The OmegaSweepHarnessCRTP target already covers
//     5 of these in CRTP-sweep form; their default-combo measurement should
//     be extracted from that harness rather than re-driven from scratch.
//   * IndexFlow x4: index-symbol corpus needed (USA30 done via IDD measure).
//
// COST MODEL
//   Same as bracket_gold_sweep -- XAUUSD slip preset 0.010% one-way derived
//   from TradeRecord.spreadAtEntry. TP_HIT exits pay half-spread; non-TP
//   exits pay both legs. Result returned in TradeRecord.pnl units.
// =============================================================================

// OmegaTimeShim must precede engine headers so OMEGA_BT_SHIM_ACTIVE is
// defined when BracketEngine.hpp etc. parse their nowSec() macros. The
// CMake target also force-includes via -include so the macro is present
// even when this header is consumed by a TU that #includes drivers.hpp
// before OmegaTimeShim -- explicit include here keeps the LSP happy.
#include "OmegaTimeShim.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <map>

#include "OmegaTradeLedger.hpp"
#include "BracketEngine.hpp"
#include "XauusdFvgEngine.hpp"
#include "DonchianEngine.hpp"
#include "CrossAssetEngines.hpp"   // VWAPReversionEngine, TrendPullbackEngine

namespace omega::dea {

// ---------------------------------------------------------------------------
// Aggregator -- per-driver Stats object. Identical to bracket_gold_sweep
// pattern; extracted here so each driver gets its own instance.
// ---------------------------------------------------------------------------
struct Stats {
    int    n_trades = 0, wins = 0;
    double gross = 0, gw = 0, gl = 0;
    double max_dd = 0;
    // Session breakdown (UTC: Asia/London/NY/Late_NY)
    int    n_asia = 0, n_london = 0, n_ny = 0, n_late = 0;
    double g_asia = 0, g_london = 0, g_ny = 0, g_late = 0;
    // Per-month breakdown
    std::map<std::string, double> g_by_month;
    std::map<std::string, int>    n_by_month;

    double running_eq = 0, peak_eq = 0;

    static const char* utc_session(int64_t ts_ms) noexcept {
        const time_t t_s = static_cast<time_t>(ts_ms / 1000);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t_s);
#else
        gmtime_r(&t_s, &tm);
#endif
        const int hr = tm.tm_hour;
        if (hr >= 22 || hr < 7)  return "Asia";
        if (hr >= 7  && hr < 12) return "London";
        if (hr >= 12 && hr < 17) return "NY";
        return "Late_NY";
    }

    static std::string utc_month(int64_t ts_ms) {
        const time_t t_s = static_cast<time_t>(ts_ms / 1000);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t_s);
#else
        gmtime_r(&t_s, &tm);
#endif
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
        return buf;
    }

    void add(const omega::TradeRecord& tr, int64_t ts_ms) {
        // Match production cost model: 0.010% XAU one-way slip ~ spread/2.
        // bracket_gold_sweep documented derivation -- cost in pnl-units:
        //   non-TP RT cost = spread / 100.0  (i.e. spreadAtEntry / tick_mult)
        //   TP RT cost     = spread / 200.0  (half-only -- TP fills at limit)
        const double rt_cost = (tr.exitReason == "TP_HIT")
            ? (tr.spreadAtEntry * 0.5)
            :  tr.spreadAtEntry;
        const double pnl_net = tr.pnl - (rt_cost / 100.0);

        ++n_trades;
        gross      += pnl_net;
        running_eq += pnl_net;
        if (running_eq > peak_eq) peak_eq = running_eq;
        const double dd = peak_eq - running_eq;
        if (dd > max_dd) max_dd = dd;
        if (pnl_net > 0) { ++wins; gw += pnl_net; }
        else             {         gl += -pnl_net; }

        const char* sess = utc_session(ts_ms);
        if      (!std::strcmp(sess, "Asia"))   { ++n_asia;   g_asia   += pnl_net; }
        else if (!std::strcmp(sess, "London")) { ++n_london; g_london += pnl_net; }
        else if (!std::strcmp(sess, "NY"))     { ++n_ny;     g_ny     += pnl_net; }
        else                                   { ++n_late;   g_late   += pnl_net; }

        const std::string mo = utc_month(ts_ms);
        g_by_month[mo] += pnl_net;
        n_by_month[mo] += 1;
    }

    double wr() const { return n_trades ? 100.0 * wins / n_trades : 0.0; }
    double pf() const { return gl > 0 ? gw / gl : 0.0; }
};

// ---------------------------------------------------------------------------
// CRTP base. Derived must provide:
//   static constexpr const char* NAME;
//   void feed_tick_impl(double bid, double ask, int64_t ts_ms);
//   Stats stats;
// ---------------------------------------------------------------------------
template <typename Derived>
class EngineDriver {
public:
    Derived&       self()       noexcept { return *static_cast<Derived*>(this); }
    Derived const& self() const noexcept { return *static_cast<Derived const*>(this); }

    void feed_tick(double bid, double ask, int64_t ts_ms) noexcept {
        self().feed_tick_impl(bid, ask, ts_ms);
    }
    const char* name() const noexcept { return Derived::NAME; }
    const Stats& stats() const noexcept { return self().stats; }
};

// ---------------------------------------------------------------------------
// Driver 1: GoldBracketEngine -- audit baseline (already disabled; default
//           shadow_mode + production class). Sink stamps spread for cost
//           model.
// ---------------------------------------------------------------------------
class GoldBracketDriver : public EngineDriver<GoldBracketDriver> {
public:
    static constexpr const char* NAME = "GoldBracket";
    omega::GoldBracketEngine eng;
    Stats stats;
    int64_t latest_ts_ms = 0;

    GoldBracketDriver() {
        eng.symbol      = "XAUUSD";
        eng.shadow_mode = true;
        eng.ENTRY_SIZE  = 0.01;
    }

    void feed_tick_impl(double bid, double ask, int64_t ts_ms) noexcept {
        latest_ts_ms = ts_ms;
        omega::bt::set_sim_time(ts_ms);
        eng.on_tick(bid, ask, ts_ms,
                    /*can_enter*/ true,
                    /*macro_regime*/ "NEUTRAL",
                    [this, bid, ask](const omega::TradeRecord& tr) {
                        omega::TradeRecord t2 = tr;
                        if (t2.spreadAtEntry <= 0.0) t2.spreadAtEntry = (ask - bid);
                        stats.add(t2, latest_ts_ms);
                    });
    }
};

// ---------------------------------------------------------------------------
// Driver 2: XauusdFvgEngine -- M5 FVG (g_disable_xauusd_fvg = true since
//           S46 2026-05-27, never class-audited). Real class measurement.
// ---------------------------------------------------------------------------
class XauusdFvgDriver : public EngineDriver<XauusdFvgDriver> {
public:
    static constexpr const char* NAME = "XauusdFvg";
    omega::XauusdFvgEngine eng;
    Stats stats;
    int64_t latest_ts_ms = 0;

    XauusdFvgDriver() {
        // engine ships with default config; just enable + shadow.
        // shadow_mode is engine-internal -- the harness ignores it for stat
        // collection (we always record trades).
    }

    void feed_tick_impl(double bid, double ask, int64_t ts_ms) noexcept {
        latest_ts_ms = ts_ms;
        omega::bt::set_sim_time(ts_ms);
        eng.on_tick(bid, ask, ts_ms,
                    /*can_enter*/ true,
                    [this, bid, ask](const omega::TradeRecord& tr) {
                        omega::TradeRecord t2 = tr;
                        if (t2.spreadAtEntry <= 0.0) t2.spreadAtEntry = (ask - bid);
                        stats.add(t2, latest_ts_ms);
                    });
    }
};

// ---------------------------------------------------------------------------
// Driver 3: DonchianEngine -- tombstoned by S37 Phase H trail audit.
//           Default-config baseline. Note: DonchianEngine.on_tick is
//           manage-only (handles open pos); entries come from on_bar
//           callbacks. Without bar pipeline, this driver will collect
//           zero trades -- documented as a Tier 2 deferral case.
// ---------------------------------------------------------------------------
class DonchianDriver : public EngineDriver<DonchianDriver> {
public:
    static constexpr const char* NAME = "Donchian";
    omega::DonchianPortfolio eng;
    Stats stats;
    int64_t latest_ts_ms = 0;

    void feed_tick_impl(double bid, double ask, int64_t ts_ms) noexcept {
        latest_ts_ms = ts_ms;
        omega::bt::set_sim_time(ts_ms);
        // Manage-only path. Bar feed required for entries -- deferred.
        eng.on_tick(bid, ask, ts_ms,
                    [this, bid, ask](const omega::TradeRecord& tr) {
                        omega::TradeRecord t2 = tr;
                        if (t2.spreadAtEntry <= 0.0) t2.spreadAtEntry = (ask - bid);
                        stats.add(t2, latest_ts_ms);
                    });
    }
};

// ---------------------------------------------------------------------------
// Driver 4: VWAPReversionEngine (XAUUSD instance) -- live, never audited
// on 2yr corpus. Per session 2026-05-28 review: emits 43 May trades, but
// 2yr verdict unknown. Engine consumes vwap_seed + optional vix + l2_imb.
// Synthesize: vwap_seed = first-tick mid; vix=0.0; l2_imb=0.5.
// ---------------------------------------------------------------------------
class VWAPRevGoldDriver : public EngineDriver<VWAPRevGoldDriver> {
public:
    static constexpr const char* NAME = "VWAPRev_XAU";
    omega::cross::VWAPReversionEngine eng;
    Stats stats;
    int64_t latest_ts_ms = 0;
    double  first_seed   = 0.0;

    void feed_tick_impl(double bid, double ask, int64_t ts_ms) noexcept {
        latest_ts_ms = ts_ms;
        if (first_seed <= 0.0) first_seed = (bid + ask) * 0.5;
        omega::bt::set_sim_time(ts_ms);
        eng.on_tick("XAUUSD", bid, ask, first_seed,
                    [this, bid, ask](const omega::TradeRecord& tr) {
                        omega::TradeRecord t2 = tr;
                        if (t2.spreadAtEntry <= 0.0) t2.spreadAtEntry = (ask - bid);
                        stats.add(t2, latest_ts_ms);
                    },
                    /*vix=*/0.0, /*l2_imb=*/0.5);
    }
};

// ---------------------------------------------------------------------------
// Driver 5: TrendPullbackEngine (XAUUSD instance) -- live, never audited
// on 2yr corpus. Per session 2026-05-28 review: emits 25 May trades.
// Clean (sym, bid, ask, on_close) signature -- internal EMA9/21/50 + ATR
// computed from ticks.
// ---------------------------------------------------------------------------
class TrendPullbackGoldDriver : public EngineDriver<TrendPullbackGoldDriver> {
public:
    static constexpr const char* NAME = "TrendPullback_XAU";
    omega::cross::TrendPullbackEngine eng;
    Stats stats;
    int64_t latest_ts_ms = 0;

    void feed_tick_impl(double bid, double ask, int64_t ts_ms) noexcept {
        latest_ts_ms = ts_ms;
        omega::bt::set_sim_time(ts_ms);
        eng.on_tick("XAUUSD", bid, ask,
                    [this, bid, ask](const omega::TradeRecord& tr) {
                        omega::TradeRecord t2 = tr;
                        if (t2.spreadAtEntry <= 0.0) t2.spreadAtEntry = (ask - bid);
                        stats.add(t2, latest_ts_ms);
                    });
    }
};

} // namespace omega::dea
