// opening_drive_nq.cpp — DISCOVERY: NAS100 opening-drive GAP-AND-GO (no-retrace).
// Distinct from g_nas_orb_retrace (retrace+ride) + tombstoned PeachyOrb (retest):
// here we enter on the FIRST 5m close beyond the opening range — NO wait for a
// retest/retrace — to catch no-look-back drive days the retrace engines miss.
// Ride exit = validated chassis (ATR-trail 30x4 + BE-ratchet + ride-in-profit).
// Long+short (open can drive either way). Optional bull-regime gate.
//
// US cash open = 13:30 UTC. OR = first OR_BARS 5m bars after 13:30. After the OR
// window, first 5m bar that CLOSES beyond OR_high/OR_low -> enter next bar open.
// One entry per session. Forced flat at session end (20:00 UTC) as backstop.
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/odq backtest/opening_drive_nq.cpp
// run:   /tmp/odq <nas_tick.csv> [or_bars=3] [cost=2.0]   (NOGATE=1 disables regime gate; LONGONLY=1)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

static int yr(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }
struct Bar{ int64_t ts; double o,h,l,c; int hh,mm,yday; };

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tick.csv> [or_bars] [cost]\n",argv[0]); return 2; }
    const char* path=argv[1];
    const int    OR_BARS = argc>2?atoi(argv[2]):3;   // 3 x 5m = 15min opening range
    const double cost    = argc>3?atof(argv[3]):2.0;
    const int    TF=300;
    const int    OPEN_HH=13, OPEN_MM=30, FLAT_HH=20;        // US cash session
    const int ATR_LEN=30; const double ATR_MULT=4.0, BE_ARM=0.03, BE_FLOOR=0.02; const int MAXHOLD=48;
    const int RG=200; const bool gate=(getenv("NOGATE")==nullptr); const bool longonly=(getenv("LONGONLY")!=nullptr);

    std::ifstream f(path); if(!f){fprintf(stderr,"open fail\n");return 1;}
    std::vector<Bar> bars; int64_t bk=-1; Bar b{}; std::string line; bool histdata=false; int probe=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue; double mid=0; int64_t s=0;
        if(probe<1){ histdata=(line.find(' ')!=std::string::npos && line.find('-')==std::string::npos); probe=1; }
        char* p=line.data();
        if(histdata){ long d=strtol(p,&p,10); while(*p==' ')++p; long t=strtol(p,&p,10); if(*p==',')++p;
            double bid=strtod(p,&p); if(*p==',')++p; double ask=strtod(p,&p); if(bid<=0||ask<=0)continue;
            struct tm g{}; g.tm_year=d/10000-1900; g.tm_mon=(d/100)%100-1; g.tm_mday=d%100;
            g.tm_hour=t/10000; g.tm_min=(t/100)%100; g.tm_sec=t%100; s=timegm(&g); mid=(bid+ask)/2;
        } else { long long ts=strtoll(p,&p,10); if(*p!=',')continue; ++p; double ask=strtod(p,&p); if(*p!=',')continue; ++p; double bid=strtod(p,&p);
            if(bid<=0||ask<=0)continue; s=ts>1000000000000LL?ts/1000:ts; mid=(bid+ask)/2; }
        int64_t k=s/TF;
        if(k!=bk){ if(bk>=0)bars.push_back(b); bk=k; time_t tt=s; struct tm g; gmtime_r(&tt,&g);
            b={s,mid,mid,mid,mid,g.tm_hour,g.tm_min,g.tm_yday}; }
        else { if(mid>b.h)b.h=mid; if(mid<b.l)b.l=mid; b.c=mid; }
    }
    if(bk>=0)bars.push_back(b);
    fprintf(stderr,"# %zu 5m bars\n",bars.size());
    if(bars.size()<300){fprintf(stderr,"few bars\n");return 1;}

    std::vector<double> atr(bars.size(),0); { double s=0,pc=0; int n=0; double a=0;
        for(size_t i=0;i<bars.size();++i){ double tr=bars[i].h-bars[i].l; if(pc>0){double x=fabs(bars[i].h-pc),y=fabs(pc-bars[i].l); if(x>tr)tr=x; if(y>tr)tr=y;} pc=bars[i].c;
            if(n<ATR_LEN){s+=tr; if(++n==ATR_LEN)a=s/ATR_LEN;} else a=(a*(ATR_LEN-1)+tr)/ATR_LEN; atr[i]=a; } }

    struct Tr{ int64_t ts; double pnl; }; std::vector<Tr> trs;
    bool inpos=false; int dir=0; double entry=0,peak=0,trough=0,sl_atr=0; int hold=0;
    // per-session OR state
    int cur_yday=-1; double or_hi=0,or_lo=0; int or_count=0; bool or_ready=false; bool traded_today=false;

    for(size_t i=ATR_LEN;i+1<bars.size();++i){
        const Bar& bar=bars[i];
        // session reset at the open bar
        bool at_open = (bar.hh==OPEN_HH && bar.mm>=OPEN_MM && bar.mm<OPEN_MM+5);
        if(bar.yday!=cur_yday && at_open){ cur_yday=bar.yday; or_hi=bar.h; or_lo=bar.l; or_count=1; or_ready=false; traded_today=false; }
        else if(bar.yday==cur_yday && !or_ready && or_count>0){
            if(or_count<OR_BARS){ if(bar.h>or_hi)or_hi=bar.h; if(bar.l<or_lo)or_lo=bar.l; or_count++; if(or_count>=OR_BARS)or_ready=true; }
        }
        // manage open position
        if(inpos){
            if(bar.h>peak)peak=bar.h; if(bar.l<trough)trough=bar.l; hold++;
            double tstop,bf;
            if(dir>0){ tstop=peak-ATR_MULT*sl_atr; if(peak>=entry*(1+BE_ARM)){bf=entry*(1+BE_FLOOR); if(bf>tstop)tstop=bf;}
                if(bar.l<=tstop){ trs.push_back({bar.ts,(tstop-entry)-cost}); inpos=false; }
                else if(hold>=MAXHOLD && !(bar.c>entry)){ trs.push_back({bar.ts,(bar.c-entry)-cost}); inpos=false; }
                else if(bar.hh>=FLAT_HH){ trs.push_back({bar.ts,(bar.c-entry)-cost}); inpos=false; } }
            else { tstop=trough+ATR_MULT*sl_atr; if(trough<=entry*(1-BE_ARM)){bf=entry*(1-BE_FLOOR); if(bf<tstop)tstop=bf;}
                if(bar.h>=tstop){ trs.push_back({bar.ts,(entry-tstop)-cost}); inpos=false; }
                else if(hold>=MAXHOLD && !(bar.c<entry)){ trs.push_back({bar.ts,(entry-bar.c)-cost}); inpos=false; }
                else if(bar.hh>=FLAT_HH){ trs.push_back({bar.ts,(entry-bar.c)-cost}); inpos=false; } }
            if(inpos) continue;
        }
        // entry: first close beyond OR, no retrace required, one per session
        if(!inpos && or_ready && !traded_today && bar.hh<FLAT_HH){
            bool regime_ok=true; if(gate&&i>=(size_t)RG){ double sma=0; for(int k=0;k<RG;k++)sma+=bars[i-k].c; sma/=RG; regime_ok=(bar.c>sma); }
            int sig=0;
            if(bar.c>or_hi && atr[i]>0) sig=1;
            else if(bar.c<or_lo && atr[i]>0 && !longonly) sig=-1;
            if(sig>0 && !regime_ok) sig=0;                       // long gated by bull regime
            if(sig<0 && gate && regime_ok) sig=0;                // short only in non-bull regime (symmetric gate)
            if(sig!=0){ inpos=true; dir=sig; entry=bars[i+1].o; peak=entry; trough=entry; sl_atr=atr[i]; hold=0; traded_today=true; }
        }
    }
    auto rep=[&](int y0,int y1,double&net,long&n,double&pf,double&wr){ double gw=0,gl=0;long w=0;net=0;n=0;
        for(auto&t:trs){int y=yr(t.ts); if(y<y0||y>y1)continue; net+=t.pnl;++n; if(t.pnl>0){gw+=t.pnl;++w;}else gl+=-t.pnl;}
        pf=gl>0?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0; };
    double net;long n;double pf,wr;
    printf("\n=== NAS opening-drive GAP-AND-GO (OR=%d bars=%dmin, %s, %s, ATR-trail+BE+ride, cost=%.1f) ===\n",
           OR_BARS,OR_BARS*5, gate?"regime-gated":"ungated", longonly?"long-only":"long+short", cost);
    if(trs.empty()){printf("  (no trades)\n");return 0;}
    int y0=yr(bars.front().ts),y1=yr(bars.back().ts);
    for(int y=y0;y<=y1;++y){ rep(y,y,net,n,pf,wr); if(n>0)printf("  %d  n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt\n",y,n,wr,pf,net); }
    double h1=0,h2=0; for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?h1:h2)+=trs[i].pnl;
    rep(y0,y1,net,n,pf,wr);
    printf("  ALL n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt  WF-H1=%+.0f H2=%+.0f %s\n",n,wr,pf,net,h1,h2,(h1>0&&h2>0)?"BOTH+":"NOT both+");
    return 0;
}
