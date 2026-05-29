// =====================================================================
// backtest/l2_discovery.cpp -- L2-native edge discovery sweep
// ---------------------------------------------------------------------
// Tick-level sweep over L2 microstructure families, on the 22-34d of
// L2 csvs logged on VPS (l2_ticks_<SYM>_<DATE>.csv).
//
// Families:
//   F1. MicroMomentum    -- mic_avg(N) crosses +thr -> long; -thr -> short
//   F2. MicroZScore       -- (mic - mean) / std crosses +/- z_thr
//   F3. ImbalanceFade     -- when imb > 0.75 -> short; imb < 0.25 -> long
//                            (only symbols with varying imb -- skip USTEC/NAS)
//   F4. MicroBreakout     -- mic > rolling_max(N) -> long; < rolling_min -> short
//
// Bracket: fixed-pt SL/TP (no ATR -- tick level), tick-count hold timeout.
// Cost: per-symbol from OmegaCostGuard production values.
//
// Walk-forward: --from-unix / --to-unix slice on tick timestamp.
//
// Build:
//   clang++ -std=c++17 -O3 backtest/l2_discovery.cpp -o backtest/l2_discovery
// =====================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct Tick {
    long long ts_ms = 0;
    double bid = 0, ask = 0, mid = 0;
    double l2_imb = 0.5;
    double micro  = 0;
};

static std::vector<Tick> load_l2_csv(const std::string& path) {
    std::vector<Tick> out;
    out.reserve(2'000'000);
    std::ifstream f(path); if (!f) return out;
    std::string line; std::getline(f, line);  // header
    while (std::getline(f, line)) {
        Tick t;
        const char* s = line.c_str();
        t.ts_ms = atoll(s);
        int col = 0;
        const char* p = s;
        while (*p) {
            if      (col == 1) t.mid    = atof(p);
            else if (col == 2) t.bid    = atof(p);
            else if (col == 3) t.ask    = atof(p);
            else if (col == 4) t.l2_imb = atof(p);
            else if (col == 15) { t.micro = atof(p); break; }
            const char* nx = strchr(p, ','); if (!nx) break;
            p = nx + 1; col++;
        }
        if (t.bid <= 0 || t.ask <= 0) continue;
        out.push_back(t);
    }
    return out;
}

// Per-symbol cost params + bracket
struct SymCfg {
    const char* name;
    double pt_size;          // price-unit per pt
    double tick_usd_per_lot; // $ per pt at lot=1.0
    double lot;
    double half_spread;      // fill model adjustment
    double cost_per_rt_usd;
    double sl_pts;           // bracket SL in pts
    double tp_pts;           // bracket TP in pts
    int    max_hold_ticks;
};

// SL/TP in pts sized so gross TP >> cost. Math: gross_usd = pp * tick_usd_per_lot * lot
// where pp = pts * pt_size. Want TP gross >= 3x cost.
// XAU: cost $0.66; pts*0.01*100*0.01 = pts*0.01. TP gross = tp_pts*0.01.
//      tp_pts=250 -> $2.50, SL=80 -> $0.80 (~3:1 R:R)
// SP : cost $2.00; pts*0.10*50*0.10 = pts*0.5. TP gross = 0.5*tp_pts.
//      tp_pts=18 -> $9, SL=6 -> $3 (3:1)
// NQ : cost $1.10; pts*0.10*20*0.10 = pts*0.2. TP gross = 0.2*tp_pts.
//      tp_pts=30 -> $6, SL=10 -> $2 (3:1)
static SymCfg cfg_for(const std::string& sym) {
    if (sym == "XAUUSD")          return { "XAUUSD", 0.01,  100.0, 0.01, 0.15,  0.66,  80, 250, 1200 };
    if (sym == "US500")           return { "US500",  0.10,   50.0, 0.10, 0.25,  2.00,   6,  18, 1200 };
    if (sym == "USTEC"||sym=="USTEC.F") return { "USTEC", 0.10,   20.0, 0.10, 0.50,  1.10,  10,  30, 1200 };
    if (sym == "NAS100")          return { "NAS100", 0.10,    1.0, 0.10, 0.50,  0.06,  10,  30, 1200 };
    return { sym.c_str(), 0.01, 100.0, 0.01, 0.15, 0.66, 80, 250, 1200 };
}

