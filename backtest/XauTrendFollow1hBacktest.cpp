// =============================================================================
// XauTrendFollow1hBacktest.cpp -- ENGINE-DRIVEN backtest of XauTrendFollow1h.
//
// Unlike XauTrendFollowBacktest.cpp (which RE-IMPLEMENTS 2h/4h/D1 cells from a
// Dukascopy tick stream), this harness #includes the REAL production engine
// header and drives it bar-by-bar, so every result reflects the actual cell
// logic in include/XauTrendFollow1hEngine.hpp -- including the S40 ensemble
// cells (Pullback_EMA20_pb0.5, Keltner_EMA50_k2.0) added this session.
//
// FIDELITY (mirrors backtest/gold_regime_edges.cpp conventions):
//   - H1 OHLC CSV: ts,o,h,l,c  (ts in seconds).
//   - cross-spread fills: every (bid,ask) = (level, level+SPREAD); the engine
//     enters long@ask and exits long@bid, exactly as live.
//   - SL-first: each bar's intrabar path is fed low -> high -> close, so an
//     adverse stop is hit before a favourable extreme (conservative).
//   - entries fire in on_h1_bar() at bar close; they are only managed from the
//     NEXT bar onward (no same-bar look-ahead).
//   - the engine computes its own Wilder ATR14 (atr14_external=0).
//
// PnL: engine tr.pnl is (points * lot). XAU 0.01 lot = $1 / point, i.e.
//      USD = tr.pnl * 100.  All $ figures below are USD.
//
// BUILD:  c++ -std=c++17 -O2 -Iinclude backtest/XauTrendFollow1hBacktest.cpp \
//              -o backtest/xau_tf_1h_bt
// RUN:    ./backtest/xau_tf_1h_bt /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <map>

#include "XauTrendFollow1hEngine.hpp"

static const double SPREAD = 0.20;   // XAU H1 typical; full spread crossed on fills

struct BarCSV { int64_t ts; double o,h,l,c; };

static std::vector<BarCSV> load_csv(const char* path){
    std::vector<BarCSV> v; std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); return v; }
    std::string line; bool first=true;
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(first){ first=false; if(line[0]<'0'||line[0]>'9') continue; }
        BarCSV b{}; double ts;
        if(std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        b.ts=(int64_t)ts; v.push_back(b);
    }
    return v;
}

// One accumulator (per cell, per timeframe-aggregate, or global).
struct Stat {
    int n=0, wins=0; double pnl=0, gw=0, gl=0, pk=0, eq=0, mdd=0;
    void record(double usd){
        ++n; pnl+=usd; if(usd>0){++wins; gw+=usd;} else gl+=std::fabs(usd);
        eq+=usd; if(eq>pk) pk=eq; if(pk-eq>mdd) mdd=pk-eq;
    }
    double pf()const{ return gl>0? gw/gl : (gw>0?999.0:0.0); }
    double wr()const{ return n? 100.0*wins/n : 0.0; }
    double avgw()const{ return wins? gw/wins : 0.0; }
    double avgl()const{ int l=n-wins; return l? gl/l : 0.0; }
    double exp_()const{ return n? pnl/n : 0.0; }
};

struct RunCfg {
    const char* label;
    uint32_t cell_mask     = 0x0F;
    double   lot           = 0.01;
    bool     use_vol_target= false;
    double   vol_target_unit=0.10;
    int      pyramid_max_adds=0;
    double   pyramid_step_atr=1.0;
    double   pyramid_sl_atr =3.0;
};

// Drive the real engine over the bar series; return the global Stat and (opt)
// fill the per-cell map keyed by the engine's tr.engine string.
static Stat run_engine(const std::vector<BarCSV>& bars, const RunCfg& cfg,
                       std::map<std::string,Stat>* per_cell=nullptr){
    omega::XauTrendFollow1hEngine eng;
    eng.shadow_mode      = true;
    eng.enabled          = true;
    eng.cell_enable_mask = cfg.cell_mask;
    eng.lot              = cfg.lot;
    eng.max_spread       = 1.0;
    eng.use_vol_target   = cfg.use_vol_target;
    eng.vol_target_unit  = cfg.vol_target_unit;
    eng.pyramid_max_adds = cfg.pyramid_max_adds;
    eng.pyramid_step_atr = cfg.pyramid_step_atr;
    eng.pyramid_sl_atr   = cfg.pyramid_sl_atr;
    eng.init();

    Stat g;
    auto cb = [&](const omega::TradeRecord& tr){
        const double usd = tr.pnl * 100.0;   // points*lot -> USD (0.01 lot => $1/pt)
        g.record(usd);
        if(per_cell) (*per_cell)[tr.engine].record(usd);
    };

    const int N = (int)bars.size();
    for(int i=0;i<N;++i){
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts*1000;
        // 1) manage open positions through THIS bar's intrabar path (SL-first).
        if(i>0){
            eng.on_tick(b.l, b.l+SPREAD, ts_ms, cb);
            eng.on_tick(b.h, b.h+SPREAD, ts_ms, cb);
            eng.on_tick(b.c, b.c+SPREAD, ts_ms, cb);
        }
        // 2) bar close: indicators + donch-exit + pyramid + new entries.
        omega::XauTfBar1h bar{};
        bar.bar_start_ms=ts_ms; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
        eng.on_h1_bar(bar, b.c, b.c+SPREAD, 0.0, ts_ms, cb);
    }
    // flatten any runners at series end (force_close exits long@bid=last close).
    const auto& last = bars.back();
    eng.force_close(last.c, last.c+SPREAD, last.ts*1000, cb, "EOD_FLAT");
    return g;
}

