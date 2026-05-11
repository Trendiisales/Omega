// =====================================================================
// backtest/edge_hunt.cpp
// ---------------------------------------------------------------------
// Aggressive cross-symbol / cross-family edge hunt with realistic
// bid/ask fills. Designed to answer the question: with cost+spread
// modeled, is there ANY robust positive cell anywhere in your data?
//
// What's new vs survivor_definitive.cpp:
//   * Tests 8 signal families (not just 4 survivor cells):
//       1. MA crossover            (baseline trend-follow)
//       2. Donchian breakout       (channel breakout)
//       3. Momentum N-lookback     (the XAU 4h survivor)
//       4. Volatility expansion    (NEW: large TR bar -> trade direction)
//       5. Inside-bar break        (NEW: classic IB setup)
//       6. Kaufman-ER trend filter (NEW: only fire when ER > thr)
//       7. Open-range break        (NEW: first-N-bars range break)
//       8. ATR-filtered momentum   (NEW: momentum gated by ATR regime)
//
//   * Tests across all symbols you have data for: XAUUSD (L2 + Duka),
//     US500 (L2), USTEC (L2), EURUSD (HistData).
//
//   * Realistic bid/ask fills (long enters ASK, exits BID, etc.).
//
//   * Two bracket geometries: tight (SL=1.0*ATR, TP=2.0*ATR, RR=2)
//     and wide (SL=2.0*ATR, TP=4.0*ATR, RR=2 wide).
//
//   * Outputs a single ranked CSV with: symbol, tf, family, params,
//     bracket, n, win_rate, gross, net@$0.06, months_positive_pct,
//     longest_neg_streak, breakeven_cost.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/edge_hunt.cpp -o backtest/edge_hunt
//
// Run:
//   backtest/edge_hunt \
//       --csv 'outputs/duka_xauusd_daily/*.csv'           --sym XAUUSD \
//       --csv 'data/l2_ticks_XAUUSD_*.csv'                --sym XAUUSD \
//       --csv 'data/l2_ticks_2026-*.csv'                  --sym XAUUSD \
//       --csv 'data/l2_ticks_US500_*.csv'                 --sym US500  \
//       --csv 'data/l2_ticks_USTEC_*.csv'                 --sym USTEC  \
//       --csv 'outputs/histdata_eurusd_daily/*.csv'       --sym EURUSD \
//       --out backtest/edge_hunt_results.csv
//
// (--sym applies to the PRECEDING --csv. If omitted, auto-detect.)
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
static std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
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

struct SymBaseline {
    std::string symbol;
    double pt_size = 0.01;
    double val_per_pt = 1.0;
    double lot = 0.01;
};
static SymBaseline baseline_for(const std::string& s_in) {
    std::string s = u::upper(s_in);
    SymBaseline b; b.symbol = s;
    if      (s == "XAUUSD") { b.pt_size = 0.01;   b.val_per_pt = 1.0; b.lot = 0.01; }
    else if (s == "US500"  || s == "SPX500") { b.pt_size = 0.1; b.val_per_pt = 1.0; b.lot = 0.1; }
    else if (s == "USTEC"  || s == "NAS100") { b.pt_size = 0.1; b.val_per_pt = 1.0; b.lot = 0.1; }
    else if (s == "EURUSD" || s == "GBPUSD") { b.pt_size = 0.0001; b.val_per_pt = 1.0; b.lot = 0.01; }
    else if (s == "USDJPY") { b.pt_size = 0.01;   b.val_per_pt = 1.0; b.lot = 0.01; }
    else { b.pt_size = 0.01; b.val_per_pt = 1.0; b.lot = 0.01; }
    return b;
}

