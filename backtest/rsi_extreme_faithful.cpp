// rsi_extreme_faithful.cpp — FAITHFUL re-check of the REAL RSIExtremeTurnEngine.
// Cull-audit: g_rsi_extreme was disabled S52b (2026-05-01) "negative-EV" with the
// engine_init comment SELF-FLAGGING "baseline contaminated, RE-TEST PENDING". The
// existing rsi_extreme_bt.cpp is an INLINE reimpl (not faithful). This drives the
// production class: per tick -> update_indicators + set_bar_rsi(M1 RSI14) + on_tick.
// M1 bars + Wilder RSI14 aggregated from the tick tape. Cross-regime XAU 2yr.
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/rsix backtest/rsi_extreme_faithful.cpp
// run:   /tmp/rsix <xau_tick.csv> [cost_rt_pts=0.37]   (tick: ts,ask,bid  ASK-FIRST)
#include "RSIExtremeTurnEngine.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

static int yr(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }
static int utc_hour(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_hour; }

int main(int argc, char** argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tick.csv> [cost]\n",argv[0]); return 2; }
    const char* path=argv[1]; const double cost=argc>2?atof(argv[2]):0.37;
    std::ifstream f(path); if(!f){ fprintf(stderr,"open %s fail\n",path); return 1; }

    omega::RSIExtremeTurnEngine eng;  // shipped defaults (RSI 20/70, sustained3, SL0.8ATR...)
    eng.enabled=true; eng.shadow_mode=true;
    const double LOT=0.01;

    struct Tr{ int64_t ts; double pnl; }; std::vector<Tr> trs;
    auto cb=[&](const omega::TradeRecord& tr){ trs.push_back({tr.exitTs, tr.pnl}); };

    // M1 bar aggregation + Wilder RSI14 on M1 closes
    int64_t m1_bucket=-1; double m1_close=0;
    double rsi=0; bool rsi_ready=false;
    double avg_gain=0, avg_loss=0; int seed_n=0; double prev_m1_close=0;
    const int RP=14;
    auto push_m1=[&](double c){
        if(prev_m1_close>0){
            double ch=c-prev_m1_close, g=ch>0?ch:0, l=ch<0?-ch:0;
            if(seed_n<RP){ avg_gain+=g; avg_loss+=l; ++seed_n;
                if(seed_n==RP){ avg_gain/=RP; avg_loss/=RP; rsi_ready=true; } }
            else { avg_gain=(avg_gain*(RP-1)+g)/RP; avg_loss=(avg_loss*(RP-1)+l)/RP; }
            if(rsi_ready){ double rs= avg_loss>1e-9? avg_gain/avg_loss : 999; rsi=100-100/(1+rs); }
        }
        prev_m1_close=c;
    };

    std::string line; long nt=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue;
        char* p=line.data();
        long long ts=strtoll(p,&p,10); if(*p!=',')continue; ++p;
        double ask=strtod(p,&p); if(*p!=',')continue; ++p;
        double bid=strtod(p,&p);
        if(bid<=0||ask<=0) continue;
        int64_t s = ts>1000000000000LL ? ts/1000 : ts;
        int64_t ms = s*1000;
        double mid=(bid+ask)*0.5;
        // M1 bar close detection
        int64_t bk=s/60;
        if(bk!=m1_bucket){ if(m1_bucket>=0) push_m1(m1_close); m1_bucket=bk; }
        m1_close=mid;
        // drive engine
        eng.update_indicators(bid,ask);
        if(rsi_ready) eng.set_bar_rsi(rsi);
        int h=utc_hour(s); int slot=(h>=7&&h<21)?2:0;  // London/NY active else dead
        eng.on_tick(bid,ask,slot,ms,cb);
        ++nt;
    }
    fprintf(stderr,"# %ld ticks, %zu trades\n", nt, trs.size());

    auto stats=[&](int y0,int y1,double&net,long&n,double&pf,double&wr){
        double gw=0,gl=0; long w=0; net=0; n=0;
        for(auto&t:trs){ int y=yr(t.ts); if(y<y0||y>y1)continue;
            double v=t.pnl-cost*LOT; net+=v; ++n; if(v>0){gw+=v;++w;}else gl+=-v; }
        pf=gl>0?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0;
    };
    double net; long n; double pf,wr;
    printf("\n=== RSIExtremeTurn FAITHFUL (real engine, shipped cfg, cost=%.2f) ===\n",cost);
    for(int y=2024;y<=2026;++y){ stats(y,y,net,n,pf,wr); printf("  %d   n=%4ld WR=%.0f%% PF=%.2f net=%+.1f\n",y,n,wr,pf,net); }
    double h1=0,h2=0; for(size_t i=0;i<trs.size();++i){ double v=trs[i].pnl-cost*LOT; (i<trs.size()/2?h1:h2)+=v; }
    stats(2024,2026,net,n,pf,wr);
    printf("  ALL  n=%4ld WR=%.0f%% PF=%.2f net=%+.1f   WF-H1=%+.1f H2=%+.1f %s\n",
           n,wr,pf,net,h1,h2,(h1>0&&h2>0)?"BOTH+":"NOT both+");
    return 0;
}
