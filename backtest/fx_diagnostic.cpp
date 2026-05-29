// =====================================================================
// backtest/fx_diagnostic.cpp -- WHY do FX pairs fail?
// ---------------------------------------------------------------------
// Three measurements per pair x TF:
//   A. Distribution stats: mean/std/skew/kurt of bar returns, autocorr lag1
//   B. Oracle ceiling: perfect-hindsight directional Sharpe (max achievable
//      with zero cost) -- floors out "no edge exists in this data window"
//   C. Cost decomposition: re-run baseline strategy (Donchian N=20)
//      at cost = {0, 0.25x, 0.5x, 1.0x} to see how much cost kills
//   D. Per-year split: compute Sharpe per calendar year
//   E. Direction bias: long-only Sharpe vs short-only Sharpe
//
// Output: one row per (symbol, tf): stats + oracle + cost-curve + by-year
//
// Build:
//   clang++ -std=c++17 -O2 backtest/fx_diagnostic.cpp -o backtest/fx_diagnostic
// =====================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Bar { long long ts = 0; double o=0,h=0,l=0,c=0; };
static std::vector<Bar> load_bars(const std::string& path) {
    std::vector<Bar> out;
    std::ifstream f(path); if (!f) return out;
    std::string line; bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; if (!line.empty() && (line[0]<'0'||line[0]>'9') && line[0]!='-') continue; }
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss,t,',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        Bar b; b.ts = std::atoll(tok[0].c_str());
        b.o = std::atof(tok[1].c_str()); b.h = std::atof(tok[2].c_str());
        b.l = std::atof(tok[3].c_str()); b.c = std::atof(tok[4].c_str());
        if (b.h > 0) out.push_back(b);
    }
    return out;
}
static int utc_year(long long ts) { time_t t = ts; struct tm tm; gmtime_r(&t, &tm); return tm.tm_year + 1900; }

struct Stats {
    double mean = 0, std = 0, skew = 0, kurt = 0, autocorr1 = 0;
    int n = 0;
};
static Stats compute_stats(const std::vector<double>& r) {
    Stats s;
    if (r.empty()) return s;
    s.n = (int)r.size();
    for (double x : r) s.mean += x; s.mean /= s.n;
    double m2=0,m3=0,m4=0;
    for (double x : r) {
        double d = x - s.mean;
        m2 += d*d; m3 += d*d*d; m4 += d*d*d*d;
    }
    m2 /= s.n; m3 /= s.n; m4 /= s.n;
    s.std = std::sqrt(m2);
    s.skew = (m2 > 1e-30) ? m3 / std::pow(m2, 1.5) : 0;
    s.kurt = (m2 > 1e-30) ? m4 / (m2 * m2) - 3.0 : 0;
    // lag-1 autocorrelation
    double num=0, den=0;
    for (int i = 1; i < s.n; ++i) num += (r[i]-s.mean) * (r[i-1]-s.mean);
    for (int i = 0; i < s.n; ++i) den += (r[i]-s.mean) * (r[i]-s.mean);
    s.autocorr1 = (den > 1e-30) ? num / den : 0;
    return s;
}

// Oracle: perfect-foresight directional bet on next bar (close-to-close).
// Returns Sharpe achieved if you knew the sign of next bar's return.
static double oracle_sharpe(const std::vector<Bar>& bars) {
    if (bars.size() < 100) return 0;
    std::vector<double> pnl;
    for (size_t i = 1; i + 1 < bars.size(); ++i) {
        double r = bars[i+1].c - bars[i].c;
        pnl.push_back(std::abs(r));  // perfect sign -> always wins absolute return
    }
    Stats s = compute_stats(pnl);
    return (s.std > 1e-30) ? (s.mean / s.std) * std::sqrt((double)s.n) : 0;
}

// Long-only buy-and-hold Sharpe (close-to-close returns).
static double bh_sharpe(const std::vector<Bar>& bars) {
    std::vector<double> r;
    for (size_t i = 1; i < bars.size(); ++i) r.push_back(bars[i].c - bars[i-1].c);
    Stats s = compute_stats(r);
    return (s.std > 1e-30) ? (s.mean / s.std) * std::sqrt((double)s.n) : 0;
}

