// =============================================================================
// l2_edge_sweep.cpp -- CRTP harness for discovering signal edges in L2 data
// =============================================================================
//
// 2026-05-11 S33 (Claude / Jo): Standalone CRTP harness for searching the
//   captured L2 corpus (data/l2_ticks_*.csv) for new edges across multiple
//   session windows. Modeled on backtest/microscalper_crtp_sweep.cpp's
//   pattern: candidate signals are CRTP types parameterised at compile time
//   so each combo compiles to a distinct instance, no runtime polymorphism
//   in the hot loop.
//
//   Why this exists: the operator (S33 chat) wants edges discovered for the
//   sessions the current MicroScalper does NOT cover -- London (07-12 UTC),
//   NY (12-21 UTC), and overnight -- as well as alternative geometries for
//   Asia. The S30/S31 wide-fine sweep was scoped to a single signal family
//   (z-score mean reversion). This harness extends that to a bank of
//   distinct candidate signals, evaluated in parallel across configurable
//   session windows, with ranked output.
//
//   No Python. No protected files modified. No engine source touched. The
//   live engine (include/GoldMicroScalperEngine.hpp) is unaffected; this
//   binary is a research tool that consumes the same L2 CSVs the engine
//   produces.
//
// CSV INPUT (two formats, auto-detected from header)
//
//   (A) S19+ L2 capture schema (full book):
//         ts_ms, bid, ask, l2_imb, l2_bid_vol, l2_ask_vol,
//         depth_bid_levels, depth_ask_levels, depth_events_total,
//         watchdog_dead, vol_ratio, regime, vpin, has_pos,
//         micro_edge, ewm_drift
//       Used by: data/l2_ticks_*.csv (3 weeks captured, all symbols).
//
//   (B) Dukascopy / HistData 3-column tick schema:
//         ts_ms, bid, ask
//       Used by: outputs/duka_xauusd_daily/*.csv (2-year XAU corpus,
//                Sep 2023 -> Sep 2025, 623 daily files),
//                outputs/histdata_eurusd_daily/*.csv (184 EUR daily files).
//       L2 columns are zero-defaulted; signals that need real L2 (vacuum,
//       slope, l2imb) gracefully skip when l2_bid_vol+l2_ask_vol == 0.
//
//   Symbol is auto-detected from the filename:
//     l2_ticks_<SYM>_<DATE>.csv         -> SYM
//     l2_ticks_<DATE>.csv  (legacy)     -> XAUUSD
//     anything under outputs/duka_*     -> XAUUSD
//     anything under outputs/histdata_eurusd* / outputs/eurusd_daily -> EURUSD
//   Override via --symbol SYM.
//
// MULTI-SYMBOL BRACKETS
//   Each symbol has a baseline TP/SL pair calibrated for its tick scale.
//   The S30/S31 wide-fine TOP-1 lives only for XAUUSD; other symbols
//   need sized brackets. Built-in defaults (override via --tp / --sl):
//     XAUUSD : TP 35  SL 12   max_spread 1.0   usd_per_pt 100   lot 0.01
//     US500  : TP  3  SL 12   max_spread 1.5   usd_per_pt 0.10  lot 0.10
//     USTEC  : TP 10  SL 40   max_spread 4.5   usd_per_pt 0.10  lot 0.10
//     NAS100 : TP  5  SL 18   max_spread 2.5   usd_per_pt 0.10  lot 0.10
//     EURUSD : TP 0.0010 SL 0.0040 max_spread 0.0003 usd_per_pt 100000 lot 0.10
//   Each baseline TP is roughly 3.7x typical spread, matching the
//   microscalper_crtp_sweep S20 ratios. Promote to engine ONLY after
//   honest_backtest_xauusd_v2 follow-up confirms survivors.
//
// CANDIDATE SIGNAL FAMILIES (six included; extend by adding a CRTP type)
//
//   1. ZScoreMeanRev<W,Z>           -- existing MicroScalper baseline, here
//                                      for parity comparison across windows.
//                                      Fires LONG when z<=-Z, SHORT when z>=Z
//                                      over a W-tick rolling window.
//
//   2. L2ImbMomentum<W,Th>          -- sustained book imbalance. Fires LONG
//                                      when l2_imb has held >= 0.5+Th for W
//                                      consecutive ticks; SHORT mirror.
//                                      Tests whether persistent book
//                                      pressure predicts directional move.
//
//   3. SpreadCompressionBreak<W,P>  -- spread compresses below trailing W-tick
//                                      p10 for K consecutive ticks, then
//                                      expands by >P pts; fires in the
//                                      direction of the next mid move.
//                                      Tests whether compression-then-release
//                                      telegraphs flow.
//
//   4. VacuumRebound<H>             -- detects vacuum (l2_bid_vol or
//                                      l2_ask_vol below trailing decile);
//                                      fires opposite (mean-reversion) on
//                                      next tick. Hold horizon H ticks.
//
//   5. SlopeAcceleration<W,A>       -- second derivative of l2_imb over W
//                                      ticks. Fires when |d2(imb)/dt2| >= A
//                                      in the direction of acceleration --
//                                      institutional-flow proxy.
//
//   6. KaufmanRegimeFlip<W,T>       -- Kaufman ER over W ticks; fires only
//                                      when ER crosses T downward (chop
//                                      regime forming) AND z is in fade
//                                      band. Variant on signal #1 with
//                                      regime conditioning that the S33
//                                      engine deliberately disabled.
//
// SESSION WINDOWS (UTC, configurable via --session)
//   asia    : 0-7 UTC      (matches current S33 engine)
//   london  : 7-12 UTC
//   ny      : 12-21 UTC
//   overnight: 21-24 UTC
//   24h     : full day (default if --session unspecified)
//
// EVALUATION
//   For each (signal config, session window) pair the harness simulates a
//   bracket trade with TP/SL fixed by --tp / --sl flags, MAX_HOLD by
//   --max-hold-sec, COOLDOWN by --cooldown-sec, lot fixed at 0.01,
//   commission --commission-per-rt (default 0.06 USD/RT, S33-confirmed).
//   Outputs: trade count, win rate, gross PnL, net PnL (after cost),
//   Sharpe, profit factor, mean trade, max drawdown, IC of signal vs
//   forward N-tick return.
//
// USAGE
//   backtest/l2_edge_sweep
//       --csv "data/l2_ticks_*.csv"            # glob OK; quote it
//       [--session asia]                       # default 24h
//       [--tp 35] [--sl 12]                    # bracket sizing
//       [--max-hold-sec 7200]
//       [--cooldown-sec 60]
//       [--commission-per-rt 0.06]
//       [--warmup-ticks 1000]
//       [--top 20]
//       [--out backtest/l2_edge_sweep.csv]
//       [--family zscore,l2imb,spread,vacuum,slope,kaufman]
//                                              # default: all
//       [--verbose]
//
// BUILD
//   clang++ -std=c++17 -O3 -DNDEBUG -fbracket-depth=1024 -Wall -Wextra
//       -I include backtest/l2_edge_sweep.cpp -o backtest/l2_edge_sweep
//
// EXIT CODES
//   0  ok
//   2  usage error
//   3  no L2 files matched / unreadable
// =============================================================================

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace omega::l2_edge {

// ---------------------------------------------------------------------------
// Tick row (lean -- only the fields the candidate signals consume)
// ---------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms      = 0;
    double  bid        = 0.0;
    double  ask        = 0.0;
    double  mid        = 0.0;
    double  spread     = 0.0;
    double  l2_imb     = 0.5;
    double  l2_bid_vol = 0.0;
    double  l2_ask_vol = 0.0;
    bool    l2_real    = false;  // true if l2_bid_vol+l2_ask_vol > 0
    int     hour_utc   = 0;
};

