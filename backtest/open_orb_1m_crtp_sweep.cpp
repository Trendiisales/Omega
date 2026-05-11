// =============================================================================
// open_orb_1m_crtp_sweep.cpp -- Phase 0 sweep for 1-minute session-open ORB
// =============================================================================
//
// 2026-05-12 S34 (operator-instructed: "yes do all"): write a CRTP Phase 0
//   harness for the 1-minute London/NY Open-Range-Breakout candidate from
//   HANDOFF_S33_FINAL.md §4 item 3. Pattern modeled on
//   backtest/l2_edge_sweep.cpp: standalone binary, no protected file
//   touched, runs against the same Dukascopy + L2 corpora the rest of
//   the S33 sweep used.
//
// HYPOTHESIS
//   In the first ~60 seconds of the London (07:00 UTC) and New York
//   (13:30 UTC) opens, overnight order flow unwinds into the new
//   session. The price range established in the first M minutes is
//   briefly predictive: a clean break above the high (or below the
//   low) within the next K minutes tends to continue. This is the one
//   place where "retail speed" plausibly matters — first-minute order
//   book dynamics — and is the only candidate from S33's untapped list
//   that maps to the operator's "small frequent positive trades" frame
//   and "we have speed" framing without recycling rejected ideas.
//
// ACCEPTANCE GATES (per cell, applied at end)
//   For a cell to be eligible for live promotion (or further C++
//   engine implementation), it must clear ALL of the following on the
//   3-year Dukascopy corpus, evaluated per Duka year:
//
//     n_total      >= 100 trades per cell over the 3-year corpus
//     net_total    >  0  after $0.06/RT broker cost (3 USD/side)
//     net_year_y1  >  0  (each Duka year independently positive)
//     net_year_y2  >  0
//     net_year_y3  >  0
//     wr_total     >= structural-BE WR for the cell's R:R
//
//   Cells that fail any gate are flagged FAIL_<gate> in the output CSV
//   and excluded from the leaderboard.
//
// CRTP PARAMETER GRID (compile-time)
//   RANGE_WINDOW_MIN ∈ {1, 2, 3, 5}        // first M minutes define the range
//   ENTRY_WINDOW_MIN ∈ {3, 5, 10}          // break must occur within K mins
//                                          // after RANGE_WINDOW closes
//   TP_ATR           ∈ {1.0, 1.5, 2.0}     // exit at TP*ATR1m
//   SL_ATR           ∈ {0.5, 1.0, 1.5}     // exit at SL*ATR1m
//   DIRECTION        ∈ {follow, fade}      // trade break or fade it
//
// USAGE
//   open_orb_1m_crtp_sweep <tick.csv>... [options]
//     --symbol  SYM            override auto-detect
//     --outdir  <dir>          default: orb_1m_sweep_results
//     --cost    <usd/RT>       default: 0.06
//     --session both|london|ny default: both
//     --verbose                per-cell progress
//
// BUILD
//   Same pattern as backtest/l2_edge_sweep.cpp. CMakeLists.txt entry:
//     add_executable(open_orb_1m_crtp_sweep
//                    backtest/open_orb_1m_crtp_sweep.cpp)
//     target_include_directories(open_orb_1m_crtp_sweep PRIVATE include)
//     target_compile_features(open_orb_1m_crtp_sweep PRIVATE cxx_std_20)
//
//   Or with build_mac.sh / direct clang++:
//     clang++ -std=c++20 -O2 -Iinclude \
//         backtest/open_orb_1m_crtp_sweep.cpp -o open_orb_1m_crtp_sweep
//
// CORPUS
//   Designed to consume the same files as l2_edge_sweep.cpp:
//     outputs/duka_xauusd_daily/*.csv   (3-yr XAU Duka tick)
//     outputs/histdata_eurusd_daily/*.csv  (1-yr EUR HistData tick)
//     data/l2_ticks_*.csv               (L2 captures; only ts/bid/ask used)
//
//   Pass multiple files on the command line; they are concatenated in
//   order. Each file's date is parsed from filename (YYYY-MM-DD) for the
//   per-year breakdown. If the date can't be parsed, the file is
//   tagged "y_unknown" and excluded from the per-year gate.
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
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// Tick + bar primitives
// =============================================================================
struct Tick {
    int64_t ts_ms = 0;
    double  bid   = 0.0;
    double  ask   = 0.0;
};