struct TradeRes { bool filled=false; bool tp=false, sl=false; double pnl=0; int holds=0; };

static TradeRes sim_bracket(const std::vector<Tick>& tks, size_t i, int side,
                            const SymCfg& c) {
    TradeRes r;
    if (i + 2 >= tks.size()) return r;
    double e = (side > 0) ? tks[i].ask : tks[i].bid;
    if (e <= 0) return r;
    double sl_px = (side > 0) ? e - c.sl_pts * c.pt_size : e + c.sl_pts * c.pt_size;
    double tp_px = (side > 0) ? e + c.tp_pts * c.pt_size : e - c.tp_pts * c.pt_size;
    r.filled = true;
    size_t end = std::min(tks.size(), i + 1 + (size_t)c.max_hold_ticks);
    double ex = e;
    for (size_t j = i + 1; j < end; ++j) {
        if (side > 0) {
            if (tks[j].bid <= sl_px) { ex = sl_px - c.half_spread; r.sl = true; r.holds = (int)(j - i); break; }
            if (tks[j].bid >= tp_px) { ex = tp_px - c.half_spread; r.tp = true; r.holds = (int)(j - i); break; }
            ex = tks[j].bid;
        } else {
            if (tks[j].ask >= sl_px) { ex = sl_px + c.half_spread; r.sl = true; r.holds = (int)(j - i); break; }
            if (tks[j].ask <= tp_px) { ex = tp_px + c.half_spread; r.tp = true; r.holds = (int)(j - i); break; }
            ex = tks[j].ask;
        }
    }
    if (!r.tp && !r.sl) r.holds = (int)(end - i - 1);
    double pp = (side > 0) ? (ex - e) : (e - ex);
    double gross = (pp / c.pt_size) * c.tick_usd_per_lot * c.pt_size * c.lot;
    // Simpler: gross = pp * tick_usd_per_lot * lot
    gross = pp * c.tick_usd_per_lot * c.lot;
    r.pnl = gross - c.cost_per_rt_usd;
    return r;
}

struct Cell {
    std::string family, params;
    int n = 0, wins = 0;
    double net = 0, sh = 0, dd = 0;
    std::vector<double> pnls;
};
static void finalize(Cell& c) {
    if (c.pnls.empty()) return;
    c.net = 0; for (auto p : c.pnls) c.net += p;
    double m = c.net / c.pnls.size();
    double v = 0; for (auto p : c.pnls) v += (p - m) * (p - m);
    v /= std::max<size_t>(1, c.pnls.size() - 1);
    double sd = std::sqrt(v);
    c.sh = (sd > 1e-30) ? (m / sd) * std::sqrt((double)c.pnls.size()) : 0;
    double eq = 0, peak = 0, dd = 0;
    for (auto p : c.pnls) { eq += p; peak = std::max(peak, eq); dd = std::max(dd, peak - eq); }
    c.dd = dd;
}

// F1 MicroMomentum: rolling-N micro_avg > thr -> long; < -thr -> short
static Cell f_micromom(const std::vector<Tick>& tks, const SymCfg& c,
                      int N, double thr, int cooldown_ticks) {
    Cell cell; cell.family = "MicroMomentum";
    std::ostringstream p; p << "N=" << N << ";thr=" << thr;
    cell.params = p.str();
    long long last_entry = -1000000;
    for (int i = N; i < (int)tks.size(); ++i) {
        if (i - last_entry < cooldown_ticks) continue;
        double sum = 0;
        for (int k = 0; k < N; ++k) sum += tks[i - 1 - k].micro;
        double avg = sum / N;
        int side = 0;
        if (avg > thr) side = +1;
        else if (avg < -thr) side = -1;
        if (!side) continue;
        auto tr = sim_bracket(tks, (size_t)i, side, c);
        if (!tr.filled) continue;
        cell.pnls.push_back(tr.pnl);
        ++cell.n; if (tr.pnl > 0) ++cell.wins;
        last_entry = i;
    }
    finalize(cell);
    return cell;
}

