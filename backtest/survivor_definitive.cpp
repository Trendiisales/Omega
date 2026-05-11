// =====================================================================
// backtest/survivor_definitive.cpp
// ---------------------------------------------------------------------
// Realistic-fill stability test for the 4 surviving cells (1h_Momentum
// dropped after stability test failed at 48% months positive).
//
// What this adds vs survivor_stability.cpp:
//
//   (1) BID/ASK SPREAD MODELED IN BARS. Resample tracks bid_open/high/
//       low/close AND ask_open/high/low/close separately. Entries use
//       ask (long) or bid (short). TP/SL triggers use the exit side:
//         long  SL: bid_low  <= sl  -> exit at sl
//         long  TP: bid_high >= tp  -> exit at tp
//         short SL: ask_high >= sl  -> exit at sl
//         short TP: ask_low  <= tp  -> exit at tp
//
//   (2) COST SWEEP. Each cell is tested at five round-trip cost
//       points: $0.06, $0.08, $0.10, $0.12, $0.15. So you see the
//       breakeven cost level for each cell.
//
//   (3) PER-MONTH BREAKDOWN at the baseline $0.06 cost.
//
// Cells tested (same definitions as survivor_stability):
//
//   10m_MACross_20_50
//   15m_DonchianBreak_50
//   1h_MACross_20_50
//   4h_Momentum_20
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/survivor_definitive.cpp -o backtest/survivor_definitive
//
// Run:
//   backtest/survivor_definitive \
//       --csv 'outputs/duka_xauusd_daily/*.csv' \
//       --csv 'data/l2_ticks_XAUUSD_*.csv' \
//       --csv 'data/l2_ticks_2026-*.csv' \
//       --verbose
// =====================================================================

#include <algorithm>
#include <array>
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
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#if __has_include(<glob.h>)
# include <glob.h>
# define HAVE_GLOB 1
#endif