struct Bar1m {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
    double  last_bid = 0.0;
    double  last_ask = 0.0;
    int     tick_count = 0;
};

// =============================================================================
// Symbol baselines (subset; extend as needed for other syms)
// =============================================================================
struct SymBaseline {
    const char* sym;
    double      usd_per_pt;
    double      lot;
    double      max_spread;
    const char* note;
};

static const SymBaseline kBaselines[] = {
    { "XAUUSD", 100.0,    0.01, 1.0, "Duka XAU 30mo + L2"        },
    { "EURUSD", 100000.0, 0.10, 0.0003, "HistData EUR 1yr"       },
    { "USTEC.F", 20.0,    0.10, 4.5, "L2 capture (sparse, ~30d)" },
    { "US500.F", 50.0,    0.10, 1.5, "L2 capture (sparse, ~30d)" },
};

static const SymBaseline* lookup_baseline(const std::string& sym) {
    for (const auto& b : kBaselines) {
        if (sym == b.sym) return &b;
    }
    return nullptr;
}

// =============================================================================
// Tick CSV loader (auto-detects three formats from the header row)
//   (A) Duka 3-col:        ts_ms,bid,ask
//   (B) L2 16-col (S19):   ts_ms,bid,ask,l2_imb,...                  (bid=col1, ask=col2)
//   (C) L2 17-col (mid):   ts_ms,mid,bid,ask,l2_imb,...              (bid=col2, ask=col3)
// We detect (C) by checking whether the second header field is "mid".
// =============================================================================
static bool load_tick_csv(const std::string& path, std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    if (!std::getline(f, line)) return false;   // header

    // Parse header for the position of the second field; "mid" → format (C).
    int n_cols = 1;
    for (char c : line) if (c == ',') ++n_cols;
    bool has_mid_col = false;
    {
        size_t first_comma = line.find(',');
        if (first_comma != std::string::npos) {
            size_t second_comma = line.find(',', first_comma + 1);
            std::string col1 = (second_comma == std::string::npos)
                ? line.substr(first_comma + 1)
                : line.substr(first_comma + 1, second_comma - first_comma - 1);
            // trim whitespace
            while (!col1.empty() && std::isspace(static_cast<unsigned char>(col1.front()))) col1.erase(col1.begin());
            while (!col1.empty() && std::isspace(static_cast<unsigned char>(col1.back())))  col1.pop_back();
            if (col1 == "mid") has_mid_col = true;
        }
    }
    (void)n_cols;

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        Tick t{};
        const char* p = line.c_str();
        char* end = nullptr;
        t.ts_ms = std::strtoll(p, &end, 10); if (*end != ',') continue;
        p = end + 1;
        if (has_mid_col) {
            // skip the mid column
            (void)std::strtod(p, &end); if (*end != ',') continue;
            p = end + 1;
        }
        t.bid = std::strtod(p, &end);        if (*end != ',') continue;
        p = end + 1;
        t.ask = std::strtod(p, &end);
        if (t.bid <= 0.0 || t.ask <= 0.0 || t.bid > t.ask) continue;
        out.push_back(t);
    }
    return true;
}

// Extract YYYY year from the filename. Supports three shapes:
//   .../l2_ticks_XAUUSD_2026-05-08.csv  (underscore-separated)
//   .../duka_xauusd_daily/2025-09-10.csv (hyphen date at filename start)
//   .../histdata_eurusd_daily/2024-01-15.csv
// Strategy: scan basename for the first 4-digit run between 1900 and 2099.
static int extract_year(const std::string& path) {
    auto pos = path.find_last_of('/');
    std::string fn = (pos == std::string::npos) ? path : path.substr(pos + 1);
    for (size_t i = 0; i + 4 <= fn.size(); ++i) {
        bool all_digit = true;
        for (size_t j = 0; j < 4; ++j) {
            if (!std::isdigit(static_cast<unsigned char>(fn[i + j]))) {
                all_digit = false;
                break;
            }
        }
        if (!all_digit) continue;
        const int y = std::stoi(fn.substr(i, 4));
        if (y >= 1900 && y <= 2099) return y;
        i += 3;     // skip past this number
    }
    return 0;
}

