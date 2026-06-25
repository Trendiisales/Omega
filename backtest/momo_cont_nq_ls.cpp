// momo_cont_nq_ls.cpp -- faithful BT to decide whether to ADD a SHORT side to the
// validated NqFutMomo long-only momentum-continuation engine.
//
// LONG path = VERBATIM from momo_cont_nq.cpp (the validated edge, PF2.27@2pt). SHORT
// path = its exact mirror: ignition DOWN >= IG% over LB bars + red bar (c<o) + regime
// DOWN (close < SMA200 of own 5m closes), ATR-trail ABOVE the low-extreme + BE-ratchet
// (down) + ride-past-maxhold. Same exits, same cost. Reports per-year + WF halves +
// 2022-bear/2024-26-bull regime split so the verdict matches BACKTEST_TRUTH (both-WF-
// halves+ AND both-regime, else it's bull/bear-beta not an edge).
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/mcls backtest/momo_cont_nq_ls.cpp
// run:   SIDE=short /tmp/mcls <nas_tick.csv> [ig=0.4] [lb=6] [cost=2.0]
//        SIDE=long  /tmp/mcls ...   (reproduces the baseline)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

static int yr(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }
struct Bar{ int64_t ts; double o,h,l,c; };

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tick.csv> [ig] [lb] [cost]  (env SIDE=long|short)\n",argv[0]); return 2; }
    const char* path=argv[1];
    const double IG=argc>2?atof(argv[2]):0.4;
    const int    LB=argc>3?atoi(argv[3]):6;
    const double cost=argc>4?atof(argv[4]):2.0;
    const int    TF=300;
    const int ATR_LEN=30; const double ATR_MULT=4.0, BE_ARM=0.03, BE_FLOOR=0.02; const int MAXHOLD=48;
    const char* se=getenv("SIDE"); bool SHORT = se && std::string(se)=="short";
    std::ifstream f(path); if(!f){fprintf(stderr,"open fail\n");return 1;}

    std::vector<Bar> bars; int64_t bk=-1; Bar b{};
    std::string line; bool histdata=false; int probe=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue;
        double mid=0; int64_t s=0;
        if(probe<1){ histdata = (line.find(' ')!=std::string::npos && line.find('-')==std::string::npos); probe=1; }
        char* p=line.data();
        if(histdata){ long d=strtol(p,&p,10); while(*p==' ')++p;
            char* tp=p; long t=strtol(p,&p,10); int tdig=(int)(p-tp); if(*p==',')++p;
            double bid=strtod(p,&p); if(*p==',')++p; double ask=strtod(p,&p);
            if(bid<=0||ask<=0)continue;
            // CORRECT parse: time token is HHMMSSmmm (9 digits, milliseconds) in this feed --
            // the original momo_cont_nq.cpp read it as HHMMSS (6 digits) -> tm_hour=t/10000=18000
            // -> timegm rolled ~750 days + scrambled intraday -> garbage 5m bars (the parser bug
            // that tombstoned the old NqMomentumEngine). Branch on digit count to be format-safe.
            int hh,mm,ss;
            if(tdig>=9){ hh=t/10000000; mm=(t/100000)%100; ss=(t/1000)%100; }   // HHMMSSmmm
            else       { hh=t/10000;     mm=(t/100)%100;     ss=t%100; }          // HHMMSS
            struct tm g{}; g.tm_year=d/10000-1900; g.tm_mon=(d/100)%100-1; g.tm_mday=d%100;
            g.tm_hour=hh; g.tm_min=mm; g.tm_sec=ss; s=timegm(&g); mid=(bid+ask)/2;
        } else { long long ts=strtoll(p,&p,10); if(*p!=',')continue; ++p;
            double ask=strtod(p,&p); if(*p!=',')continue; ++p; double bid=strtod(p,&p);
            if(bid<=0||ask<=0)continue; s = ts>1000000000000LL?ts/1000:ts; mid=(bid+ask)/2; }
        int64_t k=s/TF;
        if(k!=bk){ if(bk>=0) bars.push_back(b); bk=k; b={s,mid,mid,mid,mid}; }
        else { if(mid>b.h)b.h=mid; if(mid<b.l)b.l=mid; b.c=mid; }
    }
    if(bk>=0)bars.push_back(b);
    fprintf(stderr,"# %zu 5m bars  SIDE=%s\n",bars.size(), SHORT?"short":"long");
    if(bars.size()<200){fprintf(stderr,"few bars\n");return 1;}

    std::vector<double> atr(bars.size(),0); { double s=0,pc=0; int n=0; double a=0;
        for(size_t i=0;i<bars.size();++i){ double tr=bars[i].h-bars[i].l; if(pc>0){double x=fabs(bars[i].h-pc),y=fabs(pc-bars[i].l); if(x>tr)tr=x; if(y>tr)tr=y;} pc=bars[i].c;
            if(n<14){s+=tr; if(++n==14)a=s/14;} else a=(a*13+tr)/14; atr[i]=a; } }

    struct Tr{ int64_t ts; double pnl; }; std::vector<Tr> trs;
    bool inpos=false; double entry=0,ext=0,sl_atr=0; int hold=0;   // ext = favorable extreme (peak long / trough short)
    for(size_t i=(size_t)LB;i+1<bars.size();++i){
        if(inpos){
            const Bar& nb=bars[i]; hold++;
            if(!SHORT){
                if(nb.h>ext)ext=nb.h;
                double tstop=ext-ATR_MULT*sl_atr;
                if(ext>=entry*(1+BE_ARM)){ double bf=entry*(1+BE_FLOOR); if(bf>tstop)tstop=bf; }
                if(nb.l<=tstop){ trs.push_back({nb.ts,(tstop-entry)-cost}); inpos=false; continue; }
                if(hold>=MAXHOLD && !(nb.c>entry)){ trs.push_back({nb.ts,(nb.c-entry)-cost}); inpos=false; continue; }
            } else {
                if(nb.l<ext)ext=nb.l;
                double tstop=ext+ATR_MULT*sl_atr;
                if(ext<=entry*(1-BE_ARM)){ double bf=entry*(1-BE_FLOOR); if(bf<tstop)tstop=bf; }
                if(nb.h>=tstop){ trs.push_back({nb.ts,(entry-tstop)-cost}); inpos=false; continue; }
                if(hold>=MAXHOLD && !(nb.c<entry)){ trs.push_back({nb.ts,(entry-nb.c)-cost}); inpos=false; continue; }
            }
            continue;
        }
        double base=bars[i-LB].c; if(base<=0||atr[i]<=0) continue;
        double up=(bars[i].c/base-1)*100;
        const int RG=200; bool regime_ok=true;
        if(i>=(size_t)RG){ double sma=0; for(int k=0;k<RG;k++)sma+=bars[i-k].c; sma/=RG;
            regime_ok = SHORT ? (bars[i].c<sma) : (bars[i].c>sma); }
        bool sig = SHORT ? (up<=-IG && bars[i].c<bars[i].o) : (up>=IG && bars[i].c>bars[i].o);
        if(sig && (getenv("NOGATE")||regime_ok)){
            inpos=true; entry=bars[i+1].o; ext=entry; sl_atr=atr[i]; hold=0;
        }
    }
    auto rep=[&](int y0,int y1,double&net,long&n,double&pf,double&wr){ double gw=0,gl=0;long w=0;net=0;n=0;
        for(auto&t:trs){int y=yr(t.ts); if(y<y0||y>y1)continue; net+=t.pnl;++n; if(t.pnl>0){gw+=t.pnl;++w;}else gl+=-t.pnl;}
        pf=gl>0?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0; };
    double net;long n;double pf,wr;
    printf("\n=== NQ momentum %s (IG=%.1f%% LB=%d ATR%dx%.1f BE arm%.0f/floor%.0f maxhold=%d cost=%.1fpt) ===\n",
        SHORT?"SHORT":"LONG",IG,LB,ATR_LEN,ATR_MULT,BE_ARM*100,BE_FLOOR*100,MAXHOLD,cost);
    int y0=yr(bars.front().ts), y1=yr(bars.back().ts);
    for(int y=y0;y<=y1;++y){ rep(y,y,net,n,pf,wr); if(n>0) printf("  %d  n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt\n",y,n,wr,pf,net); }
    // 2022 bear vs 2024-26 bull
    rep(2022,2022,net,n,pf,wr); double bearnet=net; long bearn=n; double bearpf=pf;
    rep(2024,2026,net,n,pf,wr); double bullnet=net; long bulln=n; double bullpf=pf;
    double h1=0,h2=0; for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?h1:h2)+=trs[i].pnl;
    rep(y0,y1,net,n,pf,wr);
    printf("  ALL n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt  WF-H1=%+.0f H2=%+.0f %s\n",n,wr,pf,net,h1,h2,(h1>0&&h2>0)?"BOTH+":"NOT both+");
    printf("  BEAR(2022) n=%ld PF=%.2f net=%+.0fpt | BULL(24-26) n=%ld PF=%.2f net=%+.0fpt  %s\n",
        bearn,bearpf,bearnet,bulln,bullpf,bullnet,(bearnet>0&&bullnet>0)?"BOTH-REGIME+":"NOT both-regime");
    return 0;
}