namespace u {
static std::string trim(std::string s) {
    size_t a=0; while (a<s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b=s.size(); while (b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::vector<std::string> split(const std::string& l) {
    std::vector<std::string> o; std::string c;
    for (char ch : l) { if (ch == ',') { o.push_back(c); c.clear(); } else c += ch; }
    o.push_back(c); for (auto& f : o) f = trim(f); return o;
}
static bool pd(const std::string& s, double& v) {
    if (s.empty()) return false;
    try { size_t p=0; v = std::stod(s, &p); return p>0; } catch(...) { return false; }
}
static bool pl(const std::string& s, long long& v) {
    if (s.empty()) return false;
    try { size_t p=0; v = std::stoll(s, &p); return p>0; } catch(...) { return false; }
}
static std::vector<std::string> glob_expand(const std::string& pat) {
    std::vector<std::string> out;
#ifdef HAVE_GLOB
    glob_t g{}; if (glob(pat.c_str(), GLOB_TILDE, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    globfree(&g);
#endif
    if (out.empty()) out.push_back(pat);
    return out;
}
}

// Tick with bid + ask
struct Tick { long long ts = 0; double bid = 0, ask = 0; };

// Bar with separate bid + ask OHLC
struct Bar {
    long long ts_open = 0; int tf_sec = 0;
    double bid_o = 0, bid_h = 0, bid_l = 0, bid_c = 0;
    double ask_o = 0, ask_h = 0, ask_l = 0, ask_c = 0;
    int n_ticks = 0;
    double mid_c() const { return 0.5 * (bid_c + ask_c); }
    double spread() const { return ask_c - bid_c; }
};

static std::vector<Tick> load_ticks(const std::string& path) {
    std::vector<Tick> out;
    std::ifstream fs(path); if (!fs) return out;
    std::string line; if (!std::getline(fs, line)) return out;
    auto hdr = u::split(line);
    int c_ts=-1, c_bid=-1, c_ask=-1; bool ts_ms=false;
    bool first_num = !hdr.empty() && !hdr[0].empty() && (std::isdigit((unsigned char)hdr[0][0]) || hdr[0][0]=='-');
    if (!first_num) {
        for (size_t i=0; i<hdr.size(); ++i) {
            std::string h = u::lower(hdr[i]);
            if (h=="ts_unix"||h=="ts"||h=="timestamp"||h=="time") c_ts=(int)i;
            if (h=="ts_ms") { c_ts=(int)i; ts_ms=true; }
            if (h=="bid") c_bid=(int)i;
            if (h=="ask") c_ask=(int)i;
        }
    } else { fs.clear(); fs.seekg(0); c_ts=0; c_bid=1; c_ask=2; ts_ms=true; }
    if (c_ts<0||c_bid<0||c_ask<0) return out;
    while (std::getline(fs, line)) {
        if (u::trim(line).empty()) continue;
        auto row = u::split(line);
        if ((int)row.size() <= std::max({c_ts, c_bid, c_ask})) continue;
        Tick t; long long raw=0;
        if (!u::pl(row[c_ts], raw)) continue;
        t.ts = ts_ms ? raw/1000 : raw;
        if (!u::pd(row[c_bid], t.bid) || !u::pd(row[c_ask], t.ask)) continue;
        if (t.bid<=0||t.ask<=0) continue;
        out.push_back(t);
    }
    return out;
}

static std::vector<Bar> resample(const std::vector<Tick>& ts, int tf_sec) {
    std::vector<Bar> out; if (ts.empty()||tf_sec<=0) return out;
    long long cur = (ts.front().ts / tf_sec) * tf_sec;
    Bar b; b.ts_open=cur; b.tf_sec=tf_sec; bool started=false;
    for (const auto& t : ts) {
        long long buck = (t.ts / tf_sec) * tf_sec;
        if (buck != cur) {
            if (started) out.push_back(b);
            cur = buck; b = Bar{}; b.ts_open=cur; b.tf_sec=tf_sec; started=false;
        }
        if (!started) {
            b.bid_o=b.bid_h=b.bid_l=b.bid_c=t.bid;
            b.ask_o=b.ask_h=b.ask_l=b.ask_c=t.ask;
            started=true;
        } else {
            if (t.bid>b.bid_h) b.bid_h=t.bid;
            if (t.bid<b.bid_l) b.bid_l=t.bid;
            b.bid_c=t.bid;
            if (t.ask>b.ask_h) b.ask_h=t.ask;
            if (t.ask<b.ask_l) b.ask_l=t.ask;
            b.ask_c=t.ask;
        }
        ++b.n_ticks;
    }
    if (started) out.push_back(b);
    return out;
}

static std::vector<double> compute_atr(const std::vector<Bar>& bars, int n) {
    std::vector<double> atr(bars.size(), 0.0);
    if ((int)bars.size() <= n) return atr;
    auto bar_h = [&](int i) { return std::max(bars[i].bid_h, bars[i].ask_h); };
    auto bar_l = [&](int i) { return std::min(bars[i].bid_l, bars[i].ask_l); };
    auto bar_c = [&](int i) { return bars[i].mid_c(); };
    double sum=0;
    for (int i=1; i<=n; ++i) {
        double cp = bar_c(i-1);
        double tr = std::max(bar_h(i) - bar_l(i),
                             std::max(std::abs(bar_h(i)-cp), std::abs(bar_l(i)-cp)));
        sum += tr;
    }
    atr[n] = sum / n;
    for (int i=n+1; i<(int)bars.size(); ++i) {
        double cp = bar_c(i-1);
        double tr = std::max(bar_h(i) - bar_l(i),
                             std::max(std::abs(bar_h(i)-cp), std::abs(bar_l(i)-cp)));
        atr[i] = (atr[i-1]*(n-1) + tr) / n;
    }
    return atr;
}

static std::vector<double> compute_sma_close(const std::vector<Bar>& bars, int n) {
    std::vector<double> out(bars.size(), 0.0);
    if ((int)bars.size()<n) return out;
    double s=0; for (int i=0;i<n;++i) s += bars[i].mid_c();
    out[n-1] = s/n;
    for (int i=n; i<(int)bars.size(); ++i) { s += bars[i].mid_c() - bars[i-n].mid_c(); out[i] = s/n; }
    return out;
}

struct Trade {
    long long entry_ts = 0;
    int side = 0;            // +1 long, -1 short
    double entry_px = 0;
    double exit_px = 0;
    double pnl_gross = 0;   // before broker cost
    int    bars = 0;
    std::string cell;
};

// Realistic fill model: long enters at ask_close; long exit triggers when
// bid touches sl/tp.  Short enters at bid_close; exits when ask touches.
static const double LOT = 0.01;
static const double PT_SIZE = 0.01;   // XAUUSD
static const double VAL_PER_PT = 1.0;
static const int    ATR_N = 14;
static const int    MAX_HOLD_BARS = 50;

struct BracketResult { bool filled=false; double entry_px=0, exit_px=0; int bars=0; };

static BracketResult bracket_realistic(const std::vector<Bar>& bars, size_t i, int side,
                                       double atr, double sl_mult, double tp_mult) {
    BracketResult r;
    if (i>=bars.size()||atr<=0) return r;
    double ep = (side > 0) ? bars[i].ask_c : bars[i].bid_c;
    if (ep<=0) return r;
    double sl_d = sl_mult * atr, tp_d = tp_mult * atr;
    double tp = side>0 ? ep+tp_d : ep-tp_d;
    double sl = side>0 ? ep-sl_d : ep+sl_d;
    r.filled = true;
    r.entry_px = ep;
    size_t end = std::min(bars.size(), i + 1 + (size_t)MAX_HOLD_BARS);
    double xp = ep;
    for (size_t k=i+1; k<end; ++k) {
        const auto& b = bars[k];
        if (side>0) {
            // long: exit at bid
            if (b.bid_l <= sl) { xp=sl; r.bars=(int)(k-i); break; }
            if (b.bid_h >= tp) { xp=tp; r.bars=(int)(k-i); break; }
            xp = b.bid_c;
        } else {
            // short: exit at ask
            if (b.ask_h >= sl) { xp=sl; r.bars=(int)(k-i); break; }
            if (b.ask_l <= tp) { xp=tp; r.bars=(int)(k-i); break; }
            xp = b.ask_c;
        }
    }
    if (r.bars == 0) r.bars = (int)(end - i - 1);
    r.exit_px = xp;
    return r;
}

// ---------------------------------------------------------------------
// Per-cell signal definitions (same as survivor_stability)
// ---------------------------------------------------------------------
static std::vector<Trade> run_cell_macross(const std::vector<Bar>& bars,
                                           const std::vector<double>& atr,
                                           int fast_n, int slow_n,
                                           double sl_mult, double tp_mult,
                                           const std::string& cell) {
    std::vector<Trade> out;
    auto fast = compute_sma_close(bars, fast_n);
    auto slow = compute_sma_close(bars, slow_n);
    int start = std::max(slow_n + 1, ATR_N + 1);
    int cd = 0;
    for (int i = start; i < (int)bars.size(); ++i) {
        if (cd > 0) { --cd; continue; }
        if (atr[i] <= 0) continue;
        int s = 0;
        if (fast[i-1] <= slow[i-1] && fast[i] > slow[i]) s = +1;
        else if (fast[i-1] >= slow[i-1] && fast[i] < slow[i]) s = -1;
        if (s == 0) continue;
        auto br = bracket_realistic(bars, i, s, atr[i], sl_mult, tp_mult);
        if (!br.filled) continue;
        Trade t;
        t.entry_ts = bars[i].ts_open; t.side = s;
        t.entry_px = br.entry_px; t.exit_px = br.exit_px;
        double pnl_pts = (s>0) ? (br.exit_px - br.entry_px) : (br.entry_px - br.exit_px);
        t.pnl_gross = (pnl_pts / PT_SIZE) * VAL_PER_PT * LOT;
        t.bars = br.bars; t.cell = cell;
        out.push_back(t);
        cd = 1 + br.bars;
    }
    return out;
}

static std::vector<Trade> run_cell_donchian(const std::vector<Bar>& bars,
                                            const std::vector<double>& atr,
                                            int N,
                                            double sl_mult, double tp_mult,
                                            const std::string& cell) {
    std::vector<Trade> out;
    std::vector<double> dhi(bars.size(), 0), dlo(bars.size(), 0);
    for (int i = N - 1; i < (int)bars.size(); ++i) {
        double hi = bars[i-N+1].ask_h, lo = bars[i-N+1].bid_l;
        for (int k = i-N+2; k <= i; ++k) {
            if (bars[k].ask_h > hi) hi = bars[k].ask_h;
            if (bars[k].bid_l < lo) lo = bars[k].bid_l;
        }
        dhi[i] = hi; dlo[i] = lo;
    }
    int start = std::max(N + 1, ATR_N + 1);
    int cd = 0;
    for (int i = start; i < (int)bars.size(); ++i) {
        if (cd > 0) { --cd; continue; }
        if (atr[i] <= 0) continue;
        int s = 0;
        if (bars[i].ask_c > dhi[i-1]) s = +1;
        else if (bars[i].bid_c < dlo[i-1]) s = -1;
        if (s == 0) continue;
        auto br = bracket_realistic(bars, i, s, atr[i], sl_mult, tp_mult);
        if (!br.filled) continue;
        Trade t;
        t.entry_ts = bars[i].ts_open; t.side = s;
        t.entry_px = br.entry_px; t.exit_px = br.exit_px;
        double pnl_pts = (s>0) ? (br.exit_px - br.entry_px) : (br.entry_px - br.exit_px);
        t.pnl_gross = (pnl_pts / PT_SIZE) * VAL_PER_PT * LOT;
        t.bars = br.bars; t.cell = cell;
        out.push_back(t);
        cd = 1 + br.bars;
    }
    return out;
}

static std::vector<Trade> run_cell_momentum(const std::vector<Bar>& bars,
                                            const std::vector<double>& atr,
                                            int lookback,
                                            double sl_mult, double tp_mult,
                                            const std::string& cell) {
    std::vector<Trade> out;
    int start = std::max(lookback + 2, ATR_N + 1);
    int cd = 0;
    for (int i = start; i < (int)bars.size(); ++i) {
        if (cd > 0) { --cd; continue; }
        if (atr[i] <= 0) continue;
        int s = 0;
        double cur = bars[i].mid_c(), prev = bars[i-lookback].mid_c();
        if (cur > prev * 1.001) s = +1;
        else if (cur < prev * 0.999) s = -1;
        if (s == 0) continue;
        auto br = bracket_realistic(bars, i, s, atr[i], sl_mult, tp_mult);
        if (!br.filled) continue;
        Trade t;
        t.entry_ts = bars[i].ts_open; t.side = s;
        t.entry_px = br.entry_px; t.exit_px = br.exit_px;
        double pnl_pts = (s>0) ? (br.exit_px - br.entry_px) : (br.entry_px - br.exit_px);
        t.pnl_gross = (pnl_pts / PT_SIZE) * VAL_PER_PT * LOT;
        t.bars = br.bars; t.cell = cell;
        out.push_back(t);
        cd = 1 + br.bars;
    }
    return out;
}

// Run all 4 surviving cells per timeframe.
static std::vector<Trade> run_survivors_tf(const std::vector<Bar>& bars,
                                           const std::string& tf_label) {
    std::vector<Trade> out;
    if ((int)bars.size() < 100) return out;
    auto atr = compute_atr(bars, ATR_N);
    if (tf_label == "10m") {
        auto t = run_cell_macross(bars, atr, 20, 50, 1.5, 3.0, "10m_MACross_20_50");
        out.insert(out.end(), t.begin(), t.end());
    } else if (tf_label == "15m") {
        auto t = run_cell_donchian(bars, atr, 50, 1.5, 3.0, "15m_DonchianBreak_50");
        out.insert(out.end(), t.begin(), t.end());
    } else if (tf_label == "1h") {
        auto t = run_cell_macross(bars, atr, 20, 50, 1.5, 3.0, "1h_MACross_20_50");
        out.insert(out.end(), t.begin(), t.end());
    } else if (tf_label == "4h") {
        auto t = run_cell_momentum(bars, atr, 20, 1.5, 3.0, "4h_Momentum_20");
        out.insert(out.end(), t.begin(), t.end());
    }
    return out;
}

static std::string ym_of(long long ts) {
    std::time_t tt = (std::time_t)ts; std::tm tm{}; gmtime_r(&tt, &tm);
    char b[12]; std::strftime(b, sizeof(b), "%Y-%m", &tm); return b;
}
static std::string year_of(long long ts) {
    std::time_t tt = (std::time_t)ts; std::tm tm{}; gmtime_r(&tt, &tm);
    char b[6]; std::strftime(b, sizeof(b), "%Y", &tm); return b;
}

int main(int argc, char** argv) {
    std::vector<std::string> patterns;
    bool verbose = false;
    auto need = [&](int& i, const char* f) -> const char* {
        if (i+1>=argc) { std::cerr << "ERR " << f << "\n"; std::exit(2); }
        return argv[++i];
    };
    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        if      (a=="--csv")     patterns.push_back(need(i, "--csv"));
        else if (a=="--verbose") verbose = true;
        else if (a=="--help") {
            std::cout << "survivor_definitive -- realistic fills + cost sweep\n";
            return 0;
        } else { std::cerr << "unknown " << a << "\n"; return 2; }
    }
    if (patterns.empty()) { std::cerr << "ERR: --csv required\n"; return 2; }

    std::vector<std::string> files;
    for (auto& p : patterns) for (auto& f : u::glob_expand(p)) files.push_back(f);
    std::sort(files.begin(), files.end());
    std::cerr << "[main] files=" << files.size() << "\n";

    // Chunk by year to avoid OOM
    std::map<std::string, std::vector<std::string>> by_year;
    for (auto& f : files) {
        std::string ym; for (char c : f) {
            if (std::isdigit((unsigned char)c)) ym += c;
            else if (ym.size()>=4) break;
            else ym.clear();
        }
        std::string year = ym.size()>=4 ? ym.substr(0,4) : "unknown";
        by_year[year].push_back(f);
    }

    std::vector<Trade> all_trades;
    std::map<std::string, double> avg_spread_per_cell; // entry_spread average

    for (auto& yk : by_year) {
        if (verbose) std::cerr << "[year] " << yk.first << " files=" << yk.second.size() << "\n";
        std::vector<Tick> merged;
        for (auto& f : yk.second) {
            auto v = load_ticks(f);
            merged.insert(merged.end(), v.begin(), v.end());
        }
        std::sort(merged.begin(), merged.end(), [](const Tick& a, const Tick& b){ return a.ts<b.ts; });
        if (merged.empty()) continue;
        if (verbose) std::cerr << "  ticks=" << merged.size() << "\n";

        struct TF { const char* lbl; int sec; };
        TF tfs[] = { {"10m", 600}, {"15m", 900}, {"1h", 3600}, {"4h", 14400} };
        for (auto& tf : tfs) {
            auto bars = resample(merged, tf.sec);
            if (verbose) std::cerr << "  " << tf.lbl << " bars=" << bars.size() << "\n";
            if (verbose && !bars.empty()) {
                double avg_sp = 0; int n = 0;
                for (auto& b : bars) { avg_sp += b.spread(); ++n; }
                std::cerr << "    avg spread = " << (n? avg_sp/n : 0) << "\n";
            }
            auto t = run_survivors_tf(bars, tf.lbl);
            all_trades.insert(all_trades.end(), t.begin(), t.end());
        }
        merged.clear(); merged.shrink_to_fit();
    }

    // ---- Cost sweep ----
    std::vector<double> costs = {0.06, 0.08, 0.10, 0.12, 0.15};
    struct CellStat { int n=0, wins=0; double gross=0; };
    std::map<std::string, CellStat> stats;
    std::map<std::string, std::map<std::string, CellStat>> per_month;  // baseline only
    std::map<std::string, std::map<std::string, CellStat>> per_year;
    std::map<std::string, std::map<double, double>> net_by_cost; // cell -> cost -> net

    for (const auto& t : all_trades) {
        auto& s = stats[t.cell]; s.n++; s.gross += t.pnl_gross;
        if (t.pnl_gross - 0.06 > 0) s.wins++;  // baseline WR
        std::string ym = ym_of(t.entry_ts);
        std::string yr = year_of(t.entry_ts);
        auto& m = per_month[t.cell][ym]; m.n++; m.gross += t.pnl_gross;
        auto& y = per_year [t.cell][yr]; y.n++; y.gross += t.pnl_gross;
        for (double c : costs) net_by_cost[t.cell][c] += (t.pnl_gross - c);
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "================================================================\n";
    std::cout << "= SURVIVOR DEFINITIVE (realistic bid/ask fills, lot=0.01) =\n";
    std::cout << "================================================================\n";

    // Headline cost-sweep table
    std::cout << "\nCost sweep (net $ across full corpus):\n";
    std::cout << std::left << std::setw(28) << "cell"
              << std::right << std::setw(8) << "n"
              << std::setw(11) << "gross$";
    for (double c : costs) {
        std::ostringstream o; o << "@$" << c;
        std::cout << std::setw(11) << o.str();
    }
    std::cout << std::setw(11) << "BE_cost$" << "\n";

    for (auto& kv : stats) {
        const std::string& cell = kv.first;
        const auto& s = kv.second;
        // breakeven cost = gross / n
        double be_cost = s.n ? s.gross / s.n : 0;
        std::cout << std::left << std::setw(28) << cell
                  << std::right << std::setw(8) << s.n
                  << std::setw(11) << s.gross;
        for (double c : costs) {
            std::cout << std::setw(11) << net_by_cost[cell][c];
        }
        std::cout << std::setw(11) << be_cost << "\n";
    }

    std::cout << "\nLegend: BE_cost = breakeven cost per RT (= gross/n)\n";
    std::cout << "        cell is profitable at any cost below BE_cost.\n";

    // Per-cell per-month at the $0.06 baseline (the relevant deployment cost)
    std::cout << "\n\n--- PER-MONTH NET AT $0.06/RT (baseline cost) ---\n";
    for (auto& kv : stats) {
        const std::string& cell = kv.first;
        std::cout << "\n[" << cell << "]\n";
        int m_pos = 0, m_neg = 0;
        int longest_neg_streak = 0, cur_neg = 0;
        const auto& by_m = per_month[cell];
        for (auto& m : by_m) {
            double net = m.second.gross - 0.06 * m.second.n;
            std::cout << "  " << m.first
                      << "  n=" << std::setw(4) << m.second.n
                      << "  gross=$" << std::setw(8) << m.second.gross
                      << "  cost=$" << std::setw(7) << (0.06 * m.second.n)
                      << "  net=$" << std::setw(8) << net << "\n";
            if (net > 0) { m_pos++; cur_neg = 0; }
            else { m_neg++; cur_neg++; longest_neg_streak = std::max(longest_neg_streak, cur_neg); }
        }
        std::cout << "  SUMMARY: months total=" << by_m.size()
                  << "  pos=" << m_pos
                  << "  neg=" << m_neg
                  << "  pct_pos=" << (by_m.empty()?0:100.0*m_pos/by_m.size())
                  << "%  longest_neg_streak=" << longest_neg_streak << "\n";

        // Yearly
        std::cout << "  PER YEAR:\n";
        for (auto& y : per_year[cell]) {
            double net = y.second.gross - 0.06 * y.second.n;
            std::cout << "    " << y.first
                      << ": n=" << y.second.n
                      << " gross=$" << y.second.gross
                      << " net=$" << net << "\n";
        }
    }

    std::cout << "================================================================\n";
    return 0;
}
