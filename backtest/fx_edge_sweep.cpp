// =====================================================================
// backtest/fx_edge_sweep.cpp -- FX-targeted edge sweep
// ---------------------------------------------------------------------
// Different from multi_tf_sweep: tests families designed for FX cost
// structure where M5-H4 generic signals failed (2026-05-29 discovery).
//
// Inputs: bar CSV "ts,o,h,l,c" (ts unix-sec) -- the _merged.h1.csv etc.
// Families:
//   1. DonchianBO_HTF   -- H4/D1 Donch N=10/20/40 long+short
//   2. LondonOpenBO     -- H1: at 07:00 UTC break of 00:00-06:00 Asian range
//   3. NYCloseFade      -- H1: at 21:00 UTC fade move outside NY (13:00-21:00) range
//   4. AsianRangeFade   -- H1: at 07:00 UTC fade Asia (00-06 UTC) range edge to mid
//   5. VolGatedDonch    -- H1/H4 Donch but only when ATR > 1.5x median(ATR_50)
//   6. WeeklyMom        -- D1: close > close[-5] long, < short
//
// Output: symbol,timeframe,family,params,n_trades,n_wins,win_rate,
//         net_pnl,mean_r,sharpe,max_dd
//
// Cost model: per-symbol from OmegaCostGuard (mirrors multi_tf_sweep
// SymBaseline). --cost-per-rt N overrides flat.
//
// Walk-forward: --from-unix / --to-unix slice.
//
// Build:
//   clang++ -std=c++17 -O2 backtest/fx_edge_sweep.cpp -o backtest/fx_edge_sweep
// =====================================================================
#include <algorithm>
#include <array>
#include <cmath>
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

namespace u {
static std::string upper(const std::string& s){std::string o=s;for(auto&c:o)c=(char)std::toupper((unsigned char)c);return o;}
static std::string basename_(const std::string& p){auto i=p.find_last_of("/\\");return i==std::string::npos?p:p.substr(i+1);}
static std::vector<std::string> split(const std::string& s,char d=','){std::vector<std::string>o;std::stringstream ss(s);std::string t;while(std::getline(ss,t,d))o.push_back(t);return o;}
}

