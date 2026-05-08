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
};

inline const std::vector<EngineConfig>& engine_table() {
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
    out << ",calibration_n_l2_rows,calibration_n_l2_rows_filtered,calibration_n_capture_days\n";

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
            << ',' << r.spread.capture_days.size() << '\n';
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
    row.trip_wr = row.be_wr + cfg.trip_wr_buffer;

    // Expected fire rate per active hour (flat in v1).
    int session_hours = cfg.session_end_utc - cfg.session_start_utc;
    if (session_hours <= 0) session_hours += 24;  // wraparound
    row.expected_fires_per_hour =
        static_cast<double>(cfg.backtest_n_trades) /
        static_cast<double>(cfg.backtest_n_capture_days) /
        static_cast<double>(session_hours);

    // Per-hour table: flat split inside session window, zero outside.
    for (int h = 0; h < 24; ++h) {
        row.fires_per_hour_table[h] =
            in_session(h, cfg.session_start_utc, cfg.session_end_utc)
                ? row.expected_fires_per_hour
                : 0.0;
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