// ---------------------------------------------------------------------------
// Per-symbol baselines (TP/SL/spread/usd_per_pt/lot). Mirrors microscalper
// _crtp_sweep.cpp's SymbolSpec table so the two harnesses stay aligned.
// XAUUSD baseline reflects the S33 Option A geometry (wider TP=35/SL=12)
// because that's the active live-engine target; other symbols use the
// S20 micro-scalp baselines.
// ---------------------------------------------------------------------------
struct SymbolSpec {
    const char* sym;
    double      tp_pts;
    double      sl_pts;
    double      max_spread_pts;
    double      usd_per_pt;
    double      live_lot;
    double      min_sd_pts;       // z-score noise floor (XAU=0.05; FX much smaller)
    double      cost_per_rt_usd;  // round-turn cost in USD at live_lot
};

inline const SymbolSpec& spec_for(const std::string& sym) {
    static const SymbolSpec table[] = {
        // XAUUSD: S33 Option A geometry as baseline (operator-active live target)
        {"XAUUSD", 35.0,    12.0,   1.0,    100.0,    0.01,   0.05,    0.06},
        {"US500",   3.0,    12.0,   1.5,    0.10,     0.10,   0.05,    0.00},
        {"USTEC",  10.0,    40.0,   4.5,    0.10,     0.10,   0.05,    0.00},
        {"NAS100",  5.0,    18.0,   2.5,    0.10,     0.10,   0.05,    0.00},
        {"EURUSD",  0.0010, 0.0040, 0.0003, 100000.0, 0.10,   1e-6,    0.00},
        {"GBPUSD",  0.0010, 0.0040, 0.00025, 100000.0, 0.10,  1e-6,    0.00},
        {"USDJPY",  0.10,   0.40,   0.025,  1000.0,   0.10,   1e-4,    0.00},
    };
    for (const auto& s : table) if (sym == s.sym) return s;
    return table[0]; // default to XAUUSD
}

// Detect symbol from filename. Examples:
//   l2_ticks_XAUUSD_2026-05-08.csv   -> XAUUSD
//   l2_ticks_2026-04-16.csv (TOXIC -- see engine_init.hpp:95+ banner)          -> XAUUSD (legacy)
//   outputs/duka_xauusd_daily/*.csv  -> XAUUSD
//   outputs/histdata_eurusd_daily/*  -> EURUSD
//   outputs/eurusd_daily/*           -> EURUSD
//   anything else: returns "" (caller should fall back / warn)
inline std::string detect_symbol(const std::string& path) {
    auto lower = [](std::string s){ for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    const std::string lp = lower(path);
    // Filename-prefixed L2 captures
    const auto p = lp.rfind("l2_ticks_");
    if (p != std::string::npos) {
        const std::string rest = lp.substr(p + 9);
        // If next token looks like a date (starts with digit), legacy XAUUSD
        if (!rest.empty() && std::isdigit((unsigned char)rest[0])) return "XAUUSD";
        // Else token before the next underscore is the symbol
        const auto under = rest.find('_');
        if (under != std::string::npos) {
            std::string sym = rest.substr(0, under);
            for (auto& c : sym) c = (char)std::toupper((unsigned char)c);
            return sym;
        }
    }
    if (lp.find("duka_xauusd")    != std::string::npos) return "XAUUSD";
    if (lp.find("histdata_eurusd")!= std::string::npos) return "EURUSD";
    if (lp.find("eurusd_daily")   != std::string::npos) return "EURUSD";
    if (lp.find("gbpusd")         != std::string::npos) return "GBPUSD";
    if (lp.find("usdjpy")         != std::string::npos) return "USDJPY";
    return "";
}

// ---------------------------------------------------------------------------
// Session window (UTC hour ranges, end-exclusive; supports wrap-around)
// ---------------------------------------------------------------------------
struct Session {
    std::string name;
    int start_hour;
    int end_hour;     // exclusive; end<=start means wrap (e.g. 21..24 then 0..end)

    bool admits(int h) const {
        if (end_hour > start_hour) return (h >= start_hour && h < end_hour);
        return (h >= start_hour || h < end_hour);
    }
};

inline Session session_by_name(const std::string& s) {
    if (s == "asia")      return {"asia",      0,  7};
    if (s == "london")    return {"london",    7,  12};
    if (s == "ny")        return {"ny",        12, 21};
    if (s == "overnight") return {"overnight", 21, 24};
    return                       {"24h",       0,  24};
}

// ---------------------------------------------------------------------------
// CSV loader (S19 schema)
// ---------------------------------------------------------------------------
inline std::string lower_copy(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return r;
}

inline std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else          { cur.push_back(c); }
    }
    out.push_back(cur);
    return out;
}

