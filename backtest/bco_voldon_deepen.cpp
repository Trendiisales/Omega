// =============================================================================
// bco_voldon_deepen.cpp -- deepen the BCOUSD (oil) voldon ember (S42, 2026-05-31).
//
// The S41 cross-symbol scan (xsym_edge_scan.cpp) found ONE near-miss outside the
// XAU/GER40 robust set: BCOUSD voldon_N40_sl2.5 -> n=40 PF2.04, both halves +,
// but only blk+=4/6 (ROBUST needs 5/6). This harness grids the voldon family on
// BCOUSD to see if any cell reaches ROBUST and survives 2-3x cost stress.
//
// FIDELITY: ported VERBATIM from xsym_edge_scan.cpp voldon():
//   cross-spread fills (long@level+HS in, level-HS out); cost 2*HS per RT;
//   SL checked FIRST; bull-gate close>close[-tl]; no-lookahead (bars < i for the
//   breakout window); WF split at 50%; 6 equal-count blocks. bps PnL.
// NEW LEVERS vs the single baseline config:
//   - tl  : trend-filter lookback (was fixed 120)
//   - dn  : entry breakout lookback (was 40)
//   - xn  : exit breakout lookback (was = dn; classic voldon uses a SHORTER exit)
//   - sl  : SL in ATR (was 2.5)
//   - COST_MULT sweep 1/2/3x on the survivors.
//   - both H1 and H4 series (gold edge survived to H4/D1; test oil too).
//
// BUILD: c++ -std=c++17 -O2 backtest/bco_voldon_deepen.cpp -o backtest/bco_voldon_deepen
// RUN:   ./backtest/bco_voldon_deepen
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; };
struct Sym { const char* name; const char* path; double hs_bps; };

static double HS=0.0, COST_MULT=1.0;
static int64_t SPLIT_TS=0, TS_MIN=0, TS_MAX=1;
static inline int blockOf(int64_t ts){ int b=(int)(6.0*(ts-TS_MIN)/(double)(TS_MAX-TS_MIN+1)); return b<0?0:(b>5?5:b); }

static std::vector<Bar> load(const char* path){
    std::vector<Bar> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        Bar b{}; double ts;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        b.ts=(int64_t)ts; v.push_back(b);
    }
    fclose(f); return v;
}

struct Stats {
    int n=0, win=0; double gw=0, gl=0, g=0, peak=0, eq=0, mdd=0;
    int hn[3]={0,0,0}; double hg[3]={0,0,0}; // half totals: [1]=H1 [2]=H2
    double blk_g[6]={0}; int blk_n[6]={0};
    void add(double pnl, int64_t ts){
        n++; g+=pnl; if(pnl>0){win++;gw+=pnl;} else gl+=-pnl;
        int half=(ts<SPLIT_TS)?1:2; hn[half]++; hg[half]+=pnl;
        int b=blockOf(ts); blk_g[b]+=pnl; blk_n[b]++;
        eq+=pnl; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq;
    }
    double pf()const{ return gl>0? gw/gl : (gw>0?99:0); }
    double wr()const{ return n? 100.0*win/n:0; }
    int blocks_pos()const{ int p=0; for(int i=0;i<6;i++) if(blk_g[i]>0)p++; return p; }
    // robustness identical semantics to xsym_edge_scan: both halves +, n>=15 each, >=5/6 blocks
    bool robust()const{ return hg[1]>0&&hg[2]>0&&hn[1]>=15&&hn[2]>=15&&blocks_pos()>=5; }
    bool wfpos()const{  return hg[1]>0&&hg[2]>0&&hn[1]>=15&&hn[2]>=15; }
};

static double pl_long_bps(double e,double x,double price){ return ((x-e)/price*10000.0) - 2.0*(HS/price*10000.0)*COST_MULT; }

// voldon with separate entry (dn) and exit (xn) breakout windows + tl trend filter.
static Stats voldon(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int tl,int dn,int xn,double slatr){
    Stats s; bool in=false; double e=0,slx=0; int ei=0;
    int warm=std::max({tl,dn,xn})+1;
    auto cback=[&](int i,int k){ return i-k>=0?B[i-k].c:B[0].c; };
    for(int i=warm;i<N;i++){
        double hi=0; for(int k=i-dn;k<i;k++) hi=std::max(hi,B[k].h);
        bool bull=B[i].c>cback(i,tl);
        if(!in){ if(bull&&B[i].c>hi&&atr[i]>0){ e=B[i].c+HS; slx=e-slatr*atr[i]; in=true; ei=i; } }
        else { double lo=1e18; for(int k=i-xn;k<i;k++) lo=std::min(lo,B[k].l);
            double x=0; bool ex=false;
            if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
            if(ex){ s.add(pl_long_bps(e,x,B[ei].c),B[ei].ts); in=false; } }
    }
    return s;
}

