// =============================================================================
// XauTrendFollowD1Backtest.cpp -- ENGINE-DRIVEN backtest of XauTrendFollowD1.
// #includes the REAL engine header and drives it via on_h4_bar() (the engine
// synthesizes D1 bars internally by UTC date) + on_tick() for intrabar SL/TP,
// so results reflect actual production cell logic -- including the S42
// Donchian5 no-TP runner cell added this session.
//
// FIDELITY: H4 OHLC CSV (ts,o,h,l,c). Cross-spread fills (long@ask in / @bid
// out via SPREAD). SL-first intrabar path (low -> high -> close fed as ticks).
// Entries fire only at D1 close; the engine's warmup_active_ is NOT used here
// (we drive live), so the first ~22 D1 bars self-warm the indicators.
//
// PnL: tr.pnl = points*lot (0.01). XAU 0.01 lot = $1/pt -> USD = tr.pnl*100.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/XauTrendFollowD1Backtest.cpp -o backtest/xau_tf_d1_bt
// RUN:   ./backtest/xau_tf_d1_bt /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <map>

#include "XauTrendFollowD1Engine.hpp"

static const double SPREAD = 0.20;   // XAU H4 typical; full spread crossed

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

struct Stat {
    int n=0,wins=0; double pnl=0,gw=0,gl=0,pk=0,eq=0,mdd=0;
    int64_t split=0,tmin=0,tmax=1;
    int hn[3]={0,0,0}; double hg[3]={0,0,0},hgw[3]={0,0,0},hgl[3]={0,0,0};
    double blk_g[6]={0}; int blk_n[6]={0};
    void rec(double usd,int64_t ts){
        ++n; pnl+=usd; if(usd>0){++wins;gw+=usd;} else gl+=std::fabs(usd);
        eq+=usd; if(eq>pk)pk=eq; if(pk-eq>mdd)mdd=pk-eq;
        int half=(ts<split)?1:2; for(int k:{0,half}){ hn[k]++; hg[k]+=usd; if(usd>0)hgw[k]+=usd; else hgl[k]+=std::fabs(usd);}
        int b=(int)(6.0*(ts-tmin)/(double)(tmax-tmin+1)); if(b<0)b=0; if(b>5)b=5; blk_g[b]+=usd; blk_n[b]++;
    }
    double pf()const{ return gl>0?gw/gl:(gw>0?99:0); }
    double hpf(int k)const{ return hgl[k]>0?hgw[k]/hgl[k]:(hgw[k]>0?99:0); }
    double wr()const{ return n?100.0*wins/n:0; }
    int blkpos()const{ int p=0; for(int i=0;i<6;i++) if(blk_g[i]>0)p++; return p; }
    const char* verdict()const{
        if(hg[1]>0&&hg[2]>0&&hn[1]>=8&&hn[2]>=8&&blkpos()>=5) return "*** ROBUST";
        if(hg[1]>0&&hg[2]>0) return "** WF+";
        return "";
    }
};

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";
    auto bars = load_csv(path);
    if((int)bars.size()<200){ std::fprintf(stderr,"not enough bars (%zu)\n",bars.size()); return 1; }
    std::printf("[XTFD1-BT] engine-driven  %zu H4 bars  px %.1f->%.1f  spread=%.2f\n\n",
                bars.size(), bars.front().c, bars.back().c, SPREAD);

    omega::XauTrendFollowD1Engine eng;
    eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01; eng.max_spread=1.0;
    eng.use_vol_band_gate=false;   // raw cell behaviour for attribution
    if (const char* imp = getenv("IMP")) eng.min_impulse_atr = atof(imp);  // 2026-06-22 impulse-filter sweep
    eng.init();

    // NOTE: rec() receives the trade entry timestamp in SECONDS (tr.entryTs),
    // so tmin/tmax/split are taken in seconds too (bars[].ts is seconds). An
    // earlier version passed entryTs*1000 (ms) against second-scale bounds,
    // which threw every trade into H2/one block -- WF stats were meaningless.
    Stat all; std::map<std::string,Stat> cells;
    all.tmin=bars.front().ts; all.tmax=bars.back().ts; all.split=bars[bars.size()/2].ts;
    auto cb=[&](const omega::TradeRecord& tr){
        const double usd=tr.pnl*100.0;
        all.rec(usd, tr.entryTs);
        auto& c=cells[tr.engine];
        c.tmin=all.tmin; c.tmax=all.tmax; c.split=all.split;
        c.rec(usd, tr.entryTs);
    };

    const int N=(int)bars.size();
    for(int i=0;i<N;++i){
        const auto& b=bars[i]; const int64_t ts=b.ts*1000;
        // intrabar mgmt on the H4 path (SL-first: low, high, close)
        if(i>0){
            eng.on_tick(b.l, b.l+SPREAD, ts, cb);
            eng.on_tick(b.h, b.h+SPREAD, ts, cb);
            eng.on_tick(b.c, b.c+SPREAD, ts, cb);
        }
        omega::XauTfD1Bar bar{}; bar.bar_start_ms=ts; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
        eng.on_h4_bar(bar, b.c, b.c+SPREAD, ts, cb);
    }
    eng.force_close(bars.back().c, bars.back().c+SPREAD, bars.back().ts*1000, cb, "EOD_FLAT");

    std::printf("-- PER-CELL (engine-driven) --\n");
    for(auto& kv : cells){
        const Stat& s=kv.second;
        std::printf("  %-40s n=%-3d WR=%4.1f%% PF=%5.2f TOTAL=$%+8.1f maxDD=$%.0f | H1 PF=%.2f H2 PF=%.2f blk+=%d/6 %s\n",
            kv.first.c_str(), s.n, s.wr(), s.pf(), s.pnl, s.mdd, s.hpf(1), s.hpf(2), s.blkpos(), s.verdict());
    }
    std::printf("-- ENSEMBLE --\n");
    std::printf("  %-40s n=%-3d WR=%4.1f%% PF=%5.2f TOTAL=$%+8.1f maxDD=$%.0f | H1 PF=%.2f H2 PF=%.2f blk+=%d/6 %s\n",
        "ALL_CELLS", all.n, all.wr(), all.pf(), all.pnl, all.mdd, all.hpf(1), all.hpf(2), all.blkpos(), all.verdict());
    std::printf("\nDONE\n");
    return 0;
}