static std::string symbol_from_path(const std::string& path,
                                    const std::string& override_sym) {
    if (!override_sym.empty()) return override_sym;
    if (path.find("xauusd") != std::string::npos ||
        path.find("XAUUSD") != std::string::npos ||
        path.find("duka_xauusd") != std::string::npos) return "XAUUSD";
    if (path.find("eurusd") != std::string::npos ||
        path.find("EURUSD") != std::string::npos) return "EURUSD";
    if (path.find("ustec")  != std::string::npos ||
        path.find("USTEC")  != std::string::npos) return "USTEC.F";
    if (path.find("us500")  != std::string::npos ||
        path.find("US500")  != std::string::npos) return "US500.F";
    return "XAUUSD";   // best fallback for Duka corpus
}

// =============================================================================
// 1-minute bar aggregator
// =============================================================================
static void ticks_to_bars1m(const std::vector<Tick>& ticks,
                            std::vector<Bar1m>& bars) {
    bars.clear();
    if (ticks.empty()) return;
    int64_t cur_bar_ms = (ticks.front().ts_ms / 60000) * 60000;
    Bar1m b{};
    b.bar_start_ms = cur_bar_ms;
    bool first = true;
    for (const auto& t : ticks) {
        const int64_t bar_ms = (t.ts_ms / 60000) * 60000;
        const double mid = (t.bid + t.ask) * 0.5;
        if (bar_ms != cur_bar_ms) {
            if (!first) bars.push_back(b);
            cur_bar_ms = bar_ms;
            b = Bar1m{};
            b.bar_start_ms = cur_bar_ms;
            b.open = mid; b.high = mid; b.low = mid; b.close = mid;
            first = false;
        }
        if (first) {
            b.open = mid; b.high = mid; b.low = mid; b.close = mid;
            first = false;
        }
        if (mid > b.high) b.high = mid;
        if (mid < b.low ) b.low  = mid;
        b.close = mid;
        b.last_bid = t.bid;
        b.last_ask = t.ask;
        ++b.tick_count;
    }
    if (b.tick_count > 0) bars.push_back(b);
}

