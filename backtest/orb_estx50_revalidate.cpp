// =============================================================================
// orb_estx50_revalidate.cpp -- faithful real-class BT for g_orb_estx50_v2
// (OrbBreakoutEngine, ESTX50). Drives the REAL engine via on_tick() on
// EUSIDXEUR (=ESTX50) Dukascopy tick data. Prices are x1000 scaled in the
// raw files -> divided here. Column order: timestamp_ms,ask,bid,...
//
// Build: clang++ -O3 -std=c++17 -I include backtest/orb_estx50_revalidate.cpp \
//        -o backtest/orb_estx50_revalidate
// Run:   ./backtest/orb_estx50_revalidate <tick1.csv> [tick2.csv ...]
// =============================================================================
#include "OrbBreakoutEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

int main(int argc,char**argv){
    if(argc<2){ std::fprintf(stderr,"usage: %s tick.csv [...]\n",argv[0]); return 1; }

    omega::OrbBreakoutEngine e;
    e.symbol="ESTX50"; e.engine_name="OrbEstx50";
    e.or_start_min=420; e.or_end_min=480; e.flat_min=930;
    e.buf_atr=0.05; e.tp_r=2.0; e.long_only=true; e.lot=0.01;
    e.shadow_mode=true; e.enabled=true;

    std::vector<double> pnl; std::vector<int64_t> tts;
    auto cb=[&](const omega::TradeRecord& tr){ pnl.push_back(tr.pnl/e.lot); tts.push_back(tr.exitTs); };

    int64_t firstts=0,lastts=0; long long ticks=0;
    for(int a=1;a<argc;++a){
        std::ifstream f(argv[a]); if(!f){ std::fprintf(stderr,"no %s\n",argv[a]); continue; }
        std::string ln; std::getline(f,ln); // header
        while(std::getline(f,ln)){
            const char* s=ln.c_str(); char* ep;
            int64_t ts=std::strtoll(s,&ep,10); if(*ep!=',')continue; s=ep+1;
            double ask=std::strtod(s,&ep)/1000.0; if(*ep!=',')continue; s=ep+1;
            double bid=std::strtod(s,&ep)/1000.0;
            if(bid<=0||ask<=0||ask<bid) continue;
            if(!firstts)firstts=ts; lastts=ts; ++ticks;
            e.on_tick(bid, ask, ts, cb);
        }
    }
    // metrics
    auto report=[&](const char* nm, std::vector<double>& v, double cost){
        int64_t n=0,w=0; double g=0,ws=0,ls=0,eq=0,pk=0,mdd=0;
        for(double raw:v){ double x=raw-cost; ++n; g+=x; if(x>0){++w;ws+=x;}else ls+=-x;
            eq+=x; if(eq>pk)pk=eq; if(pk-eq>mdd)mdd=pk-eq; }
        double pf=ls>0?ws/ls:(ws>0?99:0);
        std::printf("%-12s cost=%.2f n=%-3lld WR=%5.1f%% net=%+8.1fpt PF=%5.2f mdd=%6.1f\n",
            nm,cost,(long long)n,n?100.0*w/n:0,g,pf,mdd);
    };
    std::vector<double> h1,h2; int64_t mid=(firstts+lastts)/2;
    for(size_t k=0;k<pnl.size();++k)(tts[k]*1000<mid?h1:h2).push_back(pnl[k]);
    std::printf("OrbEstx50 (real class) ticks=%lld range %lld..%lld  (ts in ms)\n",ticks,(long long)firstts,(long long)lastts);
    report("FULL@0",pnl,0.0);
    report("FULL@1pt",pnl,1.0);   // commission/slip stress (pts)
    report("FULL@2pt",pnl,2.0);
    report("H1@1pt",h1,1.0);
    report("H2@1pt",h2,1.0);
    std::vector<double> s=pnl; std::sort(s.rbegin(),s.rend());
    double tot=0; for(double x:pnl)tot+=x; double ex3=0; for(size_t i=3;i<s.size();++i)ex3+=s[i];
    std::printf("net_ex_top3=%+.1f (of %+.1f)\n",ex3,tot);
    return 0;
}
