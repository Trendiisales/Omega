// =====================================================================
// backtest/top_cells_monthly.cpp
// ---------------------------------------------------------------------
// Per-month detail for the TOP cells found by edge_hunt.cpp. Same
// realistic bid/ask fills + $0.06/RT cost. Hardcoded cell list (the
// confirmed survivors) for clarity.
//
// XAU 4h cells (3-year Duka + L2):
//   1. Donchian N=20      sl1.5_tp3.0
//   2. InsideBar          sl2.0_tp4.0
//   3. Momentum lb=20     sl2.0_tp4.0
//   4. ER_Trend er=0.20   sl1.5_tp3.0
//   5. Donchian N=20      sl2.0_tp4.0
//   6. ER_Trend er=0.20   sl2.0_tp4.0
//   7. MACross 10/30      sl2.0_tp4.0
//   8. InsideBar          sl1.5_tp3.0
//   9. ThreeBar           sl2.0_tp4.0
//  10. ER_Trend er=0.30   sl1.5_tp3.0
//
// XAU 1h cell (the only one with L2 confirmation):
//  11. ATR_Mom mom=50     sl2.0_tp4.0
//
// USTEC 5m cells (small sample but striking):
//  12. Donchian N=20      sl2.0_tp4.0
//  13. ER_Trend er=0.30   sl2.0_tp4.0
//  14. MACross 20/50      sl2.0_tp4.0
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/top_cells_monthly.cpp -o backtest/top_cells_monthly
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