struct Tick { long long ts=0; double bid=0, ask=0; };
struct Bar {
    long long ts_open=0; int tf_sec=0;
    double bid_o=0, bid_h=0, bid_l=0, bid_c=0;
    double ask_o=0, ask_h=0, ask_l=0, ask_c=0;
    int n_ticks=0;
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

// ATR (Wilder) over mid prices
static std::vector<double> compute_atr(const std::vector<Bar>& bars, int n) {
    std::vector<double> atr(bars.size(), 0.0);
    if ((int)bars.size() <= n) return atr;
    double sum=0;
    for (int i=1; i<=n; ++i) {
        double cp = bars[i-1].mid_c();
        double tr = std::max(bars[i].mid_h() - bars[i].mid_l(),
                             std::max(std::abs(bars[i].mid_h()-cp), std::abs(bars[i].mid_l()-cp)));
        sum += tr;
    }
    atr[n] = sum / n;
    for (int i=n+1; i<(int)bars.size(); ++i) {
        double cp = bars[i-1].mid_c();
        double tr = std::max(bars[i].mid_h() - bars[i].mid_l(),
                             std::max(std::abs(bars[i].mid_h()-cp), std::abs(bars[i].mid_l()-cp)));
        atr[i] = (atr[i-1]*(n-1) + tr) / n;
    }
    return atr;
}
static std::vector<double> compute_sma(const std::vector<Bar>& bars, int n) {
    std::vector<double> out(bars.size(), 0.0);
    if ((int)bars.size()<n) return out;
    double s=0; for (int i=0;i<n;++i) s += bars[i].mid_c();
    out[n-1] = s/n;
    for (int i=n; i<(int)bars.size(); ++i) { s += bars[i].mid_c() - bars[i-n].mid_c(); out[i] = s/n; }
    return out;
}
// Kaufman Efficiency Ratio over last N bars: |close[i]-close[i-N]| / sum(|close[k]-close[k-1]|)
static std::vector<double> compute_kaufman_er(const std::vector<Bar>& bars, int n) {
    std::vector<double> er(bars.size(), 0.0);
    if ((int)bars.size() <= n) return er;
    for (int i = n; i < (int)bars.size(); ++i) {
        double num = std::abs(bars[i].mid_c() - bars[i-n].mid_c());
        double den = 0;
        for (int k = i-n+1; k <= i; ++k) den += std::abs(bars[k].mid_c() - bars[k-1].mid_c());
        er[i] = (den > 1e-12) ? num / den : 0.0;
    }
    return er;
}

// EMA
static std::vector<double> compute_ema(const std::vector<Bar>& bars, int n) {
    std::vector<double> out(bars.size(), 0.0);
    if (bars.empty()) return out;
    double alpha = 2.0 / (n + 1);
    out[0] = bars[0].mid_c();
    for (int i = 1; i < (int)bars.size(); ++i) {
        out[i] = alpha * bars[i].mid_c() + (1 - alpha) * out[i-1];
    }
    return out;
}

// Stochastic %K and %D over period N (and D smoothing M)
struct StochResult { std::vector<double> k, d; };
static StochResult compute_stochastic(const std::vector<Bar>& bars, int N, int M) {
    StochResult r;
    r.k.assign(bars.size(), 50.0);
    r.d.assign(bars.size(), 50.0);
    if ((int)bars.size() < N) return r;
    for (int i = N - 1; i < (int)bars.size(); ++i) {
        double hh = bars[i-N+1].mid_h(), ll = bars[i-N+1].mid_l();
        for (int k = i-N+2; k <= i; ++k) {
            if (bars[k].mid_h() > hh) hh = bars[k].mid_h();
            if (bars[k].mid_l() < ll) ll = bars[k].mid_l();
        }
        double rng = hh - ll;
        r.k[i] = (rng > 1e-12) ? 100.0 * (bars[i].mid_c() - ll) / rng : 50.0;
    }
    // smooth k -> d with simple SMA over M
    for (int i = N + M - 2; i < (int)bars.size(); ++i) {
        double s = 0;
        for (int k = i - M + 1; k <= i; ++k) s += r.k[k];
        r.d[i] = s / M;
    }
    return r;
}

// ADX (Wilder, 14): trend strength 0-100
static std::vector<double> compute_adx(const std::vector<Bar>& bars, int n = 14) {
    std::vector<double> adx(bars.size(), 0.0);
    if ((int)bars.size() <= n * 2) return adx;
    std::vector<double> pdm(bars.size(), 0.0), mdm(bars.size(), 0.0), tr(bars.size(), 0.0);
    for (int i = 1; i < (int)bars.size(); ++i) {
        double up = bars[i].mid_h() - bars[i-1].mid_h();
        double dn = bars[i-1].mid_l() - bars[i].mid_l();
        pdm[i] = (up > dn && up > 0) ? up : 0;
        mdm[i] = (dn > up && dn > 0) ? dn : 0;
        double cp = bars[i-1].mid_c();
        tr[i]  = std::max(bars[i].mid_h() - bars[i].mid_l(),
                          std::max(std::abs(bars[i].mid_h() - cp), std::abs(bars[i].mid_l() - cp)));
    }
    // Wilder smoothed
    double atr_s = 0, pdi_s = 0, mdi_s = 0;
    for (int i = 1; i <= n; ++i) { atr_s += tr[i]; pdi_s += pdm[i]; mdi_s += mdm[i]; }
    std::vector<double> pdi(bars.size(), 0), mdi(bars.size(), 0), dx(bars.size(), 0);
    if (atr_s > 1e-12) {
        pdi[n] = 100.0 * pdi_s / atr_s;
        mdi[n] = 100.0 * mdi_s / atr_s;
    }
    for (int i = n + 1; i < (int)bars.size(); ++i) {
        atr_s = atr_s - atr_s/n + tr[i];
        pdi_s = pdi_s - pdi_s/n + pdm[i];
        mdi_s = mdi_s - mdi_s/n + mdm[i];
        if (atr_s > 1e-12) {
            pdi[i] = 100.0 * pdi_s / atr_s;
            mdi[i] = 100.0 * mdi_s / atr_s;
        }
        double sum = pdi[i] + mdi[i];
        dx[i] = sum > 1e-12 ? 100.0 * std::abs(pdi[i] - mdi[i]) / sum : 0.0;
    }
    // ADX = Wilder average of DX over n
    double sum_dx = 0;
    for (int i = n + 1; i <= 2*n; ++i) sum_dx += dx[i];
    if (2*n < (int)bars.size()) adx[2*n] = sum_dx / n;
    for (int i = 2*n + 1; i < (int)bars.size(); ++i) {
        adx[i] = (adx[i-1] * (n - 1) + dx[i]) / n;
    }
    return adx;
}

// ---------------------------------------------------------------------
// Trade + bracket
// ---------------------------------------------------------------------
struct Trade {
    long long entry_ts = 0;
    int side = 0;
    double entry_px = 0, exit_px = 0;
    double pnl_gross = 0;
    int bars = 0;
};

static int MAX_HOLD_BARS = 50;

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
    r.entry_ts = bars[i].ts_open; r.side = side; r.entry_px = ep;
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
    r.exit_px = xp;
    double pnl_pts = (side>0) ? (xp-ep) : (ep-xp);
    r.pnl_gross = (pnl_pts / sb.pt_size) * sb.val_per_pt * sb.lot;
    return r;
}

// ---------------------------------------------------------------------
// Signal families. Each returns a vector of trades for that signal.
// ---------------------------------------------------------------------
template <typename Sig>
static std::vector<Trade> run_cell(const std::vector<Bar>& bars,
                                   const std::vector<double>& atr,
                                   int atr_n, int warmup_start,
                                   double sl_mult, double tp_mult,
                                   const SymBaseline& sb,
                                   Sig sig) {
    std::vector<Trade> out;
    int start = std::max(warmup_start, atr_n + 1);
    int cd = 0;
    for (int i = start; i < (int)bars.size(); ++i) {
        if (cd > 0) { --cd; continue; }
        if (atr[i] <= 0) continue;
        int s = sig(i); if (s == 0) continue;
        auto t = bracket_realistic(bars, i, s, atr[i], sl_mult, tp_mult, sb);
        if (t.entry_px == 0) continue;
        out.push_back(t);
        cd = 1 + t.bars;
    }
    return out;
}