// F2 MicroZScore: rolling N-tick micro z-score; enter when |z| > z_thr
static Cell f_microz(const std::vector<Tick>& tks, const SymCfg& c,
                     int N, double z_thr, int cooldown_ticks, int fade_mode) {
    Cell cell; cell.family = (fade_mode > 0 ? "MicroZ_fade" : "MicroZ_follow");
    std::ostringstream p; p << "N=" << N << ";z=" << z_thr;
    cell.params = p.str();
    long long last_entry = -1000000;
    for (int i = N; i < (int)tks.size(); ++i) {
        if (i - last_entry < cooldown_ticks) continue;
        double s = 0;
        for (int k = 0; k < N; ++k) s += tks[i - 1 - k].micro;
        double m = s / N;
        double v = 0;
        for (int k = 0; k < N; ++k) { double d = tks[i - 1 - k].micro - m; v += d * d; }
        v /= N;
        double sd = std::sqrt(v);
        if (sd < 1e-9) continue;
        double z = (tks[i].micro - m) / sd;
        int side = 0;
        if (z > z_thr)  side = (fade_mode > 0) ? -1 : +1;
        else if (z < -z_thr) side = (fade_mode > 0) ? +1 : -1;
        if (!side) continue;
        auto tr = sim_bracket(tks, (size_t)i, side, c);
        if (!tr.filled) continue;
        cell.pnls.push_back(tr.pnl);
        ++cell.n; if (tr.pnl > 0) ++cell.wins;
        last_entry = i;
    }
    finalize(cell);
    return cell;
}

// F3 ImbalanceFade: when imb is varied (not 0.5), fade extremes
static Cell f_imbfade(const std::vector<Tick>& tks, const SymCfg& c,
                      double hi_thr, double lo_thr, int cooldown_ticks) {
    Cell cell; cell.family = "ImbalanceFade";
    std::ostringstream p; p << "hi=" << hi_thr << ";lo=" << lo_thr;
    cell.params = p.str();
    long long last_entry = -1000000;
    for (int i = 0; i < (int)tks.size(); ++i) {
        if (i - last_entry < cooldown_ticks) continue;
        double imb = tks[i].l2_imb;
        // skip when stuck at 0.5 (no info)
        if (std::fabs(imb - 0.5) < 0.001) continue;
        int side = 0;
        if (imb >= hi_thr) side = -1;       // bid-heavy -> mean revert -> sell? bid_heavy = bid_vol>ask_vol = buyer pressure -> fade = short
        else if (imb <= lo_thr) side = +1;
        if (!side) continue;
        auto tr = sim_bracket(tks, (size_t)i, side, c);
        if (!tr.filled) continue;
        cell.pnls.push_back(tr.pnl);
        ++cell.n; if (tr.pnl > 0) ++cell.wins;
        last_entry = i;
    }
    finalize(cell);
    return cell;
}

