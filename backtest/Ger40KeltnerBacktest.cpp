// =============================================================================
// Ger40KeltnerBacktest.cpp -- ENGINE-DRIVEN backtest of Ger40KeltnerH1Engine.
// #includes the REAL engine header and drives it bar-by-bar, so the result
// reflects the actual production cell logic (not a re-impl). Cross-spread,
// SL-first fills. Confirms the validated edge survives the real engine's
// warmup/exit/cost-gate path.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/Ger40KeltnerBacktest.cpp -o backtest/ger40_kelt_bt
// RUN:   ./backtest/ger40_kelt_bt /Users/jo/Tick/GER40_merged.h1.csv
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include "Ger40KeltnerH1Engine.hpp"

static const double SPREAD = 1.0;   // GER40 points; conservative H1 retail

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
    // walk-forward halves + 6 block
    int hn[3]={0,0,0}; double hg[3]={0,0,0},hgw[3]={0,0,0},hgl[3]={0,0,0};
    double blk_g[6]={0}; int blk_n[6]={0};
    int64_t split=0, tmin=0, tmax=1;
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
};

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"/Users/jo/Tick/GER40_merged.h1.csv";
    auto bars = load_csv(path);
    if((int)bars.size()<300){ std::fprintf(stderr,"not enough bars (%zu)\n",bars.size()); return 1; }
    std::printf("[GER40Kelt-BT] engine-driven  %zu H1 bars  px %.1f->%.1f  spread=%.1f\n",
                bars.size(), bars.front().c, bars.back().c, SPREAD);

    omega::Ger40KeltnerH1Engine eng;
    eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01; eng.max_spread=20.0;
    eng.init();

    Stat s; s.tmin=bars.front().ts; s.tmax=bars.back().ts; s.split=bars[bars.size()/2].ts;
    // GER40 lot $1/pt at 0.01? Use points->USD via *1 (relative). Report in points*lot.
    auto cb=[&](const omega::TradeRecord& tr){ s.rec(tr.pnl, tr.entryTs*1000); };

    const int N=(int)bars.size();
    for(int i=0;i<N;++i){
        const auto& b=bars[i]; const int64_t ts=b.ts*1000;
        if(i>0){
            eng.on_tick(b.l, b.l+SPREAD, ts, cb);
            eng.on_tick(b.h, b.h+SPREAD, ts, cb);
            eng.on_tick(b.c, b.c+SPREAD, ts, cb);
        }
        omega::Ger40KBar bar{}; bar.bar_start_ms=ts; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
        eng.on_h1_bar(bar, b.c, b.c+SPREAD, 0.0, ts, cb);
    }
    eng.force_close(bars.back().c, bars.back().c+SPREAD, bars.back().ts*1000, cb, "EOD_FLAT");

    std::printf("  RESULT  n=%d  WR=%.1f%%  PF=%.2f  TOTAL=%+.1f(pts*lot)  maxDD=%.1f | H1 PF=%.2f H2 PF=%.2f | blk+=%d/6  %s\n",
        s.n, s.wr(), s.pf(), s.pnl, s.mdd, s.hpf(1), s.hpf(2), s.blkpos(),
        (s.hg[1]>0&&s.hg[2]>0&&s.hn[1]>=15&&s.hn[2]>=15&&s.blkpos()>=5)?"*** ROBUST":
        ((s.hg[1]>0&&s.hg[2]>0)?"** WF+":""));
    std::printf("DONE\n");
    return 0;
}
