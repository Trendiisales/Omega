// =============================================================================
// index_bear_short_d1.cpp -- D1 port of IndexBearShortEngine (faithful).
//
// Tests whether the sustained-bear Donchian-breakdown SHORT + fixed-2R TP edge
// (validated H1 on NAS2022 PF1.60 + SPX2022 PF1.84) generalises to the DAILY
// timeframe across the full 2019-2026 D1 corpus -- which spans BOTH the 2020
// COVID V-crash AND the 2022 grind-down, plus bull regimes. Per-year PnL
// breakdown isolates each bear; both-halves WF + cost gate per BACKTEST_TRUTH.
//
// Mechanism (identical intent to IndexBearShortEngine.hpp, D1-scaled):
//   regime gate: close<EMA_SLOW && EMA_SLOW falling over PERSIST bars && EMA_FAST<EMA_SLOW
//   down-momentum: close<EMA_FAST && EMA_FAST falling over 5 bars
//   entry: Donchian-DON breakdown (close < prior-DON-bar low)
//   stop = recent swing high (or entry + SL_ATR*ATR); TP = entry - TP_R*risk (fixed)
//   exits: SL (bar high>=stop), TP (bar low<=tp), TIME_STOP (MAX_HOLD bars)
//   cost: COST_PTS round-trip per trade, subtracted from short pnl.
//
// BUILD: c++ -std=c++17 -O2 backtest/index_bear_short_d1.cpp -o backtest/index_bear_short_d1
// RUN:   ./backtest/index_bear_short_d1
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load_d1(const char* path){
    std::vector<Bar> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        Bar b{}; double ts;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        b.ts=(int64_t)ts; if(b.o>0&&b.h>0&&b.l>0&&b.c>0) v.push_back(b);
    }
    fclose(f); return v;
}

// year from epoch-ms (UTC, approximate via 365.2425-day years from 1970)
static int year_of(int64_t ts_ms){
    int64_t days = ts_ms/1000/86400; return 1970 + (int)(days/365.2425);
}

// --- params (D1-scaled) ---
static const int    ATR_N=14, DON=20, EMA_FAST=20, EMA_SLOW=50, PERSIST=20, MAX_HOLD=40, COOLDOWN=3;
static const double SL_ATR=2.0, TP_R=2.0;

struct Trade { int64_t ets; double pnl; bool win; };

static std::vector<Trade> run(const std::vector<Bar>& B, double cost_pts){
    std::vector<Trade> out;
    int N=(int)B.size(); if(N<EMA_SLOW+PERSIST+2) return out;
    std::vector<double> atr(N,0), ema(N,0), emaS(N,0);
    std::deque<double> tr; double trsum=0;
    double kf=2.0/(EMA_FAST+1), kls=2.0/(EMA_SLOW+1);
    for(int i=0;i<N;i++){
        double t=B[i].h-B[i].l;
        if(i>0) t=std::max(t,std::max(std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)));
        tr.push_back(t); trsum+=t; if((int)tr.size()>ATR_N){trsum-=tr.front();tr.pop_front();}
        atr[i]=trsum/(double)tr.size();
        ema[i] = i? ema[i-1]+kf*(B[i].c-ema[i-1]) : B[i].c;
        emaS[i]= i? emaS[i-1]+kls*(B[i].c-emaS[i-1]) : B[i].c;
    }
    bool in=false; double entry=0,stop=0,tp=0; int ei=0; int last_exit=-100000;
    for(int i=EMA_SLOW+PERSIST+1; i<N; i++){
        double A=atr[i]; if(A<=0) continue;
        if(in){
            double exit_px=0; const char* why=nullptr;
            if(B[i].h>=stop){exit_px=stop;why="SL";}
            else if(B[i].l<=tp){exit_px=tp;why="TP";}
            else if(i-ei>=MAX_HOLD){exit_px=B[i].c;why="TIME";}
            if(why){
                double pnl=(entry-exit_px)-cost_pts;  // SHORT
                out.push_back({B[ei].ts,pnl,pnl>0});
                in=false; last_exit=i;
            }
            continue;
        }
        if(i-last_exit<COOLDOWN) continue;
        double cl=B[i].c;
        bool bear = cl<emaS[i] && emaS[i]<emaS[i-PERSIST] && ema[i]<emaS[i];
        if(!bear) continue;
        if(!(cl<ema[i] && ema[i]<ema[i-5])) continue;       // down-momentum
        double lo=1e18; for(int k=i-DON;k<i;k++) if(B[k].l<lo)lo=B[k].l;
        if(cl>=lo) continue;                                 // Donchian breakdown
        double sh=B[i].h; for(int k=i-1;k>=i-3&&k>=0;k--) if(B[k].h>sh)sh=B[k].h;
        entry=cl; stop= sh>entry? sh : entry+SL_ATR*A;
        double risk=stop-entry; if(risk<=0) continue;
        tp=entry-TP_R*risk; ei=i; in=true;
    }
    return out;
}