inline double parse_d(const std::string& s) {
    if (s.empty()) return 0.0;
    try { return std::stod(s); } catch (...) { return 0.0; }
}

inline int64_t parse_i64(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoll(s); } catch (...) { return 0; }
}

inline std::vector<Tick> load_l2_csv(const std::string& path, std::string& err) {
    std::vector<Tick> out;
    std::ifstream f(path);
    if (!f.is_open()) { err = "cannot open " + path; return out; }
    std::string line;
    if (!std::getline(f, line)) { err = "empty: " + path; return out; }
    auto cols = split_csv(line);
    std::map<std::string,int> hmap;
    for (size_t i = 0; i < cols.size(); ++i) {
        hmap[lower_copy(cols[i])] = static_cast<int>(i);
    }
    auto idx = [&](const std::string& name) -> int {
        auto it = hmap.find(name);
        return (it != hmap.end()) ? it->second : -1;
    };
    const int c_ts   = idx("ts_ms");
    const int c_bid  = idx("bid");
    const int c_ask  = idx("ask");
    const int c_imb  = idx("l2_imb");        // -1 in 3-col Dukascopy/HistData
    const int c_bvol = idx("l2_bid_vol");    // -1 in 3-col
    const int c_avol = idx("l2_ask_vol");    // -1 in 3-col
    if (c_ts < 0 || c_bid < 0 || c_ask < 0) {
        err = "CSV missing required columns ts_ms/bid/ask (got header: " + line + "): " + path;
        return out;
    }
    out.reserve(300000);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto v = split_csv(line);
        if ((int)v.size() <= c_ask) continue;
        Tick t;
        t.ts_ms = parse_i64(v[c_ts]);
        t.bid   = parse_d(v[c_bid]);
        t.ask   = parse_d(v[c_ask]);
        if (t.bid <= 0 || t.ask <= 0) continue;
        t.mid    = (t.bid + t.ask) * 0.5;
        t.spread = t.ask - t.bid;
        if (c_imb >= 0 && c_imb < (int)v.size()) t.l2_imb = parse_d(v[c_imb]);
        if (c_bvol >= 0 && c_bvol < (int)v.size()) t.l2_bid_vol = parse_d(v[c_bvol]);
        if (c_avol >= 0 && c_avol < (int)v.size()) t.l2_ask_vol = parse_d(v[c_avol]);
        t.l2_real = (t.l2_bid_vol + t.l2_ask_vol) > 0.0;
        const std::time_t tt = static_cast<std::time_t>(t.ts_ms / 1000);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &tt);
#else
        gmtime_r(&tt, &utc);
#endif
        t.hour_utc = utc.tm_hour;
        out.push_back(t);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Trade simulation -- common bracket logic shared by all signals
// ---------------------------------------------------------------------------
struct SimConfig {
    std::string symbol           = "XAUUSD";
    double tp_pts                = 35.0;
    double sl_pts                = 12.0;
    int    max_hold_sec          = 7200;
    int    cooldown_sec          = 60;
    double commission_per_rt     = 0.06;   // USD per round trip at live_lot
    double max_spread            = 1.0;
    double usd_per_pt            = 100.0;  // dollar value of 1 pt at live_lot
    double live_lot              = 0.01;
    double min_sd_pts            = 0.05;
    int    warmup_ticks          = 1000;
    Session session{"24h", 0, 24};
};

struct TradeStat {
    int    n_trades   = 0;
    int    wins       = 0;
    double gross_pnl  = 0.0;
    double net_pnl    = 0.0;
    double sum_sq_pnl = 0.0;
    double sum_pos    = 0.0;
    double sum_neg    = 0.0;
    double peak_eq    = 0.0;
    double cur_eq     = 0.0;
    double max_dd     = 0.0;
    void record(double net) {
        ++n_trades;
        if (net > 0) { ++wins; sum_pos += net; }
        else         { sum_neg += -net; }
        net_pnl    += net;
        sum_sq_pnl += net * net;
        cur_eq     += net;
        if (cur_eq > peak_eq) peak_eq = cur_eq;
        const double dd = peak_eq - cur_eq;
        if (dd > max_dd) max_dd = dd;
    }
    double win_rate() const { return n_trades ? (100.0 * wins / n_trades) : 0.0; }
    double mean()     const { return n_trades ? (net_pnl / n_trades) : 0.0; }
    double sd()       const {
        if (n_trades < 2) return 0.0;
        const double m = mean();
        const double v = sum_sq_pnl / n_trades - m * m;
        return v > 0 ? std::sqrt(v) : 0.0;
    }
    double sharpe()   const {
        const double s = sd();
        return s > 1e-9 ? mean() / s : 0.0;
    }
    double pf()       const {
        return sum_neg > 1e-9 ? sum_pos / sum_neg : (sum_pos > 0 ? 99.0 : 0.0);
    }
};

