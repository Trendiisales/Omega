// =============================================================================
// crash_alpha_bt.cpp -- SUDDEN-COLLAPSE profit mechanism, design backtest.
//
// Question: can we systematically MAKE money during a sudden market collapse
// (hours-days velocity event from a non-bear regime), as opposed to the
// sustained-2022-style bear that IndexBearShortEngine already owns?
//
// Mechanism under test ("CrashAlpha"):
//   ARM   : drawdown from the rolling 5d(120 H1 bar) high >= DD_ARM pct AND the
//           drop is FAST (the 120-bar high sits within the last FRESH_BARS bars
//           OR the last VEL_BARS-bar return <= -VEL_PCT). Sudden, not grind.
//   ENTRY : while armed (ARM_WINDOW bars), SHORT an H1 close below the prior
//           BRK_LB-bar low with close < EMA20 (breakdown continuation -- never
//           knife-catch the first red bar).
//   STOP  : max(recent BRK_LB-bar high, entry + SL_ATR*ATR24)  [structural]
//   TP    : fixed TP_R * risk (IndexBearShort evidence: fixed TP >> trail in
//           violent tape; counter-rallies eat trails).
//   GUARDS: KILL after 3 consecutive SLs until a FRESH arm; COOLDOWN bars
//           after any exit; MAX_HOLD bars time stop; spread/glitch sanity
//           (bar range > 8*ATR ignored).
//
// Data: /Users/jo/Tick/NSXUSD_2022_2026.h1.csv (CERTIFIED 24,407 bars) +
//       SPXUSD_2022_2026.h1.csv. Cost: 4pt NAS / 1.2pt SPX round-trip
//       (ledger-real spread+slip+comm, conservative).
//
// BUILD: clang++ -O2 -std=c++17 -o /tmp/crash_alpha_bt backtest/crash_alpha_bt.cpp
// RUN  : /tmp/crash_alpha_bt /Users/jo/Tick/NSXUSD_2022_2026.h1.csv 4.0 [sweep]
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const char* p){
    std::vector<Bar> v; FILE* f=fopen(p,"r"); if(!f){fprintf(stderr,"no %s\n",p);exit(1);}
    char ln[256];
    while(fgets(ln,sizeof ln,f)){
        Bar b; if(sscanf(ln,"%lld,%lf,%lf,%lf,%lf",(long long*)&b.ts,&b.o,&b.h,&b.l,&b.c)==5) v.push_back(b);
    }
    fclose(f); return v;
}

struct Params {
    double dd_arm    = 3.0;   // % drawdown from 120-bar high to arm
    int    fresh_bars= 48;    // high must be recent (fast drop) ...
    int    vel_bars  = 24;    // ... or velocity leg: ret over vel_bars
    double vel_pct   = 2.5;   //     <= -vel_pct
    int    arm_win   = 48;    // bars armed after trigger
    int    brk_lb    = 6;     // breakdown lookback
    double sl_atr    = 1.5;
    double tp_r      = 2.0;
    int    max_hold  = 48;
    int    cooldown  = 6;
    int    max_sl_run= 3;     // consecutive-SL kill (MacroCrash lesson)
    double cost_rt   = 4.0;   // pts round trip
};

struct Trade { int64_t ets,xts; double e,x,pnl; const char* why; };

struct Result { double net=0,gross_w=0,gross_l=0; int n=0,wins=0; double maxdd=0; std::vector<Trade> trades; };

