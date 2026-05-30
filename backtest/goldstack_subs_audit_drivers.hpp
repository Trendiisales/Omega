#pragma once
// =============================================================================
// goldstack_subs_audit_drivers.hpp -- CRTP driver tuple for the 10 GoldStack
//                                     sub-engines whose abs-pt thresholds were
//                                     doubled on 2026-05-29 (S37-Z task#18,
//                                     $2400->$4700 price drift).
//
// SCOPE (Tier 1 -- engines that consume process(GoldSnapshot) only,
//                  no bar-pipeline / DOM dependency)
//   1. SessionMomentumEngine        (L288 GoldEngineStack.hpp)
//   2. IntradaySeasonalityEngine    (L495)
//   3. AsianRangeEngine             (L1159)
//   4. DynamicRangeEngine           (L1304)
//   5. NR3TickEngine                (L1486)
//   6. TwoBarReversalEngine         (L1596)
//   7. DonchianBreakoutEngine       (L651)
//   8. VWAPSnapbackEngine           (L1911)
//   9. LiquiditySweepProEngine      (L1975)
//  10. VWAPStretchReversionEngine   (L2266)
//
// DEFERRED
//   - DXYDivergenceEngine: needs s.dx_mid from a DXY tick stream we don't have
//                          on disk. Driver included but stays disabled.
//
// DESIGN
//   * One Driver per engine -- holds the engine instance, a synthetic
//     OpenTrade struct, and a per-engine Stats aggregator (re-uses the
//     dea::Stats / cost model from disabled_engine_audit_drivers.hpp).
//   * GoldSnapshot is synthesised once per tick by the shared SnapshotBuilder
//     (NOT by the production GoldFeatures, because the latter calls
//     std::time(nullptr) for session detection and daily VWAP reset --
//     wall-clock dependency we want to keep out of the backtest path).
//   * SnapshotBuilder maintains cum_pv / cum_vol for VWAP (resets at UTC
//     midnight derived from ts_ms), EWM volatility & trend, and classifies
//     session from ts_ms hour-of-day.
//   * Trade simulation: when Signal returns valid, the Driver opens a
//     position at the current mid with tp_price / sl_price derived from
//     Signal.tp / Signal.sl (gold tick = $0.10 -- documented at
//     GoldEngineStack.hpp:139 "in price ticks (0.10 per tick for gold)").
//     Subsequent ticks check TP (bid >= tp_long / ask <= tp_short), SL
//     (bid <= sl_long / ask >= sl_short), and a MAX_HOLD timeout.
//   * On exit the Driver fabricates a TradeRecord and routes it through
//     Stats which applies the same 0.010% one-way slip cost model as the
//     existing disabled_engine_audit.
//
// TIME-OVERRIDE COUPLING
//   The harness TU (goldstack_subs_audit.cpp) provides
//       extern "C" time_t time(time_t* t);
//   which intercepts every std::time(nullptr) call in the engine stack
//   (21 sites in GoldEngineStack.hpp) and returns simulated tape time.
//   Without that override session-window gates leak wall clock and
//   SessionMomentumEngine fires only if the test happens to run during
//   real London/NY hours.
// =============================================================================

#include "OmegaTimeShim.hpp"   // OMEGA_BT_SHIM_ACTIVE + omega::bt::set_sim_time

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <map>
#include <chrono>

#include "OmegaTradeLedger.hpp"
#include "GoldEngineStack.hpp"

namespace omega::gsa {

// ---------------------------------------------------------------------------
// Stats -- copy of dea::Stats (cost-model + session+month buckets).
// Lifted verbatim so this header has no dependency on
// disabled_engine_audit_drivers.hpp (which pulls in BracketEngine etc.,
// unnecessary here).
// ---------------------------------------------------------------------------
struct Stats {
    int    n_trades = 0, wins = 0;
    double gross = 0, gw = 0, gl = 0;
    double max_dd = 0;
    int    n_asia = 0, n_london = 0, n_ny = 0, n_late = 0;
    double g_asia = 0, g_london = 0, g_ny = 0, g_late = 0;
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
        std::snprintf(buf, sizeof(buf), "%04d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1);
        return buf;
    }