// --- Per-symbol cost (from OmegaCostGuard production values) ---------
struct SymBaseline {
    std::string symbol;
    double pt_size = 0.0001;
    double val_per_pt = 1.0;
    double lot = 0.01;
    double half_spread = 0.00005;
    double cost_per_rt_usd = 0.36;
};
static SymBaseline baseline_for(const std::string& s_in) {
    std::string s = u::upper(s_in);
    SymBaseline b; b.symbol = s;
    if      (s == "XAUUSD") { b.pt_size = 0.01;   b.lot = 0.01; b.half_spread = 0.15;     b.cost_per_rt_usd = 0.66; }
    else if (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" || s == "NZDUSD") { b.pt_size = 0.0001; b.lot = 0.01; b.half_spread = 0.00005; b.cost_per_rt_usd = 0.36; }
    else if (s == "EURGBP" || s == "USDCAD") { b.pt_size = 0.0001; b.lot = 0.01; b.half_spread = 0.00005; b.cost_per_rt_usd = 0.36; }
    else if (s == "USDJPY") { b.pt_size = 0.01;   b.lot = 0.01; b.half_spread = 0.005;    b.cost_per_rt_usd = 0.22; }
    else if (s == "GER40")  { b.pt_size = 0.1;    b.lot = 0.10; b.half_spread = 0.50;     b.cost_per_rt_usd = 0.20; }
    else if (s == "BCOUSD" || s == "BRENT") { b.pt_size = 0.01; b.lot = 0.01; b.half_spread = 0.02; b.cost_per_rt_usd = 0.60; }
    else if (s == "NSXUSD" || s == "USTEC") { b.pt_size = 0.1; b.lot = 0.10; b.half_spread = 0.5;  b.cost_per_rt_usd = 1.10; b.symbol = "USTEC"; }
    else if (s == "SPXUSD" || s == "US500") { b.pt_size = 0.1; b.lot = 0.10; b.half_spread = 0.25; b.cost_per_rt_usd = 2.00; b.symbol = "US500"; }
    return b;
}
static std::string detect_symbol(const std::string& path) {
    std::string up = u::upper(u::basename_(path));
    static const std::array<const char*, 11> toks = {
        "XAUUSD", "EURUSD", "GBPUSD", "USDJPY", "AUDUSD", "NZDUSD", "USDCAD",
        "EURGBP", "GER40", "BCOUSD", "NSXUSD"
    };
    for (auto* t : toks) if (up.find(t) != std::string::npos) return t;
    return "UNKNOWN";
}

// --- Bar ---
struct Bar { long long ts = 0; double o=0,h=0,l=0,c=0; };

// 2026-05-29 iter 4: SPX risk-on/risk-off overlay. Global SPX bar array
// loaded once at startup. Lookup by binary search on ts.
static std::vector<Bar> g_spx_bars;
static std::vector<double> g_spx_ema_fast, g_spx_ema_slow;
// SPX trend at unix ts: +1 = EMA_fast > EMA_slow (risk-on), -1 = inverse, 0 = unknown.
static int spx_trend_at(long long ts) {
    if (g_spx_bars.empty() || g_spx_ema_fast.empty()) return 0;
    auto it = std::upper_bound(g_spx_bars.begin(), g_spx_bars.end(), ts,
        [](long long a, const Bar& b){ return a < b.ts; });
    if (it == g_spx_bars.begin()) return 0;
    --it;
    size_t i = (size_t)(it - g_spx_bars.begin());
    if (i >= g_spx_ema_fast.size() || g_spx_ema_fast[i] <= 0 || g_spx_ema_slow[i] <= 0) return 0;
    if (g_spx_ema_fast[i] > g_spx_ema_slow[i]) return +1;
    return -1;
}

static std::vector<Bar> load_bars(const std::string& path) {
    std::vector<Bar> out;
    std::ifstream f(path); if (!f) return out;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; if (!line.empty() && (line[0]<'0'||line[0]>'9') && line[0]!='-') continue; }
        auto t = u::split(line);
        if (t.size() < 5) continue;
        Bar b;
        b.ts = std::atoll(t[0].c_str());
        b.o  = std::atof(t[1].c_str());
        b.h  = std::atof(t[2].c_str());
        b.l  = std::atof(t[3].c_str());
        b.c  = std::atof(t[4].c_str());
        if (b.h > 0) out.push_back(b);
    }
    return out;
}

// Resample H4 -> D1 (group by UTC day)
static std::vector<Bar> resample_d1(const std::vector<Bar>& src) {
    std::vector<Bar> out;
    long long cur_day = -1;
    Bar cur{};
    for (auto& b : src) {
        long long day = b.ts / 86400;
        if (day != cur_day) {
            if (cur_day != -1) out.push_back(cur);
            cur_day = day;
            cur = b;
            cur.ts = day * 86400;
        } else {
            cur.h = std::max(cur.h, b.h);
            cur.l = std::min(cur.l, b.l);
            cur.c = b.c;
        }
    }
    if (cur_day != -1) out.push_back(cur);
    return out;
}

// --- Indicators ---
static std::vector<double> compute_atr(const std::vector<Bar>& bars, int n) {
    std::vector<double> atr(bars.size(), 0.0);
    if ((int)bars.size() <= n) return atr;
    double sum = 0;
    std::vector<double> tr(bars.size(), 0.0);
    for (size_t i = 1; i < bars.size(); ++i) {
        double h = bars[i].h, l = bars[i].l, pc = bars[i-1].c;
        tr[i] = std::max({h - l, std::abs(h - pc), std::abs(l - pc)});
    }
    for (int i = 1; i <= n; ++i) sum += tr[i];
    atr[n] = sum / n;
    for (size_t i = n + 1; i < bars.size(); ++i) {
        atr[i] = (atr[i-1] * (n - 1) + tr[i]) / n;
    }
    return atr;
}

