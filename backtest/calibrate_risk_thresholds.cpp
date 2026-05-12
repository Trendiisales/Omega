// =============================================================================
// calibrate_risk_thresholds.cpp -- generate per-engine RiskMonitor thresholds
// =============================================================================
//
// 2026-05-08 (Claude / Jo S20+): Standalone calibration tool that reads the L2
//   tape captured by the production logger and emits a per-engine thresholds
//   CSV consumed by include/RiskMonitor.hpp at startup.
//
//   Companion to backtest/microscalper_crtp_sweep.cpp -- same single-TU C++
//   pattern, same CSV-by-name column resolution, same Apple/clang build flags
//   (minus -fbracket-depth=1024 since this tool does no CRTP fold).
//
// THE THREE CHECKS THIS TOOL CALIBRATES
//
//   (1) WR break-even monitor.
//       For each engine we know its TP/SL distance from the engine constants
//       (hardcoded in the ENGINE_TABLE below). The break-even win rate is:
//
//           BE_WR = SL_pts / (TP_pts + SL_pts)
//
//       For the GoldMicroScalper rk12 calibration (TP=0.79, SL=3.0) this gives
//       BE_WR = 0.792 (79.2%). The TRIP_WR threshold sits TRIP_WR_BUFFER above
//       BE (default +3pp -> 82.2%) so the monitor flags before pure break-even.
//
//   (2) Fire-rate over/under-firing monitor.
//       Computed as expected_fires_per_hour = backtest_n_trades / n_capture_days
//       / n_active_hours. The monitor compares rolling 1-hour live fire counts
//       against this baseline with asymmetric thresholds (default: trip on
//       <0.4x for 3 consecutive hours OR >2.5x for any 1 hour). The asymmetric
//       split is intentional -- under-firing is usually the engine correctly
//       avoiding bad conditions; over-firing is usually a regime mismatch.
//
//       v1 emits a single expected_fires_per_hour value (flat across the
//       active session window). v2 will extend to a 24-element per-UTC-hour
//       histogram by replaying the backtest harness's per-trade ts output;
//       the CSV schema already includes 24 columns reserved for that.
//
//   (3) Spread-at-entry distribution monitor.
//       The L2 tape is read row-by-row; ask-bid is computed at every tick
//       inside the engine's session window. We emit median and p95 over the
//       full capture. The monitor compares rolling per-fire spread_at_entry
//       quantiles against these baselines (default: trip on rolling-N median
//       > 1.3x baseline median OR rolling-N p95 > 1.5x baseline p95).
//
// USAGE
//
//   ./calibrate_risk_thresholds [--engine NAME] [--out PATH] [--verbose] FILES...
//
//   By default, calibrates EVERY entry in the ENGINE_TABLE. Pass --engine NAME
//   to calibrate just one. Files are L2 CSV tapes -- the tool auto-routes each
//   tape to the engines whose `symbol` field matches the tape's filename
//   prefix (l2_ticks_<SYMBOL>_<DATE>.csv -> SYMBOL).
//
//   Examples:
//     # Calibrate all engines from all available L2 captures.
//     ./calibrate_risk_thresholds data/l2_ticks_*.csv --verbose
//
//     # Calibrate just MicroScalperGold from XAUUSD captures.
//     ./calibrate_risk_thresholds --engine MicroScalperGold \
//         data/l2_ticks_XAUUSD_*.csv
//
// BUILD
//
//   clang++ -std=c++17 -O3 -DNDEBUG -I include \
//           backtest/calibrate_risk_thresholds.cpp \
//           -o backtest/calibrate_risk_thresholds
//
// OUTPUT SCHEMA (data/risk_monitor_thresholds.csv)
//
//   engine,symbol,tp_pts,sl_pts,be_wr,trip_wr,
//   expected_fires_per_hour,session_start_utc,session_end_utc,
//   backtest_spread_median,backtest_spread_p95,
//   window_n,window_n_minimum,
//   fires_per_hour_00..fires_per_hour_23,
//   calibration_n_l2_rows,calibration_n_capture_days
//
//   Per-UTC-hour columns (fires_per_hour_HH) are populated from a flat split
//   of expected_fires_per_hour in v1. They live in the schema so v2 can fill
//   them with a real histogram without changing the consumer header.
// =============================================================================

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace omega::risk_calibrate {

// -----------------------------------------------------------------------------
// ENGINE_TABLE -- single source of truth for per-engine calibration inputs.
//
// To add a new engine to the monitor:
//   1. Append a row below with the engine's TP/SL/session/backtest_n_trades.
//   2. Re-run this tool over the relevant L2 captures.
//   3. The monitor (RiskMonitor.hpp) auto-discovers the new row at startup.
//
// `backtest_n_trades` and `n_capture_days` are read from the engine's most
// recent sweep leaderboard top combo. For MicroScalperGold rk12: 36,575 trades
// across the 28-day April-May capture. Update if the engine is re-tuned.
// -----------------------------------------------------------------------------
struct EngineConfig {
    const char* name;
    const char* symbol;
    double      tp_pts;
    double      sl_pts;
    double      max_spread_pts;
    int         session_start_utc;
    int         session_end_utc;       // exclusive upper bound; <= start means wraparound
    int         backtest_n_trades;
    int         backtest_n_capture_days;
    double      backtest_wr;           // recorded for the diagnostic header only
    double      trip_wr_buffer;        // BE_WR + this -> TRIP_WR (3pp default)
    int         window_n;              // rolling window size for live evaluation
    int         window_n_minimum;      // minimum samples before WR check fires

    // 2026-05-12 S37-P3: optional per-engine overrides for engines whose
    //   TP/SL are ATR-derived (formula BE_WR doesn't apply) or whose
    //   fire-rate is too low for the per-hour-bucket evaluator's defaults.
    //   Leave at 0 / 0.0 to use the formula / RiskMonitor struct defaults.
    double      manual_trip_wr_override     = 0.0;   // if > 0, used as TRIP_WR directly
    double      fire_over_ratio_override    = 0.0;   // if > 0, written to CSV; else default 2.5
    double      fire_under_ratio_override   = 0.0;   // if > 0, written to CSV; else default 0.4
    int         fire_under_consec_override  = 0;     // if > 0, written to CSV; else default 3
    // Per-UTC-hour fires-per-hour override. If sum is 0, the calibrator
    //   uses the flat-split derived from backtest_n_trades. Otherwise
    //   each hour's value is written directly to the corresponding
    //   fires_per_hour_HH column in the output CSV.
    std::array<double, 24> fires_per_hour_override = {};
};

inline const std::vector<EngineConfig>& engine_table() {
    // Per-UTC-hour fires-per-hour distribution for UstecTrendFollow5m,
    // derived from outputs/ustec_trend_follow_5m_planA_baseline.csv
    // filtered to atr_at_entry >= 20 (S37 MIN_ATR=20 entry filter) and
    // scaled to the Plan B winner's 1.74-trades-per-day mean rate
    // (1326 trades / 760 days). Hours with rate < 0.05 fires/hour are
    // set to 0 so the per-hour evaluator skips them (RiskMonitor.hpp:467
    // early-returns when expected <= 0.0). The active hours (US-session-
    // adjacent: 03-04, 07-15, 18) match the engine's empirical fire
    // distribution; off-hours skip evaluation which is correct given the
    // engine has no session filter in source but the tape is structurally
    // quiet during those windows.
    static constexpr std::array<double, 24> kUstecTfFiresPerHour = {
        // 00     01     02     03     04     05     06     07
        0.000, 0.000, 0.000, 0.038, 0.036, 0.000, 0.000, 0.038,
        // 08     09     10     11     12     13     14     15
        0.088, 0.209, 0.265, 0.155, 0.142, 0.130, 0.124, 0.119,
        // 16     17     18     19     20     21     22     23
        0.000, 0.000, 0.125, 0.000, 0.000, 0.000, 0.000, 0.000
    };

    static const std::vector<EngineConfig> tbl = {
        // MicroScalperGold rk12: see HANDOFF_S20_AFTER_S19_MICROSCALPER.md and
        // include/GoldMicroScalperEngine.hpp lines 159-185 for the calibration
        // provenance. 36,575 backtest trades / 28 days / 16 active hours
        // (06-22 UTC) = ~81 fires/hour expected.
        EngineConfig{
            /* name                   */ "MicroScalperGold",
            /* symbol                 */ "XAUUSD",
            /* tp_pts                 */ 0.79,
            /* sl_pts                 */ 3.00,
            /* max_spread_pts         */ 1.00,
            /* session_start_utc      */ 6,
            /* session_end_utc        */ 22,
            /* backtest_n_trades      */ 36575,
            /* backtest_n_capture_days*/ 28,
            /* backtest_wr            */ 0.925,
            /* trip_wr_buffer         */ 0.03,
            /* window_n               */ 150,
            /* window_n_minimum       */ 50,
            // No overrides -- uses BE_WR formula + RiskMonitor defaults
            // (fire_over_ratio=2.5, fire_under_ratio=0.4, consec=3). Per-hour
            // table is flat-split inside session window, 0 outside.
        },
        // 2026-05-12 S37-P3: UstecTrendFollow5m calibration.
        //   Source of record:
        //     outputs/USTEC_TREND_FOLLOW_5M_PLAN_A_B_REPORT.md
        //   Backtest figures from the Plan B winner row in
        //     outputs/ustec_trend_follow_5m_planB_leaderboard.csv (rank 1):
        //       1326 trades over 760 days, WR 28.28%, gross $17,388,
        //       net $7,586, OOS 260 trades / +$5,207 net.
        //   TP/SL diagnostic values reflect typical ATR=22 at fire time:
        //     sl_dist = 3.0 * 22 = 66 pts (sl_mult * atr14_)
        //     tp_dist = 7.0 * 22 = 154 pts (tp_mult * atr14_)
        //   Since SL/TP are ATR-derived (not constants), the standard
        //   BE_WR = sl/(tp+sl) formula gives 0.300 but the empirical
        //   backtest WR is 0.283 -- the engine is net-positive at
        //   sub-formula-BE because mean winner > mean TP via right-tail
        //   captures. We use manual_trip_wr_override = 0.18 (10pp below
        //   backtest WR; below this WR the engine is clearly out of
        //   the modelled regime).
        //   Fire-rate overrides:
        //     fire_over_ratio=10.0     : single fires don't trip; 2+ fires
        //                                in a single hour during peak (where
        //                                expected~0.2) trips
        //     fire_under_ratio=0.1     : very low; eval is moot for hours
        //                                where expected is zero (skipped)
        //     fire_under_consec=6      : 6 consecutive active hours of zero
        //                                fires trips under-fire. During the
        //                                ~9-hour active window this means a
        //                                full session of zero fires.
        //   tp_pts/sl_pts in this row are diagnostic only -- they do not
        //   feed any RiskMonitor evaluator at runtime. BE_WR is recomputed
        //   from them at load time but immediately superseded by trip_wr
        //   which we override via manual_trip_wr_override.
        EngineConfig{
            /* name                   */ "UstecTrendFollow5m",
            /* symbol                 */ "NAS100",         // L2 capture filename prefix
            /* tp_pts                 */ 154.00,           // 7.0 * ATR~22 (diagnostic)
            /* sl_pts                 */ 66.00,            // 3.0 * ATR~22 (diagnostic)
            /* max_spread_pts         */ 5.00,             // engine MAX_SPREAD
            /* session_start_utc      */ 0,
            /* session_end_utc        */ 24,               // engine has no session filter
            /* backtest_n_trades      */ 1326,
            /* backtest_n_capture_days*/ 760,              // 25 months
            /* backtest_wr            */ 0.2828,           // Plan B IS WR
            /* trip_wr_buffer         */ 0.00,             // unused -- overridden below
            /* window_n               */ 100,              // rolling window for WR / spread
            /* window_n_minimum       */ 30,               // minimum closed trades before eval
            /* manual_trip_wr_override */ 0.18,
            /* fire_over_ratio_override*/ 10.0,
            /* fire_under_ratio_override*/0.1,
            /* fire_under_consec_override*/6,
            /* fires_per_hour_override*/ kUstecTfFiresPerHour,
        },
        // 2026-05-12 S37-P3: per-cell threshold rows for close-side
        //   WR/spread monitoring. The engine emits tr.engine with a cell
        //   suffix ("_Donchian" / "_Keltner") so the ledger differentiates
        //   the two signal families (see UstecTrendFollow5mEngine.hpp S34
        //   BUG #3). RiskMonitor::on_trade_close matches on tr.engine
        //   exactly, so per-cell rows are needed for close-side WR and
        //   spread evaluation to fire.
        //
        //   The fire-side hook in engine_init.hpp still calls under the
        //   umbrella name "UstecTrendFollow5m" so per-cell fire eval would
        //   never trigger. To avoid spurious behaviour we set every
        //   fires_per_hour_HH to 0 for the per-cell rows -- the fire
        //   evaluator's `expected <= 0.0` early-return (RiskMonitor.hpp:467)
        //   means cell rollover ticks don't trip even if (counterfactually)
        //   on_fire were called per-cell.
        //
        //   Per-cell window_n=50 reflects per-cell trade count: Donchian
        //   takes ~70% of fires (~37/month), Keltner ~30% (~16/month).
        //   A 50-trade rolling window is ~1.4 months of Donchian trades
        //   and ~3 months of Keltner trades. window_n_minimum=20 means
        //   the first ~20 trades elapse before WR eval starts.
        EngineConfig{
            /* name                   */ "UstecTrendFollow5m_Donchian",
            /* symbol                 */ "NAS100",
            /* tp_pts                 */ 154.00,
            /* sl_pts                 */ 66.00,
            /* max_spread_pts         */ 5.00,
            /* session_start_utc      */ 0,
            /* session_end_utc        */ 24,
            /* backtest_n_trades      */ 926,              // 70% of 1326 = 926
            /* backtest_n_capture_days*/ 760,
            /* backtest_wr            */ 0.2832,           // approx, per-cell breakdown
            /* trip_wr_buffer         */ 0.00,
            /* window_n               */ 50,
            /* window_n_minimum       */ 20,
            /* manual_trip_wr_override */ 0.18,
            /* fire_over_ratio_override*/ 10.0,
            /* fire_under_ratio_override*/0.1,
            /* fire_under_consec_override*/6,
            /* fires_per_hour_override*/ std::array<double, 24>{},  // all zeros -> skip fire eval
        },
        EngineConfig{
            /* name                   */ "UstecTrendFollow5m_Keltner",
            /* symbol                 */ "NAS100",
            /* tp_pts                 */ 154.00,
            /* sl_pts                 */ 66.00,
            /* max_spread_pts         */ 5.00,
            /* session_start_utc      */ 0,
            /* session_end_utc        */ 24,
            /* backtest_n_trades      */ 400,              // 30% of 1326 = 400
            /* backtest_n_capture_days*/ 760,
            /* backtest_wr            */ 0.2825,           // approx, per-cell breakdown
            /* trip_wr_buffer         */ 0.00,
            /* window_n               */ 50,
            /* window_n_minimum       */ 20,
            /* manual_trip_wr_override */ 0.18,
            /* fire_over_ratio_override*/ 10.0,
            /* fire_under_ratio_override*/0.1,
            /* fire_under_consec_override*/6,
            /* fires_per_hour_override*/ std::array<double, 24>{},  // all zeros -> skip fire eval
        },
    };
    return tbl;
}

// -----------------------------------------------------------------------------
// L2 tape parsing -- mirrors the column-by-name pattern from
// backtest/microscalper_crtp_sweep.cpp so reordered exports work.
// -----------------------------------------------------------------------------
struct L2Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

struct L2Header {
    int ts_idx  = -1;
    int bid_idx = -1;
    int ask_idx = -1;
    bool ok() const { return ts_idx >= 0 && bid_idx >= 0 && ask_idx >= 0; }
};

static std::vector<std::string> split_csv(const std::string& line, char sep = ',') {
    std::vector<std::string> out;
    std::string cell;
    for (char c : line) {
        if (c == sep) { out.push_back(cell); cell.clear(); }
        else if (c == '\r' || c == '\n') { /* drop */ }
        else cell.push_back(c);
    }
    out.push_back(cell);
    return out;
}

static L2Header parse_header(const std::string& line) {
    L2Header h;
    auto cols = split_csv(line);
    for (int i = 0; i < (int)cols.size(); ++i) {
        const std::string& col = cols[i];
        if (col == "ts_ms") h.ts_idx = i;
        else if (col == "bid") h.bid_idx = i;
        else if (col == "ask") h.ask_idx = i;
    }
    return h;
}

static std::vector<L2Tick> load_l2_csv(const std::string& path, bool verbose) {
    std::vector<L2Tick> out;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "[CALIB] WARN cannot open %s\n", path.c_str());
        return out;
    }
    std::string line;
    if (!std::getline(in, line)) return out;
    L2Header h = parse_header(line);
    if (!h.ok()) {
        std::fprintf(stderr, "[CALIB] WARN %s: header missing ts_ms/bid/ask\n",
                     path.c_str());
        return out;
    }
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto cols = split_csv(line);
        if ((int)cols.size() <= h.ask_idx) continue;
        L2Tick t;
        try {
            t.ts_ms = std::stoll(cols[h.ts_idx]);
            t.bid   = std::stod (cols[h.bid_idx]);
            t.ask   = std::stod (cols[h.ask_idx]);
        } catch (...) { continue; }
        if (t.bid <= 0.0 || t.ask <= 0.0) continue;
        if (t.ask < t.bid) continue;  // crossed book; drop
        out.push_back(t);
    }
    if (verbose) {
        std::fprintf(stderr, "[CALIB] loaded %zu rows from %s\n",
                     out.size(), path.c_str());
    }
    return out;
}

