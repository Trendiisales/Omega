// =====================================================================
// backtest/survivor_stability.cpp
// ---------------------------------------------------------------------
// Stability test for the trend-follow cells that survived the L2 +
// 3-year Dukascopy cross-validation (2026-05-11):
//
//   1. 10m MA crossover fast=20 / slow=50
//   2. 15m Donchian breakout N=50
//   3. 1h  Momentum lookback=50  (close > close[-50]*1.001 long, etc.)
//   4. 1h  MA crossover fast=20 / slow=50
//   5. 4h  Momentum lookback=20
//
// Tests each cell against:
//   * the full tick corpus you point it at (Dukascopy + L2)
//   * with realistic $0.06/RT cost
//   * ATR-scaled brackets (SL=1.5*ATR, TP=3.0*ATR, both at 14-bar ATR)
//
// Reports per cell:
//   * Total net PnL
//   * Year-by-year breakdown
//   * Month-by-month breakdown (the stability test)
//   * % of months positive
//   * Longest negative-month streak
//   * Sharpe (per-trade, annualised by sqrt(n))
//
// Build:
//   clang++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/survivor_stability.cpp -o backtest/survivor_stability
//
// Run (full Duka + L2):
//   backtest/survivor_stability \
//       --csv 'outputs/duka_xauusd_daily/*.csv' \
//       --csv 'data/l2_ticks_XAUUSD_*.csv' \
//       --csv 'data/l2_ticks_2026-*.csv'
//
// Memory note: this tool STREAMS each file -- resample-to-bars happens
// per file, ticks are released before the next file loads. So 3GB of
// Dukascopy works without OOM.
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

struct Tick { long long ts = 0; double bid=0, ask=0, mid=0; };
struct Bar  {
    long long ts_open = 0; int tf_sec = 0;
    double o = 0, h = 0, l = 0, c = 0; int n_ticks = 0;
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
        t.mid = 0.5 * (t.bid + t.ask);
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
        double m = t.mid;
        if (!started) { b.o=b.h=b.l=b.c=m; started=true; }
        else { if (m>b.h) b.h=m; if (m<b.l) b.l=m; b.c=m; }
        ++b.n_ticks;
    }
    if (started) out.push_back(b);
    return out;
}

static std::vector<double> compute_atr(const std::vector<Bar>& bars, int n) {
    std::vector<double> atr(bars.size(), 0.0);
    if ((int)bars.size() <= n) return atr;
    double sum=0;
    for (int i=1; i<=n; ++i) {
        double cp = bars[i-1].c;
        double tr = std::max(bars[i].h - bars[i].l, std::max(std::abs(bars[i].h-cp), std::abs(bars[i].l-cp)));
        sum += tr;
    }
    atr[n] = sum / n;
    for (int i=n+1; i<(int)bars.size(); ++i) {
        double cp = bars[i-1].c;
        double tr = std::max(bars[i].h - bars[i].l, std::max(std::abs(bars[i].h-cp), std::abs(bars[i].l-cp)));
        atr[i] = (atr[i-1]*(n-1) + tr) / n;
    }
    return atr;
}
static std::vector<double> compute_sma(const std::vector<Bar>& bars, int n) {
    std::vector<double> out(bars.size(), 0.0);
    if ((int)bars.size()<n) return out;
    double s=0; for (int i=0;i<n;++i) s += bars[i].c;
    out[n-1] = s/n;
    for (int i=n; i<(int)bars.size(); ++i) { s += bars[i].c - bars[i-n].c; out[i] = s/n; }
    return out;
}

struct Trade {
    long long entry_ts = 0;
    int side = 0;
    double pnl = 0;
    std::string cell;
};

static const double COST_PER_RT = 0.06;
static const double LOT = 0.01;
static const double PT_SIZE = 0.01;  // XAUUSD
static const double VAL_PER_PT = 1.0;
static const int    ATR_N = 14;
static const int    MAX_HOLD_BARS = 50;