// 1. MA crossover (fast, slow)
static std::vector<Trade> sig_macross(const std::vector<Bar>& bars,
                                      const std::vector<double>& atr,
                                      int fast_n, int slow_n,
                                      double sl_mult, double tp_mult,
                                      const SymBaseline& sb) {
    auto fast = compute_sma(bars, fast_n);
    auto slow = compute_sma(bars, slow_n);
    return run_cell(bars, atr, 14, slow_n+1, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < slow_n + 1) return 0;
            if (fast[i-1] <= slow[i-1] && fast[i] >  slow[i]) return +1;
            if (fast[i-1] >= slow[i-1] && fast[i] <  slow[i]) return -1;
            return 0;
        });
}

// 2. Donchian breakout (N)
static std::vector<Trade> sig_donchian(const std::vector<Bar>& bars,
                                       const std::vector<double>& atr,
                                       int N,
                                       double sl_mult, double tp_mult,
                                       const SymBaseline& sb) {
    std::vector<double> dhi(bars.size(),0), dlo(bars.size(),0);
    for (int i = N-1; i < (int)bars.size(); ++i) {
        double hi = bars[i-N+1].ask_h, lo = bars[i-N+1].bid_l;
        for (int k = i-N+2; k <= i; ++k) {
            if (bars[k].ask_h > hi) hi = bars[k].ask_h;
            if (bars[k].bid_l < lo) lo = bars[k].bid_l;
        }
        dhi[i] = hi; dlo[i] = lo;
    }
    return run_cell(bars, atr, 14, N+1, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < N+1) return 0;
            if (bars[i].ask_c > dhi[i-1]) return +1;
            if (bars[i].bid_c < dlo[i-1]) return -1;
            return 0;
        });
}

// 3. Momentum N-lookback
static std::vector<Trade> sig_momentum(const std::vector<Bar>& bars,
                                       const std::vector<double>& atr,
                                       int lookback,
                                       double sl_mult, double tp_mult,
                                       const SymBaseline& sb) {
    return run_cell(bars, atr, 14, lookback+2, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < lookback+2) return 0;
            double cur = bars[i].mid_c(), prev = bars[i-lookback].mid_c();
            if (cur > prev * 1.001) return +1;
            if (cur < prev * 0.999) return -1;
            return 0;
        });
}

// 4. NEW: Volatility expansion. When current bar's true range > K*ATR,
//    enter in the direction of the bar (close vs open).
static std::vector<Trade> sig_volexpand(const std::vector<Bar>& bars,
                                        const std::vector<double>& atr,
                                        double K,
                                        double sl_mult, double tp_mult,
                                        const SymBaseline& sb) {
    return run_cell(bars, atr, 14, 16, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 16) return 0;
            double tr = bars[i].mid_h() - bars[i].mid_l();
            if (tr < K * atr[i]) return 0;
            // direction of the bar
            if (bars[i].mid_c() > bars[i].mid_o()) return +1;
            if (bars[i].mid_c() < bars[i].mid_o()) return -1;
            return 0;
        });
}

// 5. NEW: Inside-bar breakout. Bar[i-1] contains bar[i-2]. Enter on
//    breakout of bar[i-1] high/low.
static std::vector<Trade> sig_inside_bar(const std::vector<Bar>& bars,
                                         const std::vector<double>& atr,
                                         double sl_mult, double tp_mult,
                                         const SymBaseline& sb) {
    return run_cell(bars, atr, 14, 16, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 16) return 0;
            // bar[i-1] is "inside" bar[i-2] iff: high[i-1] < high[i-2] AND low[i-1] > low[i-2]
            const auto& a = bars[i-2];
            const auto& b = bars[i-1];
            if (!(b.mid_h() < a.mid_h() && b.mid_l() > a.mid_l())) return 0;
            // breakout of b's range by bar i
            if (bars[i].ask_c > b.mid_h()) return +1;
            if (bars[i].bid_c < b.mid_l()) return -1;
            return 0;
        });
}

// 6. NEW: Kaufman-ER trend filter. Only trade momentum direction when ER > thr.
static std::vector<Trade> sig_er_trend(const std::vector<Bar>& bars,
                                       const std::vector<double>& atr,
                                       int er_n, double er_thr, int mom_n,
                                       double sl_mult, double tp_mult,
                                       const SymBaseline& sb) {
    auto er = compute_kaufman_er(bars, er_n);
    return run_cell(bars, atr, 14, std::max(er_n, mom_n)+2, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (er[i] < er_thr) return 0;
            double cur = bars[i].mid_c(), prev = bars[i-mom_n].mid_c();
            if (cur > prev) return +1;
            if (cur < prev) return -1;
            return 0;
        });
}