// -----------------------------------------------------------------------------
// Filename -> symbol detection.
//   l2_ticks_<SYM>_<DATE>.csv  -> SYM
//   l2_ticks_<DATE>.csv        -> "XAUUSD" (legacy unprefixed = gold-only)
// -----------------------------------------------------------------------------
static std::string detect_symbol(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (base.rfind("l2_ticks_", 0) != 0) return "";
    std::string rest = base.substr(9);  // strip "l2_ticks_"
    auto under = rest.find('_');
    if (under == std::string::npos) return "XAUUSD";
    std::string maybe_sym = rest.substr(0, under);
    if (maybe_sym.size() == 4 && std::isdigit((unsigned char)maybe_sym[0])
        && std::isdigit((unsigned char)maybe_sym[1])
        && std::isdigit((unsigned char)maybe_sym[2])
        && std::isdigit((unsigned char)maybe_sym[3])) {
        // Looks like "2026" -- legacy unprefixed gold capture.
        return "XAUUSD";
    }
    return maybe_sym;
}

// -----------------------------------------------------------------------------
// Spread statistics over the session window.
// -----------------------------------------------------------------------------
struct SpreadStats {
    size_t n              = 0;
    double median         = 0.0;        // over all in-session ticks (raw market view)
    double p95            = 0.0;        // over all in-session ticks (raw market view)
    double max            = 0.0;
    size_t n_above_max    = 0;          // count of ticks with spread > engine MAX_SPREAD
    // Post-MAX_SPREAD-filter distribution. This is what the live engine actually
    // sees at fire time, because any tick with spread > MAX_SPREAD is rejected
    // before the engine emits a TradeRecord. The monitor consumes these values
    // as its spread-at-entry baseline, NOT the raw `median` / `p95` above.
    size_t n_filtered     = 0;
    double median_filtered = 0.0;
    double p95_filtered    = 0.0;
    std::set<int> capture_days;         // unique UTC days seen in the tape
};