static void print_stat(const char* label, const Stat& s){
    std::printf("  %-26s n=%-4d WR=%4.1f%%  avgWin=$%7.2f avgLoss=$%7.2f  exp=$%6.2f  PF=%5.2f  TOTAL=$%+9.1f  maxDD=$%.0f\n",
        label, s.n, s.wr(), s.avgw(), s.avgl(), s.exp_(), s.pf(), s.pnl, s.mdd);
}

int main(int argc, char** argv){
    const char* path = argc>1 ? argv[1] : "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv";
    auto bars = load_csv(path);
    if((int)bars.size() < 200){ std::fprintf(stderr,"not enough bars (%zu)\n",bars.size()); return 1; }
    std::printf("[XTF1h-BT] engine-driven  %zu H1 bars  px %.0f->%.0f  spread=%.2f\n\n",
                bars.size(), bars.front().c, bars.back().c, SPREAD);

    // ---- BASELINE: all 4 cells, fixed 0.01 lot, no vol-target, no pyramid ----
    std::printf("===== BASELINE  all 4 cells  lot=0.01  no vol-target  no pyramid =====\n");
    std::map<std::string,Stat> base_cells;
    RunCfg base{ "baseline_all4" };
    Stat g_base = run_engine(bars, base, &base_cells);
    std::printf("-- per-cell attribution --\n");
    for(auto& kv : base_cells) print_stat(kv.first.c_str(), kv.second);
    std::printf("-- ensemble --\n");
    print_stat("ALL_4_CELLS", g_base);

    // ---- PYRAMID sweep (all cells, fixed 0.01 lot, +1.0*ATR step) ----
    std::printf("\n===== PYRAMID sweep  all cells  lot=0.01  step=1.0*ATR  trail sl=3.0*ATR =====\n");
    for(int K : {0,1,2,3,4}){
        RunCfg c{ "pyr" }; c.pyramid_max_adds=K; c.pyramid_step_atr=1.0; c.pyramid_sl_atr=3.0;
        char nm[32]; std::snprintf(nm,sizeof nm,"pyramid_K%d_step1.0",K);
        print_stat(nm, run_engine(bars,c));
    }

    // ---- PYRAMID step sensitivity (K=3) ----
    std::printf("\n===== PYRAMID step sensitivity  K=3 =====\n");
    for(double st : {0.75,1.0,1.5,2.0}){
        RunCfg c{ "pyrstep" }; c.pyramid_max_adds=3; c.pyramid_step_atr=st; c.pyramid_sl_atr=3.0;
        char nm[32]; std::snprintf(nm,sizeof nm,"pyramid_K3_step%.2f",st);
        print_stat(nm, run_engine(bars,c));
    }

    // ---- LOT sizing (no pyramid) -- PF-invariance / linear risk dial ----
    std::printf("\n===== LOT sizing  no pyramid  (expect PF invariant, $ + maxDD linear) =====\n");
    for(double lot : {0.01,0.02,0.04,0.08}){
        RunCfg c{ "lot" }; c.lot=lot;
        char nm[32]; std::snprintf(nm,sizeof nm,"lot_%.2f",lot);
        print_stat(nm, run_engine(bars,c));
    }

    // ---- VOL-TARGET sizing on/off (normalise risk across vol regimes) ----
    std::printf("\n===== VOL-TARGET sizing  unit/ATR clamp[0.01,0.08]  no pyramid =====\n");
    for(double unit : {0.05,0.10,0.20}){
        RunCfg c{ "vt" }; c.use_vol_target=true; c.vol_target_unit=unit;
        char nm[40]; std::snprintf(nm,sizeof nm,"voltarget_unit%.2f",unit);
        print_stat(nm, run_engine(bars,c));
    }

    // ---- BEST COMBO: vol-target + pyramid K3 step0.75 ----
    std::printf("\n===== COMBO  vol-target(0.10) + pyramid K3 step0.75 =====\n");
    {
        RunCfg c{ "combo" }; c.use_vol_target=true; c.vol_target_unit=0.10;
        c.pyramid_max_adds=3; c.pyramid_step_atr=0.75; c.pyramid_sl_atr=3.0;
        print_stat("vt0.10_pyrK3_step0.75", run_engine(bars,c));
    }

    std::printf("\nDONE\n");
    return 0;
}