// Donchian highest-N-bars high/low excluding current
struct DC { std::vector<double> hi, lo; };
static DC compute_donchian(const std::vector<Bar>& bars, int n) {
    DC d; d.hi.assign(bars.size(), 0); d.lo.assign(bars.size(), 0);
    for (int i = n; i < (int)bars.size(); ++i) {
        double hh = bars[i-1].h, ll = bars[i-1].l;
        for (int k = 2; k <= n; ++k) {
            if (bars[i-k].h > hh) hh = bars[i-k].h;
            if (bars[i-k].l < ll) ll = bars[i-k].l;
        }
        d.hi[i] = hh; d.lo[i] = ll;
    }
    return d;
}

static int utc_hour(long long ts_sec) {
    time_t t = (time_t)ts_sec;
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm.tm_hour;
}

// --- Trade simulator (bar-OHLC bracket) ---
static double g_cost_override = -1.0;
struct TR { bool filled=false, tp=false, sl=false; int bars=0; double pnl=0, r=0; };
static TR sim_bracket(const std::vector<Bar>& bars, size_t i, int side,
                      double atr_e, double sl_m, double tp_m,
                      const SymBaseline& sb, int max_hold) {
    TR r;
    if (i >= bars.size() || atr_e <= 0) return r;
    double e = bars[i].c;
    if (e <= 0) return r;
    double sd = sl_m * atr_e, td = tp_m * atr_e;
    if (sd <= 0 || td <= 0) return r;
    double tp = side > 0 ? e + td : e - td;
    double sl = side > 0 ? e - sd : e + sd;
    r.filled = true;
    size_t end = std::min(bars.size(), i + 1 + (size_t)max_hold);
    double ex = e;
    const double hs = sb.half_spread;
    for (size_t j = i + 1; j < end; ++j) {
        const auto& b = bars[j];
        if (side > 0) {
            if (b.l <= sl) { ex = sl - hs; r.sl = true; r.bars = (int)(j - i); break; }
            if (b.h >= tp) { ex = tp - hs; r.tp = true; r.bars = (int)(j - i); break; }
            ex = b.c;
        } else {
            if (b.h >= sl) { ex = sl + hs; r.sl = true; r.bars = (int)(j - i); break; }
            if (b.l <= tp) { ex = tp + hs; r.tp = true; r.bars = (int)(j - i); break; }
            ex = b.c;
        }
    }
    if (!r.tp && !r.sl) r.bars = (int)(end - i - 1);
    double pp = side > 0 ? (ex - e) : (e - ex);
    double gross = (pp / sb.pt_size) * sb.val_per_pt * sb.lot;
    double cost = (g_cost_override >= 0.0) ? g_cost_override : sb.cost_per_rt_usd;
    r.pnl = gross - cost;
    r.r = (sd > 0) ? (pp / sd) : 0.0;
    return r;
}

struct Cell { std::string symbol, tf_label, family, params;
    int n_trades = 0, n_wins = 0;
    double net = 0, sum_r = 0, sharpe = 0, max_dd = 0;
    std::vector<double> pnls;
    double win_rate() const { return n_trades ? (double)n_wins / n_trades : 0.0; }
    double mean_r()   const { return n_trades ? sum_r / n_trades : 0.0; }
};
static void finalize(Cell& c) {
    if (c.pnls.empty()) return;
    double m = 0; for (auto p : c.pnls) m += p; m /= c.pnls.size();
    double v = 0; for (auto p : c.pnls) v += (p - m) * (p - m);
    v /= std::max<size_t>(1, c.pnls.size() - 1);
    double sd = std::sqrt(v);
    c.sharpe = (sd > 1e-12) ? (m / sd) * std::sqrt((double)c.pnls.size()) : 0.0;
    double eq = 0, peak = 0, dd = 0;
    for (auto p : c.pnls) { eq += p; peak = std::max(peak, eq); dd = std::max(dd, peak - eq); }
    c.max_dd = dd;
}

