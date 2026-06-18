// rsi_reversal_faithful.cpp — FAITHFUL re-check of the REAL RSIReversalEngine.
// Cull-audit: g_rsi_reversal disabled S52b (2026-05-01) "negative-EV backtest".
// Drives the production class: per M1 bar -> set_bar_rsi(RSI14,close) +
// set_bar_context(ATR14, expanding, bb_squeeze=false, adx_trending=true); per tick ->
// update_indicators + on_tick with DOM BYPASSED (l2_real=false) + ewm_drift=0.
// BB/ADX fed permissive so only the real RSI-reversal signal + ATR/spread gates bind
// (DOM only ever blocks, so DOM-off = permissive upper bound on the core signal).
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/rsirev backtest/rsi_reversal_faithful.cpp
// run:   /tmp/rsirev <xau_tick.csv> [cost=0.37]   (tick: ts,ask,bid ASK-FIRST)
#include "RSIReversalEngine.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

static int yr(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }
static int utc_hour(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_hour; }

int main(int argc, char** argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tick.csv> [cost]\n",argv[0]); return 2; }
    const char* path=argv[1]; const double cost=argc>2?atof(argv[2]):0.37;
    std::ifstream f(path); if(!f){ fprintf(stderr,"open fail\n"); return 1; }

    omega::RSIReversalEngine eng; eng.enabled=true; eng.shadow_mode=true;
    const double LOT=0.05;
    struct Tr{ int64_t ts; double pnl; }; std::vector<Tr> trs;
    auto cb=[&](const omega::TradeRecord& tr){ trs.push_back({tr.exitTs, tr.pnl}); };

    // M1 OHLC + Wilder RSI14 + Wilder ATR14
    int64_t m1b=-1; double o=0,h=0,l=0,c=0; double prevC=0;
    double ag=0,al=0; int sn=0; double rsi=0; bool rsiR=false;
    double atr=0,atr_sum=0; int an=0; bool atrR=false; double prevClose_atr=0;
    const int P=14;
    auto onM1=[&](double H,double L,double C){
        if(prevC>0){ double ch=C-prevC,g=ch>0?ch:0,ll=ch<0?-ch:0;
            if(sn<P){ ag+=g; al+=ll; if(++sn==P){ag/=P;al/=P;rsiR=true;} }
            else { ag=(ag*(P-1)+g)/P; al=(al*(P-1)+ll)/P; }
            if(rsiR){ double rs=al>1e-9?ag/al:999; rsi=100-100/(1+rs); } }
        prevC=C;
        double tr_=H-L; if(prevClose_atr>0){ double a=H-prevClose_atr,b=prevClose_atr-L; if(a>tr_)tr_=a; if(b>tr_)tr_=b; }
        prevClose_atr=C;
        if(an<P){ atr_sum+=tr_; if(++an==P){atr=atr_sum/P;atrR=true;} } else atr=(atr*(P-1)+tr_)/P;
        if(rsiR&&atrR){ eng.set_bar_rsi(rsi,C); eng.set_bar_context(atr,true,false,true); }
    };

    std::string line; long nt=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue;
        char* p=line.data();
        long long ts=strtoll(p,&p,10); if(*p!=',')continue; ++p;
        double ask=strtod(p,&p); if(*p!=',')continue; ++p;
        double bid=strtod(p,&p); if(bid<=0||ask<=0) continue;
        int64_t s = ts>1000000000000LL?ts/1000:ts; int64_t ms=s*1000; double mid=(bid+ask)*0.5;
        int64_t bk=s/60;
        if(bk!=m1b){ if(m1b>=0) onM1(h,l,c); m1b=bk; o=h=l=c=mid; }
        else { if(mid>h)h=mid; if(mid<l)l=mid; c=mid; }
        eng.update_indicators(bid,ask);
        int hh=utc_hour(s); int slot=(hh>=7&&hh<21)?2:0;
        eng.on_tick(bid,ask,slot,ms, 0.5,false,false,false,false, false, cb, 0.0);
        ++nt;
    }
    fprintf(stderr,"# %ld ticks, %zu trades\n", nt, trs.size());

    auto stats=[&](int y0,int y1,double&net,long&n,double&pf,double&wr){
        double gw=0,gl=0; long w=0; net=0;n=0;
        for(auto&t:trs){int y=yr(t.ts); if(y<y0||y>y1)continue; double v=t.pnl-cost*LOT; net+=v;++n; if(v>0){gw+=v;++w;}else gl+=-v;}
        pf=gl>0?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0;
    };
    double net;long n;double pf,wr;
    printf("\n=== RSIReversal FAITHFUL (real engine, DOM-off, cost=%.2f) ===\n",cost);
    for(int y=2024;y<=2026;++y){ stats(y,y,net,n,pf,wr); printf("  %d   n=%4ld WR=%.0f%% PF=%.2f net=%+.1f\n",y,n,wr,pf,net); }
    double h1=0,h2=0; for(size_t i=0;i<trs.size();++i){double v=trs[i].pnl-cost*LOT;(i<trs.size()/2?h1:h2)+=v;}
    stats(2024,2026,net,n,pf,wr);
    printf("  ALL  n=%4ld WR=%.0f%% PF=%.2f net=%+.1f   WF-H1=%+.1f H2=%+.1f %s\n",n,wr,pf,net,h1,h2,(h1>0&&h2>0)?"BOTH+":"NOT both+");
    return 0;
}
