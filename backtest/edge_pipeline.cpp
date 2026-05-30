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
    // 2026-05-29: per-symbol round-trip cost in USD at the symbol's standard
    // lot size above. Lifted from include/OmegaCostGuard.hpp production values
    // (estimated_cost_usd: spread + slippage + commission). Replaces the
    // global flat --cost-per-rt flag when set (>0).
    double cost_per_rt_usd = 0.06;  // legacy default fallback
};
static SymBaseline baseline_for(const std::string& s_in) {
    std::string s = u::upper(s_in);
    SymBaseline b; b.symbol = s;
    // Cost values per OmegaCostGuard.hpp::estimated_cost_usd at the lot
    // shown below. Formula: spread_cost (typ_spread * tick_usd_per_lot * lot)
    // + slip_cost (slip_pts * tick_usd_per_lot * lot) + comm_cost.
    if      (s == "XAUUSD") { b.pt_size = 0.01;   b.lot = 0.01; b.half_spread = 0.15; b.cost_per_rt_usd = 0.66; }
    else if (s == "US500" || s == "SPX500" || s == "SPXUSD" || s == "US500.F") { b.pt_size = 0.1; b.lot = 0.10; b.half_spread = 0.25; b.cost_per_rt_usd = 2.00; b.symbol = "US500"; }
    else if (s == "USTEC" || s == "NAS100" || s == "NSXUSD" || s == "USTEC.F") { b.pt_size = 0.1; b.lot = 0.10; b.half_spread = 0.50; b.cost_per_rt_usd = 1.10; b.symbol = "USTEC"; }
    else if (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" || s == "NZDUSD") { b.pt_size = 0.0001; b.lot = 0.01; b.half_spread = 0.00005; b.cost_per_rt_usd = 0.36; }
    else if (s == "EURGBP") { b.pt_size = 0.0001; b.lot = 0.01; b.half_spread = 0.00005; b.cost_per_rt_usd = 0.36; }
    else if (s == "USDCAD") { b.pt_size = 0.0001; b.lot = 0.01; b.half_spread = 0.00005; b.cost_per_rt_usd = 0.36; }
    else if (s == "USDJPY") { b.pt_size = 0.01;   b.lot = 0.01; b.half_spread = 0.005;   b.cost_per_rt_usd = 0.22; }
    else if (s == "GER40")  { b.pt_size = 0.1;    b.lot = 0.10; b.half_spread = 0.50;    b.cost_per_rt_usd = 0.20; }
    else if (s == "UK100")  { b.pt_size = 0.1;    b.lot = 0.10; b.half_spread = 0.30;    b.cost_per_rt_usd = 0.15; }
    else if (s == "DJ30" || s == "DJ30.F") { b.pt_size = 1.0; b.lot = 0.10; b.half_spread = 1.5; b.cost_per_rt_usd = 0.30; b.symbol = "DJ30"; }
    else if (s == "BCOUSD" || s == "BRENT") { b.pt_size = 0.01; b.lot = 0.01; b.half_spread = 0.02; b.cost_per_rt_usd = 0.60; b.symbol = "BCOUSD"; }
    else if (s == "USOIL" || s == "WTI" || s == "USOIL.F") { b.pt_size = 0.01; b.lot = 0.01; b.half_spread = 0.02; b.cost_per_rt_usd = 0.60; b.symbol = "USOIL"; }
    else if (s == "XAGUSD") { b.pt_size = 0.001; b.lot = 0.01; b.half_spread = 0.01; b.cost_per_rt_usd = 0.40; }
    else { b.pt_size = 0.01; b.lot = 0.01; b.half_spread = 0.15; b.cost_per_rt_usd = 0.66; }  // sensible XAU fallback
    return b;
}
static std::string detect_symbol(const std::string& path) {
    std::string up = u::upper(u::basename(path));
    static const std::array<const char*, 17> toks = {
        "XAUUSD", "US500", "SPX500", "SPXUSD", "USTEC", "NAS100", "NSXUSD",
        "EURUSD", "GBPUSD", "AUDUSD", "NZDUSD", "USDJPY", "USDCAD", "EURGBP",
        "GER40", "BCOUSD", "DJ30"
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

// 2026-05-29: default -1.0 = "use SymBaseline.cost_per_rt_usd (per-symbol)".
// CLI --cost-per-rt N still overrides with a flat value across all symbols.
static double g_cost_per_rt = -1.0;
// 2026-05-30 (S39): cost-stress multiplier. Scales each symbol's per-symbol
// round-trip cost by this factor (default 1.0). --cost-mult 2.0 = "does the
// edge survive double the modeled cost?" -- the research red-flag test for
// whether an edge is a cost-margin artifact.
static double g_cost_mult = 1.0;
// 2026-05-30 (S39): side filter. 0=both, +1=long-only, -1=short-only.
// Decomposes an edge into its directional halves -- a trend edge that is
// only profitable long in a bull market is regime beta, not skill; a real
// edge shows positive expectancy on BOTH sides.
static int g_side = 0;

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
    // 2026-05-29: per-symbol cost from OmegaCostGuard production values
    // (sb.cost_per_rt_usd). Global g_cost_per_rt retained as a CLI override
    // for ad-hoc runs; when left at default it acts as a floor of 0.
    const double cost_rt = ((g_cost_per_rt >= 0.0) ? g_cost_per_rt : sb.cost_per_rt_usd) * g_cost_mult;
    r.pnl = gross - cost_rt;
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
    double gross_win = 0, gross_loss = 0;   // for profit factor
    double win_rate() const { return n_trades ? (double)n_wins / n_trades : 0.0; }
    double mean_r()   const { return n_trades ? sum_r / n_trades : 0.0; }
    double pf()       const { return gross_loss > 1e-9 ? gross_win / gross_loss : (gross_win > 0 ? 999.0 : 0.0); }
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
        if (g_side != 0 && s != g_side) continue;   // side filter (long/short decomposition)
        if (atr[i] <= 0) continue;
        auto r = simulate_atr_bracket(bars, i, s, atr[i], sl_mult, tp_mult, sb, max_hold_bars);
        if (!r.filled) continue;
        ++c.n_trades; if (r.pnl > 0) ++c.n_wins;
        c.net += r.pnl; c.sum_r += r.r; pnls.push_back(r.pnl);
        if (r.pnl > 0) c.gross_win += r.pnl; else c.gross_loss += -r.pnl;
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
    // S39 2026-05-30: finer N grid for param-plateau / overfit check.
    for (int N : {15, 20, 25, 30, 40, 55, 75, 100}) {
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
    for (auto [n, lo, hi] : (std::vector<std::tuple<int,int,int>>{
            {14,30,70},{14,20,80},{7,30,70},{7,25,75},{10,30,70},{21,30,70},{9,30,70}})) {
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

    // ===== S39 EXPANDED EDGE HUNT (2026-05-30) ============================
    // Bracket geometry is the biggest untested driver. Sweep sl/tp on the
    // proven trend families + add channel-breakout and vol-expansion families.
    const std::vector<std::pair<double,double>> BRK = {
        {1.0,2.0},{1.5,3.0},{2.0,4.0},{2.0,6.0},{1.0,3.0},{2.5,5.0}};

    // 7. Donchian breakout x bracket sweep
    for (int N : {20, 30, 55}) {
        if ((int)bars.size() < N + 5) continue;
        auto dc = compute_donchian(bars, N);
        for (auto& br : BRK) {
            std::ostringstream p; p << "N=" << N << ";sl" << br.first << ";tp" << br.second;
            out.push_back(run_cell(bars, atr, ATR_N, br.first, br.second, sb,
                max_hold_bars, cooldown_bars, "DonchianBrk", p.str(), tf_label,
                [&, N](int i)->int{ if(i<N+1)return 0;
                    if(bars[i].c>dc.hi[i-1])return +1; if(bars[i].c<dc.lo[i-1])return -1; return 0;}));
        }
    }

    // 8. Keltner channel breakout: close breaks SMA(N) +/- k*ATR (trend)
    for (auto& nk : (std::vector<std::pair<int,int>>{{20,15},{20,20},{50,20}})) {
        int N = nk.first; double k = nk.second/10.0;
        if ((int)bars.size() < N + 5) continue;
        auto mid = compute_sma(bars, N);
        for (auto& br : (std::vector<std::pair<double,double>>{{1.5,3.0},{2.0,4.0}})) {
            std::ostringstream p; p<<"N="<<N<<";k="<<k<<";sl"<<br.first<<";tp"<<br.second;
            out.push_back(run_cell(bars, atr, ATR_N, br.first, br.second, sb,
                max_hold_bars, cooldown_bars, "KeltnerBrk", p.str(), tf_label,
                [&, k](int i)->int{ if(i<1||mid[i]<=0||atr[i]<=0)return 0;
                    if(bars[i].c > mid[i]+k*atr[i])return +1;
                    if(bars[i].c < mid[i]-k*atr[i])return -1; return 0;}));
        }
    }

    // 9. Volatility-expansion breakout: bar range > k*ATR -> trade bar direction
    for (int k10 : {15,20,25}) {
        double k = k10/10.0;
        for (auto& br : (std::vector<std::pair<double,double>>{{1.5,3.0},{2.0,4.0}})) {
            std::ostringstream p; p<<"k="<<k<<";sl"<<br.first<<";tp"<<br.second;
            out.push_back(run_cell(bars, atr, ATR_N, br.first, br.second, sb,
                max_hold_bars, cooldown_bars, "VolExpBrk", p.str(), tf_label,
                [&, k](int i)->int{ if(i<1||atr[i]<=0)return 0;
                    double rng=bars[i].h-bars[i].l;
                    if(rng < k*atr[i])return 0;
                    return (bars[i].c>bars[i].o)?+1:((bars[i].c<bars[i].o)?-1:0);}));
        }
    }

    // 10. MA-cross x bracket sweep
    for (auto& fs : (std::vector<std::pair<int,int>>{{10,30},{20,50}})) {
        int f = fs.first, s = fs.second;
        if ((int)bars.size() < s + 5) continue;
        auto fast = compute_sma(bars, f), slow = compute_sma(bars, s);
        for (auto& br : BRK) {
            std::ostringstream p; p<<"f="<<f<<";s="<<s<<";sl"<<br.first<<";tp"<<br.second;
            out.push_back(run_cell(bars, atr, ATR_N, br.first, br.second, sb,
                max_hold_bars, cooldown_bars, "MACrossBrk", p.str(), tf_label,
                [&, f, s](int i)->int{ if(i<s+1)return 0;
                    bool cu=fast[i-1]<=slow[i-1]&&fast[i]>slow[i];
                    bool cd=fast[i-1]>=slow[i-1]&&fast[i]<slow[i];
                    return cu?+1:(cd?-1:0);}));
        }
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
// Direct OHLC bar loader (ts,o,h,l,c) -- the Tick corpus is already
// aggregated to bars, so we load them straight rather than resampling a
// tick stream (resampling from a single mid would destroy the H/L the
// bracket simulator needs for realistic touch fills).
// ---------------------------------------------------------------------
static std::vector<Bar> load_bars(const std::string& path, int tf_sec) {
    std::vector<Bar> out;
    std::ifstream fs(path);
    if (!fs) return out;
    std::string line;
    if (!std::getline(fs, line)) return out;  // header
    while (std::getline(fs, line)) {
        if (u::trim(line).empty()) continue;
        auto row = u::split(line);
        if (row.size() < 5) continue;
        long long ts = 0; double o = 0, h = 0, l = 0, c = 0;
        if (!u::pl(row[0], ts)) continue;
        if (ts > 100000000000LL) ts /= 1000;  // ms -> sec
        if (!u::pd(row[1], o) || !u::pd(row[2], h) ||
            !u::pd(row[3], l) || !u::pd(row[4], c)) continue;
        if (o <= 0 || h <= 0 || l <= 0 || c <= 0) continue;
        Bar b; b.ts_open = ts; b.tf_sec = tf_sec;
        b.o = o; b.h = h; b.l = l; b.c = c; b.n_ticks = 1;
        out.push_back(b);
    }
    return out;
}

// Derive (symbol, tf_label, tf_sec) from a bar filename, e.g.
//   AUDUSD_merged.h1.csv          -> AUDUSD, 1h, 3600
//   2yr_XAUUSD_tick_fresh.h4.csv  -> XAUUSD, 4h, 14400
struct BarFileInfo { std::string sym, tf_label; int tf_sec = 0; bool ok = false; };
static BarFileInfo parse_bar_filename(const std::string& path) {
    BarFileInfo bf;
    std::string base = path;
    auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    // tf suffix
    struct { const char* suf; const char* lbl; int sec; } tfs[] = {
        {".h1.csv","1h",3600},{".h4.csv","4h",14400},
        {".m5.csv","5m",300},{".m15.csv","15m",900},{".m30.csv","30m",1800},
        {".d1.csv","1d",86400},
    };
    std::string core;
    for (auto& t : tfs) {
        size_t pos = base.rfind(t.suf);
        if (pos != std::string::npos && pos + std::strlen(t.suf) == base.size()) {
            bf.tf_label = t.lbl; bf.tf_sec = t.sec; core = base.substr(0, pos); break;
        }
    }
    if (bf.tf_sec == 0) return bf;  // not a bar file
    // symbol: known token else strip _merged / known prefixes
    static const char* known[] = {"XAUUSD","XAGUSD","EURUSD","GBPUSD","AUDUSD",
        "NZDUSD","USDJPY","USDCAD","EURGBP","BCOUSD","SPXUSD","NSXUSD","GER40",
        "USA30","GBRIDXGBP","US500","USTEC","DJ30","BRENT","USOIL"};
    for (auto k : known) {
        if (core.find(k) != std::string::npos) { bf.sym = k; bf.ok = true; return bf; }
    }
    auto us = core.find("_merged");
    if (us != std::string::npos) { bf.sym = core.substr(0, us); bf.ok = true; }
    return bf;
}

// ---------------------------------------------------------------------
// Walk-forward edge pipeline.
//   For each (symbol, timeframe) bar file:
//     * split bars 50/50 by index -> TRAIN (first half) / TEST (second).
//     * run the SAME fixed signal grid on each half (grids are not tuned,
//       so this is a true held-out OOS check, not a re-fit).
//     * join cells by (family,params); a cell PASSES only if it is
//       positive and adequately-sampled in BOTH halves.
//   Output: per-cell CSV with train+test stats + PASS flag, ranked.
//   Trial-count deflation (Deflated Sharpe) is applied downstream by
//   tools/deflated_sharpe_gate.py over this CSV's TEST column.
// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string tickdir = "/Users/jo/Tick";
    std::string out_path = "outputs/edge_pipeline_wf.csv";
    int    min_n   = 30;     // min trades per half to trust a cell
    double pf_gate = 1.10;   // min TEST profit factor to PASS
    int    max_hold_bars = 50;
    int    n_blocks = 0;     // >0 => N-block consistency mode instead of 50/50 WF
    bool   verbose = false;
    std::vector<std::string> only_tfs;  // empty = all found

    auto need = [&](int& i, const char* f) -> const char* {
        if (i + 1 >= argc) { std::cerr << "ERROR " << f << "\n"; std::exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--tickdir")       tickdir = need(i, "--tickdir");
        else if (a == "--out")           out_path = need(i, "--out");
        else if (a == "--min-n")         min_n = std::atoi(need(i, "--min-n"));
        else if (a == "--pf-gate")       pf_gate = std::atof(need(i, "--pf-gate"));
        else if (a == "--max-hold-bars") max_hold_bars = std::atoi(need(i, "--max-hold-bars"));
        else if (a == "--blocks")        n_blocks = std::atoi(need(i, "--blocks"));
        else if (a == "--tf")            only_tfs.push_back(need(i, "--tf"));
        else if (a == "--cost-per-rt")   g_cost_per_rt = std::atof(need(i, "--cost-per-rt"));
        else if (a == "--cost-mult")     g_cost_mult = std::atof(need(i, "--cost-mult"));
        else if (a == "--side")          g_side = std::atoi(need(i, "--side"));
        else if (a == "--verbose")       verbose = true;
        else if (a == "--help" || a == "-h") {
            std::cout <<
                "edge_pipeline -- walk-forward edge discovery over the bar corpus\n"
                "  --tickdir DIR     bar-corpus root (default /Users/jo/Tick)\n"
                "  --out CSV         result csv (default outputs/edge_pipeline_wf.csv)\n"
                "  --min-n N         min trades per half (default 30)\n"
                "  --pf-gate F       min TEST profit factor to PASS (default 1.10)\n"
                "  --tf 1h|4h|...    restrict timeframes (repeat; default all)\n"
                "  --max-hold-bars N (default 50)\n"
                "  --verbose\n";
            return 0;
        } else { std::cerr << "unknown arg " << a << "\n"; return 2; }
    }

    // Collect bar files
    std::vector<std::string> files;
    for (const char* pat : {"/*_merged.h1.csv","/*_merged.h4.csv","/*_merged.m5.csv",
                            "/*_merged.m15.csv","/*_merged.m30.csv",
                            "/2yr_XAUUSD_tick_fresh.h1.csv","/2yr_XAUUSD_tick_fresh.h4.csv",
                            "/2yr_XAUUSD_tick_fresh.m5.csv"}) {
        for (auto& f : u::glob_expand(tickdir + pat)) files.push_back(f);
    }
    std::sort(files.begin(), files.end());
    if (verbose) std::cerr << "[files] " << files.size() << " bar files under " << tickdir << "\n";

    // ===================================================================
    // N-BLOCK CONSISTENCY MODE (--blocks N)
    //   Split each symbol/tf series into N disjoint contiguous time blocks
    //   (each block = a different market period). Run the SAME fixed grid on
    //   every block. A cell is ROBUST only if it stays net-positive across
    //   most blocks -- a far harder bar than one 50/50 split and the direct
    //   test of regime-dependence: a single-period/regime fluke fails because
    //   it cannot be positive in 4+ of 5 independent windows.
    // ===================================================================
    if (n_blocks > 0) {
        struct Agg {
            std::string sym, tf, family, params;
            int blocks_traded = 0, blocks_pos = 0, total_n = 0;
            double total_net = 0, min_pf = 1e18, sum_pf = 0; int pf_cnt = 0;
            std::vector<double> block_net;
        };
        std::map<std::string, Agg> agg;
        const int min_block = std::max(5, min_n / std::max(1, n_blocks));
        for (const auto& f : files) {
            BarFileInfo bf = parse_bar_filename(f);
            if (!bf.ok) continue;
            if (!only_tfs.empty() &&
                std::find(only_tfs.begin(), only_tfs.end(), bf.tf_label) == only_tfs.end()) continue;
            SymBaseline sb = baseline_for(bf.sym);
            if (sb.symbol.empty() || sb.symbol == "UNKNOWN") { sb = baseline_for("XAUUSD"); sb.symbol = bf.sym; }
            auto bars = load_bars(f, bf.tf_sec);
            if ((int)bars.size() < n_blocks * (min_block + 120)) {
                if (verbose) std::cerr << "[thin] " << bf.sym << " " << bf.tf_label
                                       << " bars=" << bars.size() << " for " << n_blocks << " blocks -- skip\n";
                continue;
            }
            size_t bsz = bars.size() / n_blocks;
            for (int b = 0; b < n_blocks; ++b) {
                size_t lo = b * bsz;
                size_t hi = (b == n_blocks - 1) ? bars.size() : (b + 1) * bsz;
                std::vector<Bar> blk(bars.begin() + lo, bars.begin() + hi);
                auto cells = sweep_grid(blk, sb, bf.tf_label, max_hold_bars, 1);
                for (auto& c : cells) {
                    std::string key = bf.sym + "|" + bf.tf_label + "|" + c.family + "|" + c.params;
                    auto& a = agg[key];
                    a.sym = bf.sym; a.tf = bf.tf_label; a.family = c.family; a.params = c.params;
                    if (c.n_trades >= min_block) {
                        ++a.blocks_traded;
                        if (c.net > 0) ++a.blocks_pos;
                        double pf = c.pf();
                        a.min_pf = std::min(a.min_pf, pf); a.sum_pf += pf; ++a.pf_cnt;
                    }
                    a.total_n += c.n_trades; a.total_net += c.net; a.block_net.push_back(c.net);
                }
            }
        }
        std::ofstream out(out_path);
        if (!out) { std::cerr << "ERR write " << out_path << "\n"; return 1; }
        out << "symbol,timeframe,family,params,blocks,blocks_traded,blocks_pos,consistency,"
               "total_n,total_net,min_block_pf,mean_block_pf\n";
        out << std::fixed << std::setprecision(4);
        std::vector<Agg> robust;
        for (auto& kv : agg) {
            Agg& a = kv.second;
            double cons = a.blocks_traded ? (double)a.blocks_pos / a.blocks_traded : 0.0;
            double meanpf = a.pf_cnt ? a.sum_pf / a.pf_cnt : 0.0;
            double minpf = (a.min_pf > 1e17) ? 0.0 : a.min_pf;
            out << a.sym << "," << a.tf << "," << a.family << ",\"" << a.params << "\","
                << n_blocks << "," << a.blocks_traded << "," << a.blocks_pos << "," << cons << ","
                << a.total_n << "," << a.total_net << "," << minpf << "," << meanpf << "\n";
            // ROBUST = traded in >=n_blocks-1 blocks, positive in >=2/3, total n>=min_n, net>0
            if (a.blocks_traded >= n_blocks - 1 && cons >= 0.66
                && a.total_n >= min_n && a.total_net > 0)
                robust.push_back(a);
        }
        out.close();
        std::sort(robust.begin(), robust.end(),
                  [](const Agg& x, const Agg& y){
                      double cx = x.blocks_traded?(double)x.blocks_pos/x.blocks_traded:0;
                      double cy = y.blocks_traded?(double)y.blocks_pos/y.blocks_traded:0;
                      if (cx != cy) return cx > cy; return x.total_net > y.total_net; });
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n=== " << n_blocks << "-BLOCK CONSISTENCY SURVIVORS "
                  << "(positive in >=2/3 of disjoint periods, traded >=" << (n_blocks-1)
                  << " blocks, total n>=" << min_n << ") ===\n";
        std::cout << std::left
                  << std::setw(9) << "sym" << std::setw(5) << "tf"
                  << std::setw(18) << "family" << std::setw(22) << "params"
                  << std::setw(8) << "pos/trd" << std::setw(8) << "cons" << std::setw(8) << "minPF"
                  << std::setw(7) << "n" << std::setw(10) << "net$" << "\n";
        for (auto& a : robust) {
            double cons = a.blocks_traded ? (double)a.blocks_pos / a.blocks_traded : 0.0;
            double minpf = (a.min_pf > 1e17) ? 0.0 : a.min_pf;
            std::ostringstream pt; pt << a.blocks_pos << "/" << a.blocks_traded;
            std::cout << std::left
                      << std::setw(9) << a.sym << std::setw(5) << a.tf
                      << std::setw(18) << a.family << std::setw(22) << a.params
                      << std::setw(8) << pt.str() << std::setw(8) << cons << std::setw(8) << minpf
                      << std::setw(7) << a.total_n << std::setw(10) << a.total_net << "\n";
        }
        std::cout << "\nRobust survivors: " << robust.size() << " of " << agg.size() << " cells.\n";
        std::cerr << "[out] " << out_path << "\n";
        return 0;
    }

    std::ofstream out(out_path);
    if (!out) { std::cerr << "ERR write " << out_path << "\n"; return 1; }
    out << "symbol,timeframe,family,params,"
           "n_train,wr_train,net_train,pf_train,sharpe_train,"
           "n_test,wr_test,net_test,pf_test,sharpe_test,maxdd_test,pass\n";
    out << std::fixed << std::setprecision(4);

    int n_pass = 0, n_cells = 0;
    std::vector<std::tuple<std::string,std::string,std::string,std::string,int,double,double,double>> passes;
        // sym,tf,family,params,n_test,pf_test,sharpe_test,net_test

    for (const auto& f : files) {
        BarFileInfo bf = parse_bar_filename(f);
        if (!bf.ok) { if (verbose) std::cerr << "[skip] " << f << " (no sym/tf)\n"; continue; }
        if (!only_tfs.empty() &&
            std::find(only_tfs.begin(), only_tfs.end(), bf.tf_label) == only_tfs.end()) continue;

        SymBaseline sb = baseline_for(bf.sym);
        if (sb.symbol.empty() || sb.symbol == "UNKNOWN") { sb = baseline_for("XAUUSD"); sb.symbol = bf.sym; }

        auto bars = load_bars(f, bf.tf_sec);
        if ((int)bars.size() < 2 * (min_n + 120)) {
            if (verbose) std::cerr << "[thin] " << bf.sym << " " << bf.tf_label
                                   << " bars=" << bars.size() << " -- skip\n";
            continue;
        }
        size_t mid = bars.size() / 2;
        std::vector<Bar> train(bars.begin(), bars.begin() + mid);
        std::vector<Bar> test (bars.begin() + mid, bars.end());

        auto cells_tr = sweep_grid(train, sb, bf.tf_label, max_hold_bars, 1);
        auto cells_te = sweep_grid(test,  sb, bf.tf_label, max_hold_bars, 1);

        std::map<std::string, Cell> te_by_key;
        for (auto& c : cells_te) te_by_key[c.family + "|" + c.params] = c;

        for (auto& ctr : cells_tr) {
            auto it = te_by_key.find(ctr.family + "|" + ctr.params);
            if (it == te_by_key.end()) continue;
            const Cell& cte = it->second;
            ++n_cells;
            bool pass = ctr.n_trades >= min_n && cte.n_trades >= min_n
                     && ctr.net > 0 && cte.net > 0
                     && cte.pf() >= pf_gate && cte.sharpe > 0;
            if (pass) ++n_pass;
            out << bf.sym << "," << bf.tf_label << "," << ctr.family << ",\"" << ctr.params << "\","
                << ctr.n_trades << "," << ctr.win_rate() << "," << ctr.net << "," << ctr.pf() << "," << ctr.sharpe << ","
                << cte.n_trades << "," << cte.win_rate() << "," << cte.net << "," << cte.pf() << "," << cte.sharpe << "," << cte.max_dd << ","
                << (pass ? 1 : 0) << "\n";
            if (pass)
                passes.emplace_back(bf.sym, bf.tf_label, ctr.family, ctr.params,
                                    cte.n_trades, cte.pf(), cte.sharpe, cte.net);
        }
    }
    out.close();

    std::sort(passes.begin(), passes.end(),
              [](auto& a, auto& b){ return std::get<6>(a) > std::get<6>(b); });  // by test sharpe
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n=== WALK-FORWARD SURVIVORS (positive in BOTH halves, n>=" << min_n
              << ", test PF>=" << pf_gate << ") ===\n";
    std::cout << std::left
              << std::setw(9) << "sym" << std::setw(5) << "tf"
              << std::setw(18) << "family" << std::setw(22) << "params"
              << std::setw(7) << "n_te" << std::setw(8) << "pf_te"
              << std::setw(9) << "shrp_te" << std::setw(10) << "net_te$" << "\n";
    for (auto& p : passes) {
        std::cout << std::left
                  << std::setw(9) << std::get<0>(p) << std::setw(5) << std::get<1>(p)
                  << std::setw(18) << std::get<2>(p) << std::setw(22) << std::get<3>(p)
                  << std::setw(7) << std::get<4>(p) << std::setw(8) << std::get<5>(p)
                  << std::setw(9) << std::get<6>(p) << std::setw(10) << std::get<7>(p) << "\n";
    }
    std::cout << "\nCells evaluated (train+test joined): " << n_cells
              << "   survivors: " << n_pass << "\n";
    std::cerr << "[out] " << out_path << "\n";
    std::cout << "Next: python3 tools/deflated_sharpe_gate.py " << out_path
              << "  (deflate survivors for trial count = " << n_cells << ")\n";
    return 0;
}
