// BE-arm giveback sweep on XauTrendFollowD1 (engine-driven, faithful).
// Loops a grid of (BE_ARM_PCT, BE_BUFFER_PCT) -- fresh engine per combo --
// to test whether a host break-even ratchet protects giveback WITHOUT
// gutting the trend fat-tail edge. Baseline = (0,0) = shipped config.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude /tmp/cullwork/xau_tf_d1_bearm_sweep.cpp -o /tmp/cullwork/d1_bearm
// RUN:   /tmp/cullwork/d1_bearm <h4_csv>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include "XauTrendFollowD1Engine.hpp"

static double SPREAD = 0.20;
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
    int64_t split=0; double h1=0,h2=0; int h1n=0,h2n=0;
    double h1gw=0,h1gl=0,h2gw=0,h2gl=0;
    void rec(double usd,int64_t ts){
        ++n; pnl+=usd; if(usd>0){++wins;gw+=usd;} else gl+=std::fabs(usd);
        eq+=usd; if(eq>pk)pk=eq; if(pk-eq>mdd)mdd=pk-eq;
        if(ts<split){h1+=usd;h1n++; if(usd>0)h1gw+=usd; else h1gl+=std::fabs(usd);}
        else        {h2+=usd;h2n++; if(usd>0)h2gw+=usd; else h2gl+=std::fabs(usd);}
    }
    double pf()const{ return gl>0?gw/gl:(gw>0?99:0); }
    double wr()const{ return n?100.0*wins/n:0; }
    double h1pf()const{ return h1gl>0?h1gw/h1gl:(h1gw>0?99:0); }
    double h2pf()const{ return h2gl>0?h2gw/h2gl:(h2gw>0?99:0); }
};
static Stat run_combo(const std::vector<BarCSV>& bars,double arm,double buf){
    omega::XauTrendFollowD1Engine eng;
    eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01; eng.max_spread=1.0;
    eng.use_vol_band_gate=false;
    eng.BE_ARM_PCT=arm; eng.BE_BUFFER_PCT=buf;
    eng.init();
    Stat s; s.split=bars[bars.size()/2].ts;
    auto cb=[&](const omega::TradeRecord& tr){ s.rec(tr.pnl*100.0, tr.entryTs); };
    const int N=(int)bars.size();
    for(int i=0;i<N;++i){
        const auto& b=bars[i]; const int64_t ts=b.ts*1000;
        if(i>0){ eng.on_tick(b.l,b.l+SPREAD,ts,cb); eng.on_tick(b.h,b.h+SPREAD,ts,cb); eng.on_tick(b.c,b.c+SPREAD,ts,cb); }
        omega::XauTfD1Bar bar{}; bar.bar_start_ms=ts; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
        eng.on_h4_bar(bar,b.c,b.c+SPREAD,ts,cb);
    }
    eng.force_close(bars.back().c,bars.back().c+SPREAD,bars.back().ts*1000,cb,"EOD_FLAT");
    return s;
}
int main(int argc,char**argv){
    const char* path=argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";
    if(argc>2) SPREAD=std::atof(argv[2]);
    auto bars=load_csv(path);
    if((int)bars.size()<200){ std::fprintf(stderr,"not enough bars (%zu)\n",bars.size()); return 1; }
    std::printf("[D1 BE-ARM SWEEP] %s  %zu H4 bars  px %.1f->%.1f\n",
                path,bars.size(),bars.front().c,bars.back().c);
    std::printf("%-18s %4s %6s %6s %9s %8s  %6s %6s  %s\n",
                "config","n","WR%","PF","TOTAL$","maxDD$","H1pf","H2pf","wf");
    // grid: arm % of entry (D1 ~ swings of 2-8% common), buffer % retain
    double arms[]={0.0, 1.0, 2.0, 3.0, 5.0};
    double bufs[]={0.0, 0.5, 1.0};
    for(double arm:arms){
        for(double buf:bufs){
            if(arm==0.0 && buf!=0.0) continue; // baseline once
            Stat s=run_combo(bars,arm,buf);
            char cfg[32]; std::snprintf(cfg,sizeof(cfg),"arm%.1f/buf%.1f",arm,buf);
            const char* wf=(s.h1>0&&s.h2>0)?"WF+":"";
            std::printf("%-18s %4d %6.1f %6.2f %+9.1f %8.0f  %6.2f %6.2f  %s%s\n",
                cfg,s.n,s.wr(),s.pf(),s.pnl,s.mdd,s.h1pf(),s.h2pf(),
                wf, (arm==0.0?"  <= BASELINE":""));
        }
    }
    return 0;
}