// 7. NEW: Open-range breakout (London open or NY open). first K bars
//    of the session define [hi, lo]. On break of either, enter.
//    For 1h bars: K=2 means first 2 hours of session.
static std::vector<Trade> sig_orb(const std::vector<Bar>& bars,
                                  const std::vector<double>& atr,
                                  int session_start_hr, int session_end_hr,
                                  int K_open_bars,
                                  double sl_mult, double tp_mult,
                                  const SymBaseline& sb) {
    // Group bars by date (UTC). For each date's session window, take first K bars.
    std::vector<Trade> out;
    int cd = 0;
    int start = std::max(14+1, K_open_bars + 1);
    long long prev_date = -1;
    double open_hi = 0, open_lo = 0;
    int open_bars_seen = 0;
    bool fired_today = false;
    for (int i = start; i < (int)bars.size(); ++i) {
        // get date (UTC)
        std::time_t tt = (std::time_t)bars[i].ts_open;
        std::tm tm{}; gmtime_r(&tt, &tm);
        long long date = tm.tm_year*10000LL + tm.tm_mon*100LL + tm.tm_mday;
        if (date != prev_date) {
            prev_date = date;
            open_hi = 0; open_lo = 0; open_bars_seen = 0; fired_today = false;
        }
        int hr = tm.tm_hour;
        if (hr < session_start_hr || hr >= session_end_hr) continue;
        if (cd > 0) { --cd; continue; }
        if (atr[i] <= 0) continue;

        // accumulate open range from first K bars within the session
        if (open_bars_seen < K_open_bars) {
            if (open_bars_seen == 0) { open_hi = bars[i].ask_h; open_lo = bars[i].bid_l; }
            else {
                if (bars[i].ask_h > open_hi) open_hi = bars[i].ask_h;
                if (bars[i].bid_l < open_lo) open_lo = bars[i].bid_l;
            }
            ++open_bars_seen;
            continue;
        }
        if (fired_today) continue;
        int s = 0;
        if (bars[i].ask_c > open_hi) s = +1;
        else if (bars[i].bid_c < open_lo) s = -1;
        if (s == 0) continue;
        auto t = bracket_realistic(bars, i, s, atr[i], sl_mult, tp_mult, sb);
        if (t.entry_px == 0) continue;
        out.push_back(t);
        cd = 1 + t.bars;
        fired_today = true;
    }
    return out;
}

// 8. NEW: ATR-filtered momentum. Only fire when current ATR is between
//    [low_pct, high_pct] of its own 100-bar percentile. Tests the
//    hypothesis "momentum works in normal-vol, not extreme-vol".
static std::vector<Trade> sig_atr_momentum(const std::vector<Bar>& bars,
                                           const std::vector<double>& atr,
                                           int mom_n,
                                           double low_pct, double high_pct,
                                           double sl_mult, double tp_mult,
                                           const SymBaseline& sb) {
    return run_cell(bars, atr, 14, std::max(mom_n, 100)+2, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 100 + mom_n + 2) return 0;
            // estimate ATR percentile over last 100 bars
            std::vector<double> recent;
            recent.reserve(100);
            for (int k = i-99; k <= i; ++k) if (atr[k] > 0) recent.push_back(atr[k]);
            if (recent.size() < 50) return 0;
            std::sort(recent.begin(), recent.end());
            double pl_v = recent[(size_t)(low_pct  * recent.size())];
            double ph_v = recent[(size_t)(std::min<double>(high_pct, 0.999) * recent.size())];
            if (atr[i] < pl_v || atr[i] > ph_v) return 0;
            double cur = bars[i].mid_c(), prev = bars[i-mom_n].mid_c();
            if (cur > prev * 1.001) return +1;
            if (cur < prev * 0.999) return -1;
            return 0;
        });
}

// 9. NEW: PinBar reversal. Bar with body <= 30% of range AND tail >= 2x body
//    on one side. Long if lower tail (bullish pin); short if upper tail.
static std::vector<Trade> sig_pinbar(const std::vector<Bar>& bars,
                                     const std::vector<double>& atr,
                                     double sl_mult, double tp_mult,
                                     const SymBaseline& sb) {
    return run_cell(bars, atr, 14, 16, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 16) return 0;
            double o = bars[i-1].mid_o(), c = bars[i-1].mid_c();
            double h = bars[i-1].mid_h(), l = bars[i-1].mid_l();
            double range = h - l;
            if (range < 1e-9) return 0;
            double body = std::abs(c - o);
            if (body / range > 0.30) return 0;
            double upper_tail = h - std::max(o, c);
            double lower_tail = std::min(o, c) - l;
            if (lower_tail > 2.0 * body && lower_tail > upper_tail) return +1;  // bullish pin
            if (upper_tail > 2.0 * body && upper_tail > lower_tail) return -1;  // bearish pin
            return 0;
        });
}

// 10. NEW: Three-bar consecutive trend. 3 bars same direction; enter
//     on breakout of bar[i-1] in trend direction.
static std::vector<Trade> sig_three_bar(const std::vector<Bar>& bars,
                                        const std::vector<double>& atr,
                                        double sl_mult, double tp_mult,
                                        const SymBaseline& sb) {
    return run_cell(bars, atr, 14, 16, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 16) return 0;
            // bars[i-3..i-1] should be all up (c>o) or all down
            bool all_up   = (bars[i-3].mid_c() > bars[i-3].mid_o())
                         && (bars[i-2].mid_c() > bars[i-2].mid_o())
                         && (bars[i-1].mid_c() > bars[i-1].mid_o());
            bool all_down = (bars[i-3].mid_c() < bars[i-3].mid_o())
                         && (bars[i-2].mid_c() < bars[i-2].mid_o())
                         && (bars[i-1].mid_c() < bars[i-1].mid_o());
            if (all_up && bars[i].ask_c > bars[i-1].mid_h()) return +1;
            if (all_down && bars[i].bid_c < bars[i-1].mid_l()) return -1;
            return 0;
        });
}

// 11. NEW: NR4 narrow-range breakout. Bar[i-1] has the narrowest TR of
//     the previous N bars. Enter on breakout of bar[i-1] high/low.
static std::vector<Trade> sig_nr4(const std::vector<Bar>& bars,
                                  const std::vector<double>& atr,
                                  int N,
                                  double sl_mult, double tp_mult,
                                  const SymBaseline& sb) {
    return run_cell(bars, atr, 14, N+2, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < N + 2) return 0;
            double r_im1 = bars[i-1].mid_h() - bars[i-1].mid_l();
            for (int k = i-N; k < i-1; ++k) {
                double r = bars[k].mid_h() - bars[k].mid_l();
                if (r < r_im1) return 0;
            }
            // Bar[i-1] is narrowest in last N bars (NRN). Trade breakout.
            if (bars[i].ask_c > bars[i-1].mid_h()) return +1;
            if (bars[i].bid_c < bars[i-1].mid_l()) return -1;
            return 0;
        });
}