    void add(const omega::TradeRecord& tr, int64_t ts_ms) {
        // Production cost model: 0.010% XAU one-way slip ~ spread/2.
        // bracket_gold_sweep / disabled_engine_audit derivation:
        //   non-TP RT cost = spread / 100.0
        //   TP RT cost     = spread / 200.0
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
// SnapshotBuilder -- replays a tick stream into GoldSnapshot.
// Owns its own VWAP / EWM-vol / EWM-trend state; uses ts_ms (NOT wall clock)
// for session classification + UTC midnight VWAP reset.
//
// One instance shared across all drivers; updated once per tick before
// dispatching to engines.
// ---------------------------------------------------------------------------
class SnapshotBuilder {
public:
    omega::gold::GoldSnapshot snap{};

    void update(double bid, double ask, int64_t ts_ms) noexcept {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // Daily VWAP reset on UTC day change (derived from ts_ms, NOT
        // std::time -- the whole point of synthesising in the harness).
        const time_t t_s = static_cast<time_t>(ts_ms / 1000);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t_s);
#else
        gmtime_r(&t_s, &tm);
#endif
        const int yday = tm.tm_yday;
        if (yday != last_yday_) {
            cum_pv_ = 0.0; cum_vol_ = 0.0; vwap_ = 0.0;
            last_yday_ = yday;
        }

        // Spread-weighted VWAP (matches GoldFeatures::update L201)
        const double gvwap_w = (spread > 1e-10) ? (1.0 / spread) : 1.0;
        cum_pv_  += mid * gvwap_w;
        cum_vol_ += gvwap_w;
        vwap_     = (cum_vol_ > 0.0) ? (cum_pv_ / cum_vol_) : mid;

        last_price_ = mid;

        // 256-tick price-window stddev -- this is what production assigns to
        // snap.volatility (GoldFeatures::get_price_stddev, L231/239-248), and
        // it is the z-score denominator for VWAPSnapback / VWAPStretchReversion.
        // The prior harness assigned the EWM tick-to-tick move (~$0.05) here,
        // which blew every z-score past VWAP_DEV_STRONG and silently gated those
        // engines to zero fires. Replicate the real stddev path verbatim.
        price_ring_[pr_head_] = mid;
        pr_head_ = (pr_head_ + 1) % 256;
        if (pr_count_ < 256) ++pr_count_;

        // Sweep detection -- verbatim from GoldFeatures L215-223. 50-tick rolling
        // hi/lo reset every 50 ticks; sweep_size = signed distance from the swing
        // extreme. VWAPSnapback gates on |sweep_size| >= MOMENTUM_SPIKE (2.5) and
        // LiquiditySweepPro keys off it -- both were dead with the old 0.0 stub.
        if (tick_counter_ == 0) { sweep_hi_ = mid; sweep_lo_ = mid; }
        if (tick_counter_++ % 50 == 0 && tick_counter_ > 1) {
            sweep_hi_ = mid; sweep_lo_ = mid;
        }
        if (mid > sweep_hi_) sweep_hi_ = mid;
        if (mid < sweep_lo_) sweep_lo_ = mid;
        const double sweep_size = (mid > (sweep_hi_ + sweep_lo_) * 0.5)
            ? (mid - sweep_lo_) : -(sweep_hi_ - mid);

        // EWM trend (matches GoldFeatures L226)
        trend_ = 0.95 * trend_ + 0.05 * (mid - vwap_);

        // Session classification from ts_ms (mirrors GoldFeatures L168-174
        // but reads tape time, not wall time). Production uses Asian as the
        // catch-all, but engines also branch on SessionType::UNKNOWN for the
        // 05:00-07:00 UTC dead zone -- we mirror that here so dead-zone
        // gates behave identically to live.
        const int mins = tm.tm_hour * 60 + tm.tm_min;
        using omega::gold::SessionType;
        SessionType sess;
        if      (mins >= 420 && mins < 630)  sess = SessionType::LONDON;   // 07:00-10:30
        else if (mins >= 630 && mins < 780)  sess = SessionType::OVERLAP;  // 10:30-13:00
        else if (mins >= 780 && mins < 1080) sess = SessionType::NEWYORK;  // 13:00-18:00
        else if (mins >= 300 && mins < 420)  sess = SessionType::UNKNOWN;  // 05:00-07:00 dead
        else                                 sess = SessionType::ASIAN;

        snap.bid               = bid;
        snap.ask               = ask;
        snap.mid               = mid;
        snap.spread            = spread;
        snap.vwap              = vwap_;
        snap.volatility        = price_stddev();
        snap.trend             = trend_;
        snap.sweep_size        = sweep_size;
        snap.prev_mid          = prev_mid_;
        snap.dx_mid            = 0.0;   // no DXY corpus -- DXYDivergenceEngine deferred
        snap.session           = sess;
        snap.supervisor_regime = "NEUTRAL";

        prev_mid_ = mid;
    }

private:
    // 256-tick price-window stddev -- mirrors GoldFeatures::get_price_stddev
    // (L239-248): floor 0.1, returns 1.0 when degenerate. Order-independent
    // (variance), so a plain ring suffices.
    double price_stddev() const {
        const int n = pr_count_ < 256 ? pr_count_ : 256;
        if (n < 2) return 1.0;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += price_ring_[i];
        const double mean = sum / n;
        double sq = 0.0;
        for (int i = 0; i < n; ++i) { const double d = price_ring_[i] - mean; sq += d * d; }
        const double sd = std::sqrt(sq / (n - 1));
        return (sd > 0.1) ? sd : 1.0;
    }

    double   cum_pv_     = 0.0;
    double   cum_vol_    = 0.0;
    double   vwap_       = 0.0;
    double   last_price_ = 0.0;
    double   trend_      = 0.0;
    double   prev_mid_   = 0.0;
    int      last_yday_  = -1;
    double   price_ring_[256] = {};
    int      pr_head_    = 0;
    int      pr_count_   = 0;
    double   sweep_hi_   = 0.0;
    double   sweep_lo_   = 0.0;
    uint64_t tick_counter_ = 0;
};

// ---------------------------------------------------------------------------
// TradeSim -- per-driver synthetic position management. Given a valid Signal
// at the current tick, opens at mid (entry side determines bid/ask later)
// and monitors subsequent ticks for TP / SL / timeout.
//
// Gold tick size = $0.10 per GoldEngineStack.hpp:139. Signal.tp / Signal.sl
// are in ticks; convert to absolute price with TICK_USD.
// ---------------------------------------------------------------------------
struct TradeSim {
    static constexpr double TICK_USD     = 0.10;
    static constexpr int    MAX_HOLD_SEC = 60 * 60;   // 60-min runout default