static int utc_hour(int64_t ts_ms) {
    std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    return utc.tm_hour;
}

static int utc_day_index(int64_t ts_ms) {
    // Days since unix epoch -- gives a unique int per UTC date.
    return static_cast<int>(ts_ms / 1000 / 86400);
}

static bool in_session(int hour_utc, int start_utc, int end_utc) {
    if (end_utc > start_utc) return hour_utc >= start_utc && hour_utc < end_utc;
    return hour_utc >= start_utc || hour_utc < end_utc;
}

static SpreadStats compute_spread_stats(
    const std::vector<L2Tick>& ticks,
    int session_start_utc, int session_end_utc,
    double max_spread_filter)
{
    SpreadStats s;
    std::vector<double> spreads;             // all in-session
    std::vector<double> spreads_filtered;    // in-session AND spread <= max_spread_filter
    spreads.reserve(ticks.size());
    spreads_filtered.reserve(ticks.size());
    for (const auto& t : ticks) {
        const int h = utc_hour(t.ts_ms);
        if (!in_session(h, session_start_utc, session_end_utc)) continue;
        const double sp = t.ask - t.bid;
        if (sp > max_spread_filter) {
            s.n_above_max++;
        } else {
            spreads_filtered.push_back(sp);
        }
        spreads.push_back(sp);
        if (sp > s.max) s.max = sp;
        s.capture_days.insert(utc_day_index(t.ts_ms));
    }
    s.n          = spreads.size();
    s.n_filtered = spreads_filtered.size();
    auto pctl = [](std::vector<double>& v, double p) {
        if (v.empty()) return 0.0;
        const size_t idx = static_cast<size_t>(p * (v.size() - 1));
        return v[idx];
    };
    if (!spreads.empty()) {
        std::sort(spreads.begin(), spreads.end());
        s.median = pctl(spreads, 0.50);
        s.p95    = pctl(spreads, 0.95);
    }
    if (!spreads_filtered.empty()) {
        std::sort(spreads_filtered.begin(), spreads_filtered.end());
        s.median_filtered = pctl(spreads_filtered, 0.50);
        s.p95_filtered    = pctl(spreads_filtered, 0.95);
    }
    return s;
}