int main(int argc, char** argv) {
    std::vector<std::string> csvs;
    std::string sym_override, out_path;
    long long from_ts = 0, to_ts = 0;
    bool verbose = false;
    auto need = [&](int& i, const char* f)->const char*{
        if (i+1>=argc){std::cerr<<"ERR "<<f<<"\n";std::exit(2);}
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--csv")             csvs.push_back(need(i, "--csv"));
        else if (a == "--symbol-override") sym_override = need(i, "--symbol-override");
        else if (a == "--out")             out_path = need(i, "--out");
        else if (a == "--from-unix")       from_ts = std::atoll(need(i, "--from-unix"));
        else if (a == "--to-unix")         to_ts   = std::atoll(need(i, "--to-unix"));
        else if (a == "--verbose")         verbose = true;
        else if (a == "--help") {
            std::cout << "l2_discovery --csv <l2_ticks.csv> [--csv ...] --symbol-override SYM [--from-unix N --to-unix N] [--out csv]\n";
            return 0;
        }
    }
    if (csvs.empty() || sym_override.empty()) { std::cerr<<"ERR --csv + --symbol-override required\n"; return 2; }
    SymCfg c = cfg_for(sym_override);

    std::vector<Tick> tks;
    for (auto& p : csvs) {
        auto v = load_l2_csv(p);
        if (verbose) std::cerr<<"[load] "<<p<<" ticks="<<v.size()<<"\n";
        tks.insert(tks.end(), v.begin(), v.end());
    }
    std::sort(tks.begin(), tks.end(), [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    // Apply window filter
    if (from_ts > 0 || to_ts > 0) {
        std::vector<Tick> sl; sl.reserve(tks.size());
        for (auto& t : tks) {
            long long ts_s = t.ts_ms / 1000;
            if (from_ts > 0 && ts_s < from_ts) continue;
            if (to_ts > 0 && ts_s >= to_ts) continue;
            sl.push_back(t);
        }
        tks.swap(sl);
    }
    if (verbose) std::cerr<<"[total] "<<c.name<<" ticks="<<tks.size()<<"\n";
    if (tks.empty()) return 1;

    std::vector<Cell> cells;
    // MicroMomentum sweep
    for (int N : {10, 30, 100, 300})
      for (double thr : {0.05, 0.10, 0.20, 0.40})
        cells.push_back(f_micromom(tks, c, N, thr, 100));
    // MicroZ sweep -- both follow and fade
    for (int N : {30, 100, 300})
      for (double z : {1.5, 2.0, 3.0}) {
        cells.push_back(f_microz(tks, c, N, z, 100, 0)); // follow
        cells.push_back(f_microz(tks, c, N, z, 100, 1)); // fade
      }
    // ImbalanceFade sweep
    for (auto [hi, lo] : (std::vector<std::pair<double,double>>{
        {0.65, 0.35}, {0.70, 0.30}, {0.80, 0.20}
    })) cells.push_back(f_imbfade(tks, c, hi, lo, 100));

    std::ofstream out;
    if (!out_path.empty()) {
        out.open(out_path);
        out << "symbol,family,params,n_trades,n_wins,win_rate,net_pnl,sharpe,max_dd\n";
        out << std::fixed << std::setprecision(5);
    }
    std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b){ return a.sh > b.sh; });
    std::cout<<"\n=== TOP 25 BY SHARPE (n>=20) "<<c.name<<" ===\n";
    std::cout<<std::left<<std::setw(18)<<"family"<<std::setw(26)<<"params"
             <<std::right<<std::setw(8)<<"n"<<std::setw(9)<<"wr%"
             <<std::setw(12)<<"net$"<<std::setw(10)<<"sharpe"<<"\n";
    int shown = 0;
    for (auto& cc : cells) {
        if (out.is_open()) {
            double wr = cc.n > 0 ? (double)cc.wins / cc.n : 0;
            out << c.name << "," << cc.family << ",\"" << cc.params << "\","
                << cc.n << "," << cc.wins << "," << wr << ","
                << cc.net << "," << cc.sh << "," << cc.dd << "\n";
        }
        if (cc.n < 20) continue;
        std::cout<<std::left<<std::setw(18)<<cc.family<<std::setw(26)<<cc.params
                 <<std::right<<std::setw(8)<<cc.n
                 <<std::setw(9)<<std::fixed<<std::setprecision(2)
                 <<(cc.n>0?100.0*cc.wins/cc.n:0.0)
                 <<std::setw(12)<<std::setprecision(2)<<cc.net
                 <<std::setw(10)<<std::setprecision(3)<<cc.sh<<"\n";
        if (++shown >= 25) break;
    }
    return 0;
}