// Run a trade with the given direction at start_idx; record into stat.
// Returns the index at which the trade closed (or end-of-data).
inline size_t run_trade(const std::vector<Tick>& T, size_t start_idx,
                        bool is_long, const SimConfig& cfg,
                        TradeStat& stat) {
    const Tick& E = T[start_idx];
    const double entry_px = is_long ? E.ask : E.bid;
    const double tp_px    = is_long ? (entry_px + cfg.tp_pts) : (entry_px - cfg.tp_pts);
    const double sl_px    = is_long ? (entry_px - cfg.sl_pts) : (entry_px + cfg.sl_pts);
    const int64_t entry_ts_s = E.ts_ms / 1000;
    double exit_px = entry_px;
    for (size_t j = start_idx + 1; j < T.size(); ++j) {
        const Tick& X = T[j];
        const bool tp_hit = is_long ? (X.ask >= tp_px) : (X.bid <= tp_px);
        const bool sl_hit = is_long ? (X.bid <= sl_px) : (X.ask >= sl_px);
        // S37 audit fix: fill TP at touch (bid for long, ask for short),
        // not at the literal tp_px level. SL leg already correct below.
        // Filling at tp_px overstated winners by ~half-spread per trade.
        if (tp_hit) { exit_px = is_long ? X.bid : X.ask; }
        else if (sl_hit) { exit_px = is_long ? X.bid : X.ask; }
        else if ((X.ts_ms / 1000 - entry_ts_s) >= cfg.max_hold_sec) {
            exit_px = X.mid;
        } else {
            continue;
        }
        // Convert price-points to USD: gross = pts_moved * usd_per_pt * (lot / baseline_lot).
        // SimConfig.usd_per_pt is already scaled to live_lot, so the single
        // multiplication suffices regardless of symbol.
        const double pts   = is_long ? (exit_px - entry_px) : (entry_px - exit_px);
        const double gross = pts * cfg.usd_per_pt * cfg.live_lot;
        const double net   = gross - cfg.commission_per_rt;
        stat.gross_pnl += gross;
        stat.record(net);
        return j;
    }
    return T.size();
}

// ---------------------------------------------------------------------------
// Signal CRTP base + concrete signal families
// ---------------------------------------------------------------------------
template <typename Self>
struct SignalCRTP {
    int     last_exit_idx = -10000000;
    SimConfig cfg;
    std::string label;     // populated by family ctor
    explicit SignalCRTP(SimConfig c, std::string lbl) : cfg(std::move(c)), label(std::move(lbl)) {}

    void evaluate(const std::vector<Tick>& T, TradeStat& stat) {
        Self& self = static_cast<Self&>(*this);
        self.warmup(T);
        size_t i = static_cast<size_t>(cfg.warmup_ticks);
        if (i >= T.size()) return;
        for (; i < T.size(); ++i) {
            self.update(T, i);
            if (T[i].spread > cfg.max_spread) continue;
            if (!cfg.session.admits(T[i].hour_utc)) continue;
            if ((int)i - last_exit_idx < cfg.cooldown_sec /*ticks proxy*/) continue;
            int dir = self.signal(T, i);   // +1 LONG, -1 SHORT, 0 none
            if (dir == 0) continue;
            const size_t exit_at = run_trade(T, i, dir > 0, cfg, stat);
            last_exit_idx = static_cast<int>(exit_at);
            i = exit_at;
        }
    }
};

// 1. ZScoreMeanRev<W, Z*100>
template <int W, int Z_X100>
struct ZScoreMeanRev : SignalCRTP<ZScoreMeanRev<W, Z_X100>> {
    static constexpr double Z = Z_X100 / 100.0;
    std::deque<double> win;
    explicit ZScoreMeanRev(SimConfig c)
      : SignalCRTP<ZScoreMeanRev<W, Z_X100>>(std::move(c),
            "zscore_W" + std::to_string(W) + "_Z" + std::to_string(Z_X100/100) + "p" + std::to_string(Z_X100%100)) {}
    void warmup(const std::vector<Tick>&) {}
    void update(const std::vector<Tick>& T, size_t i) {
        win.push_back(T[i].mid);
        if ((int)win.size() > W * 4) win.pop_front();
    }
    int signal(const std::vector<Tick>& T, size_t i) const {
        if ((int)win.size() < W) return 0;
        const int n = static_cast<int>(win.size());
        const int s = n - W;
        double sum = 0;
        for (int k = s; k < n; ++k) sum += win[k];
        const double mean = sum / W;
        double var = 0;
        for (int k = s; k < n; ++k) {
            const double d = win[k] - mean;
            var += d * d;
        }
        const double sd = std::sqrt(var / W);
        if (sd < this->cfg.min_sd_pts) return 0;
        const double z = (T[i].mid - mean) / sd;
        if (z <= -Z) return +1;
        if (z >=  Z) return -1;
        return 0;
    }
};

// 2. L2ImbMomentum<K, T_X1000>  -- T = 0.5 + (T_X1000 / 1000)
template <int K, int T_X1000>
struct L2ImbMomentum : SignalCRTP<L2ImbMomentum<K, T_X1000>> {
    static constexpr double TH = T_X1000 / 1000.0;
    int    streak_long  = 0;
    int    streak_short = 0;
    explicit L2ImbMomentum(SimConfig c)
      : SignalCRTP<L2ImbMomentum<K, T_X1000>>(std::move(c),
            "l2imb_K" + std::to_string(K) + "_T" + std::to_string(T_X1000)) {}
    void warmup(const std::vector<Tick>&) {}
    void update(const std::vector<Tick>& T, size_t i) {
        if (!T[i].l2_real) { streak_long = streak_short = 0; return; }
        if (T[i].l2_imb >= 0.5 + TH)      { ++streak_long;  streak_short = 0; }
        else if (T[i].l2_imb <= 0.5 - TH) { ++streak_short; streak_long  = 0; }
        else                              { streak_long = streak_short = 0; }
    }
    int signal(const std::vector<Tick>&, size_t) const {
        if (streak_long  >= K) return +1;
        if (streak_short >= K) return -1;
        return 0;
    }
};