// 12. NEW: Bollinger Band squeeze release. When the BB width over the
//     last W bars is at its lowest in the last 2W bars (squeeze),
//     fire on the direction of the next bar's break of the mid SMA.
static std::vector<Trade> sig_bb_squeeze(const std::vector<Bar>& bars,
                                         const std::vector<double>& atr,
                                         int W,
                                         double sl_mult, double tp_mult,
                                         const SymBaseline& sb) {
    auto sma = compute_sma(bars, W);
    std::vector<double> bw(bars.size(), 0.0);
    double s=0, s2=0;
    for (int i=0; i<W; ++i) { s += bars[i].mid_c(); s2 += bars[i].mid_c() * bars[i].mid_c(); }
    for (int i=W-1; i<(int)bars.size(); ++i) {
        double mean = s/W;
        double var  = std::max(0.0, s2/W - mean*mean);
        bw[i] = 2.0 * std::sqrt(var); // band width = 2 * sd (one side)
        if (i + 1 < (int)bars.size()) {
            s  += bars[i+1].mid_c() - bars[i+1-W].mid_c();
            s2 += bars[i+1].mid_c()*bars[i+1].mid_c() - bars[i+1-W].mid_c()*bars[i+1-W].mid_c();
        }
    }
    return run_cell(bars, atr, 14, 2*W+1, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 2*W + 1) return 0;
            // is current bw minimum in last 2W bars?
            double min_bw = bw[i];
            for (int k=i-2*W+1; k<=i; ++k) if (bw[k] < min_bw) min_bw = bw[k];
            if (std::abs(bw[i] - min_bw) > 1e-9) return 0;
            // squeeze confirmed -- trade direction of close vs SMA mid
            if (bars[i].mid_c() > sma[i] && bars[i].ask_c > bars[i-1].mid_h()) return +1;
            if (bars[i].mid_c() < sma[i] && bars[i].bid_c < bars[i-1].mid_l()) return -1;
            return 0;
        });
}

// =====================================================================
// PASS 1 NEW SIGNAL FAMILIES (13-17)
// =====================================================================

// 13. NEW: Keltner channel break. EMA20 ± 2*ATR14 envelope.
static std::vector<Trade> sig_keltner(const std::vector<Bar>& bars,
                                      const std::vector<double>& atr,
                                      double mult,
                                      double sl_mult, double tp_mult,
                                      const SymBaseline& sb) {
    auto ema = compute_ema(bars, 20);
    return run_cell(bars, atr, 14, 22, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 22) return 0;
            double up = ema[i] + mult * atr[i];
            double lo = ema[i] - mult * atr[i];
            if (bars[i].ask_c > up) return +1;
            if (bars[i].bid_c < lo) return -1;
            return 0;
        });
}

// 14. NEW: Engulfing pattern. bar[i-1] body fully inside bar[i] body,
//     opposite direction. Bullish: prev bear, curr bull, curr close > prev open,
//     curr open < prev close.
static std::vector<Trade> sig_engulfing(const std::vector<Bar>& bars,
                                        const std::vector<double>& atr,
                                        double sl_mult, double tp_mult,
                                        const SymBaseline& sb) {
    return run_cell(bars, atr, 14, 16, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 16) return 0;
            const auto& a = bars[i-1];
            const auto& b = bars[i];
            double a_o = a.mid_o(), a_c = a.mid_c();
            double b_o = b.mid_o(), b_c = b.mid_c();
            bool a_bear = (a_c < a_o), a_bull = (a_c > a_o);
            bool b_bull = (b_c > b_o), b_bear = (b_c < b_o);
            // bullish engulfing
            if (a_bear && b_bull && b_c > a_o && b_o < a_c) return +1;
            // bearish engulfing
            if (a_bull && b_bear && b_c < a_o && b_o > a_c) return -1;
            return 0;
        });
}

// 15. NEW: Stochastic %K/%D extreme cross.  Long when both < 20 and %K crosses
//     above %D. Short when both > 80 and %K crosses below %D.
static std::vector<Trade> sig_stochastic(const std::vector<Bar>& bars,
                                         const std::vector<double>& atr,
                                         double lo_thr, double hi_thr,
                                         double sl_mult, double tp_mult,
                                         const SymBaseline& sb) {
    auto st = compute_stochastic(bars, 14, 3);
    return run_cell(bars, atr, 14, 20, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 20) return 0;
            double k_prev = st.k[i-1], d_prev = st.d[i-1];
            double k_cur  = st.k[i],   d_cur  = st.d[i];
            bool cross_up   = (k_prev <= d_prev) && (k_cur >  d_cur);
            bool cross_down = (k_prev >= d_prev) && (k_cur <  d_cur);
            if (cross_up   && k_cur < lo_thr && d_cur < lo_thr) return +1;
            if (cross_down && k_cur > hi_thr && d_cur > hi_thr) return -1;
            return 0;
        });
}

// 16. NEW: ADX-gated momentum. Only fire momentum signal when ADX above
//     trend_thr.  Filters out chop where momentum is unreliable.
static std::vector<Trade> sig_adx_momentum(const std::vector<Bar>& bars,
                                           const std::vector<double>& atr,
                                           int mom_n, double adx_thr,
                                           double sl_mult, double tp_mult,
                                           const SymBaseline& sb) {
    auto adx = compute_adx(bars, 14);
    return run_cell(bars, atr, 14, std::max(mom_n, 30)+2, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 30) return 0;
            if (adx[i] < adx_thr) return 0;
            double cur = bars[i].mid_c(), prev = bars[i-mom_n].mid_c();
            if (cur > prev * 1.001) return +1;
            if (cur < prev * 0.999) return -1;
            return 0;
        });
}