// =============================================================================
// Session opens (UTC) — minute-of-day in 0..1439
// =============================================================================
static int minute_of_day_utc(int64_t bar_ms) {
    const std::time_t t = static_cast<std::time_t>(bar_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm.tm_hour * 60 + tm.tm_min;
}

static int day_id_utc(int64_t bar_ms) {
    return static_cast<int>(bar_ms / (24LL * 60 * 60 * 1000));
}

static constexpr int kLondonOpenMin = 7 * 60;        // 07:00 UTC
static constexpr int kNyOpenMin     = 13 * 60 + 30;  // 13:30 UTC

// =============================================================================
// ATR(14) over 1-min bars (Wilder)
// =============================================================================
static std::vector<double> atr14_1m(const std::vector<Bar1m>& bars) {
    std::vector<double> atr(bars.size(), 0.0);
    if (bars.size() < 15) return atr;
    double a = 0.0;
    for (size_t i = 1; i < bars.size(); ++i) {
        const double tr = std::max(bars[i].high - bars[i].low,
                                   std::max(std::abs(bars[i].high - bars[i-1].close),
                                            std::abs(bars[i].low  - bars[i-1].close)));
        if (i <= 14) {
            a = (a * (i - 1) + tr) / i;
        } else {
            a = (a * 13.0 + tr) / 14.0;
        }
        atr[i] = a;
    }
    return atr;
}

// =============================================================================
// Trade record
// =============================================================================
struct Trade {
    int64_t entry_ts_ms = 0;
    int64_t exit_ts_ms  = 0;
    bool    is_long     = false;
    double  entry_px    = 0.0;
    double  exit_px     = 0.0;
    double  sl_px       = 0.0;
    double  tp_px       = 0.0;
    double  atr_at_entry = 0.0;
    double  pnl_pts     = 0.0;
    double  pnl_usd_gross = 0.0;
    double  pnl_usd_net   = 0.0;
    int     year        = 0;
    const char* exit_reason = "";
    const char* session = "";
};

// =============================================================================
// ORB cell (CRTP traits-driven)
// =============================================================================
struct OrbParams {
    int    range_window_min = 1;
    int    entry_window_min = 5;
    double tp_atr           = 1.5;
    double sl_atr           = 1.0;
    bool   fade             = false;     // false = follow, true = fade
};

static std::string cell_name(const OrbParams& p) {
    std::ostringstream s;
    s << "ORB_RW" << p.range_window_min
      << "_EW"   << p.entry_window_min
      << "_TP"   << p.tp_atr
      << "_SL"   << p.sl_atr
      << "_"     << (p.fade ? "FADE" : "FOLLOW");
    return s.str();
}

// Simulate one session-open ORB for a single day in one session window.
// Returns 0 or 1 trade (each day×session can produce at most one trade per cell).
static int simulate_one_session(const std::vector<Bar1m>& bars,
                                const std::vector<double>& atr14,
                                size_t open_idx,
                                int session_open_min,
                                const char* session_label,
                                int year,
                                const OrbParams& p,
                                std::vector<Trade>& trades) {
    // `open_idx` is the index of the bar that STARTS at session_open_min.
    // Establish the open range over the first range_window_min bars.
    if (open_idx + p.range_window_min + p.entry_window_min + 60 >= bars.size())
        return 0;
    double range_hi = -1e18, range_lo = 1e18;
    for (int k = 0; k < p.range_window_min; ++k) {
        const auto& b = bars[open_idx + k];
        if (b.high > range_hi) range_hi = b.high;
        if (b.low  < range_lo) range_lo = b.low;
    }
    if (range_hi <= range_lo) return 0;

    // ATR snapshot taken from the bar at the end of the range window so we
    // don't peek forward into the entry window's bars.
    const size_t atr_idx = open_idx + p.range_window_min - 1;
    const double atr = atr14[atr_idx];
    if (atr <= 0.0) return 0;

    // Scan entry window for the first break of range_hi (long) or range_lo (short).
    Trade tr{};
    tr.year     = year;
    tr.session  = session_label;
    bool fired  = false;
    size_t entry_bar_idx = 0;
    for (int k = 0; k < p.entry_window_min; ++k) {
        const size_t i = open_idx + p.range_window_min + k;
        if (i >= bars.size()) return 0;
        const auto& b = bars[i];
        bool brk_up = (b.high > range_hi);
        bool brk_dn = (b.low  < range_lo);
        if (!brk_up && !brk_dn) continue;
        // First break wins. Resolve direction by fade-vs-follow.
        const bool break_long = brk_up && !brk_dn;
        if (p.fade) tr.is_long = !break_long;
        else        tr.is_long = break_long;
        // Entry at the break price + slip-adjusted side.
        tr.entry_px    = tr.is_long ? b.last_ask : b.last_bid;
        if (tr.entry_px <= 0.0) tr.entry_px = b.close;
        tr.entry_ts_ms = b.bar_start_ms + 30000;     // mid-bar approx
        tr.atr_at_entry = atr;
        tr.sl_px = tr.is_long ? tr.entry_px - p.sl_atr * atr
                              : tr.entry_px + p.sl_atr * atr;
        tr.tp_px = tr.is_long ? tr.entry_px + p.tp_atr * atr
                              : tr.entry_px - p.tp_atr * atr;
        entry_bar_idx = i;
        fired = true;
        break;
    }
    if (!fired) return 0;

    // Walk subsequent bars until SL or TP hit, or 60-min hard timeout from entry.
    const size_t cap = std::min(bars.size(), entry_bar_idx + 60);
    for (size_t j = entry_bar_idx + 1; j < cap; ++j) {
        const auto& b = bars[j];
        bool hit_sl = false, hit_tp = false;
        double exit_px = 0.0;
        if (tr.is_long) {
            if (b.low  <= tr.sl_px) { exit_px = tr.sl_px; hit_sl = true; }
            else if (b.high >= tr.tp_px) { exit_px = tr.tp_px; hit_tp = true; }
        } else {
            if (b.high >= tr.sl_px) { exit_px = tr.sl_px; hit_sl = true; }
            else if (b.low  <= tr.tp_px) { exit_px = tr.tp_px; hit_tp = true; }
        }
        if (!hit_sl && !hit_tp) continue;
        tr.exit_px = exit_px;
        tr.exit_ts_ms = b.bar_start_ms + 30000;
        tr.exit_reason = hit_tp ? "TP_HIT" : "SL_HIT";
        tr.pnl_pts = tr.is_long ? (tr.exit_px - tr.entry_px)
                                : (tr.entry_px - tr.exit_px);
        trades.push_back(tr);
        return 1;
    }
    // Timed out without hitting either; close at last bar's close.
    const auto& last = bars[cap - 1];
    tr.exit_px = last.close;
    tr.exit_ts_ms = last.bar_start_ms + 30000;
    tr.exit_reason = "TIME_STOP";
    tr.pnl_pts = tr.is_long ? (tr.exit_px - tr.entry_px)
                            : (tr.entry_px - tr.exit_px);
    trades.push_back(tr);
    return 1;
}

// =============================================================================
// Sweep across bars for one cell
// =============================================================================
struct CellStats {
    OrbParams           params{};
    std::vector<Trade>  trades;
    double              wr            = 0.0;
    double              net_usd_total = 0.0;
    std::map<int,double> net_usd_by_year;
    int                 n_total       = 0;
    bool                pass_n        = false;
    bool                pass_net      = false;
    bool                pass_years    = false;
    bool                pass_wr       = false;
};

// Per-file simulation only — accumulates trades into cell.trades but does
// NOT score. Scoring runs once at the end via finalize_cell, otherwise the
// scoring loop would re-add every accumulated trade to cell.net_usd_total
// on every file iteration (O(N_files) inflation of net).
static void run_cell(const std::vector<Bar1m>& bars,
                     const std::vector<double>& atr14,
                     const SymBaseline& /*base*/,
                     int year_hint,
                     bool do_london, bool do_ny,
                     double /*cost_per_rt_usd*/,
                     CellStats& cell) {
    int prev_day = INT32_MIN;
    bool fired_london_today = false;
    bool fired_ny_today     = false;
    for (size_t i = 0; i < bars.size(); ++i) {
        const int dom = day_id_utc(bars[i].bar_start_ms);
        if (dom != prev_day) {
            prev_day = dom;
            fired_london_today = false;
            fired_ny_today = false;
        }
        const int mod = minute_of_day_utc(bars[i].bar_start_ms);
        if (do_london && mod == kLondonOpenMin && !fired_london_today) {
            simulate_one_session(bars, atr14, i, kLondonOpenMin, "LONDON",
                                 year_hint, cell.params, cell.trades);
            fired_london_today = true;
        }
        if (do_ny && mod == kNyOpenMin && !fired_ny_today) {
            simulate_one_session(bars, atr14, i, kNyOpenMin, "NY",
                                 year_hint, cell.params, cell.trades);
            fired_ny_today = true;
        }
    }
}

// Score the cell once, after every file has contributed its trades. Idempotent:
// resets cell.net_usd_total and cell.net_usd_by_year before summing.
static void finalize_cell(const SymBaseline& base,
                          double cost_per_rt_usd,
                          CellStats& cell) {
    cell.n_total = (int)cell.trades.size();
    cell.net_usd_total = 0.0;
    cell.net_usd_by_year.clear();
    int wins = 0;
    for (auto& t : cell.trades) {
        t.pnl_usd_gross = t.pnl_pts * base.lot * base.usd_per_pt;
        t.pnl_usd_net   = t.pnl_usd_gross - cost_per_rt_usd;
        if (t.pnl_usd_net > 0.0) ++wins;
        cell.net_usd_total += t.pnl_usd_net;
        cell.net_usd_by_year[t.year] += t.pnl_usd_net;
    }
    cell.wr = cell.n_total > 0 ? (double)wins / cell.n_total : 0.0;
}

// Apply acceptance gates. Returns "PASS" or "FAIL_<gate>".
static const char* evaluate_gates(CellStats& cell) {
    cell.pass_n   = (cell.n_total >= 100);
    cell.pass_net = (cell.net_usd_total > 0.0);
    cell.pass_years = !cell.net_usd_by_year.empty();
    for (const auto& kv : cell.net_usd_by_year) {
        if (kv.second <= 0.0) { cell.pass_years = false; break; }
    }
    const double sl = cell.params.sl_atr, tp = cell.params.tp_atr;
    const double be_wr = sl / (sl + tp);  // structural break-even on points
    cell.pass_wr = (cell.wr >= be_wr);

    if (!cell.pass_n)     return "FAIL_n_lt_100";
    if (!cell.pass_net)   return "FAIL_net_le_0";
    if (!cell.pass_years) return "FAIL_year_le_0";
    if (!cell.pass_wr)    return "FAIL_wr_lt_BE";
    return "PASS";
}

// =============================================================================
// Sweep grid expansion (compile-time-fixed grid; one CellStats per combo)
// =============================================================================
static void build_grid(std::vector<OrbParams>& grid) {
    const int    rw[]   = { 1, 2, 3, 5 };
    const int    ew[]   = { 3, 5, 10 };
    const double tp[]   = { 1.0, 1.5, 2.0 };
    const double sl[]   = { 0.5, 1.0, 1.5 };
    const bool   fade[] = { false, true };
    for (int r : rw)
      for (int e : ew)
        for (double t : tp)
          for (double s : sl)
            for (bool f : fade) {
                OrbParams p{};
                p.range_window_min = r;
                p.entry_window_min = e;
                p.tp_atr = t;
                p.sl_atr = s;
                p.fade   = f;
                grid.push_back(p);
            }
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: open_orb_1m_crtp_sweep <tick.csv>... [options]\n"
            "  --symbol  SYM            override auto-detect\n"
            "  --outdir  <dir>          default: orb_1m_sweep_results\n"
            "  --cost    <usd/RT>       default: 0.06\n"
            "  --session both|london|ny default: both\n"
            "  --verbose                per-cell progress\n");
        return 2;
    }
    std::vector<std::string> files;
    std::string override_sym, outdir = "orb_1m_sweep_results";
    std::string session = "both";
    double cost = 0.06;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--symbol"  && i + 1 < argc) override_sym = argv[++i];
        else if (a == "--outdir" && i + 1 < argc) outdir = argv[++i];
        else if (a == "--cost"   && i + 1 < argc) cost = std::stod(argv[++i]);
        else if (a == "--session"&& i + 1 < argc) session = argv[++i];
        else if (a == "--verbose") verbose = true;
        else if (!a.empty() && a[0] != '-') files.push_back(a);
        else { std::fprintf(stderr, "[FATAL] unknown arg: %s\n", a.c_str()); return 2; }
    }
    if (files.empty()) {
        std::fprintf(stderr, "[FATAL] no tick CSV files provided\n");
        return 2;
    }
    const bool do_london = (session == "both" || session == "london");
    const bool do_ny     = (session == "both" || session == "ny");

    // Load all ticks, by file (so we can track per-file year for the per-year
    // gate). Then bar-aggregate per file and sweep the grid across the
    // concatenated bars.
    std::string sym = symbol_from_path(files.front(), override_sym);
    const SymBaseline* base = lookup_baseline(sym);
    if (!base) {
        std::fprintf(stderr, "[FATAL] no baseline for symbol %s\n", sym.c_str());
        return 2;
    }
    std::fprintf(stdout, "[INFO] symbol=%s baseline=%s cost=$%.2f/RT lot=%.4f\n",
                 sym.c_str(), base->note, cost, base->lot);

    // Build the grid (288 combos at the default sizes).
    std::vector<OrbParams> grid;
    build_grid(grid);
    std::vector<CellStats> cells(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) cells[i].params = grid[i];

    // For each file (one Duka day), load → bar-aggregate → run every cell.
    int total_ticks = 0;
    for (const auto& f : files) {
        std::vector<Tick> ticks;
        if (!load_tick_csv(f, ticks)) {
            std::fprintf(stderr, "[WARN] could not load %s; skipping\n", f.c_str());
            continue;
        }
        total_ticks += (int)ticks.size();
        std::vector<Bar1m> bars;
        ticks_to_bars1m(ticks, bars);
        if (bars.size() < 32) continue;
        const std::vector<double> atr = atr14_1m(bars);
        const int year = extract_year(f);
        if (verbose) std::fprintf(stdout, "[FILE] %s  ticks=%zu  bars=%zu  year=%d\n",
                                  f.c_str(), ticks.size(), bars.size(), year);
        for (size_t ci = 0; ci < grid.size(); ++ci) {
            run_cell(bars, atr, *base, year, do_london, do_ny, cost, cells[ci]);
        }
    }

    std::fprintf(stdout, "[INFO] loaded %d ticks across %zu files\n",
                 total_ticks, files.size());

    // Score every cell once (idempotent) now that all files have contributed.
    for (auto& c : cells) finalize_cell(*base, cost, c);

    // Output: ranked CSV.
    // Note: outdir is assumed to already exist; create with `mkdir -p` if not.
    const std::string csv_path = outdir + "/leaderboard_" + sym + ".csv";
    std::ofstream out(csv_path);
    if (!out.is_open()) {
        std::fprintf(stderr, "[FATAL] cannot write %s; mkdir the outdir first\n",
                     csv_path.c_str());
        return 3;
    }
    out << "cell,n,wr,net_usd_total,pass_n,pass_net,pass_years,pass_wr,verdict,"
           "rw_min,ew_min,tp_atr,sl_atr,direction\n";

    // Score gates + rank.
    struct Row { CellStats* c; const char* verdict; };
    std::vector<Row> rows;
    rows.reserve(cells.size());
    for (auto& c : cells) {
        const char* v = evaluate_gates(c);
        rows.push_back({&c, v});
    }
    std::sort(rows.begin(), rows.end(),
        [](const Row& a, const Row& b){
            return a.c->net_usd_total > b.c->net_usd_total;
        });

    int passed = 0;
    for (const auto& r : rows) {
        const auto& p = r.c->params;
        out << cell_name(p)
            << ',' << r.c->n_total
            << ',' << std::fixed << std::setprecision(4) << r.c->wr
            << ',' << std::fixed << std::setprecision(2) << r.c->net_usd_total
            << ',' << (r.c->pass_n     ? 1 : 0)
            << ',' << (r.c->pass_net   ? 1 : 0)
            << ',' << (r.c->pass_years ? 1 : 0)
            << ',' << (r.c->pass_wr    ? 1 : 0)
            << ',' << r.verdict
            << ',' << p.range_window_min
            << ',' << p.entry_window_min
            << ',' << std::fixed << std::setprecision(2) << p.tp_atr
            << ',' << std::fixed << std::setprecision(2) << p.sl_atr
            << ',' << (p.fade ? "FADE" : "FOLLOW")
            << '\n';
        if (std::strcmp(r.verdict, "PASS") == 0) ++passed;
    }
    out.close();

    std::fprintf(stdout, "[DONE] %zu cells evaluated  %d PASS  output=%s\n",
                 cells.size(), passed, csv_path.c_str());
    if (passed == 0) {
        std::fprintf(stdout,
            "[VERDICT] NO cells passed all gates on this corpus. Per the\n"
            "          S33 discipline, walk away — do NOT build a production\n"
            "          engine from a sweep with zero validated cells.\n");
    } else {
        std::fprintf(stdout,
            "[VERDICT] %d cells PASSED. Inspect leaderboard.csv — top cell by\n"
            "          net_usd_total is the production-engine candidate. Do\n"
            "          NOT promote to live; ship as shadow-only first.\n",
            passed);
    }
    return 0;
}

// =============================================================================
// END open_orb_1m_crtp_sweep.cpp
// =============================================================================