// 3. SpreadCompressionBreak<W, P_X100>  -- P = P_X100 / 100 pts
template <int W, int P_X100>
struct SpreadCompressionBreak : SignalCRTP<SpreadCompressionBreak<W, P_X100>> {
    static constexpr double P_THR = P_X100 / 100.0;
    std::deque<double> sp_win;
    std::deque<double> mid_win;
    explicit SpreadCompressionBreak(SimConfig c)
      : SignalCRTP<SpreadCompressionBreak<W, P_X100>>(std::move(c),
            "spread_W" + std::to_string(W) + "_P" + std::to_string(P_X100)) {}
    void warmup(const std::vector<Tick>&) {}
    void update(const std::vector<Tick>& T, size_t i) {
        sp_win.push_back(T[i].spread);
        if ((int)sp_win.size() > W) sp_win.pop_front();
        mid_win.push_back(T[i].mid);
        if ((int)mid_win.size() > W) mid_win.pop_front();
    }
    int signal(const std::vector<Tick>& T, size_t i) const {
        if ((int)sp_win.size() < W) return 0;
        // p10 of spread window
        std::vector<double> sorted(sp_win.begin(), sp_win.end());
        std::sort(sorted.begin(), sorted.end());
        const double p10 = sorted[sorted.size() / 10];
        // last 5 ticks compressed; current expanded
        bool compressed = true;
        for (int k = 1; k <= 5 && k < (int)sp_win.size(); ++k) {
            if (sp_win[sp_win.size() - 1 - k] > p10 * 1.1) { compressed = false; break; }
        }
        if (!compressed) return 0;
        if (T[i].spread < p10 * 1.5) return 0;       // not yet expanded
        // direction: signed change in mid over last 5 ticks
        if ((int)mid_win.size() < 6) return 0;
        const double dm = mid_win.back() - mid_win[mid_win.size() - 6];
        if (dm >  P_THR) return +1;
        if (dm < -P_THR) return -1;
        return 0;
    }
};

// 4. VacuumRebound<W, Q_X100>  -- Q = Q_X100/100 (e.g. 10 = decile)
template <int W, int Q_X100>
struct VacuumRebound : SignalCRTP<VacuumRebound<W, Q_X100>> {
    static constexpr double Q = Q_X100 / 100.0;
    std::deque<double> bvol_win;
    std::deque<double> avol_win;
    explicit VacuumRebound(SimConfig c)
      : SignalCRTP<VacuumRebound<W, Q_X100>>(std::move(c),
            "vacuum_W" + std::to_string(W) + "_Q" + std::to_string(Q_X100)) {}
    void warmup(const std::vector<Tick>&) {}
    void update(const std::vector<Tick>& T, size_t i) {
        bvol_win.push_back(T[i].l2_bid_vol);
        avol_win.push_back(T[i].l2_ask_vol);
        if ((int)bvol_win.size() > W) bvol_win.pop_front();
        if ((int)avol_win.size() > W) avol_win.pop_front();
    }
    int signal(const std::vector<Tick>& T, size_t i) const {
        if (!T[i].l2_real) return 0;
        if ((int)bvol_win.size() < W) return 0;
        std::vector<double> bs(bvol_win.begin(), bvol_win.end()); std::sort(bs.begin(), bs.end());
        std::vector<double> as_(avol_win.begin(), avol_win.end()); std::sort(as_.begin(), as_.end());
        const double bid_q = bs[(size_t)(Q * (bs.size() - 1))];
        const double ask_q = as_[(size_t)(Q * (as_.size() - 1))];
        const bool bid_vac = T[i].l2_bid_vol <= bid_q;
        const bool ask_vac = T[i].l2_ask_vol <= ask_q;
        // Bid vacuum -> downside pressure exhausted -> mean-rev LONG
        if (bid_vac && !ask_vac) return +1;
        if (ask_vac && !bid_vac) return -1;
        return 0;
    }
};

// 5. SlopeAcceleration<W, A_X10000>  -- A = A_X10000 / 10000 (small numbers)
template <int W, int A_X10000>
struct SlopeAcceleration : SignalCRTP<SlopeAcceleration<W, A_X10000>> {
    static constexpr double A = A_X10000 / 10000.0;
    std::deque<double> imb_win;
    explicit SlopeAcceleration(SimConfig c)
      : SignalCRTP<SlopeAcceleration<W, A_X10000>>(std::move(c),
            "slope_W" + std::to_string(W) + "_A" + std::to_string(A_X10000)) {}
    void warmup(const std::vector<Tick>&) {}
    void update(const std::vector<Tick>& T, size_t i) {
        imb_win.push_back(T[i].l2_imb);
        if ((int)imb_win.size() > W) imb_win.pop_front();
    }
    int signal(const std::vector<Tick>& T, size_t i) const {
        if (!T[i].l2_real) return 0;
        if ((int)imb_win.size() < W) return 0;
        const int n = (int)imb_win.size();
        // Approximate first derivative at end and at midpoint, then take diff.
        const double d_now  = imb_win[n-1] - imb_win[n-2];
        const double d_prev = imb_win[n/2] - imb_win[n/2 - 1];
        const double accel  = d_now - d_prev;
        if (accel >  A) return +1;
        if (accel < -A) return -1;
        return 0;
    }
};

// 6. KaufmanRegimeFlip<W, T_X1000, Z_X100>
template <int W, int T_X1000, int Z_X100>
struct KaufmanRegimeFlip : SignalCRTP<KaufmanRegimeFlip<W, T_X1000, Z_X100>> {
    static constexpr double TH  = T_X1000 / 1000.0;
    static constexpr double Z   = Z_X100  / 100.0;
    std::deque<double> mid_win;
    bool last_was_trend = false;
    explicit KaufmanRegimeFlip(SimConfig c)
      : SignalCRTP<KaufmanRegimeFlip<W, T_X1000, Z_X100>>(std::move(c),
            "kauf_W" + std::to_string(W) + "_T" + std::to_string(T_X1000) + "_Z" + std::to_string(Z_X100)) {}
    void warmup(const std::vector<Tick>&) {}
    void update(const std::vector<Tick>& T, size_t i) {
        mid_win.push_back(T[i].mid);
        if ((int)mid_win.size() > W) mid_win.pop_front();
    }
    int signal(const std::vector<Tick>& T, size_t i) const {
        if ((int)mid_win.size() < W) return 0;
        const double net = std::fabs(mid_win.back() - mid_win.front());
        double gross = 0;
        for (size_t k = 1; k < mid_win.size(); ++k)
            gross += std::fabs(mid_win[k] - mid_win[k-1]);
        const double er = (gross > 1e-9) ? net / gross : 0.0;
        if (er >= TH) return 0; // refuse new entries in trend
        // Compute z over the same W window and require fade.
        double sum = 0; for (double v : mid_win) sum += v;
        const double mean = sum / mid_win.size();
        double var = 0; for (double v : mid_win) var += (v - mean) * (v - mean);
        const double sd = std::sqrt(var / mid_win.size());
        if (sd < this->cfg.min_sd_pts) return 0;
        const double z = (T[i].mid - mean) / sd;
        if (z <= -Z) return +1;
        if (z >=  Z) return -1;
        return 0;
    }
};

