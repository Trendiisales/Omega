// survivor_exit_protect_bt.cpp -- does a BE-ratchet / chandelier trail help the SurvivorPortfolio
// XAU_4h_DonchN20 cell, or give back like prior runner-trail tests said?
// Replicates the cell EXACTLY (Donchian N20 bidirectional, ATR Wilder, SL=1.5*ATR, TP=3.0*ATR,
// timeout=30 bars; reclaim=first-3-bars failed-breakout cut) and overlays exit variants:
//   BASE  : sl/tp/timeout (+reclaim) -- current production
//   BE    : + once favorable >= be_arm*ATR, ratchet SL to break-even (entry)
//   TRAIL : + chandelier -- SL trails at (best_favorable -/+ trail*ATR), tighten-only
// Reports net/PF/maxDD per dataset (bull 2yr + 2022 bear) + both halves. drive-real-engine-aligned
// (exact cell replica, the same way survivor_trendgate_test validated the trend gate pre-deploy).
// Build: clang++ -O2 -std=c++17 survivor_exit_protect_bt.cpp -o /tmp/sep && /tmp/sep
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
struct Bar{ long t; double o,h,l,c; };
static std::vector<Bar> load(const char* p){
    std::vector<Bar> v; FILE* f=fopen(p,"r"); if(!f){printf("no %s\n",p);return v;}
    char ln[512];
    while(fgets(ln,sizeof ln,f)){ Bar b; if(sscanf(ln,"%ld,%lf,%lf,%lf,%lf",&b.t,&b.o,&b.h,&b.l,&b.c)==5 && b.c>0) v.push_back(b);}
    fclose(f); return v;
}
struct Res{ int n=0,w=0; double net=0,gp=0,gl=0,maxdd=0; };
static double PF(const Res&r){ return r.gl>1e-9? r.gp/r.gl:(r.gp>0?9.99:0); }

enum Mode{BASE,BE,TRAIL};
static Res sim(const std::vector<Bar>& b,int N,double sl_m,double tp_m,int max_hold,
               double tick_usd,double lot,Mode mode,double be_arm,double trail_m){
    Res r; const int ATR_N=14; double atr=0,prevc=0; std::vector<double> trv; bool atr_ok=false;
    bool active=false; int side=0,entry_idx=0; double entry=0,sl=0,tp=0,atr_e=0,bestfav=0;
    double eq=0,peak=0;
    for(int i=0;i<(int)b.size();++i){
        if(i==0)prevc=b[i].c;
        double tr=std::max({b[i].h-b[i].l,std::fabs(b[i].h-prevc),std::fabs(b[i].l-prevc)}); prevc=b[i].c;
        if(!atr_ok){ if((int)trv.size()<ATR_N){trv.push_back(tr); if((int)trv.size()==ATR_N){double s=0;for(double v:trv)s+=v; atr=s/ATR_N; atr_ok=true;}} }
        else atr=(atr*(ATR_N-1)+tr)/ATR_N;
        if(active){
            int held=i-entry_idx;
            // update best favorable + dynamic stop
            if(side>0){ bestfav=std::max(bestfav,b[i].h);
                if(mode==BE && bestfav-entry>=be_arm*atr_e) sl=std::max(sl,entry);
                if(mode==TRAIL){ double ts=bestfav-trail_m*atr_e; sl=std::max(sl,ts);} }
            else { bestfav=std::min(bestfav,b[i].l);
                if(mode==BE && entry-bestfav>=be_arm*atr_e) sl=std::min(sl,entry);
                if(mode==TRAIL){ double ts=bestfav+trail_m*atr_e; sl=std::min(sl,ts);} }
            bool sl_hit=side>0?(b[i].l<=sl):(b[i].h>=sl);
            bool tp_hit=side>0?(b[i].h>=tp):(b[i].l<=tp);
            bool to=held>=max_hold;
            // reclaim: first-3-bars failed breakout (price closes back inside the channel)
            bool reclaim=false;
            if(held<=3 && i>=N){ double hi=b[i-1].h,lo=b[i-1].l; for(int k=2;k<=N;++k){hi=std::max(hi,b[i-k].h);lo=std::min(lo,b[i-k].l);}
                if(side<0 && b[i].c>lo) reclaim=true; }   // short-only reclaim (cell cfg)
            if(sl_hit||tp_hit||to||reclaim){
                double ex= sl_hit?sl:(tp_hit?tp:b[i].c);
                double pts= side>0?(ex-entry):(entry-ex);
                double usd=pts*tick_usd*lot;
                r.net+=usd; ++r.n; if(usd>0){++r.w; r.gp+=usd;} else r.gl+=-usd;
                eq+=usd; peak=std::max(peak,eq); r.maxdd=std::max(r.maxdd,peak-eq);
                active=false;
            }
        }
        if(!active && atr_ok && atr>0 && i>=N){
            double hh=b[i-1].h,ll=b[i-1].l; for(int k=2;k<=N;++k){hh=std::max(hh,b[i-k].h);ll=std::min(ll,b[i-k].l);}
            int dir=0; if(b[i].c>hh)dir=1; else if(b[i].c<ll)dir=-1;
            if(dir!=0){ active=true; side=dir; entry=b[i].c; entry_idx=i; atr_e=atr; bestfav=entry;
                sl=dir>0?entry-sl_m*atr:entry+sl_m*atr; tp=dir>0?entry+tp_m*atr:entry-tp_m*atr; }
        }
    }
    return r;
}
static void run(const char* name,const std::vector<Bar>& b){
    if(b.size()<60){printf("%-14s: too few bars (%zu)\n",name,b.size());return;}
    const int N=20,MH=30; const double SL=1.5,TP=3.0,TU=100,LOT=0.01;
    Res base=sim(b,N,SL,TP,MH,TU,LOT,BASE,0,0);
    Res be  =sim(b,N,SL,TP,MH,TU,LOT,BE,1.0,0);
    Res tr2 =sim(b,N,SL,TP,MH,TU,LOT,TRAIL,0,2.0);
    Res tr3 =sim(b,N,SL,TP,MH,TU,LOT,TRAIL,0,3.0);
    printf("== %s (n_bars=%zu) ==\n",name,b.size());
    auto pr=[&](const char* t,const Res&r){ printf("  %-16s n=%3d W%3d net=%+9.2f PF=%4.2f maxDD=%8.2f\n",t,r.n,r.w,r.net,PF(r),r.maxdd); };
    pr("BASE",base); pr("BE-ratchet(1ATR)",be); pr("TRAIL(2ATR)",tr2); pr("TRAIL(3ATR)",tr3);
}
int main(int argc,char**argv){
    const char* bull = argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv";
    const char* bear = argc>2?argv[2]:"/Users/jo/Tick/XAUUSD_2022_2023.h4.csv";
    auto B=load(bull), R=load(bear);
    printf("SurvivorPortfolio XAU_4h_DonchN20 exit-protection BT (BASE vs BE-ratchet vs chandelier trail)\n\n");
    run("BULL 2yr",B);
    // both halves of bull
    if(B.size()>120){ std::vector<Bar> h1(B.begin(),B.begin()+B.size()/2), h2(B.begin()+B.size()/2,B.end());
        run("BULL H1",h1); run("BULL H2",h2); }
    run("BEAR 2022-23",R);
    return 0;
}
