// =====================================================================
// backtest/multi_tf_sweep.cpp
// ---------------------------------------------------------------------
// Multi-timeframe bar sweep across L2 / tick CSVs.
//
// Different beast from the L2 tick sweep:
//   * resamples tick CSV (S19 16-col L2 or 3-col Dukascopy/HistData)
//     to OHLC bars at multiple timeframes (5m, 10m, 15m, 1h, 4h).
//   * signal families chosen for bar timeframes: Donchian breakout,
//     Bollinger mean reversion, RSI extremes, MA crossover, momentum,
//     z-score mean reversion.
//   * brackets are ATR-scaled (SL = sl_mult * ATR, TP = tp_mult * ATR)
//     so geometry fits the bar size automatically. Configurable.
//   * realistic round-trip cost subtracted from every trade ($0.06 default).
//
// Build:
//   clang++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/multi_tf_sweep.cpp -o backtest/multi_tf_sweep
//
// Run:
//   backtest/multi_tf_sweep \
//       --csv 'outputs/duka_xauusd_daily/*.csv' \
//       --csv 'data/l2_ticks_XAUUSD_*.csv' \
//       --tf 5m --tf 10m --tf 15m --tf 1h --tf 4h \
//       --out backtest/multi_tf_results.csv
//
// Outputs a single ranked CSV: symbol, timeframe, family, params,
//   n_trades, n_wins, win_rate, net_pnl, mean_r, sharpe, max_dd.
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

// ---------------------------------------------------------------------
// Utility helpers (kept inline for single-file build)
// ---------------------------------------------------------------------
namespace u {
static std::string trim(std::string s) {
    size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}
static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
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
static std::string basename(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos+1);
}
static std::vector<std::string> glob_expand(const std::string& pat) {
    std::vector<std::string> out;
#ifdef HAVE_GLOB
    glob_t g{}; if (glob(pat.c_str(), GLOB_TILDE, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    }
    globfree(&g);
#endif
    if (out.empty()) out.push_back(pat);
    return out;
}
} // namespace u