// -----------------------------------------------------------------------------
// Output row + CSV writer.
// -----------------------------------------------------------------------------
struct CalibratedRow {
    EngineConfig cfg;
    SpreadStats  spread;
    double       be_wr   = 0.0;
    double       trip_wr = 0.0;
    double       expected_fires_per_hour = 0.0;
    std::array<double, 24> fires_per_hour_table{};
};

static void write_thresholds_csv(const std::string& path,
                                 const std::vector<CalibratedRow>& rows)
{
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "[CALIB] ERROR cannot open %s for write\n", path.c_str());
        std::exit(1);
    }
    // Schema notes:
    //   backtest_spread_median / p95          -- over ALL in-session ticks (raw market).
    //   backtest_spread_median_filtered / p95 -- over ticks that pass MAX_SPREAD
    //                                            (the engine's actual fire-time view).
    //   The monitor consumes the *_filtered fields. The raw fields are kept in
    //   the CSV as diagnostic context (regime sanity-check on broker tape).
    out << "engine,symbol,tp_pts,sl_pts,be_wr,trip_wr,"
           "expected_fires_per_hour,session_start_utc,session_end_utc,"
           "backtest_spread_median,backtest_spread_p95,"
           "backtest_spread_median_filtered,backtest_spread_p95_filtered,"
           "window_n,window_n_minimum";
    for (int h = 0; h < 24; ++h) out << ",fires_per_hour_" << (h < 10 ? "0" : "") << h;
    out << ",calibration_n_l2_rows,calibration_n_l2_rows_filtered,calibration_n_capture_days"
           // S37-P3 new optional columns. Engines without overrides emit
           // the default values (2.5 / 0.4 / 3); RiskMonitor.hpp:load_thresholds
           // also defaults to those values when the columns are absent, so
           // the new schema is forward+backward compatible with old CSVs.
           ",fire_over_ratio,fire_under_ratio,fire_under_consec_hours\n";

    out << std::fixed;
    for (const auto& r : rows) {
        out.precision(4);
        out << r.cfg.name << ',' << r.cfg.symbol << ','
            << r.cfg.tp_pts << ',' << r.cfg.sl_pts << ','
            << r.be_wr << ',' << r.trip_wr << ','
            << r.expected_fires_per_hour << ','
            << r.cfg.session_start_utc << ',' << r.cfg.session_end_utc << ','
            << r.spread.median << ',' << r.spread.p95 << ','
            << r.spread.median_filtered << ',' << r.spread.p95_filtered << ','
            << r.cfg.window_n << ',' << r.cfg.window_n_minimum;
        for (int h = 0; h < 24; ++h) out << ',' << r.fires_per_hour_table[h];
        out << ',' << r.spread.n << ',' << r.spread.n_filtered
            << ',' << r.spread.capture_days.size();
        // S37-P3 fire-rate evaluator overrides. Use the per-engine override
        // if set, otherwise emit the RiskMonitor struct defaults so the CSV
        // is self-describing.
        const double over_v   = r.cfg.fire_over_ratio_override   > 0.0 ? r.cfg.fire_over_ratio_override   : 2.5;
        const double under_v  = r.cfg.fire_under_ratio_override  > 0.0 ? r.cfg.fire_under_ratio_override  : 0.4;
        const int    consec_v = r.cfg.fire_under_consec_override > 0   ? r.cfg.fire_under_consec_override : 3;
        out << ',' << over_v << ',' << under_v << ',' << consec_v << '\n';
    }
    std::fprintf(stderr, "[CALIB] wrote %zu engine rows -> %s\n",
                 rows.size(), path.c_str());
}