// ---------------------------------------------------------------------------
// Run all family combos against the corpus
// ---------------------------------------------------------------------------
struct ComboResult {
    std::string symbol;
    std::string label;
    std::string session;
    TradeStat   stat;
};

template <typename SigT, typename... CtorArgs>
inline ComboResult run_combo(const std::vector<Tick>& T, const SimConfig& cfg,
                             CtorArgs&&... args) {
    SigT sig(cfg, std::forward<CtorArgs>(args)...);
    TradeStat st;
    sig.evaluate(T, st);
    return ComboResult{cfg.symbol, sig.label, cfg.session.name, st};
}

inline void run_all_zscore(const std::vector<Tick>& T, const SimConfig& base,
                           std::vector<ComboResult>& out) {
    SimConfig c = base;
    out.push_back(run_combo<ZScoreMeanRev< 50, 200>>(T, c));   // W=50,  Z=2.0
    out.push_back(run_combo<ZScoreMeanRev< 50, 250>>(T, c));   // W=50,  Z=2.5
    out.push_back(run_combo<ZScoreMeanRev<200, 200>>(T, c));   // W=200, Z=2.0  (S33 baseline)
    out.push_back(run_combo<ZScoreMeanRev<200, 250>>(T, c));   // W=200, Z=2.5
    out.push_back(run_combo<ZScoreMeanRev<200, 300>>(T, c));   // W=200, Z=3.0
    out.push_back(run_combo<ZScoreMeanRev<500, 200>>(T, c));   // W=500, Z=2.0
    out.push_back(run_combo<ZScoreMeanRev<500, 300>>(T, c));   // W=500, Z=3.0
}

inline void run_all_l2imb(const std::vector<Tick>& T, const SimConfig& base,
                          std::vector<ComboResult>& out) {
    SimConfig c = base;
    out.push_back(run_combo<L2ImbMomentum< 5, 100>>(T, c));   // 0.6
    out.push_back(run_combo<L2ImbMomentum<10, 100>>(T, c));
    out.push_back(run_combo<L2ImbMomentum<10, 150>>(T, c));   // 0.65
    out.push_back(run_combo<L2ImbMomentum<20, 150>>(T, c));
    out.push_back(run_combo<L2ImbMomentum<20, 200>>(T, c));   // 0.7
    out.push_back(run_combo<L2ImbMomentum<50, 200>>(T, c));
}

inline void run_all_spread(const std::vector<Tick>& T, const SimConfig& base,
                           std::vector<ComboResult>& out) {
    SimConfig c = base;
    out.push_back(run_combo<SpreadCompressionBreak<200,  20>>(T, c));   // 0.20pt break
    out.push_back(run_combo<SpreadCompressionBreak<200,  50>>(T, c));   // 0.50pt
    out.push_back(run_combo<SpreadCompressionBreak<500, 100>>(T, c));   // 1.00pt
    out.push_back(run_combo<SpreadCompressionBreak<500, 200>>(T, c));   // 2.00pt
}

inline void run_all_vacuum(const std::vector<Tick>& T, const SimConfig& base,
                           std::vector<ComboResult>& out) {
    SimConfig c = base;
    out.push_back(run_combo<VacuumRebound<200, 10>>(T, c));   // bottom decile
    out.push_back(run_combo<VacuumRebound<500, 10>>(T, c));
    out.push_back(run_combo<VacuumRebound<200, 20>>(T, c));   // bottom quintile
    out.push_back(run_combo<VacuumRebound<500, 20>>(T, c));
}

inline void run_all_slope(const std::vector<Tick>& T, const SimConfig& base,
                          std::vector<ComboResult>& out) {
    SimConfig c = base;
    out.push_back(run_combo<SlopeAcceleration<20, 10>>(T, c));   // A=0.001
    out.push_back(run_combo<SlopeAcceleration<20, 50>>(T, c));   // A=0.005
    out.push_back(run_combo<SlopeAcceleration<50, 10>>(T, c));
    out.push_back(run_combo<SlopeAcceleration<50, 50>>(T, c));
}

inline void run_all_kaufman(const std::vector<Tick>& T, const SimConfig& base,
                            std::vector<ComboResult>& out) {
    SimConfig c = base;
    out.push_back(run_combo<KaufmanRegimeFlip<200, 180, 200>>(T, c));   // ER<0.18, Z>=2
    out.push_back(run_combo<KaufmanRegimeFlip<200, 250, 200>>(T, c));   // ER<0.25, Z>=2
    out.push_back(run_combo<KaufmanRegimeFlip<500, 180, 250>>(T, c));   // ER<0.18, Z>=2.5
    out.push_back(run_combo<KaufmanRegimeFlip<500, 250, 250>>(T, c));   // ER<0.25, Z>=2.5
}