template <typename SigFn>
static Cell run_cell(const std::vector<Bar>& bars, const std::vector<double>& atr,
                     int atr_n, double sl_m, double tp_m,
                     const SymBaseline& sb, int max_hold, int cooldown,
                     const std::string& family, const std::string& params,
                     const std::string& tf, SigFn sig) {
    Cell c; c.symbol = sb.symbol; c.tf_label = tf; c.family = family; c.params = params;
    int last_entry = -1;
    for (int i = atr_n + 1; i < (int)bars.size(); ++i) {
        if (atr[i] <= 0) continue;
        if (last_entry >= 0 && (i - last_entry) <= cooldown) continue;
        int s = sig(i);
        if (s == 0) continue;
        auto r = sim_bracket(bars, (size_t)i, s, atr[i], sl_m, tp_m, sb, max_hold);
        if (!r.filled) continue;
        ++c.n_trades;
        c.net += r.pnl;
        c.sum_r += r.r;
        if (r.pnl > 0) ++c.n_wins;
        c.pnls.push_back(r.pnl);
        last_entry = i;
    }
    finalize(c);
    return c;
}

// --- Families ---
static std::vector<Cell> sweep_fx(const std::vector<Bar>& bars,
                                  const SymBaseline& sb,
                                  const std::string& tf) {
    std::vector<Cell> out;
    if ((int)bars.size() < 100) return out;
    const int ATR_N = 14;
    auto atr = compute_atr(bars, ATR_N);

    // 0. SPX_Conditional: risk-on/risk-off overlay using global SPX bars.
    //    Only fires if SPX trend is non-zero. Direction per pair: USDJPY +ve
    //    risk-on (USD up vs JPY safe-haven); EUR/AUD/NZD vs USD also +ve
    //    risk-on (commodity currencies up). USDCAD/USDCHF -ve (USD down on
    //    risk-on).
    if (!g_spx_bars.empty() && (tf == "H1" || tf == "H4" || tf == "D1")) {
        // Sign per (pair, overlay). Default = SPX overlay (risk-on/off).
        // Override via env OVERLAY_SIGN=+1|-1.
        int sgn = 0;
        const char* env_sgn = std::getenv("OVERLAY_SIGN");
        if (env_sgn) sgn = std::atoi(env_sgn);
        if (sgn == 0) {
            const std::string& s = sb.symbol;
            if      (s == "USDJPY") sgn = +1;
            else if (s == "USDCAD") sgn = -1;
            else if (s == "USDCHF") sgn = -1;
            else if (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" || s == "NZDUSD") sgn = +1;
            else if (s == "EURGBP") sgn = 0;
            else sgn = 0;
        }
        if (sgn != 0) {
            // Sweep: SPX EMA pair already global (preset by main). Test multiple
            // bracket geometries to find which lot/SL/TP captures the regime.
            struct VS { double sl, tp; int max_hold; const char* tag; };
            const std::vector<VS> vs = {
                {1.0, 3.0, 24, "sl=1;tp=3"},
                {0.5, 2.5, 24, "sl=0.5;tp=2.5"},
                {2.0, 5.0, 48, "sl=2;tp=5"},
                {1.0, 5.0, 48, "sl=1;tp=5"},
                {1.5, 4.0, 48, "sl=1.5;tp=4"},
            };
            for (auto& v : vs) {
                std::ostringstream p; p << "sgn=" << sgn << ";" << v.tag;
                out.push_back(run_cell(bars, atr, ATR_N, v.sl, v.tp, sb, v.max_hold, 1,
                    "SPX_Conditional", p.str(), tf,
                    [&, sgn](int i) -> int {
                        int tr = spx_trend_at(bars[i].ts);
                        if (tr == 0) return 0;
                        return sgn * tr;
                    }));
            }
            // Also: SPX_Conditional + VolGated (only fire when ATR > median ATR_50)
            for (auto& v : vs) {
                std::ostringstream p; p << "sgn=" << sgn << ";vg;" << v.tag;
                out.push_back(run_cell(bars, atr, ATR_N, v.sl, v.tp, sb, v.max_hold, 1,
                    "SPX_VolGated", p.str(), tf,
                    [&, sgn](int i) -> int {
                        if (i < 50) return 0;
                        std::vector<double> w(atr.begin() + i - 50, atr.begin() + i);
                        std::nth_element(w.begin(), w.begin() + w.size()/2, w.end());
                        double med = w[w.size()/2];
                        if (med <= 0 || atr[i] < 1.2 * med) return 0;
                        int tr = spx_trend_at(bars[i].ts);
                        if (tr == 0) return 0;
                        return sgn * tr;
                    }));
            }
        }
    }

    // 1. DonchianBO (HTF specific N values)
    for (int N : {10, 20, 40}) {
        if ((int)bars.size() < N + 5) continue;
        auto dc = compute_donchian(bars, N);
        std::ostringstream p; p << "N=" << N;
        out.push_back(run_cell(bars, atr, ATR_N, 1.5, 3.0, sb, 30, 1,
            "DonchianBO_HTF", p.str(), tf,
            [&, N](int i) -> int {
                if (i < N + 1) return 0;
                if (bars[i].c > dc.hi[i-1]) return +1;
                if (bars[i].c < dc.lo[i-1]) return -1;
                return 0;
            }));
    }

    // 2. WeeklyMom (only sensible on D1 -- lookback bars)
    if (tf == "D1") {
        for (int N : {3, 5, 10, 20}) {
            if ((int)bars.size() < N + 5) continue;
            std::ostringstream p; p << "lookback=" << N;
            out.push_back(run_cell(bars, atr, ATR_N, 1.5, 3.0, sb, 30, 1,
                "WeeklyMom", p.str(), tf,
                [&, N](int i) -> int {
                    if (i < N + 1) return 0;
                    if (bars[i].c > bars[i-N].c * 1.001) return +1;
                    if (bars[i].c < bars[i-N].c * 0.999) return -1;
                    return 0;
                }));
        }
    }

    // 3. VolGatedDonch (Donch but ATR > Nx median ATR_50). Sweep vol_mult
    //    {1.0, 1.2, 1.5} x R:R {sl=1.0 tp=3.0, sl=0.5 tp=2.5, sl=2.0 tp=5.0}
    //    on N={10, 20, 40}.
    if (tf == "H1" || tf == "H4" || tf == "D1") {
        struct VS { double mult, sl, tp; };
        const std::vector<VS> vs = {
            {1.0, 1.0, 3.0}, {1.2, 1.0, 3.0}, {1.5, 1.0, 3.0},
            {1.0, 0.5, 2.5}, {1.2, 0.5, 2.5}, {1.5, 0.5, 2.5},
            {1.0, 2.0, 5.0}, {1.5, 2.0, 5.0},
        };
        for (int N : {10, 20, 40}) {
            if ((int)bars.size() < N + 50) continue;
            auto dc = compute_donchian(bars, N);
            for (auto& v : vs) {
                std::ostringstream p; p << "N=" << N << ";vm=" << v.mult << ";sl=" << v.sl << ";tp=" << v.tp;
                out.push_back(run_cell(bars, atr, ATR_N, v.sl, v.tp, sb, 30, 1,
                    "VolGatedDonch", p.str(), tf,
                    [&, N, v](int i) -> int {
                        if (i < N + 50) return 0;
                        std::vector<double> w(atr.begin() + i - 50, atr.begin() + i);
                        std::nth_element(w.begin(), w.begin() + w.size()/2, w.end());
                        double med = w[w.size()/2];
                        if (med <= 0 || atr[i] < v.mult * med) return 0;
                        if (bars[i].c > dc.hi[i-1]) return +1;
                        if (bars[i].c < dc.lo[i-1]) return -1;
                        return 0;
                    }));
            }
        }
    }

    // Session-based families need H1 bars + UTC hour.
    // 2026-05-29 iter 2: first iteration had AsianRangeFade WR 3% / LondonOpenBO 8%
    // = strong contra-edge. Inverting signal direction: at session opens FX tends
    // to FAIL the breakout (false-break), so fade is the correct direction.
    if (tf == "H1") {
        // 4. LondonOpen Range-Fade-of-Asia: at hour 7-8, if price ABOVE Asia
        //    range -> SHORT (fade the upside break); below -> LONG. Target Asia mid.
        {
            std::ostringstream p; p << "hold_h=8;fade";
            out.push_back(run_cell(bars, atr, ATR_N, 1.0, 1.5, sb, 8, 1,
                "LondonFadeAsia", p.str(), tf,
                [&](int i) -> int {
                    int hr = utc_hour(bars[i].ts);
                    if (hr != 7 && hr != 8) return 0;
                    double hi = -1e18, lo = 1e18;
                    bool ok = false;
                    for (int k = 1; k <= 9 && i - k >= 0; ++k) {
                        int h = utc_hour(bars[i-k].ts);
                        if (h >= 0 && h <= 6) {
                            hi = std::max(hi, bars[i-k].h);
                            lo = std::min(lo, bars[i-k].l);
                            ok = true;
                        }
                    }
                    if (!ok) return 0;
                    if (bars[i].c > hi) return -1;  // fade upside break
                    if (bars[i].c < lo) return +1;  // fade downside break
                    return 0;
                }));
        }
        // 5. AsianContinuation: at hour 7-8, if price BREAKS Asia range, FOLLOW
        //    direction (the original BO_long that failed gets inverted again
        //    to confirm whether direction or geometry was the issue).
        //    Keep tighter SL to test.
        {
            std::ostringstream p; p << "follow;sl=0.7;tp=2.0;hold=10";
            out.push_back(run_cell(bars, atr, ATR_N, 0.7, 2.0, sb, 10, 1,
                "AsianContinuation", p.str(), tf,
                [&](int i) -> int {
                    int hr = utc_hour(bars[i].ts);
                    if (hr != 7 && hr != 8) return 0;
                    double hi = -1e18, lo = 1e18;
                    bool ok = false;
                    for (int k = 1; k <= 9 && i - k >= 0; ++k) {
                        int h = utc_hour(bars[i-k].ts);
                        if (h >= 0 && h <= 6) {
                            hi = std::max(hi, bars[i-k].h);
                            lo = std::min(lo, bars[i-k].l);
                            ok = true;
                        }
                    }
                    if (!ok) return 0;
                    if (bars[i].c > hi) return +1;
                    if (bars[i].c < lo) return -1;
                    return 0;
                }));
        }
        // 6. NYClose Continuation (invert NYCloseFade): if outside NY range at
        //    21:00 UTC, FOLLOW the move. Hold short into Asia.
        {
            std::ostringstream p; p << "follow;hold_h=8";
            out.push_back(run_cell(bars, atr, ATR_N, 1.0, 2.0, sb, 8, 1,
                "NYCloseContinuation", p.str(), tf,
                [&](int i) -> int {
                    if (utc_hour(bars[i].ts) != 21) return 0;
                    double hi = -1e18, lo = 1e18;
                    for (int k = 1; k <= 9 && i - k >= 0; ++k) {
                        int h = utc_hour(bars[i-k].ts);
                        if (h >= 13 && h <= 21) {
                            hi = std::max(hi, bars[i-k].h);
                            lo = std::min(lo, bars[i-k].l);
                        }
                    }
                    if (hi < lo) return 0;
                    if (bars[i].c > hi) return +1;
                    if (bars[i].c < lo) return -1;
                    return 0;
                }));
        }
        // 7. NYOpen Range Expansion: at hour 13-14, breakout of London (08-12)
        //    range. Hold to NY close.
        {
            std::ostringstream p; p << "follow;hold_h=8";
            out.push_back(run_cell(bars, atr, ATR_N, 1.0, 2.5, sb, 8, 1,
                "NYOpenBO", p.str(), tf,
                [&](int i) -> int {
                    int hr = utc_hour(bars[i].ts);
                    if (hr != 13 && hr != 14) return 0;
                    double hi = -1e18, lo = 1e18;
                    bool ok = false;
                    for (int k = 1; k <= 7 && i - k >= 0; ++k) {
                        int h = utc_hour(bars[i-k].ts);
                        if (h >= 8 && h <= 12) {
                            hi = std::max(hi, bars[i-k].h);
                            lo = std::min(lo, bars[i-k].l);
                            ok = true;
                        }
                    }
                    if (!ok) return 0;
                    if (bars[i].c > hi) return +1;
                    if (bars[i].c < lo) return -1;
                    return 0;
                }));
        }
    }

    return out;
}

// --- Main ---
int main(int argc, char** argv) {
    std::vector<std::string> csvs;
    std::vector<std::string> tfs;
    std::string out_path;
    std::string sym_override;
    long long from_ts = 0, to_ts = 0;
    bool verbose = false;
    std::string spx_csv;
    int spx_ema_fast = 20, spx_ema_slow = 100;

    auto need = [&](int& i, const char* f)->const char*{
        if (i+1>=argc){std::cerr<<"ERROR "<<f<<"\n";std::exit(2);}
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--csv")             csvs.push_back(need(i, "--csv"));
        else if (a == "--tf")              tfs.push_back(need(i, "--tf"));
        else if (a == "--out")             out_path = need(i, "--out");
        else if (a == "--symbol-override") sym_override = need(i, "--symbol-override");
        else if (a == "--cost-per-rt")     g_cost_override = std::atof(need(i, "--cost-per-rt"));
        else if (a == "--from-unix")       from_ts = std::atoll(need(i, "--from-unix"));
        else if (a == "--to-unix")         to_ts   = std::atoll(need(i, "--to-unix"));
        else if (a == "--spx-csv")         spx_csv = need(i, "--spx-csv");
        else if (a == "--spx-ema-fast")    spx_ema_fast = std::atoi(need(i, "--spx-ema-fast"));
        else if (a == "--spx-ema-slow")    spx_ema_slow = std::atoi(need(i, "--spx-ema-slow"));
        else if (a == "--verbose")         verbose = true;
        else if (a == "--help") {
            std::cout << "fx_edge_sweep\n"
                      << "  --csv <bar.csv>      ts,o,h,l,c (repeat for multi-tf)\n"
                      << "  --tf <H1|H4|D1>      label for each CSV in order\n"
                      << "  --symbol-override SYM\n"
                      << "  --cost-per-rt N      override per-symbol cost\n"
                      << "  --from-unix N --to-unix N\n"
                      << "  --out <csv>\n  --verbose\n";
            return 0;
        } else { std::cerr << "unknown " << a << "\n"; return 2; }
    }
    if (csvs.empty()) { std::cerr << "ERROR --csv required\n"; return 2; }
    if (tfs.size() != csvs.size()) { std::cerr << "ERROR --tf count must match --csv count\n"; return 2; }

    // Load SPX overlay bars + precompute EMA pair for trend lookup.
    if (!spx_csv.empty()) {
        g_spx_bars = load_bars(spx_csv);
        std::sort(g_spx_bars.begin(), g_spx_bars.end(),
                  [](const Bar& a, const Bar& b){ return a.ts < b.ts; });
        if (verbose) std::cerr << "[spx] bars=" << g_spx_bars.size()
                               << " ema_fast=" << spx_ema_fast
                               << " ema_slow=" << spx_ema_slow << "\n";
        if (!g_spx_bars.empty()) {
            g_spx_ema_fast.assign(g_spx_bars.size(), 0.0);
            g_spx_ema_slow.assign(g_spx_bars.size(), 0.0);
            const double kf = 2.0 / (spx_ema_fast + 1);
            const double ks = 2.0 / (spx_ema_slow + 1);
            double ef = g_spx_bars[0].c, es = g_spx_bars[0].c;
            for (size_t i = 0; i < g_spx_bars.size(); ++i) {
                double c = g_spx_bars[i].c;
                ef = kf * c + (1 - kf) * ef;
                es = ks * c + (1 - ks) * es;
                g_spx_ema_fast[i] = ef;
                g_spx_ema_slow[i] = es;
            }
        }
    }

    std::ofstream out;
    if (!out_path.empty()) {
        out.open(out_path);
        out << "symbol,timeframe,family,params,n_trades,n_wins,win_rate,net_pnl,mean_r,sharpe,max_dd\n";
        out << std::fixed << std::setprecision(5);
    }

    std::vector<Cell> all;
    for (size_t k = 0; k < csvs.size(); ++k) {
        std::string sym = sym_override.empty() ? detect_symbol(csvs[k]) : sym_override;
        SymBaseline sb = baseline_for(sym);
        if (sb.symbol.empty() || sb.symbol == "UNKNOWN") { sb = baseline_for("EURUSD"); sb.symbol = sym; }

        auto bars = load_bars(csvs[k]);
        if (verbose) std::cerr << "[load] " << csvs[k] << " bars=" << bars.size() << "\n";
        if (bars.empty()) continue;
        if (from_ts > 0 || to_ts > 0) {
            std::vector<Bar> sl; sl.reserve(bars.size());
            for (auto& b : bars) {
                if (from_ts > 0 && b.ts < from_ts) continue;
                if (to_ts   > 0 && b.ts >= to_ts) continue;
                sl.push_back(b);
            }
            bars.swap(sl);
        }
        // If TF is D1, resample from H4 input
        std::vector<Bar> bb = bars;
        if (tfs[k] == "D1") bb = resample_d1(bars);
        if (verbose) std::cerr << "[" << sym << " " << tfs[k] << "] usable_bars=" << bb.size() << "\n";

        auto cells = sweep_fx(bb, sb, tfs[k]);
        for (auto& c : cells) {
            if (out.is_open()) {
                out << c.symbol << "," << c.tf_label << "," << c.family
                    << ",\"" << c.params << "\","
                    << c.n_trades << "," << c.n_wins << "," << c.win_rate() << ","
                    << c.net << "," << c.mean_r() << "," << c.sharpe << "," << c.max_dd << "\n";
            }
            all.push_back(c);
        }
    }
    std::sort(all.begin(), all.end(), [](const Cell& a, const Cell& b){ return a.net > b.net; });
    std::cout << "\n=== TOP 25 BY NET (n>=20) ===\n";
    std::cout << std::left << std::setw(8) << "sym" << std::setw(5) << "tf"
              << std::setw(20) << "family" << std::setw(28) << "params"
              << std::right << std::setw(8) << "n" << std::setw(9) << "wr%"
              << std::setw(12) << "net$" << std::setw(10) << "sharpe" << "\n";
    int shown = 0;
    for (auto& c : all) {
        if (c.n_trades < 20) continue;
        std::cout << std::left << std::setw(8) << c.symbol << std::setw(5) << c.tf_label
                  << std::setw(20) << c.family << std::setw(28) << c.params
                  << std::right << std::setw(8) << c.n_trades
                  << std::setw(9) << std::fixed << std::setprecision(2) << 100.0 * c.win_rate()
                  << std::setw(12) << std::setprecision(2) << c.net
                  << std::setw(10) << std::setprecision(3) << c.sharpe << "\n";
        if (++shown >= 25) break;
    }
    if (out.is_open()) std::cout << "\n[out] " << out_path << "\n";
    return 0;
}