struct Agg { int n=0,win=0; double gw=0,gl=0; };
static void add(Agg&a,double pnl){ a.n++; if(pnl>0){a.win++;a.gw+=pnl;} else a.gl+=-pnl; }
static double pf(const Agg&a){ return a.gl>0? a.gw/a.gl : (a.gw>0?99:0); }

int main(){
    struct Sym{const char*name;const char*path;double cost;};
    std::vector<Sym> syms={
        {"NAS",   "download/usatechidxusd-d1-bid-2019-01-01-2026-05-31.csv", 2.0},
        {"SPX",   "download/usa500idxusd-d1-bid-2019-01-01-2026-05-31.csv",  0.6},
        {"DJ30",  "download/usa30idxusd-d1-bid-2019-01-01-2026-05-31.csv",   2.0},
        {"GER40", "download/deuidxeur-d1-bid-2019-01-01-2026-05-31.csv",     1.5},
        {"ESTX",  "download/eusidxeur-d1-bid-2019-01-01-2026-05-31.csv",     1.0},
        {"FTSE",  "download/gbridxgbp-d1-bid-2019-01-01-2026-05-31.csv",     1.0},
    };
    printf("D1 BEAR-SHORT (Donchian-%d breakdown + sustained-bear EMA%d/%d persist%d + fixed-%gR TP)\n",
           DON,EMA_FAST,EMA_SLOW,PERSIST,TP_R);
    printf("%-7s %4s %5s %6s %8s | %s\n","sym","n","WR%","PF","net_pt","per-year net (n)");
    printf("--------------------------------------------------------------------------------\n");
    Agg book;
    for(auto& s:syms){
        auto B=load_d1(s.path); if(B.empty()){printf("%-7s NO DATA\n",s.name);continue;}
        auto T=run(B,s.cost);
        Agg all; int64_t tmin=B.front().ts, tmax=B.back().ts;
        Agg h1,h2; double yr_net[16]={0}; int yr_n[16]={0};
        double net=0;
        for(auto&t:T){
            add(all,t.pnl); net+=t.pnl; add(book,t.pnl);
            if(t.ets < (tmin+tmax)/2) add(h1,t.pnl); else add(h2,t.pnl);
            int y=year_of(t.ets)-2018; if(y>=0&&y<16){yr_net[y]+=t.pnl;yr_n[y]++;}
        }
        printf("%-7s %4d %5.1f %6.2f %+8.0f | ",s.name,all.n,
               all.n?100.0*all.win/all.n:0, pf(all), net);
        for(int y=1;y<=8;y++) if(yr_n[y]) printf("%d:%+.0f(%d) ",2018+y,yr_net[y],yr_n[y]);
        printf("\n");
        printf("        WF: H1 PF=%.2f n=%d | H2 PF=%.2f n=%d %s\n",
               pf(h1),h1.n,pf(h2),h2.n,
               (h1.gw-h1.gl>0&&h2.gw-h2.gl>0&&h1.n>=8&&h2.n>=8)?"** both-halves+":"");
    }
    printf("--------------------------------------------------------------------------------\n");
    printf("BOOK pooled: n=%d WR=%.1f PF=%.2f net=%+.0fpt\n",
           book.n, book.n?100.0*book.win/book.n:0, pf(book), book.gw-book.gl);
    return 0;
}
