// =====================================================================
// backtest/s33_revised_backtest.cpp
// ---------------------------------------------------------------------
// One purpose: does the S33 Option A geometry pay for itself once we
// subtract realistic round-trip cost? No sweep, no families, no ceremony.
//
// Geometry (hard-coded, identical to the live SHADOW engine):
//   ENTRY_Z              = 2.0
//   ENTRY_LOOKBACK       = 200 ticks
//   TP_DIST_PTS          = 35.0   (= 0.35 USD on XAUUSD at pt_size 0.01)
//   SL_DIST_PTS          = 12.0   (= 0.12 USD)
//   SESSION              = 0..7 UTC (Asia)
//   COOLDOWN             = 60s
//   MAX_HOLD             = 7200s
//   LOT                  = 0.01
//   No BE / trail / reversal / L2-flip (all live overlays disabled --
//   matches the S33 "literal backtest port" semantics).
//
// Cost model:
//   --cost-per-rt N      USD per round-trip, subtracted from each
//                        closed trade's pnl. Default 0.06 (BlackBull
//                        ECN Prime web-spec, operator-confirmed 2026-05-11).
//
// Inputs (any mix; auto-detects per file):
//   --csv <glob>         repeatable. Accepts:
//     * S19 16-column L2 CSV (header with ts_unix, bid, ask, l2_imb...)
//     * 3-column Dukascopy/HistData (ts_ms, bid, ask, headerless or headered)
//
// Output (stdout, single page):
//   - total trades, win rate, mean R, gross PnL, total cost, NET PnL
//   - per-day breakdown (date, n, gross, cost, net) -- so you can see
//     if it's consistent or one freak winning day
//   - PnL/day average, Sharpe, max drawdown
//
// Build:
//   clang++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/s33_revised_backtest.cpp -o backtest/s33_revised_backtest
//
// Run examples on your Mac (assuming repo root):
//   backtest/s33_revised_backtest --csv 'outputs/duka_xauusd_daily/*.csv'
//   backtest/s33_revised_backtest --csv 'outputs/duka_xauusd_daily/*.csv' --csv 'data/l2_ticks_XAUUSD_*.csv'
//   backtest/s33_revised_backtest --csv 'data/l2_ticks_*.csv' --cost-per-rt 0.06
//
// Safety: read-only on inputs, prints to stdout only. No engine link.
// =====================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if __has_include(<glob.h>)
# include <glob.h>
# define HAVE_GLOB 1
#endif

// ---- S33 Option A constants (DO NOT EDIT without operator sign-off) ----
namespace s33 {
    constexpr int    ENTRY_LOOKBACK   = 200;
    constexpr double ENTRY_Z          = 2.0;
    constexpr double TP_DIST_PTS      = 35.0;
    constexpr double SL_DIST_PTS      = 12.0;
    constexpr int    SESSION_START    = 0;   // UTC hour
    constexpr int    SESSION_END      = 7;   // UTC hour exclusive
    constexpr int    COOLDOWN_S       = 60;
    constexpr int    MAX_HOLD_S       = 7200;
    constexpr double LOT              = 0.01;
    constexpr double PT_SIZE          = 0.01;  // XAUUSD: $0.01 per point
    constexpr double VAL_PER_PT       = 1.0;   // $ per price-unit per lot
}

// ---------------------------------------------------------------------
struct Tick {
    long long ts = 0;
    double bid = 0, ask = 0, mid = 0;
};

static std::string trim(std::string s) {
    size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}