namespace u {
static std::string trim(std::string s) {
    size_t a=0; while (a<s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b=s.size(); while (b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
static std::vector<std::string> split(const std::string& l) {
    std::vector<std::string> o; std::string c;
    for (char ch : l) { if (ch == ',') { o.push_back(c); c.clear(); } else c += ch; }
    o.push_back(c); for (auto& f : o) f = trim(f); return o;
}
static bool pd(const std::string& s, double& v) { if (s.empty()) return false;
    try { size_t p=0; v = std::stod(s, &p); return p>0; } catch(...) { return false; } }
static bool pl(const std::string& s, long long& v) { if (s.empty()) return false;
    try { size_t p=0; v = std::stoll(s, &p); return p>0; } catch(...) { return false; } }
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

struct SymBaseline { std::string symbol; double pt_size=0.01, val_per_pt=1, lot=0.01; };
static SymBaseline baseline_for(const std::string& s_in) {
    std::string s = u::upper(s_in);
    SymBaseline b; b.symbol = s;
    if      (s == "XAUUSD") { b.pt_size = 0.01; b.lot = 0.01; }
    else if (s == "USTEC" || s == "NAS100") { b.pt_size = 0.1; b.lot = 0.1; }
    else if (s == "US500") { b.pt_size = 0.1; b.lot = 0.1; }
    return b;
}

struct Tick { long long ts=0; double bid=0, ask=0; };
struct Bar {
    long long ts_open=0; int tf_sec=0;
    double bid_o=0, bid_h=0, bid_l=0, bid_c=0;
    double ask_o=0, ask_h=0, ask_l=0, ask_c=0;
    double mid_o() const { return 0.5 * (bid_o + ask_o); }
    double mid_c() const { return 0.5 * (bid_c + ask_c); }
    double mid_h() const { return 0.5 * (bid_h + ask_h); }
    double mid_l() const { return 0.5 * (bid_l + ask_l); }
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
    Bar b; b.ts_open=cur; b.tf_sec=tf_sec; bool st=false;
    for (const auto& t : ts) {
        long long buck = (t.ts / tf_sec) * tf_sec;
        if (buck != cur) { if (st) out.push_back(b); cur=buck; b=Bar{}; b.ts_open=cur; b.tf_sec=tf_sec; st=false; }
        if (!st) { b.bid_o=b.bid_h=b.bid_l=b.bid_c=t.bid; b.ask_o=b.ask_h=b.ask_l=b.ask_c=t.ask; st=true; }
        else {
            if (t.bid>b.bid_h) b.bid_h=t.bid; if (t.bid<b.bid_l) b.bid_l=t.bid; b.bid_c=t.bid;
            if (t.ask>b.ask_h) b.ask_h=t.ask; if (t.ask<b.ask_l) b.ask_l=t.ask; b.ask_c=t.ask;
        }
    }
    if (st) out.push_back(b);
    return out;
}

static std::vector<double> compute_atr(const std::vector<Bar>& bars, int n) {
    std::vector<double> atr(bars.size(), 0.0); if ((int)bars.size() <= n) return atr;
    double s=0;
    for (int i=1; i<=n; ++i) {
        double cp = bars[i-1].mid_c();
        double tr = std::max(bars[i].mid_h()-bars[i].mid_l(), std::max(std::abs(bars[i].mid_h()-cp), std::abs(bars[i].mid_l()-cp)));
        s += tr;
    }
    atr[n] = s/n;
    for (int i=n+1; i<(int)bars.size(); ++i) {
        double cp = bars[i-1].mid_c();
        double tr = std::max(bars[i].mid_h()-bars[i].mid_l(), std::max(std::abs(bars[i].mid_h()-cp), std::abs(bars[i].mid_l()-cp)));
        atr[i] = (atr[i-1]*(n-1) + tr) / n;
    }
    return atr;
}
static std::vector<double> compute_sma(const std::vector<Bar>& bars, int n) {
    std::vector<double> o(bars.size(),0.0); if ((int)bars.size()<n) return o;
    double s=0; for (int i=0;i<n;++i) s += bars[i].mid_c();
    o[n-1] = s/n;
    for (int i=n; i<(int)bars.size(); ++i) { s += bars[i].mid_c() - bars[i-n].mid_c(); o[i] = s/n; }
    return o;
}
static std::vector<double> compute_kaufman_er(const std::vector<Bar>& bars, int n) {
    std::vector<double> er(bars.size(),0); if ((int)bars.size()<=n) return er;
    for (int i=n; i<(int)bars.size(); ++i) {
        double num = std::abs(bars[i].mid_c() - bars[i-n].mid_c());
        double den = 0;
        for (int k=i-n+1; k<=i; ++k) den += std::abs(bars[k].mid_c() - bars[k-1].mid_c());
        er[i] = (den > 1e-12) ? num/den : 0.0;
    }
    return er;
}

struct Trade { long long entry_ts=0; int side=0; double pnl_gross=0; int bars=0; std::string cell; };
static const double COST = 0.06;
static const int    MAX_HOLD_BARS = 50;

static Trade bracket_realistic(const std::vector<Bar>& bars, size_t i, int side,
                               double atr, double sl_mult, double tp_mult,
                               const SymBaseline& sb) {
    Trade r{};
    if (i>=bars.size() || atr<=0) return r;
    double ep = (side > 0) ? bars[i].ask_c : bars[i].bid_c;
    if (ep<=0) return r;
    double sl_d = sl_mult * atr, tp_d = tp_mult * atr;
    double tp = side>0 ? ep+tp_d : ep-tp_d;
    double sl = side>0 ? ep-sl_d : ep+sl_d;
    r.entry_ts = bars[i].ts_open; r.side = side;
    size_t end = std::min(bars.size(), i + 1 + (size_t)MAX_HOLD_BARS);
    double xp = ep;
    for (size_t k=i+1; k<end; ++k) {
        const auto& b = bars[k];
        if (side>0) {
            if (b.bid_l <= sl) { xp=sl; r.bars=(int)(k-i); break; }
            if (b.bid_h >= tp) { xp=tp; r.bars=(int)(k-i); break; }
            xp = b.bid_c;
        } else {
            if (b.ask_h >= sl) { xp=sl; r.bars=(int)(k-i); break; }
            if (b.ask_l <= tp) { xp=tp; r.bars=(int)(k-i); break; }
            xp = b.ask_c;
        }
    }
    if (r.bars == 0) r.bars = (int)(end - i - 1);
    double pnl_pts = side>0 ? (xp-ep) : (ep-xp);
    r.pnl_gross = (pnl_pts / sb.pt_size) * sb.val_per_pt * sb.lot;
    return r;
}

template <typename Sig>
static std::vector<Trade> run_cell(const std::vector<Bar>& bars, const std::vector<double>& atr,
                                   int atr_n, int warmup, double sl_m, double tp_m,
                                   const SymBaseline& sb, const std::string& cell, Sig sig) {
    std::vector<Trade> out;
    int start = std::max(warmup, atr_n + 1);
    int cd = 0;
    for (int i = start; i < (int)bars.size(); ++i) {
        if (cd > 0) { --cd; continue; }
        if (atr[i] <= 0) continue;
        int s = sig(i); if (s == 0) continue;
        auto t = bracket_realistic(bars, i, s, atr[i], sl_m, tp_m, sb);
        if (t.entry_ts == 0) continue;
        t.cell = cell;
        out.push_back(t);
        cd = 1 + t.bars;
    }
    return out;
}

// Cells (each takes bars+atr+sb -> trades)
static std::vector<Trade> cell_donchian20(const std::vector<Bar>& b, const std::vector<double>& atr,
                                          double sl, double tp, const SymBaseline& sb, const std::string& cn) {
    int N=20;
    std::vector<double> dhi(b.size(),0), dlo(b.size(),0);
    for (int i = N-1; i < (int)b.size(); ++i) {
        double hi = b[i-N+1].ask_h, lo = b[i-N+1].bid_l;
        for (int k = i-N+2; k <= i; ++k) {
            if (b[k].ask_h > hi) hi = b[k].ask_h;
            if (b[k].bid_l < lo) lo = b[k].bid_l;
        }
        dhi[i] = hi; dlo[i] = lo;
    }
    return run_cell(b, atr, 14, N+1, sl, tp, sb, cn,
        [&](int i) -> int {
            if (i<N+1) return 0;
            if (b[i].ask_c > dhi[i-1]) return +1;
            if (b[i].bid_c < dlo[i-1]) return -1;
            return 0;
        });
}
static std::vector<Trade> cell_inside_bar(const std::vector<Bar>& b, const std::vector<double>& atr,
                                          double sl, double tp, const SymBaseline& sb, const std::string& cn) {
    return run_cell(b, atr, 14, 16, sl, tp, sb, cn,
        [&](int i) -> int {
            if (i<16) return 0;
            const auto& a = b[i-2];
            const auto& bb = b[i-1];
            if (!(bb.mid_h() < a.mid_h() && bb.mid_l() > a.mid_l())) return 0;
            if (b[i].ask_c > bb.mid_h()) return +1;
            if (b[i].bid_c < bb.mid_l()) return -1;
            return 0;
        });
}
static std::vector<Trade> cell_momentum20(const std::vector<Bar>& b, const std::vector<double>& atr,
                                          double sl, double tp, const SymBaseline& sb, const std::string& cn) {
    int N=20;
    return run_cell(b, atr, 14, N+2, sl, tp, sb, cn,
        [&](int i) -> int {
            if (i<N+2) return 0;
            double c=b[i].mid_c(), p=b[i-N].mid_c();
            if (c > p*1.001) return +1; if (c < p*0.999) return -1; return 0;
        });
}
static std::vector<Trade> cell_er_trend(const std::vector<Bar>& b, const std::vector<double>& atr,
                                        double er_thr, double sl, double tp, const SymBaseline& sb, const std::string& cn) {
    auto er = compute_kaufman_er(b, 20);
    int N=20;
    return run_cell(b, atr, 14, N+2, sl, tp, sb, cn,
        [&](int i) -> int {
            if (er[i] < er_thr) return 0;
            double c=b[i].mid_c(), p=b[i-N].mid_c();
            if (c > p) return +1; if (c < p) return -1; return 0;
        });
}
static std::vector<Trade> cell_macross(const std::vector<Bar>& b, const std::vector<double>& atr,
                                       int fast_n, int slow_n, double sl, double tp,
                                       const SymBaseline& sb, const std::string& cn) {
    auto fast = compute_sma(b, fast_n);
    auto slow = compute_sma(b, slow_n);
    return run_cell(b, atr, 14, slow_n+1, sl, tp, sb, cn,
        [&](int i) -> int {
            if (i<slow_n+1) return 0;
            if (fast[i-1] <= slow[i-1] && fast[i] >  slow[i]) return +1;
            if (fast[i-1] >= slow[i-1] && fast[i] <  slow[i]) return -1;
            return 0;
        });
}
static std::vector<Trade> cell_three_bar(const std::vector<Bar>& b, const std::vector<double>& atr,
                                         double sl, double tp, const SymBaseline& sb, const std::string& cn) {
    return run_cell(b, atr, 14, 16, sl, tp, sb, cn,
        [&](int i) -> int {
            if (i<16) return 0;
            bool up = b[i-3].mid_c() > b[i-3].mid_o() && b[i-2].mid_c() > b[i-2].mid_o() && b[i-1].mid_c() > b[i-1].mid_o();
            bool dn = b[i-3].mid_c() < b[i-3].mid_o() && b[i-2].mid_c() < b[i-2].mid_o() && b[i-1].mid_c() < b[i-1].mid_o();
            if (up && b[i].ask_c > b[i-1].mid_h()) return +1;
            if (dn && b[i].bid_c < b[i-1].mid_l()) return -1;
            return 0;
        });
}
static std::vector<Trade> cell_atr_mom50(const std::vector<Bar>& b, const std::vector<double>& atr,
                                         double sl, double tp, const SymBaseline& sb, const std::string& cn) {
    int N=50;
    return run_cell(b, atr, 14, std::max(N, 100)+2, sl, tp, sb, cn,
        [&](int i) -> int {
            if (i < 100+N+2) return 0;
            std::vector<double> r; r.reserve(100);
            for (int k=i-99; k<=i; ++k) if (atr[k]>0) r.push_back(atr[k]);
            if (r.size() < 50) return 0;
            std::sort(r.begin(), r.end());
            double low = r[(size_t)(0.20 * r.size())];
            double hi  = r[(size_t)(0.80 * r.size())];
            if (atr[i] < low || atr[i] > hi) return 0;
            double c=b[i].mid_c(), p=b[i-N].mid_c();
            if (c > p*1.001) return +1; if (c < p*0.999) return -1; return 0;
        });
}

static std::string ym_of(long long ts) { std::time_t tt=(std::time_t)ts; std::tm tm{}; gmtime_r(&tt,&tm);
    char b[12]; std::strftime(b,sizeof(b),"%Y-%m",&tm); return b; }

int main(int argc, char** argv) {
    std::string sym_arg, tf_arg;
    std::vector<std::string> patterns;
    auto need = [&](int& i, const char* f){ if (i+1>=argc) std::exit(2); return argv[++i]; };
    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        if      (a=="--csv") patterns.push_back(need(i,"--csv"));
        else if (a=="--sym") sym_arg = need(i,"--sym");
        else if (a=="--tf")  tf_arg  = need(i,"--tf");
    }
    if (patterns.empty() || sym_arg.empty() || tf_arg.empty()) {
        std::cerr << "usage: --sym XAUUSD --tf 4h --csv <glob>\n";
        return 2;
    }

    int tf_sec = 14400;
    if      (tf_arg == "1h")  tf_sec = 3600;
    else if (tf_arg == "5m")  tf_sec = 300;
    else if (tf_arg == "15m") tf_sec = 900;
    else if (tf_arg == "4h")  tf_sec = 14400;

    SymBaseline sb = baseline_for(sym_arg);

    // For XAU: chunk by year to fit memory
    std::map<std::string, std::vector<std::string>> by_year;
    for (auto& p : patterns) for (auto& f : u::glob_expand(p)) {
        std::string ym; for (char c : f) {
            if (std::isdigit((unsigned char)c)) ym += c;
            else if (ym.size()>=4) break; else ym.clear();
        }
        std::string yr = ym.size()>=4 ? ym.substr(0,4) : "unknown";
        by_year[yr].push_back(f);
    }

    // Collect bars
    std::vector<Bar> all_bars;
    for (auto& yk : by_year) {
        std::vector<Tick> merged;
        for (auto& f : yk.second) { auto v = load_ticks(f); merged.insert(merged.end(), v.begin(), v.end()); }
        std::sort(merged.begin(), merged.end(), [](const Tick& a, const Tick& b){ return a.ts<b.ts; });
        if (merged.empty()) continue;
        auto bars = resample(merged, tf_sec);
        all_bars.insert(all_bars.end(), bars.begin(), bars.end());
        merged.clear(); merged.shrink_to_fit();
    }
    std::cerr << "[main] " << sym_arg << " " << tf_arg << " bars=" << all_bars.size() << "\n";
    if ((int)all_bars.size() < 50) return 1;
    auto atr = compute_atr(all_bars, 14);

    // Run all the relevant cells.
    std::vector<std::vector<Trade>> cells;
    std::vector<std::string> labels;
    auto add = [&](std::vector<Trade> t, const std::string& l) {
        cells.push_back(std::move(t)); labels.push_back(l);
    };

    if (sym_arg == "XAUUSD" && tf_arg == "4h") {
        add(cell_donchian20  (all_bars, atr, 1.5, 3.0, sb, "Donchian_N20_sl1.5tp3.0"), "Donchian_N20_sl1.5tp3.0");
        add(cell_donchian20  (all_bars, atr, 2.0, 4.0, sb, "Donchian_N20_sl2.0tp4.0"), "Donchian_N20_sl2.0tp4.0");
        add(cell_inside_bar  (all_bars, atr, 2.0, 4.0, sb, "InsideBar_sl2.0tp4.0"),    "InsideBar_sl2.0tp4.0");
        add(cell_inside_bar  (all_bars, atr, 1.5, 3.0, sb, "InsideBar_sl1.5tp3.0"),    "InsideBar_sl1.5tp3.0");
        add(cell_momentum20  (all_bars, atr, 2.0, 4.0, sb, "Momentum20_sl2.0tp4.0"),   "Momentum20_sl2.0tp4.0");
        add(cell_er_trend    (all_bars, atr, 0.20, 1.5, 3.0, sb, "ER0.20_sl1.5tp3.0"), "ER0.20_sl1.5tp3.0");
        add(cell_er_trend    (all_bars, atr, 0.20, 2.0, 4.0, sb, "ER0.20_sl2.0tp4.0"), "ER0.20_sl2.0tp4.0");
        add(cell_er_trend    (all_bars, atr, 0.30, 1.5, 3.0, sb, "ER0.30_sl1.5tp3.0"), "ER0.30_sl1.5tp3.0");
        add(cell_macross     (all_bars, atr, 10, 30, 2.0, 4.0, sb, "MA10_30_sl2.0tp4.0"), "MA10_30_sl2.0tp4.0");
        add(cell_three_bar   (all_bars, atr, 2.0, 4.0, sb, "ThreeBar_sl2.0tp4.0"),     "ThreeBar_sl2.0tp4.0");
    } else if (sym_arg == "XAUUSD" && tf_arg == "1h") {
        add(cell_atr_mom50   (all_bars, atr, 2.0, 4.0, sb, "ATR_Mom50_sl2.0tp4.0"),    "ATR_Mom50_sl2.0tp4.0");
        add(cell_three_bar   (all_bars, atr, 2.0, 4.0, sb, "ThreeBar_sl2.0tp4.0"),     "ThreeBar_sl2.0tp4.0");
    } else if (sym_arg == "USTEC" && tf_arg == "5m") {
        add(cell_donchian20  (all_bars, atr, 2.0, 4.0, sb, "Donchian_N20_sl2.0tp4.0"), "Donchian_N20_sl2.0tp4.0");
        add(cell_donchian20  (all_bars, atr, 1.5, 3.0, sb, "Donchian_N20_sl1.5tp3.0"), "Donchian_N20_sl1.5tp3.0");
        add(cell_er_trend    (all_bars, atr, 0.30, 2.0, 4.0, sb, "ER0.30_sl2.0tp4.0"), "ER0.30_sl2.0tp4.0");
        add(cell_macross     (all_bars, atr, 20, 50, 2.0, 4.0, sb, "MA20_50_sl2.0tp4.0"), "MA20_50_sl2.0tp4.0");
    } else {
        std::cerr << "no cells defined for " << sym_arg << " " << tf_arg << "\n";
        return 2;
    }

    // Per-cell per-month summary
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "================================================================\n";
    std::cout << " " << sym_arg << " " << tf_arg << "  PER-CELL PER-MONTH @ $0.06/RT\n";
    std::cout << "================================================================\n";
    for (size_t ci = 0; ci < cells.size(); ++ci) {
        const auto& trs = cells[ci];
        std::map<std::string, std::pair<int, double>> by_m; // ym -> (n, net)
        std::map<std::string, std::pair<int, double>> by_y;
        double tot_gross=0, tot_net=0; int tot_n=0, tot_w=0;
        int m_pos=0, m_neg=0, longest_neg=0, cur_neg=0;
        for (const auto& t : trs) {
            double net = t.pnl_gross - COST;
            tot_gross += t.pnl_gross; tot_net += net; ++tot_n;
            if (net > 0) ++tot_w;
            std::string ym = ym_of(t.entry_ts);
            std::string yr = ym.substr(0,4);
            by_m[ym].first++; by_m[ym].second += net;
            by_y[yr].first++; by_y[yr].second += net;
        }
        for (auto& m : by_m) {
            if (m.second.second > 0) { ++m_pos; cur_neg=0; }
            else { ++m_neg; ++cur_neg; if (cur_neg>longest_neg) longest_neg=cur_neg; }
        }
        std::cout << "\n--- [" << ci+1 << "] " << labels[ci] << " ---\n";
        std::cout << "  total:  n=" << tot_n
                  << "  wins=" << tot_w
                  << "  WR=" << (tot_n? 100.0*tot_w/tot_n:0) << "%"
                  << "  gross=$" << tot_gross
                  << "  net=$" << tot_net
                  << "  per_trade=$" << (tot_n? tot_net/tot_n : 0) << "\n";
        std::cout << "  months: total=" << by_m.size()
                  << " pos=" << m_pos
                  << " neg=" << m_neg
                  << " pct_pos=" << (by_m.empty()?0:100.0*m_pos/by_m.size())
                  << "% longest_neg_streak=" << longest_neg << "\n";
        std::cout << "  per year:\n";
        for (auto& y : by_y) std::cout << "    " << y.first << ": n=" << y.second.first
                                       << "  net=$" << y.second.second << "\n";
        std::cout << "  per month:\n";
        for (auto& m : by_m) std::cout << "    " << m.first
                                       << "  n=" << std::setw(3) << m.second.first
                                       << "  net=$" << std::setw(8) << m.second.second << "\n";
    }
    return 0;
}