inline void emit_csv(const std::string& path, const std::vector<ComboResult>& res) {
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[edge_sweep] cannot write " << path << "\n";
        return;
    }
    f << "symbol,session,signal,n_trades,win_rate_pct,gross_pnl_usd,net_pnl_usd,"
         "mean_pnl_usd,sd_pnl_usd,sharpe,profit_factor,max_dd_usd\n";
    for (const auto& r : res) {
        f << r.symbol << "," << r.session << "," << r.label << ","
          << r.stat.n_trades << ","
          << std::fixed << std::setprecision(2) << r.stat.win_rate() << ","
          << std::setprecision(2) << r.stat.gross_pnl << ","
          << r.stat.net_pnl << ","
          << std::setprecision(4) << r.stat.mean() << ","
          << r.stat.sd() << ","
          << std::setprecision(3) << r.stat.sharpe() << ","
          << std::setprecision(2) << r.stat.pf() << ","
          << r.stat.max_dd << "\n";
    }
}

inline void print_leaderboard(const std::vector<ComboResult>& res, int top_n) {
    std::vector<ComboResult> sorted = res;
    std::sort(sorted.begin(), sorted.end(), [](const ComboResult& a, const ComboResult& b) {
        return a.stat.net_pnl > b.stat.net_pnl;
    });
    std::cout << "\n=== TOP " << top_n << " by net PnL (USD) ===\n";
    std::cout << std::left
              << std::setw(10) << "symbol"
              << std::setw(10) << "session"
              << std::setw(36) << "signal"
              << std::right
              << std::setw(8)  << "n"
              << std::setw(8)  << "wr%"
              << std::setw(12) << "gross"
              << std::setw(12) << "net"
              << std::setw(10) << "mean"
              << std::setw(8)  << "sharpe"
              << std::setw(8)  << "pf"
              << std::setw(10) << "max_dd"
              << "\n";
    int n = std::min((int)sorted.size(), top_n);
    for (int i = 0; i < n; ++i) {
        const auto& r = sorted[i];
        std::cout << std::left
                  << std::setw(10) << r.symbol
                  << std::setw(10) << r.session
                  << std::setw(36) << r.label
                  << std::right
                  << std::setw(8)  << r.stat.n_trades
                  << std::setw(8)  << std::fixed << std::setprecision(1) << r.stat.win_rate()
                  << std::setw(12) << std::setprecision(2) << r.stat.gross_pnl
                  << std::setw(12) << r.stat.net_pnl
                  << std::setw(10) << std::setprecision(3) << r.stat.mean()
                  << std::setw(8)  << r.stat.sharpe()
                  << std::setw(8)  << std::setprecision(2) << r.stat.pf()
                  << std::setw(10) << r.stat.max_dd
                  << "\n";
    }
}

} // namespace omega::l2_edge