static std::vector<double> wilder_atr(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> atr(N,0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a = i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; atr[i]=a; }
    return atr;
}

struct Cell { int tl,dn,xn; double sl; Stats s; };

static void scan_tf(const char* tag, const Sym& sy){
    auto B=load(sy.path); int N=B.size();
    if(N<400){ printf("%s %s only %d bars, skip\n",tag,sy.name,N); return; }
    TS_MIN=B.front().ts; TS_MAX=B.back().ts; SPLIT_TS=B[N/2].ts;
    auto atr=wilder_atr(B);
    double px0=B.front().c,px1=B.back().c;
    HS = sy.hs_bps/10000.0*((px0+px1)*0.5);
    printf("\n===== %s  %s  %d bars  px %.3f->%.3f  ret %+.0f%%  HS=%.4f(%.1fbp) =====\n",
           tag,sy.name,N,px0,px1,(px1/px0-1)*100,HS,sy.hs_bps);
    COST_MULT=1.0;
    int tls[]={80,120,160}, dns[]={20,30,40,55,80}, xns[]={10,20,40}; double sls[]={2.0,2.5,3.0,3.5};
    std::vector<Cell> wf, rob;
    int total=0;
    for(int tl:tls)for(int dn:dns)for(int xn:xns)for(double sl:sls){
        Stats s=voldon(B,atr,N,tl,dn,xn,sl); total++;
        if(s.n<10) continue;
        if(s.robust()) rob.push_back({tl,dn,xn,sl,s});
        else if(s.wfpos()) wf.push_back({tl,dn,xn,sl,s});
    }
    auto byg=[](const Cell&a,const Cell&b){return a.s.g>b.s.g;};
    std::sort(rob.begin(),rob.end(),byg); std::sort(wf.begin(),wf.end(),byg);
    printf("  cells=%d  ROBUST=%zu  WF+only=%zu\n",total,rob.size(),wf.size());
    auto pr=[&](const char* mark,const Cell&c){
        printf("  %s tl=%-3d dn=%-2d xn=%-2d sl=%.1f | n=%-3d WR=%4.1f PF=%.2f g=%+8.1f mdd=%6.1f | H1 g=%+7.1f H2 g=%+7.1f | blk+=%d/6\n",
            mark,c.tl,c.dn,c.xn,c.sl,c.s.n,c.s.wr(),c.s.pf(),c.s.g,c.s.mdd,c.s.hg[1],c.s.hg[2],c.s.blocks_pos());
    };
    for(auto&c:rob) pr("ROBUST",c);
    int show=0; for(auto&c:wf){ if(show++>=8)break; pr("  wf+ ",c); }
    // cost-stress the best ROBUST (or best WF+ if none robust)
    std::vector<Cell>& best = rob.empty()? wf : rob;
    if(!best.empty()){
        Cell c=best.front();
        printf("  -- cost-stress best cell (tl=%d dn=%d xn=%d sl=%.1f) --\n",c.tl,c.dn,c.xn,c.sl);
        for(double m:{1.0,2.0,3.0}){ COST_MULT=m; Stats s=voldon(B,atr,N,c.tl,c.dn,c.xn,c.sl);
            printf("     %.0fx cost: n=%d PF=%.2f g=%+8.1f blk+=%d/6 H1=%+.1f H2=%+.1f %s\n",
                m,s.n,s.pf(),s.g,s.blocks_pos(),s.hg[1],s.hg[2], s.robust()?"ROBUST":(s.wfpos()?"wf+":"FAIL")); }
        COST_MULT=1.0;
    }
}

int main(){
    printf("BCOUSD VOLDON DEEPEN -- grid tl{80,120,160} x dn{20,30,40,55,80} x xn{10,20,40} x sl{2,2.5,3,3.5}\n");
    printf("(bps PnL, cross-spread, SL-first, bull-gated, WF50%% + 6-block; same fidelity as xsym_edge_scan)\n");
    Sym h1{"BCOUSD","/Users/jo/Tick/BCOUSD_merged.h1.csv",3.0};
    Sym h4{"BCOUSD","/Users/jo/Tick/BCOUSD_merged.h4.csv",3.0};
    scan_tf("H1",h1);
    scan_tf("H4",h4);
    printf("\nDONE\n");
    return 0;
}
