// =============================================================================
// xau_sess_nypm_revalidate.cpp -- faithful real-class backtest for the
// SessionMomentumEngine instance g_xau_sess_nypm (XAU NY-PM session long).
// Drives the REAL engine class (#include SessionMomentumEngine.hpp) at the
// production config (entry_hour=16, hold=4, EMA200 trend filter, skip Friday,
// pure time exit) over 2yr XAU H1. Cost-honest via per-trade pts dump.
//
// Build: clang++ -O3 -std=c++17 -I include backtest/xau_sess_nypm_revalidate.cpp \
//        -o backtest/xau_sess_nypm_revalidate
// Run:   ./backtest/xau_sess_nypm_revalidate <h1.csv> [--half 0.15]
// =============================================================================
#include "SessionMomentumEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

struct B { int64_t ts; double o,h,l,c; };

static std::vector<B> load(const std::string& p){
    std::vector<B> v; std::ifstream f(p); if(!f) return v;
    std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){
        B b; const char* s=ln.c_str(); char* e;
        b.ts=std::strtoll(s,&e,10); if(*e!=',')continue; s=e+1;
        b.o=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.h=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.l=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.c=std::strtod(s,&e);
        v.push_back(b);
    }
    return v;
}

static void stats(const char* name, std::vector<double>& pnl, double cost){
    int64_t n=0,w=0; double gross=0,ws=0,ls=0,eq=0,peak=0,mdd=0;
    for(double raw: pnl){ double x=raw-cost; ++n; gross+=x;
        if(x>0){++w; ws+=x;} else ls+=-x;
        eq+=x; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf = ls>0? ws/ls : (ws>0?99:0);
    std::printf("%-14s cost=%.2f  n=%-3lld WR=%5.1f%%  net=%+8.1fpt  PF=%5.2f  mdd=%6.1fpt\n",
        name, cost, (long long)n, n?100.0*w/n:0, gross, pf, mdd);
}

int main(int argc,char**argv){
    if(argc<2){ std::fprintf(stderr,"usage: %s h1.csv [--half X]\n",argv[0]); return 1; }
    double half=0.15;
    for(int i=2;i<argc;i++){ std::string a=argv[i]; if(a=="--half"&&i+1<argc) half=std::atof(argv[++i]); }
    auto bars=load(argv[1]);
    if(bars.empty()){ std::fprintf(stderr,"no bars\n"); return 1; }

    omega::SessionMomentumEngine e;
    e.symbol="XAUUSD"; e.entry_hour=16; e.hold_hours=4;
    e.use_trend_filter=true; e.ema_period=200; e.sl_atr=0.0;
    e.skip_dow_mask=(1<<5); e.shadow_mode=true; e.enabled=true;
    e.lot=0.01; e.max_spread=2.0; e.init();

    std::vector<double> pnl, pnlH1, pnlH2; std::vector<int64_t> tts;
    int64_t t0=bars.front().ts, t1=bars.back().ts, mid=(t0+t1)/2;
    auto cb=[&](const omega::TradeRecord& tr){ pnl.push_back(tr.pnl/e.lot); tts.push_back(tr.exitTs); };

    const int WARM=250;
    for(size_t i=0;i<bars.size();++i){
        const auto&b=bars[i];
        omega::SessBar sb; sb.bar_start_ms=b.ts*1000; sb.open=b.o; sb.high=b.h; sb.low=b.l; sb.close=b.c;
        int64_t now_ms=b.ts*1000 + 3600LL*1000;
        if((int)i<WARM) e.on_h1_bar(sb, b.c-half, b.c+half, 0.0, now_ms, omega::SessionMomentumEngine::OnCloseFn{});
        else            e.on_h1_bar(sb, b.c-half, b.c+half, 0.0, now_ms, cb);
    }
    for(size_t k=0;k<pnl.size();++k){ (tts[k]<mid?pnlH1:pnlH2).push_back(pnl[k]); }

    std::printf("SessionMomentum g_xau_sess_nypm (real class) bars=%zu  range %lld..%lld\n",
                bars.size(),(long long)t0,(long long)t1);
    stats("FULL@0",  pnl,  0.0);
    stats("FULL@0.37",pnl, 0.37);
    stats("FULL@0.60",pnl, 0.60);
    stats("H1@0.37", pnlH1,0.37);
    stats("H2@0.37", pnlH2,0.37);
    // fat-tail
    std::vector<double> s=pnl; std::sort(s.rbegin(),s.rend());
    double tot=0; for(double x:pnl)tot+=x; double ex3=0; for(size_t i=3;i<s.size();++i)ex3+=s[i];
    std::printf("top3=%s  net_ex_top3=%+.1fpt (of %+.1f)\n",
        s.size()>=3?"":" (n<3)", ex3, tot);
    return 0;
}