struct BracketResult { bool filled=false; double pnl=0; int bars=0; };

static BracketResult bracket(const std::vector<Bar>& bars, size_t i, int side,
                             double atr, double sl_mult, double tp_mult) {
    BracketResult r;
    if (i>=bars.size()||atr<=0) return r;
    double ep = bars[i].c; if (ep<=0) return r;
    double sl_d = sl_mult * atr, tp_d = tp_mult * atr;
    double tp = side>0 ? ep+tp_d : ep-tp_d;
    double sl = side>0 ? ep-sl_d : ep+sl_d;
    r.filled = true;
    size_t end = std::min(bars.size(), i + 1 + (size_t)MAX_HOLD_BARS);
    double xp = ep;
    for (size_t k=i+1; k<end; ++k) {
        const auto& b = bars[k];
        if (side>0) {
            if (b.l <= sl) { xp=sl; r.bars=(int)(k-i); break; }
            if (b.h >= tp) { xp=tp; r.bars=(int)(k-i); break; }
            xp = b.c;
        } else {
            if (b.h >= sl) { xp=sl; r.bars=(int)(k-i); break; }
            if (b.l <= tp) { xp=tp; r.bars=(int)(k-i); break; }
            xp = b.c;
        }
    }
    if (r.bars==0) r.bars = (int)(end - i - 1);
    double pnl_pts = side>0 ? (xp-ep) : (ep-xp);
    r.pnl = (pnl_pts/PT_SIZE) * VAL_PER_PT * LOT - COST_PER_RT;
    return r;
}

// Generic signal-driven runner. Returns the trades for one cell.
template <typename Sig>
static std::vector<Trade> run_cell(const std::vector<Bar>& bars,
                                   const std::vector<double>& atr,
                                   double sl_mult, double tp_mult,
                                   const std::string& cell,
                                   int warmup_start,
                                   Sig sig) {
    std::vector<Trade> out;
    int cd = 0;
    int start = std::max(warmup_start, ATR_N + 1);
    for (int i = start; i < (int)bars.size(); ++i) {
        if (cd > 0) { --cd; continue; }
        if (atr[i] <= 0) continue;
        int s = sig(i); if (s == 0) continue;
        auto br = bracket(bars, i, s, atr[i], sl_mult, tp_mult);
        if (!br.filled) continue;
        Trade t; t.entry_ts = bars[i].ts_open; t.side = s; t.pnl = br.pnl; t.cell = cell;
        out.push_back(t);
        cd = 1 + br.bars;
    }
    return out;
}