// 17. NEW: Two-bar pullback in trend.  Identify a 5-bar trend, then look
//     for 2 consecutive opposite-color bars (pullback), then enter on
//     break of pullback range in trend direction.
static std::vector<Trade> sig_two_bar_pullback(const std::vector<Bar>& bars,
                                               const std::vector<double>& atr,
                                               double sl_mult, double tp_mult,
                                               const SymBaseline& sb) {
    return run_cell(bars, atr, 14, 16, sl_mult, tp_mult, sb,
        [&](int i) -> int {
            if (i < 16) return 0;
            // 5-bar trend: close[i-3] vs close[i-8]
            bool up_trend   = bars[i-3].mid_c() > bars[i-8].mid_c() * 1.001;
            bool down_trend = bars[i-3].mid_c() < bars[i-8].mid_c() * 0.999;
            const auto& b1 = bars[i-2];
            const auto& b2 = bars[i-1];
            bool b1_bear = (b1.mid_c() < b1.mid_o());
            bool b1_bull = (b1.mid_c() > b1.mid_o());
            bool b2_bear = (b2.mid_c() < b2.mid_o());
            bool b2_bull = (b2.mid_c() > b2.mid_o());
            // Up trend + 2 bear pullback bars, then break above b2 high
            if (up_trend && b1_bear && b2_bear && bars[i].ask_c > b2.mid_h()) return +1;
            // Down trend + 2 bull pullback bars, then break below b2 low
            if (down_trend && b1_bull && b2_bull && bars[i].bid_c < b2.mid_l()) return -1;
            return 0;
        });
}

// ---------------------------------------------------------------------
// Result aggregation
// ---------------------------------------------------------------------
struct CellResult {
    std::string symbol, tf, family, params, bracket;
    int n_trades = 0, n_wins = 0;
    double gross = 0;
    int months_total = 0, months_pos = 0;
    int longest_neg_streak = 0;
    double net_at_006 = 0;
    double breakeven_cost = 0;
};

static std::string ym_of(long long ts) {
    std::time_t tt=(std::time_t)ts; std::tm tm{}; gmtime_r(&tt,&tm);
    char b[12]; std::strftime(b, sizeof(b), "%Y-%m", &tm); return b;
}

static CellResult summarize(const std::vector<Trade>& trades,
                            const std::string& symbol, const std::string& tf,
                            const std::string& family, const std::string& params,
                            const std::string& bracket) {
    CellResult c;
    c.symbol = symbol; c.tf = tf; c.family = family; c.params = params; c.bracket = bracket;
    c.n_trades = (int)trades.size();
    if (trades.empty()) return c;
    std::map<std::string, double> by_m;
    for (const auto& t : trades) {
        c.gross += t.pnl_gross;
        double net = t.pnl_gross - 0.06;
        c.net_at_006 += net;
        if (net > 0) ++c.n_wins;
        by_m[ym_of(t.entry_ts)] += net;
    }
    c.months_total = (int)by_m.size();
    int streak = 0;
    for (auto& m : by_m) {
        if (m.second > 0) { ++c.months_pos; streak = 0; }
        else { ++streak; c.longest_neg_streak = std::max(c.longest_neg_streak, streak); }
    }
    c.breakeven_cost = (c.n_trades > 0) ? c.gross / c.n_trades : 0.0;
    return c;
}