// -----------------------------------------------------------------------------
// Main calibration pass per engine.
// -----------------------------------------------------------------------------
static CalibratedRow calibrate_engine(const EngineConfig& cfg,
                                      const std::vector<std::string>& tape_paths,
                                      bool verbose)
{
    CalibratedRow row;
    row.cfg = cfg;

    // Spread distribution: aggregate every tape that matches this engine's symbol.
    std::vector<L2Tick> all_ticks;
    for (const auto& path : tape_paths) {
        const std::string sym = detect_symbol(path);
        if (sym != cfg.symbol) continue;
        auto ticks = load_l2_csv(path, verbose);
        all_ticks.insert(all_ticks.end(), ticks.begin(), ticks.end());
    }
    if (all_ticks.empty()) {
        std::fprintf(stderr, "[CALIB] WARN engine %s symbol %s: no matching L2 tapes -- "
                     "spread stats will be zero\n",
                     cfg.name, cfg.symbol);
    }
    row.spread = compute_spread_stats(all_ticks,
                                      cfg.session_start_utc, cfg.session_end_utc,
                                      cfg.max_spread_pts);

    // BE_WR and TRIP_WR.
    row.be_wr   = cfg.sl_pts / (cfg.tp_pts + cfg.sl_pts);
    if (cfg.manual_trip_wr_override > 0.0) {
        // S37-P3: empirical override for engines whose TP/SL are ATR-derived
        // and the formula BE_WR doesn't fit the actual backtest WR.
        row.trip_wr = cfg.manual_trip_wr_override;
    } else {
        row.trip_wr = row.be_wr + cfg.trip_wr_buffer;
    }

    // Expected fire rate per active hour (flat in v1).
    int session_hours = cfg.session_end_utc - cfg.session_start_utc;
    if (session_hours <= 0) session_hours += 24;  // wraparound
    row.expected_fires_per_hour =
        static_cast<double>(cfg.backtest_n_trades) /
        static_cast<double>(cfg.backtest_n_capture_days) /
        static_cast<double>(session_hours);

    // Per-hour table: use the explicit override if present, else flat split
    // inside session window with zero outside.
    double override_sum = 0.0;
    for (int h = 0; h < 24; ++h) override_sum += cfg.fires_per_hour_override[h];
    if (override_sum > 0.0) {
        // S37-P3: explicit per-hour distribution from backtest ledger
        // analysis. Each value is a fires-per-hour rate written verbatim
        // to the corresponding fires_per_hour_HH column. Hours with 0
        // skip evaluation in RiskMonitor.hpp:467.
        for (int h = 0; h < 24; ++h) {
            row.fires_per_hour_table[h] = cfg.fires_per_hour_override[h];
        }
    } else {
        for (int h = 0; h < 24; ++h) {
            row.fires_per_hour_table[h] =
                in_session(h, cfg.session_start_utc, cfg.session_end_utc)
                    ? row.expected_fires_per_hour
                    : 0.0;
        }
    }

    // Diagnostic dump.
    const double pct_above = row.spread.n > 0
        ? 100.0 * static_cast<double>(row.spread.n_above_max) /
          static_cast<double>(row.spread.n)
        : 0.0;
    std::fprintf(stderr,
        "[CALIB] %s (%s)\n"
        "         TP=%.2f SL=%.2f -> BE_WR=%.4f TRIP_WR=%.4f (buffer=+%.3f)\n"
        "         backtest %d trades / %d days / %dh active = %.1f fires/hour\n"
        "         spread on %zu in-session L2 rows over %zu capture days:\n"
        "           raw      : median=%.4f  p95=%.4f  max=%.4f\n"
        "           filtered : median=%.4f  p95=%.4f  (n=%zu, post MAX_SPREAD=%.2fpt)\n"
        "           rejected : n_above_max=%zu (%.2f%% of in-session rows)\n",
        cfg.name, cfg.symbol,
        cfg.tp_pts, cfg.sl_pts, row.be_wr, row.trip_wr, cfg.trip_wr_buffer,
        cfg.backtest_n_trades, cfg.backtest_n_capture_days, session_hours,
        row.expected_fires_per_hour,
        row.spread.n, row.spread.capture_days.size(),
        row.spread.median, row.spread.p95, row.spread.max,
        row.spread.median_filtered, row.spread.p95_filtered,
        row.spread.n_filtered, cfg.max_spread_pts,
        row.spread.n_above_max, pct_above);
    return row;
}