// ---------------------------------------------------------------------
// Symbol baselines (lot, pt_size, val_per_pt). Same as l2_edge_sweep.
// ---------------------------------------------------------------------
struct SymBaseline {
    std::string symbol;
    double pt_size = 1.0;        // price-unit per "point"
    double val_per_pt = 1.0;     // $ per (price unit) per lot
    double lot = 0.01;
    // S37 audit fix: per-symbol half-spread for touch-fill on bar-OHLC exits.
    double half_spread = 0.15;   // price-units; conservative typical spread/2
};
static SymBaseline baseline_for(const std::string& s_in) {
    std::string s = u::upper(s_in);
    SymBaseline b; b.symbol = s;
    if      (s == "XAUUSD") { b.pt_size = 0.01;   b.val_per_pt = 1.0; b.lot = 0.01; b.half_spread = 0.15; }
    else if (s == "US500" || s == "SPX500") { b.pt_size = 0.1;  b.val_per_pt = 1.0; b.lot = 0.1; b.half_spread = 0.25; }
    else if (s == "USTEC" || s == "NAS100") { b.pt_size = 0.1;  b.val_per_pt = 1.0; b.lot = 0.1; b.half_spread = 0.5; }
    else if (s == "EURUSD" || s == "GBPUSD") { b.pt_size = 0.0001; b.val_per_pt = 1.0; b.lot = 0.01; b.half_spread = 0.00005; }
    else if (s == "USDJPY") { b.pt_size = 0.01;   b.val_per_pt = 1.0; b.lot = 0.01; b.half_spread = 0.005; }
    else { b.pt_size = 0.01; b.val_per_pt = 1.0; b.lot = 0.01; b.half_spread = 0.15; }  // sensible XAU fallback
    return b;
}
static std::string detect_symbol(const std::string& path) {
    std::string up = u::upper(u::basename(path));
    static const std::array<const char*, 7> toks = {
        "XAUUSD", "US500", "USTEC", "NAS100", "EURUSD", "GBPUSD", "USDJPY"
    };
    for (auto* t : toks) if (up.find(t) != std::string::npos) return t;
    if (up.find("XAU") != std::string::npos) return "XAUUSD";
    if (up.find("EUR") != std::string::npos) return "EURUSD";
    if (up.find("20") == 0 || up.size() > 4) {  // duka filename like "2023-09-27.csv"
        // assume folder context; caller can override
        return "XAUUSD";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------
// Tick + bar
// ---------------------------------------------------------------------
struct Tick { long long ts = 0; double bid = 0, ask = 0, mid = 0; };
struct Bar  {
    long long ts_open = 0;     // unix sec of bar's open
    int    tf_sec = 0;
    double o = 0, h = 0, l = 0, c = 0;
    int    n_ticks = 0;
};

// Load tick CSV. Streaming, returns vector<Tick>. Same auto-detect as
// s33_revised_backtest.
static std::vector<Tick> load_ticks(const std::string& path, bool verbose) {
    std::vector<Tick> out;
    std::ifstream fs(path); if (!fs) { if (verbose) std::cerr << "WARN open " << path << "\n"; return out; }
    std::string line; if (!std::getline(fs, line)) return out;
    auto hdr = u::split(line);

    int c_ts = -1, c_bid = -1, c_ask = -1, c_mid = -1; bool ts_ms = false; bool has_header = false;
    bool first_numeric = !hdr.empty() && !hdr[0].empty() &&
        (std::isdigit((unsigned char)hdr[0][0]) || hdr[0][0] == '-');
    if (!first_numeric) {
        has_header = true;
        for (size_t i = 0; i < hdr.size(); ++i) {
            std::string h = u::lower(hdr[i]);
            if (h == "ts_unix" || h == "ts" || h == "timestamp" || h == "time") c_ts = (int)i;
            if (h == "ts_ms") { c_ts = (int)i; ts_ms = true; }
            if (h == "bid") c_bid = (int)i;
            if (h == "ask") c_ask = (int)i;
            if (h == "mid") c_mid = (int)i;
        }
    } else {
        fs.clear(); fs.seekg(0);
        c_ts = 0; c_bid = 1; c_ask = 2; ts_ms = true;
    }
    if (c_ts < 0 || c_bid < 0 || c_ask < 0) {
        if (verbose) std::cerr << "WARN no ts/bid/ask in " << path << "\n";
        return out;
    }

    while (std::getline(fs, line)) {
        if (u::trim(line).empty()) continue;
        auto row = u::split(line);
        if ((int)row.size() <= std::max({c_ts, c_bid, c_ask})) continue;
        Tick t; long long raw = 0;
        if (!u::pl(row[c_ts], raw)) continue;
        t.ts = ts_ms ? raw / 1000 : raw;
        if (!u::pd(row[c_bid], t.bid) || !u::pd(row[c_ask], t.ask)) continue;
        if (t.bid <= 0 || t.ask <= 0) continue;
        if (c_mid >= 0 && (int)row.size() > c_mid) { u::pd(row[c_mid], t.mid); }
        if (t.mid == 0) t.mid = 0.5 * (t.bid + t.ask);
        out.push_back(t);
    }
    if (verbose) std::cerr << "[tick] " << path << " ticks=" << out.size()
                           << (has_header ? " (hdr)" : " (3col)") << "\n";
    return out;
}

// Resample ticks to OHLC bars at tf seconds.
static std::vector<Bar> resample(const std::vector<Tick>& ts, int tf_sec) {
    std::vector<Bar> out;
    if (ts.empty() || tf_sec <= 0) return out;
    long long cur_bucket = (ts.front().ts / tf_sec) * tf_sec;
    Bar b; b.ts_open = cur_bucket; b.tf_sec = tf_sec;
    bool started = false;
    for (const auto& t : ts) {
        long long buck = (t.ts / tf_sec) * tf_sec;
        if (buck != cur_bucket) {
            if (started) out.push_back(b);
            cur_bucket = buck;
            b = Bar{}; b.ts_open = cur_bucket; b.tf_sec = tf_sec;
            started = false;
        }
        double m = t.mid;
        if (!started) { b.o = b.h = b.l = b.c = m; started = true; }
        else { if (m > b.h) b.h = m; if (m < b.l) b.l = m; b.c = m; }
        ++b.n_ticks;
    }
    if (started) out.push_back(b);
    return out;
}

// ---------------------------------------------------------------------
// Indicators (computed once per pass through bars).
// ---------------------------------------------------------------------
static std::vector<double> compute_atr(const std::vector<Bar>& bars, int n) {
    std::vector<double> atr(bars.size(), 0.0);
    if ((int)bars.size() < n + 1) return atr;
    // Wilder ATR: TR = max(H-L, |H-Cp|, |L-Cp|); ATR = EMA of TR with alpha=1/n
    double tr_sum = 0;
    for (int i = 1; i <= n; ++i) {
        const auto& b = bars[i]; double cp = bars[i-1].c;
        double tr = std::max(b.h - b.l, std::max(std::abs(b.h - cp), std::abs(b.l - cp)));
        tr_sum += tr;
    }
    atr[n] = tr_sum / n;
    for (int i = n + 1; i < (int)bars.size(); ++i) {
        const auto& b = bars[i]; double cp = bars[i-1].c;
        double tr = std::max(b.h - b.l, std::max(std::abs(b.h - cp), std::abs(b.l - cp)));
        atr[i] = (atr[i-1] * (n - 1) + tr) / n;
    }
    return atr;
}

static std::vector<double> compute_sma(const std::vector<Bar>& bars, int n) {
    std::vector<double> out(bars.size(), 0.0);
    if ((int)bars.size() < n) return out;
    double s = 0; for (int i = 0; i < n; ++i) s += bars[i].c;
    out[n-1] = s / n;
    for (int i = n; i < (int)bars.size(); ++i) { s += bars[i].c - bars[i-n].c; out[i] = s / n; }
    return out;
}

static std::vector<double> compute_zscore(const std::vector<Bar>& bars, int n) {
    std::vector<double> z(bars.size(), 0.0);
    if ((int)bars.size() < n) return z;
    double s = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { s += bars[i].c; s2 += bars[i].c * bars[i].c; }
    for (int i = n - 1; i < (int)bars.size(); ++i) {
        double mean = s / n;
        double var = std::max(0.0, s2 / n - mean * mean);
        double sd = std::sqrt(var);
        z[i] = (sd > 1e-12) ? (bars[i].c - mean) / sd : 0.0;
        if (i + 1 < (int)bars.size()) {
            s  += bars[i+1].c - bars[i+1-n].c;
            s2 += bars[i+1].c * bars[i+1].c - bars[i+1-n].c * bars[i+1-n].c;
        }
    }
    return z;
}

// RSI (Wilder)
static std::vector<double> compute_rsi(const std::vector<Bar>& bars, int n) {
    std::vector<double> rsi(bars.size(), 50.0);
    if ((int)bars.size() <= n) return rsi;
    double g = 0, l = 0;
    for (int i = 1; i <= n; ++i) {
        double d = bars[i].c - bars[i-1].c;
        if (d > 0) g += d; else l -= d;
    }
    g /= n; l /= n;
    for (int i = n; i < (int)bars.size(); ++i) {
        if (i > n) {
            double d = bars[i].c - bars[i-1].c;
            double gu = std::max(0.0, d), ld = std::max(0.0, -d);
            g = (g * (n - 1) + gu) / n;
            l = (l * (n - 1) + ld) / n;
        }
        if (l < 1e-12) rsi[i] = 100.0;
        else { double rs = g / l; rsi[i] = 100.0 - 100.0 / (1 + rs); }
    }
    return rsi;
}

// Donchian channel (period N): rolling max(high) / min(low) over last N bars
struct DC { std::vector<double> hi, lo; };
static DC compute_donchian(const std::vector<Bar>& bars, int n) {
    DC d; d.hi.assign(bars.size(), 0); d.lo.assign(bars.size(), 0);
    if ((int)bars.size() < n) return d;
    // O(N*M); n small; fine.
    for (int i = n - 1; i < (int)bars.size(); ++i) {
        double hi = bars[i-n+1].h, lo = bars[i-n+1].l;
        for (int k = i - n + 2; k <= i; ++k) { if (bars[k].h > hi) hi = bars[k].h; if (bars[k].l < lo) lo = bars[k].l; }
        d.hi[i] = hi; d.lo[i] = lo;
    }
    return d;
}

// ---------------------------------------------------------------------
// Bracket trade simulator (bars). ATR-scaled SL/TP.
// Enter at bar[i].c (close), exit on subsequent bar's high/low triggers,
// or after max_hold_bars timeout.
// ---------------------------------------------------------------------
struct TradeResult {
    bool   filled = false;
    bool   hit_tp = false, hit_sl = false;
    int    bars   = 0;
    double pnl    = 0.0;   // $ net (cost subtracted)
    double r      = 0.0;   // R-multiple
};

static double g_cost_per_rt = 0.06;

static TradeResult simulate_atr_bracket(const std::vector<Bar>& bars,
                                        size_t entry_idx,
                                        int side,
                                        double atr_at_entry,
                                        double sl_mult,
                                        double tp_mult,
                                        const SymBaseline& sb,
                                        int max_hold_bars) {
    TradeResult r;
    if (entry_idx >= bars.size() || atr_at_entry <= 0) return r;
    double entry_px = bars[entry_idx].c;
    if (entry_px <= 0) return r;

    double sl_dist = sl_mult * atr_at_entry;
    double tp_dist = tp_mult * atr_at_entry;
    if (sl_dist <= 0 || tp_dist <= 0) return r;

    double tp = (side > 0) ? entry_px + tp_dist : entry_px - tp_dist;
    double sl = (side > 0) ? entry_px - sl_dist : entry_px + sl_dist;
    r.filled = true;

    size_t end_idx = std::min(bars.size(), entry_idx + 1 + (size_t)max_hold_bars);
    double exit_px = entry_px;
    // S37 audit fix: fill TP/SL at touch (bid for long exit, ask for short exit).
    // Bar-OHLC only — model touch as level ± half-spread (long sells worse bid,
    // short covers worse ask). Filling at the literal level overstates winners
    // by ~half-spread per trade (~30-60% phantom edge on XAU).
    const double hs = sb.half_spread;
    for (size_t i = entry_idx + 1; i < end_idx; ++i) {
        const auto& b = bars[i];
        if (side > 0) {
            if (b.l <= sl) { exit_px = sl - hs; r.hit_sl = true; r.bars = (int)(i - entry_idx); break; }
            if (b.h >= tp) { exit_px = tp - hs; r.hit_tp = true; r.bars = (int)(i - entry_idx); break; }
            exit_px = b.c;
        } else {
            if (b.h >= sl) { exit_px = sl + hs; r.hit_sl = true; r.bars = (int)(i - entry_idx); break; }
            if (b.l <= tp) { exit_px = tp + hs; r.hit_tp = true; r.bars = (int)(i - entry_idx); break; }
            exit_px = b.c;
        }
    }
    if (!r.hit_tp && !r.hit_sl) r.bars = (int)(end_idx - entry_idx - 1);

    double pnl_pts = side > 0 ? (exit_px - entry_px) : (entry_px - exit_px);
    double gross = (pnl_pts / sb.pt_size) * sb.val_per_pt * sb.lot;
    r.pnl = gross - g_cost_per_rt;
    r.r = (sl_dist > 0) ? (pnl_pts / sl_dist) : 0.0;
    return r;
}

// ---------------------------------------------------------------------
// Cell result + finaliser
// ---------------------------------------------------------------------
struct Cell {
    std::string symbol, tf_label, family, params;
    int n_trades = 0, n_wins = 0;
    double net = 0, sum_r = 0, sharpe = 0, max_dd = 0;
    double win_rate() const { return n_trades ? (double)n_wins / n_trades : 0.0; }
    double mean_r()   const { return n_trades ? sum_r / n_trades : 0.0; }
};
static void finalize(Cell& c, const std::vector<double>& pnls) {
    if (pnls.empty()) return;
    double mean = 0; for (auto p : pnls) mean += p; mean /= pnls.size();
    double var = 0; for (auto p : pnls) var += (p - mean) * (p - mean);
    var /= std::max<size_t>(1, pnls.size() - 1);
    double sd = std::sqrt(var);
    c.sharpe = (sd > 1e-12) ? (mean / sd) * std::sqrt((double)pnls.size()) : 0.0;
    double peak = 0, eq = 0, dd = 0;
    for (auto p : pnls) { eq += p; if (eq > peak) peak = eq; double d = peak - eq; if (d > dd) dd = d; }
    c.max_dd = dd;
}

// Run a single signal over bars with ATR-scaled bracket.
//   sig(i) returns +1 long / -1 short / 0 none, given pre-computed indicators.
template <typename Sig>
static Cell run_cell(const std::vector<Bar>& bars,
                     const std::vector<double>& atr,
                     int atr_n, double sl_mult, double tp_mult,
                     const SymBaseline& sb,
                     int max_hold_bars,
                     int cooldown_bars,
                     const std::string& family,
                     const std::string& params,
                     const std::string& tf_label,
                     Sig sig) {
    Cell c; c.symbol = sb.symbol; c.tf_label = tf_label; c.family = family; c.params = params;
    if (bars.empty()) return c;
    std::vector<double> pnls;
    int cd = 0;
    int start = atr_n + 5;
    for (int i = start; i < (int)bars.size(); ++i) {
        if (cd > 0) { --cd; continue; }
        int s = sig(i); if (s == 0) continue;
        if (atr[i] <= 0) continue;
        auto r = simulate_atr_bracket(bars, i, s, atr[i], sl_mult, tp_mult, sb, max_hold_bars);
        if (!r.filled) continue;
        ++c.n_trades; if (r.pnl > 0) ++c.n_wins;
        c.net += r.pnl; c.sum_r += r.r; pnls.push_back(r.pnl);
        cd = cooldown_bars + r.bars;
    }
    finalize(c, pnls);
    return c;
}

// ---------------------------------------------------------------------
// Sweep grid: 6 families per (symbol, tf). Keep cells modest so the
// total sweep finishes in seconds.
// ---------------------------------------------------------------------
static std::vector<Cell> sweep_grid(const std::vector<Bar>& bars,
                                    const SymBaseline& sb,
                                    const std::string& tf_label,
                                    int max_hold_bars,
                                    int cooldown_bars) {
    std::vector<Cell> out;
    if ((int)bars.size() < 100) return out;

    // ATR baseline (for brackets)
    const int ATR_N = 14;
    auto atr = compute_atr(bars, ATR_N);

    // 1. Donchian breakout (trend-follow): long on close > donchian.hi[i-1], short on close < donchian.lo[i-1]
    for (int N : {20, 50, 100}) {
        if ((int)bars.size() < N + 5) continue;
        auto dc = compute_donchian(bars, N);
        // sl_mult/tp_mult chosen for trend-follow (wider tp)
        std::ostringstream p; p << "N=" << N;
        out.push_back(run_cell(bars, atr, ATR_N, 1.5, 3.0, sb,
            max_hold_bars, cooldown_bars, "DonchianBreakout", p.str(), tf_label,
            [&](int i) -> int {
                if (i < N + 1) return 0;
                if (bars[i].c > dc.hi[i-1]) return +1;
                if (bars[i].c < dc.lo[i-1]) return -1;
                return 0;
            }));
    }

    // 2. Bollinger mean reversion: sell at upper, buy at lower (W=20, k=2.0; W=50, k=2.0)
    for (int W : {20, 50}) {
        if ((int)bars.size() < W + 5) continue;
        auto sma = compute_sma(bars, W);
        // compute rolling stdev for bands
        std::vector<double> band_hi(bars.size(), 0), band_lo(bars.size(), 0);
        double s2 = 0, s = 0;
        for (int i = 0; i < W; ++i) { s += bars[i].c; s2 += bars[i].c * bars[i].c; }
        for (int i = W - 1; i < (int)bars.size(); ++i) {
            double mean = s / W;
            double var = std::max(0.0, s2 / W - mean * mean);
            double sd = std::sqrt(var);
            band_hi[i] = mean + 2.0 * sd;
            band_lo[i] = mean - 2.0 * sd;
            if (i + 1 < (int)bars.size()) {
                s  += bars[i+1].c - bars[i+1-W].c;
                s2 += bars[i+1].c * bars[i+1].c - bars[i+1-W].c * bars[i+1-W].c;
            }
        }
        std::ostringstream p; p << "W=" << W << ";k=2";
        out.push_back(run_cell(bars, atr, ATR_N, 1.0, 2.0, sb,
            max_hold_bars, cooldown_bars, "BollingerMR", p.str(), tf_label,
            [&](int i) -> int {
                if (i < W) return 0;
                if (bars[i].c > band_hi[i]) return -1;  // mean revert
                if (bars[i].c < band_lo[i]) return +1;
                return 0;
            }));
    }

    // 3. RSI extremes (mean rev): oversold buy, overbought sell
    for (auto [n, lo, hi] : (std::vector<std::tuple<int,int,int>>{{14,30,70},{14,20,80},{7,30,70}})) {
        if ((int)bars.size() < n + 5) continue;
        auto rsi = compute_rsi(bars, n);
        std::ostringstream p; p << "N=" << n << ";lo=" << lo << ";hi=" << hi;
        out.push_back(run_cell(bars, atr, ATR_N, 1.0, 2.0, sb,
            max_hold_bars, cooldown_bars, "RSI_Extreme", p.str(), tf_label,
            [&, n, lo, hi](int i) -> int {
                if (i < n + 1) return 0;
                if (rsi[i-1] > hi && rsi[i] < hi) return -1;  // exit overbought
                if (rsi[i-1] < lo && rsi[i] > lo) return +1;  // exit oversold
                return 0;
            }));
    }

    // 4. MA crossover (trend follow): fast/slow cross
    for (auto [f, s] : (std::vector<std::pair<int,int>>{{10,30},{20,50}})) {
        if ((int)bars.size() < s + 5) continue;
        auto fast = compute_sma(bars, f);
        auto slow = compute_sma(bars, s);
        std::ostringstream p; p << "fast=" << f << ";slow=" << s;
        out.push_back(run_cell(bars, atr, ATR_N, 1.5, 3.0, sb,
            max_hold_bars, cooldown_bars, "MACrossover", p.str(), tf_label,
            [&, f, s](int i) -> int {
                if (i < s + 1) return 0;
                bool cross_up   = fast[i-1] <= slow[i-1] && fast[i] >  slow[i];
                bool cross_down = fast[i-1] >= slow[i-1] && fast[i] <  slow[i];
                return cross_up ? +1 : (cross_down ? -1 : 0);
            }));
    }

    // 5. Simple momentum: close > close[-N] long; close < close[-N] short
    for (int N : {20, 50}) {
        if ((int)bars.size() < N + 5) continue;
        std::ostringstream p; p << "lookback=" << N;
        out.push_back(run_cell(bars, atr, ATR_N, 1.5, 3.0, sb,
            max_hold_bars, cooldown_bars, "MomentumN", p.str(), tf_label,
            [&, N](int i) -> int {
                if (i < N + 1) return 0;
                if (bars[i].c > bars[i-N].c * 1.001) return +1;  // 0.1% threshold
                if (bars[i].c < bars[i-N].c * 0.999) return -1;
                return 0;
            }));
    }

    // 6. Z-score mean reversion
    for (auto [W, Z10] : (std::vector<std::pair<int,int>>{{20,20},{20,25},{50,20}})) {
        if ((int)bars.size() < W + 5) continue;
        auto z = compute_zscore(bars, W);
        std::ostringstream p; p << "W=" << W << ";Z=" << (Z10/10.0);
        double zthr = Z10 / 10.0;
        out.push_back(run_cell(bars, atr, ATR_N, 1.0, 2.0, sb,
            max_hold_bars, cooldown_bars, "ZScoreMR", p.str(), tf_label,
            [&, zthr](int i) -> int {
                if (z[i] >=  zthr) return -1;
                if (z[i] <= -zthr) return +1;
                return 0;
            }));
    }

    return out;
}

// ---------------------------------------------------------------------
// Parse timeframe like "5m", "15m", "1h", "4h" -> seconds.
// ---------------------------------------------------------------------
static int parse_tf(const std::string& s) {
    if (s.empty()) return 0;
    char unit = (char)std::tolower((unsigned char)s.back());
    std::string num = s.substr(0, s.size() - 1);
    int v = std::atoi(num.c_str());
    if (v <= 0) return 0;
    if (unit == 's') return v;
    if (unit == 'm') return v * 60;
    if (unit == 'h') return v * 3600;
    if (unit == 'd') return v * 86400;
    return std::atoi(s.c_str());
}

// ---------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    std::vector<std::string> patterns;
    std::vector<std::string> tfs;
    std::string out_path;
    std::string sym_override;
    int max_hold_bars = 50;
    int cooldown_bars = 1;
    bool verbose = false;

    auto need = [&](int& i, const char* f) -> const char* {
        if (i + 1 >= argc) { std::cerr << "ERROR " << f << "\n"; std::exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--csv")            patterns.push_back(need(i, "--csv"));
        else if (a == "--tf")             tfs.push_back(need(i, "--tf"));
        else if (a == "--out")            out_path = need(i, "--out");
        else if (a == "--symbol-override") sym_override = need(i, "--symbol-override");
        else if (a == "--cost-per-rt")    g_cost_per_rt = std::atof(need(i, "--cost-per-rt"));
        else if (a == "--max-hold-bars")  max_hold_bars = std::atoi(need(i, "--max-hold-bars"));
        else if (a == "--verbose")        verbose = true;
        else if (a == "--help" || a == "-h") {
            std::cout <<
                "multi_tf_sweep -- bar-level sweep across 6 signal families\n"
                "  --csv <glob>            tick csv (repeat)\n"
                "  --tf  5m|10m|15m|1h|4h  (repeat; default 5m,15m,1h,4h)\n"
                "  --out <csv>             result csv\n"
                "  --symbol-override SYM   force symbol baseline\n"
                "  --cost-per-rt N         default 0.06\n"
                "  --max-hold-bars N       default 50\n"
                "  --verbose\n";
            return 0;
        } else { std::cerr << "unknown arg " << a << "\n"; return 2; }
    }
    if (patterns.empty()) { std::cerr << "ERROR: --csv required\n"; return 2; }
    if (tfs.empty()) tfs = {"5m","15m","1h","4h"};

    // Group files by symbol
    std::map<std::string, std::vector<std::string>> by_sym;
    for (const auto& p : patterns) {
        for (const auto& f : u::glob_expand(p)) {
            std::string s = sym_override.empty() ? detect_symbol(f) : sym_override;
            by_sym[s].push_back(f);
        }
    }
    if (verbose)
        for (auto& kv : by_sym) std::cerr << "[sym] " << kv.first << "  files=" << kv.second.size() << "\n";

    std::ofstream out;
    if (!out_path.empty()) {
        out.open(out_path);
        if (!out) { std::cerr << "ERR write " << out_path << "\n"; return 1; }
        out << "symbol,timeframe,family,params,n_trades,n_wins,win_rate,net_pnl,mean_r,sharpe,max_dd\n";
        out << std::fixed << std::setprecision(5);
    }

    std::vector<Cell> all;
    for (auto& kv : by_sym) {
        const std::string& sym = kv.first;
        SymBaseline sb = baseline_for(sym);
        if (sb.symbol.empty() || sb.symbol == "UNKNOWN") {
            sb = baseline_for("XAUUSD"); sb.symbol = sym;
        }

        // Merge ticks across files (preserves order for sorted filenames).
        std::vector<Tick> merged;
        for (const auto& f : kv.second) {
            auto v = load_ticks(f, verbose);
            merged.insert(merged.end(), v.begin(), v.end());
        }
        std::sort(merged.begin(), merged.end(),
                  [](const Tick& a, const Tick& b){ return a.ts < b.ts; });
        if (merged.empty()) continue;
        if (verbose) std::cerr << "[merged] " << sym << " ticks=" << merged.size() << "\n";

        for (const auto& tf : tfs) {
            int tf_sec = parse_tf(tf);
            if (tf_sec <= 0) continue;
            auto bars = resample(merged, tf_sec);
            if (verbose) std::cerr << "[bars] " << sym << " " << tf << " bars=" << bars.size() << "\n";
            auto cells = sweep_grid(bars, sb, tf, max_hold_bars, 1);
            for (auto& c : cells) {
                if (out) {
                    out << c.symbol << "," << c.tf_label << "," << c.family
                        << ",\"" << c.params << "\","
                        << c.n_trades << "," << c.n_wins << "," << c.win_rate() << ","
                        << c.net << "," << c.mean_r() << "," << c.sharpe << "," << c.max_dd << "\n";
                }
                all.push_back(c);
            }
        }
    }

    // stdout leaderboard: top 10 by net_pnl (across all)
    std::sort(all.begin(), all.end(),
              [](const Cell& a, const Cell& b){ return a.net > b.net; });
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n=== TOP 30 CELLS BY NET PNL (n_trades >= 30) ===\n";
    std::cout << std::left
              << std::setw(10) << "sym"
              << std::setw(6)  << "tf"
              << std::setw(20) << "family"
              << std::setw(24) << "params"
              << std::setw(7)  << "n"
              << std::setw(8)  << "wr%"
              << std::setw(11) << "net$"
              << std::setw(10) << "sharpe"
              << "\n";
    int shown = 0;
    for (const auto& c : all) {
        if (c.n_trades < 30) continue;
        if (shown++ >= 30) break;
        std::cout << std::left
                  << std::setw(10) << c.symbol
                  << std::setw(6)  << c.tf_label
                  << std::setw(20) << c.family
                  << std::setw(24) << c.params
                  << std::setw(7)  << c.n_trades
                  << std::setw(8)  << (100.0 * c.win_rate())
                  << std::setw(11) << c.net
                  << std::setw(10) << c.sharpe
                  << "\n";
    }
    if (!out_path.empty()) std::cerr << "[out] " << out_path << "\n";
    return 0;
}