static Result run(const std::vector<Bar>& B, const Params& P, bool log=false){
    const int N=(int)B.size(); Result R;
    std::vector<double> atr(N,0), ema20(N,0);
    double a=0; for(int i=1;i<N;++i){
        double tr=std::max({B[i].h-B[i].l, std::fabs(B[i].h-B[i-1].c), std::fabs(B[i].l-B[i-1].c)});
        a = (i<25)? (a*(i-1)+tr)/i : (a*23+tr)/24; atr[i]=a;
        ema20[i] = (i==1)? B[i].c : ema20[i-1]+(B[i].c-ema20[i-1])*2.0/21.0;
    }
    int armed_until=-1, cool_until=-1, sl_run=0; bool killed=false;
    bool in=false; double e=0,sl=0,tp=0; int ebar=0; int64_t ets=0;
    double eq=0, peak=0;
    for(int i=130;i<N;++i){
        // gap-glitch sanity
        if(B[i].h-B[i].l > 8*atr[i] && atr[i]>0) continue;
        // ---- manage ----
        if(in){
            bool out=false; double px=0; const char* why="";
            if(B[i].h>=sl){ px=sl; why="SL"; out=true; ++sl_run; }
            else if(B[i].l<=tp){ px=tp; why="TP"; out=true; sl_run=0; }
            else if(i-ebar>=P.max_hold){ px=B[i].c; why="TIME"; out=true; sl_run=0; }
            if(out){
                double pnl=(e-px)-P.cost_rt;
                R.net+=pnl; ++R.n; if(pnl>0){++R.wins;R.gross_w+=pnl;} else R.gross_l+=-pnl;
                eq+=pnl; peak=std::max(peak,eq); R.maxdd=std::max(R.maxdd,peak-eq);
                R.trades.push_back({ets,B[i].ts,e,px,pnl,why});
                if(log) printf("  %lld SHORT %.1f -> %.1f  %+.1f  %s\n",(long long)ets,e,px,pnl,why);
                in=false; cool_until=i+P.cooldown;
                if(sl_run>=P.max_sl_run){ killed=true; }   // disarm until FRESH arm
                continue;
            }
        }
        // ---- arm detect ----
        int lo120=std::max(0,i-120);
        double hh=0; int hh_i=lo120;
        for(int k=lo120;k<i;++k) if(B[k].h>hh){hh=B[k].h;hh_i=k;}
        double dd=100.0*(1.0-B[i].c/hh);
        double vel = 100.0*(B[i].c/B[i-P.vel_bars].c-1.0);
        bool fast = (i-hh_i)<=P.fresh_bars || vel<=-P.vel_pct;
        if(dd>=P.dd_arm && fast){
            if(killed && armed_until<i){ killed=false; sl_run=0; }   // fresh arm clears kill
            armed_until=i+P.arm_win;
        }
        // ---- entry ----
        if(!in && !killed && i<=armed_until && i>cool_until){
            double ll=1e18,hh2=0;
            for(int k=i-P.brk_lb;k<i;++k){ ll=std::min(ll,B[k].l); hh2=std::max(hh2,B[k].h); }
            if(B[i].c<ll && B[i].c<ema20[i]){
                e=B[i].c; sl=std::max(hh2, e+P.sl_atr*atr[i]); tp=e-P.tp_r*(sl-e);
                in=true; ebar=i; ets=B[i].ts;
            }
        }
    }
    return R;
}

static void report(const char* tag, const Result& R, const std::vector<Bar>& B){
    double pf = R.gross_l>0? R.gross_w/R.gross_l : 99;
    printf("%-28s net=%+9.1f  n=%3d  WR=%2.0f%%  PF=%.2f  maxDD=%.1f\n",
           tag,R.net,R.n,R.n?100.0*R.wins/R.n:0,pf,R.maxdd);
}

int main(int argc,char**argv){
    const char* path = argc>1? argv[1] : "/Users/jo/Tick/NSXUSD_2022_2026.h1.csv";
    double cost = argc>2? atof(argv[2]) : 4.0;
    bool sweep = argc>3 && !strcmp(argv[3],"sweep");
    auto B=load(path);
    printf("bars=%zu  %s  cost_rt=%.1f\n",B.size(),path,cost);

    if(sweep){
        printf("dd_arm vel  brk tp_r | net      n   WR  PF   maxDD  | h1net  h2net\n");
        for(double dd : {2.5,3.0,3.5,4.0})
        for(double vel: {2.0,2.5,3.0})
        for(int brk : {4,6,8})
        for(double tp : {1.5,2.0,3.0}){
            Params P; P.dd_arm=dd; P.vel_pct=vel; P.brk_lb=brk; P.tp_r=tp; P.cost_rt=cost;
            Result R=run(B,P);
            // WF halves
            size_t half=B.size()/2;
            std::vector<Bar> B1(B.begin(),B.begin()+half), B2(B.begin()+half,B.end());
            Result R1=run(B1,P), R2=run(B2,P);
            double pf = R.gross_l>0? R.gross_w/R.gross_l : 99;
            printf("%4.1f %4.1f %3d %4.1f | %+8.1f %3d %3.0f%% %4.2f %7.1f | %+7.1f %+7.1f%s\n",
                dd,vel,brk,tp,R.net,R.n,R.n?100.0*R.wins/R.n:0,pf,R.maxdd,R1.net,R2.net,
                (R.net>0&&R1.net>0&&R2.net>0)?"  **":"");
        }
        return 0;
    }
    Params P; P.cost_rt=cost;
    Result R=run(B,P,true);
    report("CrashAlpha default",R,B);
    // per-year
    for(int y=2022;y<=2026;++y){
        Result Y; Y.trades.clear();
        double net=0; int n=0,w=0;
        for(auto&t:R.trades){
            time_t tt=(time_t)t.ets; struct tm g; gmtime_r(&tt,&g);
            if(g.tm_year+1900==y){ net+=t.pnl; ++n; if(t.pnl>0)++w; }
        }
        printf("  %d: net=%+8.1f n=%d WR=%.0f%%\n",y,net,n,n?100.0*w/n:0);
    }
    return 0;
}