static std::string tolow(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s;
}
static std::vector<std::string> splitcsv(const std::string& line) {
    std::vector<std::string> out; std::string cur;
    for (char c : line) { if (c == ',') { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur);
    for (auto& f : out) f = trim(f);
    return out;
}
static bool pd(const std::string& s, double& v) {
    if (s.empty()) return false;
    try { size_t p=0; v = std::stod(s, &p); return p>0; } catch(...) { return false; }
}
static bool pl(const std::string& s, long long& v) {
    if (s.empty()) return false;
    try { size_t p=0; v = std::stoll(s, &p); return p>0; } catch(...) { return false; }
}

static std::vector<std::string> expand_glob(const std::string& pat) {
    std::vector<std::string> out;
#ifdef HAVE_GLOB
    glob_t g{}; int r = glob(pat.c_str(), GLOB_TILDE, nullptr, &g);
    if (r == 0) for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    globfree(&g);
#endif
    if (out.empty()) out.push_back(pat);
    return out;
}

// Load one CSV. Auto-detects 3-col (ts_ms,bid,ask) vs 16-col S19 L2.
static std::vector<Tick> load_csv(const std::string& path, bool verbose) {
    std::vector<Tick> out;
    std::ifstream fs(path);
    if (!fs) { std::cerr << "WARN: cannot open " << path << "\n"; return out; }
    std::string line;
    if (!std::getline(fs, line)) return out;
    auto hdr = splitcsv(line);

    int c_ts = -1, c_bid = -1, c_ask = -1;
    bool ts_in_ms = false;
    bool has_header = false;

    // Detect header by checking first cell for non-numeric
    bool first_looks_numeric = !hdr.empty() && !hdr[0].empty() &&
        (std::isdigit((unsigned char)hdr[0][0]) || hdr[0][0] == '-');

    if (!first_looks_numeric) {
        has_header = true;
        for (size_t i = 0; i < hdr.size(); ++i) {
            std::string h = tolow(hdr[i]);
            if (h == "ts_unix" || h == "ts" || h == "timestamp" || h == "time") c_ts = (int)i;
            if (h == "ts_ms") { c_ts = (int)i; ts_in_ms = true; }
            if (h == "bid") c_bid = (int)i;
            if (h == "ask") c_ask = (int)i;
        }
    } else {
        // 3-col headerless Dukascopy/HistData. Treat first line as data.
        fs.clear(); fs.seekg(0);
        c_ts = 0; c_bid = 1; c_ask = 2; ts_in_ms = true;
    }
    if (c_ts < 0 || c_bid < 0 || c_ask < 0) {
        std::cerr << "WARN: cannot find ts/bid/ask in " << path << "\n";
        return out;
    }

    while (std::getline(fs, line)) {
        if (trim(line).empty()) continue;
        auto row = splitcsv(line);
        if ((int)row.size() <= std::max({c_ts, c_bid, c_ask})) continue;
        Tick t;
        long long raw = 0;
        if (!pl(row[c_ts], raw)) continue;
        t.ts = ts_in_ms ? (raw / 1000) : raw;
        if (!pd(row[c_bid], t.bid)) continue;
        if (!pd(row[c_ask], t.ask)) continue;
        if (t.bid <= 0 || t.ask <= 0) continue;
        t.mid = 0.5 * (t.bid + t.ask);
        out.push_back(t);
    }
    if (verbose) std::cerr << "[load] " << path << "  ticks=" << out.size()
                           << (has_header ? "  (header)" : "  (3-col)") << "\n";
    return out;
}

static int hour_of_ts(long long ts) {
    std::time_t tt = (std::time_t)ts; std::tm tm{}; gmtime_r(&tt, &tm); return tm.tm_hour;
}
static std::string date_of_ts(long long ts) {
    std::time_t tt = (std::time_t)ts; std::tm tm{}; gmtime_r(&tt, &tm);
    char b[16]; std::strftime(b, sizeof(b), "%Y-%m-%d", &tm); return b;
}
static bool in_session(long long ts) {
    int h = hour_of_ts(ts);
    return h >= s33::SESSION_START && h < s33::SESSION_END;
}

// ---------------------------------------------------------------------
// Trade simulation: market entry at z-cross, hold until TP / SL / max hold
// ---------------------------------------------------------------------
struct Trade {
    long long entry_ts = 0, exit_ts = 0;
    int side = 0;  // +1 long, -1 short
    double entry_px = 0, exit_px = 0;
    double gross_pnl = 0;
    double cost = 0;
    double net_pnl = 0;
    std::string exit_reason;
    std::string entry_date;
};

static std::vector<Trade> run_backtest(const std::vector<Tick>& ticks,
                                       double cost_per_rt,
                                       bool verbose) {
    std::vector<Trade> trades;
    if (ticks.size() < (size_t)s33::ENTRY_LOOKBACK + 10) return trades;

    // Rolling stats over last ENTRY_LOOKBACK mids
    const int W = s33::ENTRY_LOOKBACK;
    std::vector<double> buf(W, 0.0);
    double sum = 0, sum2 = 0;
    int filled = 0;
    size_t head = 0;

    long long cooldown_until_long  = 0;
    long long cooldown_until_short = 0;
    bool in_pos = false;
    Trade cur{};
    double tp_level = 0, sl_level = 0;

    for (size_t i = 0; i < ticks.size(); ++i) {
        const Tick& t = ticks[i];
        double mid = t.mid;

        // ---- update rolling z stats with new mid ----
        if (filled < W) {
            buf[head] = mid; sum += mid; sum2 += mid * mid;
            head = (head + 1) % W; ++filled;
        } else {
            double old = buf[head];
            sum  -= old;  sum2 -= old * old;
            buf[head] = mid;
            sum  += mid;  sum2 += mid * mid;
            head = (head + 1) % W;
        }

        // ---- if in position, manage TP/SL/timeout ----
        if (in_pos) {
            double hi = t.ask, lo = t.bid;
            bool hit_tp = false, hit_sl = false, timed_out = false;
            // Fill crosses the spread: a long flattens at BID (lo), a short at ASK
            // (hi). Filling at the literal sl_level/tp_level overstated SL exits
            // (the triggering bid/ask was already past the level). SL checked
            // first = conservative same-bar resolution (kept).
            if (cur.side > 0) {
                if (lo <= sl_level) { cur.exit_px = lo; hit_sl = true; }
                else if (hi >= tp_level) { cur.exit_px = lo; hit_tp = true; }
            } else {
                if (hi >= sl_level) { cur.exit_px = hi; hit_sl = true; }
                else if (lo <= tp_level) { cur.exit_px = hi; hit_tp = true; }
            }
            if (!hit_tp && !hit_sl && (t.ts - cur.entry_ts) >= s33::MAX_HOLD_S) {
                cur.exit_px = (cur.side > 0) ? t.bid : t.ask;
                timed_out = true;
            }
            if (hit_tp || hit_sl || timed_out) {
                cur.exit_ts = t.ts;
                double pnl_pts = (cur.side > 0)
                    ? (cur.exit_px - cur.entry_px)
                    : (cur.entry_px - cur.exit_px);
                cur.gross_pnl = (pnl_pts / s33::PT_SIZE) * s33::VAL_PER_PT * s33::LOT;
                cur.cost      = cost_per_rt;
                cur.net_pnl   = cur.gross_pnl - cur.cost;
                cur.exit_reason = hit_tp ? "TP_HIT" : (hit_sl ? "SL_HIT" : "MAX_HOLD");
                trades.push_back(cur);

                if (cur.side > 0) cooldown_until_long  = t.ts + s33::COOLDOWN_S;
                else              cooldown_until_short = t.ts + s33::COOLDOWN_S;
                in_pos = false;
                cur = Trade{};
            }
            continue;
        }

        // ---- not in position: gate, then check z ----
        if (filled < W) continue;
        if (!in_session(t.ts)) continue;

        double n = (double)W;
        double mean = sum / n;
        double var = std::max(0.0, sum2 / n - mean * mean);
        double sd = std::sqrt(var);
        if (sd < 1e-9) continue;
        double z = (mid - mean) / sd;

        int side = 0;
        if      (z >=  s33::ENTRY_Z) side = -1;  // mean revert: high z -> sell
        else if (z <= -s33::ENTRY_Z) side = +1;
        else continue;

        if (side > 0 && t.ts < cooldown_until_long)  continue;
        if (side < 0 && t.ts < cooldown_until_short) continue;

        // fire
        cur.side       = side;
        cur.entry_ts   = t.ts;
        cur.entry_px   = (side > 0) ? t.ask : t.bid;
        cur.entry_date = date_of_ts(t.ts);
        tp_level = (side > 0) ? (cur.entry_px + s33::TP_DIST_PTS * s33::PT_SIZE)
                              : (cur.entry_px - s33::TP_DIST_PTS * s33::PT_SIZE);
        sl_level = (side > 0) ? (cur.entry_px - s33::SL_DIST_PTS * s33::PT_SIZE)
                              : (cur.entry_px + s33::SL_DIST_PTS * s33::PT_SIZE);
        in_pos = true;
    }

    if (verbose) std::cerr << "[bt] generated " << trades.size() << " trades\n";
    return trades;
}

// ---------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------
static void print_report(const std::vector<Trade>& trades, double cost_per_rt,
                         long long first_ts, long long last_ts) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "================ S33 OPTION A REVISED BACKTEST ================\n";
    std::cout << "Geometry: Z=" << s33::ENTRY_Z
              << "  W=" << s33::ENTRY_LOOKBACK
              << "  TP=" << s33::TP_DIST_PTS
              << "  SL=" << s33::SL_DIST_PTS
              << "pt  Asia " << s33::SESSION_START << "-" << s33::SESSION_END << " UTC\n";
    std::cout << "Cost:     $" << cost_per_rt << " / round-trip\n";
    std::cout << "Lot:      " << s33::LOT
              << "   pt_size=" << s33::PT_SIZE
              << "   val/pt/lot=" << s33::VAL_PER_PT << "\n";
    if (first_ts && last_ts) {
        std::cout << "Period:   " << date_of_ts(first_ts)
                  << " .. " << date_of_ts(last_ts)
                  << "  (" << ((last_ts - first_ts) / 86400) << " calendar days)\n";
    }
    std::cout << "Trades:   " << trades.size() << "\n";

    if (trades.empty()) {
        std::cout << "\n(No trades fired. Either data thin in Asia hours, or geometry too selective for this sample.)\n";
        std::cout << "================================================================\n";
        return;
    }

    int wins = 0, tp_hits = 0, sl_hits = 0, timeouts = 0;
    double gross = 0, cost_total = 0, net = 0;
    std::vector<double> pnls;
    pnls.reserve(trades.size());

    // Per-day rollup
    std::map<std::string, std::vector<const Trade*>> by_day;
    for (const auto& t : trades) {
        by_day[t.entry_date].push_back(&t);
        gross += t.gross_pnl;
        cost_total += t.cost;
        net   += t.net_pnl;
        if (t.net_pnl > 0) ++wins;
        if (t.exit_reason == "TP_HIT")   ++tp_hits;
        if (t.exit_reason == "SL_HIT")   ++sl_hits;
        if (t.exit_reason == "MAX_HOLD") ++timeouts;
        pnls.push_back(t.net_pnl);
    }

    // Sharpe (per-trade, annualised by sqrt(N))
    double mean_pnl = net / trades.size();
    double var = 0;
    for (auto p : pnls) var += (p - mean_pnl) * (p - mean_pnl);
    var /= std::max<size_t>(1, pnls.size() - 1);
    double sd = std::sqrt(var);
    double sharpe = sd > 1e-12 ? mean_pnl / sd * std::sqrt((double)trades.size()) : 0.0;

    // Drawdown on net cumulative
    double peak = 0, eq = 0, dd = 0;
    for (auto p : pnls) { eq += p; peak = std::max(peak, eq); dd = std::max(dd, peak - eq); }

    int trading_days = (int)by_day.size();
    std::cout << "\n--- HEADLINE ---\n";
    std::cout << "Gross PnL : $" << gross  << "\n";
    std::cout << "Cost total: $" << cost_total << " (" << trades.size() << " RT * $" << cost_per_rt << ")\n";
    std::cout << "NET PnL   : $" << net   << "\n";
    std::cout << "Trading days (with >= 1 trade): " << trading_days << "\n";
    if (trading_days > 0) {
        std::cout << "Net per trading day: $" << (net / trading_days) << "\n";
    }
    if (first_ts && last_ts) {
        long long cal_days = std::max<long long>(1, (last_ts - first_ts) / 86400);
        std::cout << "Net per calendar day: $" << (net / cal_days) << "\n";
    }
    std::cout << "Win rate  : " << (100.0 * wins / trades.size()) << "% (" << wins << "/" << trades.size() << ")\n";
    std::cout << "TP_HIT    : " << tp_hits << "   SL_HIT: " << sl_hits << "   MAX_HOLD: " << timeouts << "\n";
    std::cout << "Sharpe    : " << sharpe << "\n";
    std::cout << "Max DD    : $" << dd << "\n";

    std::cout << "\n--- PER-DAY BREAKDOWN ---\n";
    std::cout << std::left
              << std::setw(12) << "date"
              << std::setw(6)  << "n"
              << std::setw(12) << "gross$"
              << std::setw(10) << "cost$"
              << std::setw(12) << "net$"
              << "exits (TP/SL/HOLD)\n";
    for (auto& kv : by_day) {
        double g = 0, c = 0, nv = 0; int tp = 0, sl = 0, to = 0;
        for (auto* tp_ : kv.second) {
            g += tp_->gross_pnl; c += tp_->cost; nv += tp_->net_pnl;
            if (tp_->exit_reason == "TP_HIT")   ++tp;
            if (tp_->exit_reason == "SL_HIT")   ++sl;
            if (tp_->exit_reason == "MAX_HOLD") ++to;
        }
        std::cout << std::left
                  << std::setw(12) << kv.first
                  << std::setw(6)  << kv.second.size()
                  << std::setw(12) << g
                  << std::setw(10) << c
                  << std::setw(12) << nv
                  << tp << "/" << sl << "/" << to << "\n";
    }
    std::cout << "================================================================\n";
}

// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    std::vector<std::string> patterns;
    double cost_per_rt = 0.06;
    bool verbose = false;

    auto need = [&](int& i, const char* f) -> const char* {
        if (i + 1 >= argc) { std::cerr << "ERROR: " << f << " requires value\n"; std::exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--csv")          patterns.push_back(need(i, "--csv"));
        else if (a == "--cost-per-rt")  cost_per_rt = std::atof(need(i, "--cost-per-rt"));
        else if (a == "--verbose")      verbose = true;
        else if (a == "--help" || a == "-h") {
            std::cout <<
                "s33_revised_backtest -- single-purpose S33 Option A backtest with cost\n"
                "  --csv <glob>          one or more glob patterns of XAUUSD CSVs\n"
                "  --cost-per-rt <USD>   default 0.06 (BlackBull ECN Prime)\n"
                "  --verbose\n"
                "Outputs total net PnL and per-day breakdown to stdout.\n";
            return 0;
        }
        else { std::cerr << "ERROR: unknown arg: " << a << "\n"; return 2; }
    }
    if (patterns.empty()) {
        std::cerr << "ERROR: at least one --csv required. Try --help.\n";
        return 2;
    }

    // Load all files, concat, sort by ts.
    std::vector<Tick> all;
    int file_count = 0;
    for (const auto& pat : patterns) {
        for (const auto& f : expand_glob(pat)) {
            auto v = load_csv(f, verbose);
            all.insert(all.end(), v.begin(), v.end());
            if (!v.empty()) ++file_count;
        }
    }
    std::sort(all.begin(), all.end(), [](const Tick& a, const Tick& b){ return a.ts < b.ts; });
    // De-dup identical timestamps (Dukascopy occasionally repeats)
    if (!all.empty()) {
        auto last = std::unique(all.begin(), all.end(), [](const Tick& a, const Tick& b){
            return a.ts == b.ts && a.bid == b.bid && a.ask == b.ask;
        });
        all.erase(last, all.end());
    }
    std::cerr << "[main] files=" << file_count << "  ticks=" << all.size() << "\n";
    if (all.empty()) { std::cerr << "ERROR: no ticks loaded\n"; return 1; }

    auto trades = run_backtest(all, cost_per_rt, verbose);
    print_report(trades, cost_per_rt, all.front().ts, all.back().ts);
    return 0;
}