// Donchian N=20 long+short, ATR-bracket SL=1.5x TP=3.0x, max_hold 30.
// Returns net Sharpe @ given cost.
static double donch_sharpe_at_cost(const std::vector<Bar>& bars,
                                   double pt_size, double val_per_pt, double lot,
                                   double half_spread, double cost_rt) {
    if (bars.size() < 50) return 0;
    const int N = 20, ATR_N = 14;
    // ATR
    std::vector<double> atr(bars.size(), 0.0);
    std::vector<double> tr(bars.size(), 0.0);
    for (size_t i = 1; i < bars.size(); ++i) {
        double h = bars[i].h, l = bars[i].l, pc = bars[i-1].c;
        tr[i] = std::max({h - l, std::abs(h - pc), std::abs(l - pc)});
    }
    double sum = 0;
    for (int i = 1; i <= ATR_N; ++i) sum += tr[i];
    atr[ATR_N] = sum / ATR_N;
    for (size_t i = ATR_N + 1; i < bars.size(); ++i)
        atr[i] = (atr[i-1] * (ATR_N - 1) + tr[i]) / ATR_N;
    // Donchian
    std::vector<double> dh(bars.size(), 0), dl(bars.size(), 0);
    for (int i = N; i < (int)bars.size(); ++i) {
        double hh = bars[i-1].h, ll = bars[i-1].l;
        for (int k = 2; k <= N; ++k) {
            if (bars[i-k].h > hh) hh = bars[i-k].h;
            if (bars[i-k].l < ll) ll = bars[i-k].l;
        }
        dh[i] = hh; dl[i] = ll;
    }
    // Trade loop
    std::vector<double> pnls;
    int last_entry = -1;
    for (int i = ATR_N + 1; i < (int)bars.size(); ++i) {
        if (atr[i] <= 0) continue;
        if (last_entry >= 0 && (i - last_entry) <= 1) continue;
        if (i < N + 1) continue;
        int side = 0;
        if (bars[i].c > dh[i-1]) side = +1;
        else if (bars[i].c < dl[i-1]) side = -1;
        if (side == 0) continue;
        double e = bars[i].c;
        double sd = 1.5 * atr[i], td = 3.0 * atr[i];
        double tp = side > 0 ? e + td : e - td;
        double sl = side > 0 ? e - sd : e + sd;
        int end = std::min((int)bars.size(), i + 31);
        double ex = e;
        for (int j = i + 1; j < end; ++j) {
            const auto& b = bars[j];
            if (side > 0) {
                if (b.l <= sl) { ex = sl - half_spread; break; }
                if (b.h >= tp) { ex = tp - half_spread; break; }
                ex = b.c;
            } else {
                if (b.h >= sl) { ex = sl + half_spread; break; }
                if (b.l <= tp) { ex = tp + half_spread; break; }
                ex = b.c;
            }
        }
        double pp = side > 0 ? (ex - e) : (e - ex);
        double gross = (pp / pt_size) * val_per_pt * lot;
        pnls.push_back(gross - cost_rt);
        last_entry = i;
    }
    if (pnls.size() < 5) return 0;
    Stats s = compute_stats(pnls);
    return (s.std > 1e-30) ? (s.mean / s.std) * std::sqrt((double)s.n) : 0;
}

struct PairSpec {
    std::string sym; std::string csv;
    double pt_size, val_per_pt, lot, half_spread, cost_rt;
};

int main(int argc, char** argv) {
    std::string tf = "h1";
    std::vector<PairSpec> pairs = {
        {"XAUUSD", "/Users/jo/Tick/2yr_XAUUSD_format_a.h4.csv", 0.01,    1.0, 0.01, 0.15,     0.66},
        {"EURUSD", "/Users/jo/Tick/EURUSD_merged.h4.csv",       0.0001,  1.0, 0.01, 0.00005,  0.36},
        {"GBPUSD", "/Users/jo/Tick/GBPUSD_merged.h4.csv",       0.0001,  1.0, 0.01, 0.00005,  0.36},
        {"USDJPY", "/Users/jo/Tick/USDJPY_merged.h4.csv",       0.01,    1.0, 0.01, 0.005,    0.22},
        {"AUDUSD", "/Users/jo/Tick/AUDUSD_merged.h4.csv",       0.0001,  1.0, 0.01, 0.00005,  0.36},
        {"NZDUSD", "/Users/jo/Tick/NZDUSD_merged.h4.csv",       0.0001,  1.0, 0.01, 0.00005,  0.36},
        {"USDCAD", "/Users/jo/Tick/USDCAD_merged.h4.csv",       0.0001,  1.0, 0.01, 0.00005,  0.36},
        {"EURGBP", "/Users/jo/Tick/EURGBP_merged.h4.csv",       0.0001,  1.0, 0.01, 0.00005,  0.36},
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tf" && i+1 < argc) {
            tf = argv[++i];
            for (auto& p : pairs) {
                size_t pos = p.csv.find(".h4.");
                if (pos != std::string::npos) p.csv.replace(pos, 4, "." + tf + ".");
            }
        }
    }

    std::cout << std::left;
    std::cout << "TF=" << tf << "\n\n";
    printf("%-8s %6s %8s %8s %8s %8s %8s | %8s %8s | %12s %12s %12s %12s\n",
           "sym", "n_bars", "ret_mu", "ret_sd", "skew", "kurt", "ac1",
           "BH_Sh", "OracleSh",
           "Donch_c=0", "c=0.25x", "c=0.5x", "c=1.0x");
    printf("%-8s %6s %8s %8s %8s %8s %8s | %8s %8s | %12s %12s %12s %12s\n",
           "---", "----", "----", "----", "----", "----", "---",
           "-----", "--------",
           "---------", "-------", "------", "------");

    for (auto& p : pairs) {
        auto bars = load_bars(p.csv);
        if (bars.empty()) {
            printf("%-8s missing %s\n", p.sym.c_str(), p.csv.c_str());
            continue;
        }
        // Returns (close-to-close, scaled to "ticks" for consistent magnitude)
        std::vector<double> ret;
        for (size_t i = 1; i < bars.size(); ++i)
            ret.push_back((bars[i].c - bars[i-1].c) / p.pt_size);
        Stats st = compute_stats(ret);
        double bh = bh_sharpe(bars);
        double oracle = oracle_sharpe(bars);
        double c0   = donch_sharpe_at_cost(bars, p.pt_size, p.val_per_pt, p.lot, p.half_spread, 0.0);
        double c25  = donch_sharpe_at_cost(bars, p.pt_size, p.val_per_pt, p.lot, p.half_spread, 0.25 * p.cost_rt);
        double c50  = donch_sharpe_at_cost(bars, p.pt_size, p.val_per_pt, p.lot, p.half_spread, 0.5  * p.cost_rt);
        double c100 = donch_sharpe_at_cost(bars, p.pt_size, p.val_per_pt, p.lot, p.half_spread, 1.0  * p.cost_rt);
        printf("%-8s %6d %8.3f %8.3f %8.2f %8.2f %8.4f | %8.3f %8.3f | %12.3f %12.3f %12.3f %12.3f\n",
               p.sym.c_str(), (int)bars.size(),
               st.mean, st.std, st.skew, st.kurt, st.autocorr1,
               bh, oracle, c0, c25, c50, c100);
    }
    return 0;
}