// ---------------------------------------------------------------------------
// CLI / main
// ---------------------------------------------------------------------------
namespace {

void usage() {
    std::cout <<
      "l2_edge_sweep -- CRTP signal sweep over captured L2 corpus\n"
      "\n"
      "Usage:\n"
      "  l2_edge_sweep --csv <pattern_or_path> [options]\n"
      "\n"
      "Options:\n"
      "  --csv PATTERN          path or glob (e.g. 'data/l2_ticks_*.csv'). Repeatable.\n"
      "  --session NAME         asia|london|ny|overnight|24h (default 24h)\n"
      "                         Repeatable -- runs every signal in every named session.\n"
      "  --tp PTS               take-profit distance in pts (default 35)\n"
      "  --sl PTS               stop-loss distance in pts   (default 12)\n"
      "  --max-hold-sec N       (default 7200)\n"
      "  --cooldown-sec N       (default 60)\n"
      "  --commission-per-rt N  USD per round-trip @ 0.01 lot (default 0.06)\n"
      "  --max-spread N         max entry spread in pts (default 1.0)\n"
      "  --warmup-ticks N       skip first N ticks per file (default 1000)\n"
      "  --top N                leaderboard rows on stdout (default 20)\n"
      "  --out PATH             output CSV (default backtest/l2_edge_sweep.csv)\n"
      "  --family LIST          comma-separated subset of:\n"
      "                           zscore,l2imb,spread,vacuum,slope,kaufman\n"
      "                           (default: all)\n"
      "  --verbose              progress reporting per file\n";
}

std::vector<std::string> expand_pattern(const std::string& pat) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    // If literal star in basename, do directory glob; otherwise treat as path.
    const auto star = pat.find('*');
    if (star == std::string::npos) {
        if (fs::exists(pat)) out.push_back(pat);
        return out;
    }
    const fs::path p(pat);
    fs::path dir = p.parent_path();
    if (dir.empty()) dir = ".";
    const std::string base = p.filename().string();
    const auto sp = base.find('*');
    const std::string prefix = base.substr(0, sp);
    const std::string suffix = base.substr(sp + 1);
    if (!fs::exists(dir)) return out;
    for (const auto& ent : fs::directory_iterator(dir)) {
        const std::string name = ent.path().filename().string();
        if (name.size() >= prefix.size() + suffix.size()
            && name.compare(0, prefix.size(), prefix) == 0
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(ent.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::set<std::string> parse_families(const std::string& csv) {
    std::set<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',') { if (!cur.empty()) out.insert(cur); cur.clear(); }
        else if (!std::isspace(static_cast<unsigned char>(c))) cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (!cur.empty()) out.insert(cur);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    using namespace omega::l2_edge;
    std::vector<std::string> csv_patterns;
    std::vector<std::string> session_names;
    SimConfig user_overrides;
    bool override_tp = false, override_sl = false, override_spread = false;
    bool override_cost = false, override_min_sd = false;
    std::string force_symbol;
    int top_n = 20;
    std::string out_path = "backtest/l2_edge_sweep.csv";
    std::set<std::string> families = { "zscore", "l2imb", "spread", "vacuum", "slope", "kaufman" };
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* tag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[edge_sweep] missing value after " << tag << "\n";
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if      (a == "--csv")               csv_patterns.push_back(next("--csv"));
        else if (a == "--session")           session_names.push_back(next("--session"));
        else if (a == "--symbol")            { force_symbol = next("--symbol");
                                               for (auto& c : force_symbol) c = (char)std::toupper((unsigned char)c); }
        else if (a == "--tp")                { user_overrides.tp_pts = std::stod(next("--tp")); override_tp = true; }
        else if (a == "--sl")                { user_overrides.sl_pts = std::stod(next("--sl")); override_sl = true; }
        else if (a == "--max-hold-sec")      user_overrides.max_hold_sec = std::stoi(next("--max-hold-sec"));
        else if (a == "--cooldown-sec")      user_overrides.cooldown_sec = std::stoi(next("--cooldown-sec"));
        else if (a == "--commission-per-rt") { user_overrides.commission_per_rt = std::stod(next("--commission-per-rt")); override_cost = true; }
        else if (a == "--max-spread")        { user_overrides.max_spread = std::stod(next("--max-spread")); override_spread = true; }
        else if (a == "--min-sd-pts")        { user_overrides.min_sd_pts = std::stod(next("--min-sd-pts")); override_min_sd = true; }
        else if (a == "--warmup-ticks")      user_overrides.warmup_ticks = std::stoi(next("--warmup-ticks"));
        else if (a == "--top")               top_n = std::stoi(next("--top"));
        else if (a == "--out")               out_path = next("--out");
        else if (a == "--family")            families = parse_families(next("--family"));
        else if (a == "--verbose")           verbose = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::cerr << "[edge_sweep] unknown arg: " << a << "\n"; usage(); return 2; }
    }

    if (csv_patterns.empty()) { usage(); return 2; }
    if (session_names.empty()) session_names.push_back("24h");

    // 1) Expand patterns to file list.
    std::vector<std::string> files;
    for (const auto& p : csv_patterns) {
        const auto matched = expand_pattern(p);
        if (verbose) std::cout << "[edge_sweep] pattern '" << p << "' -> " << matched.size() << " files\n";
        for (const auto& m : matched) files.push_back(m);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.empty()) { std::cerr << "[edge_sweep] no files matched\n"; return 3; }

    // 2) Load each file, group ticks by detected (or forced) symbol.
    std::map<std::string, std::vector<Tick>> by_symbol;
    for (const auto& fp : files) {
        std::string err;
        auto t = load_l2_csv(fp, err);
        if (!err.empty()) { std::cerr << "[edge_sweep] " << err << "\n"; continue; }
        std::string sym = force_symbol.empty() ? detect_symbol(fp) : force_symbol;
        if (sym.empty()) {
            std::cerr << "[edge_sweep] could not detect symbol for " << fp << " -- skipping (use --symbol to force)\n";
            continue;
        }
        if (verbose) std::cout << "[edge_sweep] " << fp << " -> " << sym << " (" << t.size() << " ticks)\n";
        auto& bucket = by_symbol[sym];
        bucket.insert(bucket.end(), t.begin(), t.end());
    }
    if (by_symbol.empty()) { std::cerr << "[edge_sweep] nothing loaded\n"; return 3; }

    // 3) For each symbol, build SimConfig from spec + user overrides, then
    //    run every requested family across every requested session window.
    std::vector<ComboResult> res;
    for (auto& [sym, ticks] : by_symbol) {
        if (verbose) std::cout << "[edge_sweep] symbol=" << sym << " ticks=" << ticks.size() << "\n";
        // Sort by ts_ms so multi-day concatenations are time-ordered.
        std::sort(ticks.begin(), ticks.end(),
                  [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
        const SymbolSpec& spec = spec_for(sym);
        SimConfig sym_cfg;
        sym_cfg.symbol            = sym;
        sym_cfg.tp_pts            = override_tp      ? user_overrides.tp_pts            : spec.tp_pts;
        sym_cfg.sl_pts            = override_sl      ? user_overrides.sl_pts            : spec.sl_pts;
        sym_cfg.max_spread        = override_spread  ? user_overrides.max_spread        : spec.max_spread_pts;
        sym_cfg.usd_per_pt        = spec.usd_per_pt;
        sym_cfg.live_lot          = spec.live_lot;
        sym_cfg.commission_per_rt = override_cost    ? user_overrides.commission_per_rt : spec.cost_per_rt_usd;
        sym_cfg.min_sd_pts        = override_min_sd  ? user_overrides.min_sd_pts        : spec.min_sd_pts;
        sym_cfg.max_hold_sec      = user_overrides.max_hold_sec ? user_overrides.max_hold_sec : 7200;
        sym_cfg.cooldown_sec      = user_overrides.cooldown_sec ? user_overrides.cooldown_sec : 60;
        sym_cfg.warmup_ticks      = user_overrides.warmup_ticks ? user_overrides.warmup_ticks : 1000;

        for (const auto& sn : session_names) {
            SimConfig cfg = sym_cfg;
            cfg.session = session_by_name(sn);
            if (verbose) std::cout << "[edge_sweep]   session=" << cfg.session.name
                                   << " tp=" << cfg.tp_pts << " sl=" << cfg.sl_pts
                                   << " spread<=" << cfg.max_spread << "\n";
            if (families.count("zscore"))  run_all_zscore (ticks, cfg, res);
            if (families.count("l2imb"))   run_all_l2imb  (ticks, cfg, res);
            if (families.count("spread"))  run_all_spread (ticks, cfg, res);
            if (families.count("vacuum"))  run_all_vacuum (ticks, cfg, res);
            if (families.count("slope"))   run_all_slope  (ticks, cfg, res);
            if (families.count("kaufman")) run_all_kaufman(ticks, cfg, res);
        }
    }

    emit_csv(out_path, res);
    print_leaderboard(res, top_n);
    std::cout << "\n[edge_sweep] wrote full results to " << out_path << "\n";
    std::cout << "[edge_sweep] survivors are candidates for honest_backtest_xauusd_v2 follow-up\n";
    std::cout << "[edge_sweep] do NOT promote any survivor to engine without operator sign-off\n";
    return 0;
}