    bool    active     = false;
    bool    is_long    = false;
    double  entry      = 0.0;
    double  tp_price   = 0.0;
    double  sl_price   = 0.0;
    double  spread_at_entry = 0.0;
    int64_t entry_ts_ms = 0;
    double  mfe = 0.0, mae = 0.0;
    char    engine_tag[32]{};
    char    reason_tag[32]{};

    void open(const omega::gold::Signal& sig, double bid, double ask, int64_t ts_ms) noexcept {
        const double mid    = (bid + ask) * 0.5;
        active        = true;
        is_long       = sig.is_long();
        entry         = mid;
        spread_at_entry = ask - bid;
        entry_ts_ms   = ts_ms;
        mfe = mae    = 0.0;
        const double tp_dist = sig.tp * TICK_USD;
        const double sl_dist = sig.sl * TICK_USD;
        if (is_long) { tp_price = entry + tp_dist; sl_price = entry - sl_dist; }
        else         { tp_price = entry - tp_dist; sl_price = entry + sl_dist; }
        std::strncpy(engine_tag, sig.engine, sizeof(engine_tag) - 1);
        std::strncpy(reason_tag, sig.reason, sizeof(reason_tag) - 1);
    }

    // Per-tick mark-to-market. Returns true if the position closed on this
    // tick; fills `out` with the synthetic TradeRecord ready for Stats::add.
    bool tick(double bid, double ask, int64_t ts_ms,
              const char* symbol_tag,
              omega::TradeRecord& out) noexcept {
        if (!active) return false;
        const double mid = (bid + ask) * 0.5;
        const double excursion = is_long ? (mid - entry) : (entry - mid);
        if (excursion > mfe) mfe = excursion;
        if (excursion < mae) mae = excursion;

        const char* exit_reason = nullptr;
        double exit_px = 0.0;

        // TP / SL check -- use the side-correct quote (long exits at bid,
        // short exits at ask) so the spread cost shows up in raw pnl too.
        if (is_long) {
            if (bid >= tp_price)      { exit_reason = "TP_HIT"; exit_px = tp_price; }
            else if (bid <= sl_price) { exit_reason = "SL_HIT"; exit_px = sl_price; }
        } else {
            if (ask <= tp_price)      { exit_reason = "TP_HIT"; exit_px = tp_price; }
            else if (ask >= sl_price) { exit_reason = "SL_HIT"; exit_px = sl_price; }
        }
        if (!exit_reason) {
            const int64_t age_s = (ts_ms - entry_ts_ms) / 1000;
            if (age_s >= MAX_HOLD_SEC) { exit_reason = "TIMEOUT"; exit_px = mid; }
        }
        if (!exit_reason) return false;

        const double raw = is_long ? (exit_px - entry) : (entry - exit_px);
        out = omega::TradeRecord{};
        out.symbol         = symbol_tag;
        out.side           = is_long ? "LONG" : "SHORT";
        out.entryPrice     = entry;
        out.exitPrice      = exit_px;
        out.tp             = tp_price;
        out.sl             = sl_price;
        out.size           = 1.0;
        out.pnl            = raw;
        out.mfe            = mfe;
        out.mae            = mae;
        out.entryTs        = entry_ts_ms / 1000;
        out.exitTs         = ts_ms / 1000;
        out.exitReason     = exit_reason;
        out.spreadAtEntry  = spread_at_entry;
        out.engine         = engine_tag;
        out.regime         = "NEUTRAL";
        active = false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// CRTP base. Derived must provide:
//   static constexpr const char* NAME;
//   omega::gold::EngineBase* engine_ptr();   (so process() can be invoked
//                                             without knowing the concrete
//                                             engine type)
//   Stats stats; TradeSim sim;
// ---------------------------------------------------------------------------
template <typename Derived>
class EngineDriver {
public:
    Derived&       self()       noexcept { return *static_cast<Derived*>(this); }
    Derived const& self() const noexcept { return *static_cast<Derived const*>(this); }

    void feed(const omega::gold::GoldSnapshot& snap,
              double bid, double ask, int64_t ts_ms) noexcept {
        // 1. Manage open position first (TP/SL can hit this tick).
        if (self().sim.active) {
            omega::TradeRecord tr;
            if (self().sim.tick(bid, ask, ts_ms, "XAUUSD", tr)) {
                self().stats.add(tr, ts_ms);
            }
            return;  // no new entries while one is open
        }
        // 2. Look for an entry.
        omega::gold::Signal sig = self().engine_ptr()->process(snap);
        if (sig.valid) self().sim.open(sig, bid, ask, ts_ms);
    }

    const char*  name()  const noexcept { return Derived::NAME; }
    const Stats& stats() const noexcept { return self().stats; }
};

// ---------------------------------------------------------------------------
// Per-engine drivers. Each holds the engine instance, calls setEnabled(true),
// exposes engine_ptr() for the CRTP base.
// ---------------------------------------------------------------------------
#define GOLD_DRIVER(Driver_, EngineType_, NameStr_)                       \
class Driver_ : public EngineDriver<Driver_> {                             \
public:                                                                    \
    static constexpr const char* NAME = NameStr_;                          \
    EngineType_ eng;                                                       \
    Stats       stats;                                                     \
    TradeSim    sim;                                                       \
    Driver_() { eng.setEnabled(true); }                                    \
    omega::gold::EngineBase* engine_ptr() noexcept { return &eng; }        \
}

GOLD_DRIVER(SessionMomentumDriver,     omega::gold::SessionMomentumEngine,      "SessionMomentum");
GOLD_DRIVER(IntradaySeasonalityDriver, omega::gold::IntradaySeasonalityEngine,  "IntradaySeasonality");
GOLD_DRIVER(AsianRangeDriver,          omega::gold::AsianRangeEngine,           "AsianRange");
GOLD_DRIVER(DynamicRangeDriver,        omega::gold::DynamicRangeEngine,         "DynamicRange");
GOLD_DRIVER(NR3TickDriver,             omega::gold::NR3TickEngine,              "NR3Tick");
GOLD_DRIVER(TwoBarReversalDriver,      omega::gold::TwoBarReversalEngine,       "TwoBarReversal");
GOLD_DRIVER(DonchianBreakoutDriver,    omega::gold::DonchianBreakoutEngine,     "DonchianBreakout");
GOLD_DRIVER(VWAPSnapbackDriver,        omega::gold::VWAPSnapbackEngine,         "VWAPSnapback");
GOLD_DRIVER(LiquiditySweepProDriver,   omega::gold::LiquiditySweepProEngine,    "LiquiditySweepPro");
GOLD_DRIVER(VWAPStretchReversionDriver, omega::gold::VWAPStretchReversionEngine, "VWAPStretchReversion");

#undef GOLD_DRIVER

} // namespace omega::gsa