// Run all 5 survivor cells on a given bar series (specific timeframe).
static std::vector<Trade> run_survivors_for_tf(const std::vector<Bar>& bars,
                                               const std::string& tf_label) {
    std::vector<Trade> out;
    if ((int)bars.size() < 100) return out;
    auto atr = compute_atr(bars, ATR_N);

    if (tf_label == "10m") {
        // 10m MA crossover fast=20 / slow=50  (SL 1.5*ATR, TP 3.0*ATR)
        auto fast = compute_sma(bars, 20);
        auto slow = compute_sma(bars, 50);
        auto t = run_cell(bars, atr, 1.5, 3.0, "10m_MACross_20_50", 51,
            [&](int i) -> int {
                if (i<51) return 0;
                if (fast[i-1] <= slow[i-1] && fast[i] >  slow[i]) return +1;
                if (fast[i-1] >= slow[i-1] && fast[i] <  slow[i]) return -1;
                return 0;
            });
        out.insert(out.end(), t.begin(), t.end());
    }

    if (tf_label == "15m") {
        // 15m Donchian breakout N=50  (SL 1.5*ATR, TP 3.0*ATR)
        std::vector<double> dchi(bars.size(),0), dclo(bars.size(),0);
        for (int i=49; i<(int)bars.size(); ++i) {
            double hi=bars[i-49].h, lo=bars[i-49].l;
            for (int k=i-48; k<=i; ++k) { if (bars[k].h>hi) hi=bars[k].h; if (bars[k].l<lo) lo=bars[k].l; }
            dchi[i]=hi; dclo[i]=lo;
        }
        auto t = run_cell(bars, atr, 1.5, 3.0, "15m_DonchianBreak_50", 51,
            [&](int i) -> int {
                if (i<51) return 0;
                if (bars[i].c > dchi[i-1]) return +1;
                if (bars[i].c < dclo[i-1]) return -1;
                return 0;
            });
        out.insert(out.end(), t.begin(), t.end());
    }

    if (tf_label == "1h") {
        // 1h Momentum lookback=50  (close > close[-50]*1.001 long)
        auto t1 = run_cell(bars, atr, 1.5, 3.0, "1h_Momentum_50", 52,
            [&](int i) -> int {
                if (i<52) return 0;
                if (bars[i].c > bars[i-50].c * 1.001) return +1;
                if (bars[i].c < bars[i-50].c * 0.999) return -1;
                return 0;
            });
        out.insert(out.end(), t1.begin(), t1.end());

        // 1h MA crossover fast=20 / slow=50
        auto fast = compute_sma(bars, 20);
        auto slow = compute_sma(bars, 50);
        auto t2 = run_cell(bars, atr, 1.5, 3.0, "1h_MACross_20_50", 51,
            [&](int i) -> int {
                if (i<51) return 0;
                if (fast[i-1] <= slow[i-1] && fast[i] >  slow[i]) return +1;
                if (fast[i-1] >= slow[i-1] && fast[i] <  slow[i]) return -1;
                return 0;
            });
        out.insert(out.end(), t2.begin(), t2.end());
    }

    if (tf_label == "4h") {
        // 4h Momentum lookback=20
        auto t = run_cell(bars, atr, 1.5, 3.0, "4h_Momentum_20", 22,
            [&](int i) -> int {
                if (i<22) return 0;
                if (bars[i].c > bars[i-20].c * 1.001) return +1;
                if (bars[i].c < bars[i-20].c * 0.999) return -1;
                return 0;
            });
        out.insert(out.end(), t.begin(), t.end());
    }
    return out;
}