// -----------------------------------------------------------------------------
// CLI.
// -----------------------------------------------------------------------------
static void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--engine NAME] [--out PATH] [--verbose] FILES...\n"
        "  --engine NAME   calibrate only the named engine (default: all)\n"
        "  --out PATH      output thresholds CSV (default: data/risk_monitor_thresholds.csv)\n"
        "  --verbose       per-tape progress reporting\n",
        argv0);
}

}  // namespace omega::risk_calibrate

int main(int argc, char** argv) {
    using namespace omega::risk_calibrate;

    std::string out_path = "data/risk_monitor_thresholds.csv";
    std::string only_engine;
    bool verbose = false;
    std::vector<std::string> tape_paths;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--engine" && i + 1 < argc) { only_engine = argv[++i]; }
        else if (a == "--out" && i + 1 < argc) { out_path = argv[++i]; }
        else if (a == "--verbose") { verbose = true; }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
        else { tape_paths.push_back(a); }
    }

    if (tape_paths.empty()) {
        std::fprintf(stderr, "error: at least one L2 tape path is required\n");
        usage(argv[0]);
        return 2;
    }

    std::vector<CalibratedRow> rows;
    for (const auto& cfg : engine_table()) {
        if (!only_engine.empty() && only_engine != cfg.name) continue;
        rows.push_back(calibrate_engine(cfg, tape_paths, verbose));
    }
    if (rows.empty()) {
        std::fprintf(stderr, "error: no engines matched (--engine %s)\n",
                     only_engine.c_str());
        return 2;
    }
    write_thresholds_csv(out_path, rows);
    return 0;
}
