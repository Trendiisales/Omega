// =============================================================================
// xau_session_scan.cpp -- hunt a TIME-OF-DAY / session edge on XAU (S42, 2026-05-31).
//
// All trend families to date are time-blind. Gold has strong session structure
// (London open ~07 UTC, AM fix ~10, NY open 13:30, PM fix ~15). This harness asks:
//   (1) Is there a directional hour-of-day bias in XAU H1 bars? (diagnostic)
//   (2) Day-of-week bias?
//   (3) Does a simple session-window long (enter hour A, exit hour B) beat cost?
//
// Long-only first (shorts are DEAD per omega-s41-edge-hunt). Cost = 2*HS, HS=0.5bp
// of price (same as xsym XAU). bps PnL. WF50% split + 6-block for any tradable cell.
//
// BUILD: c++ -std=c++17 -O2 backtest/xau_session_scan.cpp -o backtest/xau_session_scan
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

struct Bar{ int64_t ts; double o,h,l,c; };
static std::vector<Bar> load(const char* p){
    std::vector<Bar> v; FILE* f=fopen(p,"r"); if(!f){perror(p);return v;}
    char l[256]; bool fst=true;
    while(fgets(l,256,f)){ if(fst){fst=false; if(l[0]<'0'||l[0]>'9')continue;}
        Bar b{}; double t; if(sscanf(l,"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;
        b.ts=(int64_t)t; v.push_back(b); }
    fclose(f); return v;
}
static inline int hourUTC(int64_t ts){ return (int)((ts/3600)%24); }
static inline int dowUTC (int64_t ts){ return (int)((ts/86400+4)%7); } // 1970-01-01 = Thu=4

int main(){
    auto B=load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv");
    int N=B.size(); if(N<400){printf("only %d bars\n",N);return 1;}
    double HS = 0.5/10000.0*((B.front().c+B.back().c)*0.5);
    double cost_bps = 2.0*(HS/((B.front().c+B.back().c)*0.5)*10000.0); // = 1.0bp here
    printf("XAU SESSION SCAN  %d H1 bars  px %.1f->%.1f  HS=%.4f  RT cost=%.2fbp\n\n",
           N,B.front().c,B.back().c,HS,cost_bps);

    // (1) hour-of-day: mean (c-o)/o bps, WR, count
    double hsum[24]={0}; int hn[24]={0}, hw[24]={0};
    for(auto&b:B){ int h=hourUTC(b.ts); double r=(b.c-b.o)/b.o*10000.0; hsum[h]+=r; hn[h]++; if(r>0)hw[h]++; }
    printf("== HOUR-OF-DAY (UTC) intrabar return, gross bps ==\n");
    printf("  hr   n   meanBps   WR%%   net(mean-cost)\n");
    for(int h=0;h<24;h++) if(hn[h]) printf("  %2d  %4d  %+7.2f  %4.1f   %+7.2f\n",
        h,hn[h],hsum[h]/hn[h],100.0*hw[h]/hn[h],hsum[h]/hn[h]-cost_bps);

    // (2) day-of-week
    double dsum[7]={0}; int dn[7]={0},dw[7]={0};
    for(auto&b:B){ int d=dowUTC(b.ts); double r=(b.c-b.o)/b.o*10000.0; dsum[d]+=r; dn[d]++; if(r>0)dw[d]++; }
    const char* dn_s[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    printf("\n== DAY-OF-WEEK intrabar return, gross bps ==\n");
    for(int d=0;d<7;d++) if(dn[d]) printf("  %s  n=%-5d meanBps=%+6.2f WR=%4.1f\n",dn_s[d],dn[d],dsum[d]/dn[d],100.0*dw[d]/dn[d]);

    // (3) session-window long: enter at open of hour A, exit at close of hour B (same day, A<=B).
    // PnL = (exit_c - entry_o)/entry_o*1e4 - cost. Group consecutive same-day bars.
    // Scan all A in 0..23, hold lengths L in 1..8 hours.
    printf("\n== SESSION-WINDOW LONG (enter hr A open, hold L hrs, exit) net bps, WF+6blk ==\n");
    int64_t TS_MIN=B.front().ts,TS_MAX=B.back().ts,SPLIT=B[N/2].ts;
    auto blk=[&](int64_t ts){int b=(int)(6.0*(ts-TS_MIN)/(double)(TS_MAX-TS_MIN+1));return b<0?0:(b>5?5:b);};
    struct R{int A,L;int n;double g,h1,h2;int bp;double pf;double wr;};
    std::vector<R> res;
    for(int A=0;A<24;A++)for(int L=1;L<=8;L++){
        double g=0,gw=0,gl=0,h1=0,h2=0; int n=0,win=0; double bg[6]={0};
        for(int i=0;i+L<N;i++){
            if(hourUTC(B[i].ts)!=A) continue;
            // require the L-bar window to be contiguous hourly (same session, no gap > ~2h)
            if(B[i+L].ts - B[i].ts != (int64_t)L*3600) continue;
            double e=B[i].o, x=B[i+L].c;
            double pnl=(x-e)/e*10000.0 - cost_bps;
            g+=pnl; n++; if(pnl>0){win++;gw+=pnl;}else gl+=-pnl;
            (B[i].ts<SPLIT?h1:h2)+=pnl; bg[blk(B[i].ts)]+=pnl;
        }
        if(n<30) continue;
        int bp=0; for(int k=0;k<6;k++) if(bg[k]>0)bp++;
        double pf=gl>0?gw/gl:99;
        res.push_back({A,L,n,g,h1,h2,bp,pf,100.0*win/n});
    }
    std::sort(res.begin(),res.end(),[](const R&a,const R&b){return a.g>b.g;});
    printf("  A=enterHr L=holdHrs | n WR PF totBps H1 H2 blk+\n");
    int sh=0; for(auto&r:res){ if(sh++>=15)break;
        bool rob = r.h1>0&&r.h2>0&&r.bp>=5;
        printf("  A=%2d L=%d | n=%-5d WR=%4.1f PF=%.2f g=%+8.1f H1=%+7.1f H2=%+7.1f blk+=%d/6 %s\n",
            r.A,r.L,r.n,r.wr,r.pf,r.g,r.h1,r.h2,r.bp, rob?"<= WF+":"");
    }
    printf("\nDONE\n");
    return 0;
}