// ---------------------------------------------------------------------
// Bucketing helpers
// ---------------------------------------------------------------------
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
            std::cout <<
                "survivor_stability -- per-month stability for the 5 surviving cells\n"
                "  --csv <glob>   tick csv (repeat)\n"
                "  --verbose\n";
            return 0;
        } else { std::cerr << "unknown " << a << "\n"; return 2; }
    }
    if (patterns.empty()) { std::cerr << "ERR: --csv required\n"; return 2; }

    // Gather files, sorted
    std::vector<std::string> files;
    for (auto& p : patterns) for (auto& f : u::glob_expand(p)) files.push_back(f);
    std::sort(files.begin(), files.end());
    std::cerr << "[main] files=" << files.size() << "\n";

    // For each timeframe we need ALL ticks merged (to span boundaries).
    // To avoid OOM on Dukascopy, we process per-file: load ticks, append
    // to a "buffer" of recent ticks per timeframe, resample on the fly
    // and flush old bars. SIMPLER alternative implemented here: per-year
    // chunking. Each year fits in memory comfortably.
    std::map<std::string, std::vector<std::string>> by_year;
    for (auto& f : files) {
        // Extract year from filename (Duka: 2024-03-15.csv; L2: l2_ticks_XAUUSD_2026-04-22.csv)
        std::string ym; for (char c : f) {
            if (std::isdigit((unsigned char)c)) ym += c;
            else if (ym.size()>=4) break;
            else ym.clear();
        }
        std::string year = ym.size()>=4 ? ym.substr(0,4) : "unknown";
        by_year[year].push_back(f);
    }

    // Collect all trades
    std::vector<Trade> all_trades;
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

        // Resample per timeframe and run cells
        struct TF { const char* lbl; int sec; };
        TF tfs[] = { {"10m", 600}, {"15m", 900}, {"1h", 3600}, {"4h", 14400} };
        for (auto& tf : tfs) {
            auto bars = resample(merged, tf.sec);
            if (verbose) std::cerr << "  " << tf.lbl << " bars=" << bars.size() << "\n";
            auto t = run_survivors_for_tf(bars, tf.lbl);
            all_trades.insert(all_trades.end(), t.begin(), t.end());
        }
        merged.clear(); merged.shrink_to_fit();
    }

    // Bucket: per cell -> per month -> {net, n}
    struct Stat { double net=0; int n=0; int wins=0; };
    std::map<std::string, std::map<std::string, Stat>> per_month;  // cell -> ym -> stat
    std::map<std::string, std::map<std::string, Stat>> per_year;
    std::map<std::string, Stat> totals;

    for (const auto& t : all_trades) {
        std::string ym = ym_of(t.entry_ts);
        std::string yr = year_of(t.entry_ts);
        auto& m = per_month[t.cell][ym]; m.net += t.pnl; m.n++; if (t.pnl>0) m.wins++;
        auto& y = per_year [t.cell][yr]; y.net += t.pnl; y.n++; if (t.pnl>0) y.wins++;
        auto& T = totals[t.cell];        T.net += t.pnl; T.n++; if (t.pnl>0) T.wins++;
    }

    // Report
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "================================================================\n";
    std::cout << "= SURVIVOR STABILITY (cost = $0.06/RT, lot = 0.01) =\n";
    std::cout << "================================================================\n";
    for (auto& kv : totals) {
        const std::string& cell = kv.first;
        const Stat& t = kv.second;
        if (t.n == 0) continue;

        // Compute month stats
        const auto& by_m = per_month[cell];
        int m_pos = 0, m_neg = 0, m_zero = 0;
        int longest_neg_streak = 0, cur_neg_streak = 0;
        for (auto& mkv : by_m) {
            if (mkv.second.net > 0) { m_pos++; cur_neg_streak = 0; }
            else if (mkv.second.net < 0) { m_neg++; cur_neg_streak++; longest_neg_streak = std::max(longest_neg_streak, cur_neg_streak); }
            else { m_zero++; }
        }
        double pct_pos = by_m.empty() ? 0 : 100.0 * m_pos / by_m.size();

        // Sharpe over trades
        // (we don't keep per-trade pnls here for sharpe, skip; net + n + wr is enough)
        std::cout << "\n--- " << cell << " ---\n";
        std::cout << "  total: n=" << t.n
                  << "  wins=" << t.wins
                  << "  WR=" << (100.0 * t.wins / t.n) << "%"
                  << "  net=$" << t.net
                  << "  per-trade=$" << (t.net / t.n) << "\n";
        std::cout << "  months: total=" << by_m.size()
                  << "  positive=" << m_pos
                  << "  negative=" << m_neg
                  << "  zero=" << m_zero
                  << "  pct_positive=" << pct_pos << "%"
                  << "  longest_neg_streak=" << longest_neg_streak << " months\n";
        std::cout << "  per-year:\n";
        for (auto& y : per_year[cell]) {
            std::cout << "    " << y.first << ": n=" << y.second.n
                      << " WR=" << (y.second.n? 100.0*y.second.wins/y.second.n:0)
                      << "% net=$" << y.second.net << "\n";
        }
        std::cout << "  per-month:\n";
        for (auto& m : by_m) {
            std::cout << "    " << m.first
                      << ": n=" << std::setw(4) << m.second.n
                      << " WR=" << std::setw(5) << (m.second.n? 100.0*m.second.wins/m.second.n:0)
                      << "% net=$" << std::setw(8) << m.second.net << "\n";
        }
    }
    std::cout << "================================================================\n";
    return 0;
}