// ---------------------------------------------------------------------
// CLI / driver
// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    // Pairs: pattern -> symbol override
    std::vector<std::pair<std::string, std::string>> csv_with_sym;
    std::string out_path;
    bool verbose = false;
    std::string pending_pat;
    bool pending_pat_set = false;

    auto flush_pending = [&](const std::string& sym = "") {
        if (pending_pat_set) {
            csv_with_sym.push_back({pending_pat, sym});
            pending_pat_set = false;
            pending_pat.clear();
        }
    };
    auto need = [&](int& i, const char* f) -> const char* {
        if (i+1>=argc) { std::cerr << "ERR " << f << "\n"; std::exit(2); }
        return argv[++i];
    };
    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        if      (a == "--csv") {
            flush_pending();
            pending_pat = need(i, "--csv"); pending_pat_set = true;
        } else if (a == "--sym") {
            std::string s = need(i, "--sym");
            if (!pending_pat_set) { std::cerr << "--sym without preceding --csv\n"; return 2; }
            csv_with_sym.push_back({pending_pat, s});
            pending_pat_set = false; pending_pat.clear();
        } else if (a == "--out") {
            out_path = need(i, "--out");
        } else if (a == "--verbose") verbose = true;
        else if (a == "--help") { std::cout << "edge_hunt -- comprehensive edge hunt\n"; return 0; }
        else { std::cerr << "unknown " << a << "\n"; return 2; }
    }
    flush_pending();

    if (csv_with_sym.empty()) { std::cerr << "ERR: --csv required\n"; return 2; }

    // Group by (symbol, year) to chunk memory.
    std::map<std::string, std::map<std::string, std::vector<std::string>>> by_sym_year;
    for (auto& sp : csv_with_sym) {
        std::string sym = sp.second.empty() ? "UNKNOWN" : sp.second;
        for (auto& f : u::glob_expand(sp.first)) {
            // year from filename
            std::string ym; for (char c : f) {
                if (std::isdigit((unsigned char)c)) ym += c;
                else if (ym.size()>=4) break;
                else ym.clear();
            }
            std::string yr = ym.size()>=4 ? ym.substr(0,4) : "unknown";
            by_sym_year[sym][yr].push_back(f);
        }
    }
    if (verbose) for (auto& sy : by_sym_year) {
        std::cerr << "[sym] " << sy.first << "\n";
        for (auto& yk : sy.second) std::cerr << "  " << yk.first << " files=" << yk.second.size() << "\n";
    }

    std::ofstream out;
    if (!out_path.empty()) {
        out.open(out_path);
        out << "symbol,timeframe,family,params,bracket,n_trades,n_wins,wr,"
               "gross,net_at_006,months_total,months_pos,pct_months_pos,longest_neg_streak,breakeven_cost\n";
        out << std::fixed << std::setprecision(4);
    }

    std::vector<CellResult> all_results;

    for (auto& sy : by_sym_year) {
        const std::string& sym = sy.first;
        SymBaseline sb = baseline_for(sym);
        if (sb.symbol == "UNKNOWN" || sb.symbol.empty()) sb = baseline_for("XAUUSD");

        // For each timeframe, accumulate per-symbol trades (across years).
        struct TF { const char* lbl; int sec; };
        TF tfs[] = { {"5m",300}, {"15m",900}, {"30m",1800}, {"1h",3600}, {"2h",7200}, {"4h",14400}, {"D1",86400} };

        // We need bars across the whole sym corpus per timeframe (for indicators
        // that need long histories). Within each year, resample and concatenate
        // bars. Adjacent years' bars are not contiguous (Dukascopy days may
        // miss weekends, etc.) but indicators tolerate that.
        std::map<std::string, std::vector<Bar>> all_bars; // tf_label -> bars

        for (auto& yk : sy.second) {
            std::vector<Tick> merged;
            for (auto& f : yk.second) {
                auto v = load_ticks(f);
                merged.insert(merged.end(), v.begin(), v.end());
            }
            std::sort(merged.begin(), merged.end(), [](const Tick& a, const Tick& b){ return a.ts<b.ts; });
            if (merged.empty()) continue;

            for (auto& tf : tfs) {
                auto bars = resample(merged, tf.sec);
                auto& ab = all_bars[tf.lbl];
                ab.insert(ab.end(), bars.begin(), bars.end());
            }
            merged.clear(); merged.shrink_to_fit();
        }

        for (auto& tf : tfs) {
            auto& bars = all_bars[tf.lbl];
            if ((int)bars.size() < 200) continue;
            auto atr = compute_atr(bars, 14);
            if (verbose) std::cerr << "[" << sym << " " << tf.lbl << "] bars=" << bars.size() << "\n";

            // Two bracket geometries to test
            struct Br { const char* lbl; double sl; double tp; };
            Br brackets[] = { {"sl1.5_tp3.0", 1.5, 3.0}, {"sl2.0_tp4.0", 2.0, 4.0} };

            auto push = [&](const CellResult& c) {
                all_results.push_back(c);
                if (out) {
                    out << c.symbol << "," << c.tf << "," << c.family << ",\"" << c.params << "\","
                        << c.bracket << "," << c.n_trades << "," << c.n_wins << ","
                        << (c.n_trades? (double)c.n_wins/c.n_trades : 0) << ","
                        << c.gross << "," << c.net_at_006 << ","
                        << c.months_total << "," << c.months_pos << ","
                        << (c.months_total? 100.0*c.months_pos/c.months_total : 0) << ","
                        << c.longest_neg_streak << "," << c.breakeven_cost << "\n";
                }
            };

            for (auto& br : brackets) {
                // 1. MA crossover: 20/50, 10/30
                push(summarize(sig_macross(bars,atr,20,50,br.sl,br.tp,sb), sym, tf.lbl, "MACross", "fast=20;slow=50", br.lbl));
                push(summarize(sig_macross(bars,atr,10,30,br.sl,br.tp,sb), sym, tf.lbl, "MACross", "fast=10;slow=30", br.lbl));
                // 2. Donchian: N=20, 50
                push(summarize(sig_donchian(bars,atr,20,br.sl,br.tp,sb), sym, tf.lbl, "Donchian", "N=20", br.lbl));
                push(summarize(sig_donchian(bars,atr,50,br.sl,br.tp,sb), sym, tf.lbl, "Donchian", "N=50", br.lbl));
                // 3. Momentum: lookback=20, 50
                push(summarize(sig_momentum(bars,atr,20,br.sl,br.tp,sb), sym, tf.lbl, "Momentum", "lookback=20", br.lbl));
                push(summarize(sig_momentum(bars,atr,50,br.sl,br.tp,sb), sym, tf.lbl, "Momentum", "lookback=50", br.lbl));
                // 4. Vol-expansion: K=2.0, 3.0
                push(summarize(sig_volexpand(bars,atr,2.0,br.sl,br.tp,sb), sym, tf.lbl, "VolExpand", "K=2.0", br.lbl));
                push(summarize(sig_volexpand(bars,atr,3.0,br.sl,br.tp,sb), sym, tf.lbl, "VolExpand", "K=3.0", br.lbl));
                // 5. Inside-bar breakout
                push(summarize(sig_inside_bar(bars,atr,br.sl,br.tp,sb), sym, tf.lbl, "InsideBar", "", br.lbl));
                // 6. Kaufman-ER trend filter: ER>0.2, mom_n=20
                push(summarize(sig_er_trend(bars,atr,20,0.20,20,br.sl,br.tp,sb), sym, tf.lbl, "ER_Trend", "er=0.20;mom=20", br.lbl));
                push(summarize(sig_er_trend(bars,atr,20,0.30,20,br.sl,br.tp,sb), sym, tf.lbl, "ER_Trend", "er=0.30;mom=20", br.lbl));
                // 7. Open-range breakout - only relevant for intraday TFs
                if (std::string(tf.lbl) == "1h" || std::string(tf.lbl) == "15m" || std::string(tf.lbl) == "5m") {
                    // London open: 7-12 UTC; NY open: 12-17 UTC
                    push(summarize(sig_orb(bars,atr,7,12,2,br.sl,br.tp,sb), sym, tf.lbl, "ORB", "session=lon;K=2", br.lbl));
                    push(summarize(sig_orb(bars,atr,12,17,2,br.sl,br.tp,sb), sym, tf.lbl, "ORB", "session=ny;K=2", br.lbl));
                }
                // 8. ATR-filtered momentum (low-vol normal regime only)
                push(summarize(sig_atr_momentum(bars,atr,20,0.20,0.80,br.sl,br.tp,sb), sym, tf.lbl, "ATR_Mom", "mom=20;atr_band=0.2-0.8", br.lbl));
                push(summarize(sig_atr_momentum(bars,atr,50,0.20,0.80,br.sl,br.tp,sb), sym, tf.lbl, "ATR_Mom", "mom=50;atr_band=0.2-0.8", br.lbl));
                // 9. NEW: PinBar reversal
                push(summarize(sig_pinbar(bars,atr,br.sl,br.tp,sb), sym, tf.lbl, "PinBar", "", br.lbl));
                // 10. NEW: Three-bar consecutive trend
                push(summarize(sig_three_bar(bars,atr,br.sl,br.tp,sb), sym, tf.lbl, "ThreeBar", "", br.lbl));
                // 11. NEW: NR4 narrow-range breakout
                push(summarize(sig_nr4(bars,atr,4,br.sl,br.tp,sb), sym, tf.lbl, "NR4", "N=4", br.lbl));
                push(summarize(sig_nr4(bars,atr,7,br.sl,br.tp,sb), sym, tf.lbl, "NR7", "N=7", br.lbl));
                // 12. NEW: Bollinger Band squeeze release
                push(summarize(sig_bb_squeeze(bars,atr,20,br.sl,br.tp,sb), sym, tf.lbl, "BB_Squeeze", "W=20", br.lbl));
                // 13. PASS-1 NEW: Keltner channel break (K=2.0)
                push(summarize(sig_keltner(bars,atr,2.0,br.sl,br.tp,sb), sym, tf.lbl, "Keltner", "K=2.0", br.lbl));
                // 14. PASS-1 NEW: Engulfing pattern
                push(summarize(sig_engulfing(bars,atr,br.sl,br.tp,sb), sym, tf.lbl, "Engulfing", "", br.lbl));
                // 15. PASS-1 NEW: Stochastic extreme crosses
                push(summarize(sig_stochastic(bars,atr,20,80,br.sl,br.tp,sb), sym, tf.lbl, "Stochastic", "lo=20;hi=80", br.lbl));
                // 16. PASS-1 NEW: ADX-gated momentum
                push(summarize(sig_adx_momentum(bars,atr,20,25.0,br.sl,br.tp,sb), sym, tf.lbl, "ADX_Mom", "mom=20;adx>25", br.lbl));
                push(summarize(sig_adx_momentum(bars,atr,50,25.0,br.sl,br.tp,sb), sym, tf.lbl, "ADX_Mom", "mom=50;adx>25", br.lbl));
                // 17. PASS-1 NEW: Two-bar pullback in trend
                push(summarize(sig_two_bar_pullback(bars,atr,br.sl,br.tp,sb), sym, tf.lbl, "TwoBarPullback", "", br.lbl));
            }
            all_bars[tf.lbl].clear(); all_bars[tf.lbl].shrink_to_fit();
        }
    }

    // Print top-30 robust cells
    std::sort(all_results.begin(), all_results.end(),
              [](const CellResult& a, const CellResult& b){ return a.net_at_006 > b.net_at_006; });
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== TOP 30 CELLS BY NET PNL AT $0.06/RT  (with realistic bid/ask fills) ===\n";
    std::cout << std::left
              << std::setw(8) << "sym"
              << std::setw(5) << "tf"
              << std::setw(12) << "family"
              << std::setw(26) << "params"
              << std::setw(14) << "bracket"
              << std::setw(6)  << "n"
              << std::setw(7)  << "wr%"
              << std::setw(10) << "gross$"
              << std::setw(10) << "net$"
              << std::setw(8)  << "M+%"
              << std::setw(5)  << "DD"
              << std::setw(8)  << "BE$"
              << "\n";
    int shown = 0;
    for (const auto& c : all_results) {
        if (c.n_trades < 30) continue;
        if (shown++ >= 30) break;
        std::cout << std::left
                  << std::setw(8) << c.symbol
                  << std::setw(5) << c.tf
                  << std::setw(12) << c.family
                  << std::setw(26) << c.params
                  << std::setw(14) << c.bracket
                  << std::setw(6)  << c.n_trades
                  << std::setw(7)  << (c.n_trades? 100.0*c.n_wins/c.n_trades : 0)
                  << std::setw(10) << c.gross
                  << std::setw(10) << c.net_at_006
                  << std::setw(8)  << (c.months_total? 100.0*c.months_pos/c.months_total : 0)
                  << std::setw(5)  << c.longest_neg_streak
                  << std::setw(8)  << c.breakeven_cost
                  << "\n";
    }

    // Also: ROBUST filter -- net>0 + months>=10 + pct_pos>=60% + DD<=3
    std::cout << "\n=== ROBUST CELLS (net>0 AND months>=10 AND pct_months_pos>=60% AND DD<=3) ===\n";
    int rshown = 0;
    for (const auto& c : all_results) {
        if (c.n_trades < 30) continue;
        if (c.net_at_006 <= 0) continue;
        if (c.months_total < 10) continue;
        double pct = 100.0 * c.months_pos / std::max(1, c.months_total);
        if (pct < 60.0) continue;
        if (c.longest_neg_streak > 3) continue;
        if (rshown++ >= 50) break;
        std::cout << "  " << c.symbol << "  " << c.tf << "  " << c.family
                  << "  " << c.params << "  " << c.bracket
                  << "  n=" << c.n_trades
                  << "  net=$" << c.net_at_006
                  << "  M+=" << pct << "%"
                  << "  DD=" << c.longest_neg_streak
                  << "  BE=$" << c.breakeven_cost << "\n";
    }
    if (rshown == 0) std::cout << "  (NONE)\n";

    if (out) std::cerr << "[out] " << out_path << "\n";
    return 0;
}
