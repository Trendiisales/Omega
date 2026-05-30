// =============================================================================
// xsym_retune.cpp -- S41 per-symbol RE-TUNE of the 3 near-miss instruments
// (GER40 / NSXUSD / BCOUSD). The default XAU params (voldon N40 sl2.5,
// pullback e20 pb0.5, keltner e50 k2.0) were NOT robust on these in the
// cross-symbol scan -- but each is a strong bull instrument, so the question
// is whether a DIFFERENT param set firms any to deploy-grade.
//
// Deploy-grade bar (same as gold ENGINE_PROMOTION_GATE spirit):
//   - WF: both halves PF>1 AND n>=15 each
//   - 6-block: >=5/6 blocks positive  (this is what they ALL failed before)
//   - 3x-cost-robust: still WF+ at 3x half-spread
//   - param PLATEAU: neighbours in the grid agree (not a lone spike)
//
// Same fidelity as gold_regime_edges / xsym_edge_scan: cross-spread fills,
// SL-first, no-lookahead, bps-cost. Grids widened per family.
//
// BUILD: c++ -std=c++17 -O2 backtest/xsym_retune.cpp -o backtest/xsym_retune
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

static std::vector<Bar> load_h1(const char* path){
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
    int n[3]={0,0,0}, win[3]={0,0,0};
    double gw[3]={0,0,0}, gl[3]={0,0,0}, g[3]={0,0,0}, peak=0, eq=0, mdd=0;
    double blk_g[6]={0}; int blk_n[6]={0};
    void add(double pnl, int64_t ts){
        int half=(ts<SPLIT_TS)?1:2;
        for(int k:{0,half}){ n[k]++; g[k]+=pnl; if(pnl>0){win[k]++;gw[k]+=pnl;} else gl[k]+=-pnl; }
        int b=blockOf(ts); blk_g[b]+=pnl; blk_n[b]++;
        eq+=pnl; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq;
    }
    double pf(int k)const{ return gl[k]>0? gw[k]/gl[k] : (gw[k]>0?99:0); }
    double wr(int k)const{ return n[k]? 100.0*win[k]/n[k]:0; }
    int blocks_pos()const{ int p=0; for(int i=0;i<6;i++) if(blk_g[i]>0)p++; return p; }
    bool robust()const{ return g[1]>0&&g[2]>0&&n[1]>=15&&n[2]>=15&&blocks_pos()>=5; }
    bool wfpos()const{  return g[1]>0&&g[2]>0&&n[1]>=15&&n[2]>=15; }
};
static double pl_long_bps(double e,double x,double price){ return ((x-e)/price*10000.0) - 2.0*(HS/price*10000.0)*COST_MULT; }

static void report(const char* name, const Stats& s){
    printf("    %-26s n=%-4d WR=%4.1f PF=%.2f g=%+8.1f mdd=%6.1f | H1 PF=%.2f | H2 PF=%.2f | blk+=%d/6 %s\n",
        name,s.n[0],s.wr(0),s.pf(0),s.g[0],s.mdd,s.pf(1),s.pf(2),s.blocks_pos(),
        s.robust()?"*** ROBUST":(s.wfpos()?"** WF+":""));
}

// trend-strength gate lookback for bull filter (was hardcoded 120 on XAU H1).
static int BULL_LB = 120;

static Stats voldon(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int dn,double slatr){
    Stats s; bool in=false; double e=0,slx=0; int ei=0;
    auto cb=[&](int i,int k){ return i-k>=0?B[i-k].c:B[0].c; };
    for(int i=dn+1;i<N;i++){
        double hi=0,lo=1e18; for(int k=i-dn;k<i;k++){hi=std::max(hi,B[k].h);lo=std::min(lo,B[k].l);}
        bool bull=B[i].c>cb(i,BULL_LB);
        if(!in){ if(bull&&B[i].c>hi&&atr[i]>0){ e=B[i].c+HS; slx=e-slatr*atr[i]; in=true; ei=i; } }
        else { double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
            if(ex){ s.add(pl_long_bps(e,x,B[ei].c),B[ei].ts); in=false; } }
    }
    return s;
}
static Stats pullback(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double pb_atr,double slatr){
    Stats s; std::vector<double> ema(N,0); double k=2.0/(ep+1); ema[0]=B[0].c;
    for(int i=1;i<N;i++) ema[i]=k*B[i].c+(1-k)*ema[i-1];
    auto cb=[&](int i,int kk){ return i-kk>=0?B[i-kk].c:B[0].c; };
    bool in=false; double e=0,slx=0; int ei=0;
    for(int i=130;i<N;i++){
        double don_lo=1e18; for(int x=i-40;x<i;x++) don_lo=std::min(don_lo,B[x].l);
        bool bull=B[i].c>cb(i,BULL_LB) && ema[i]>ema[i-24];
        if(!in){ bool nearema=B[i].l<=ema[i]+pb_atr*atr[i] && B[i].c>ema[i];
            if(bull&&nearema&&atr[i]>0){ e=B[i].c+HS; slx=e-slatr*atr[i]; in=true; ei=i; } }
        else { double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<don_lo){x=B[i].c-HS;ex=true;}
            if(ex){ s.add(pl_long_bps(e,x,B[ei].c),B[ei].ts); in=false; } }
    }
    return s;
}
static Stats keltner(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double kmult,double slatr){
    Stats s; std::vector<double> ema(N,0); double a2=2.0/(ep+1); ema[0]=B[0].c;
    for(int i=1;i<N;i++) ema[i]=a2*B[i].c+(1-a2)*ema[i-1];
    auto cb=[&](int i,int kk){ return i-kk>=0?B[i-kk].c:B[0].c; };
    bool in=false; double e=0,slx=0; int ei=0;
    for(int i=130;i<N;i++){
        bool bull=B[i].c>cb(i,BULL_LB);
        if(!in){ if(bull&&B[i].c>ema[i]+kmult*atr[i]&&atr[i]>0){ e=B[i].c+HS; slx=e-slatr*atr[i]; in=true; ei=i; } }
        else { double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<ema[i]){x=B[i].c-HS;ex=true;}
            if(ex){ s.add(pl_long_bps(e,x,B[ei].c),B[ei].ts); in=false; } }
    }
    return s;
}

static std::vector<double> mkatr(const std::vector<Bar>&B,int N){
    std::vector<double> atr(N,0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a=i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; atr[i]=a; }
    return atr;
}

int main(){
    std::vector<Sym> syms = {
        {"GER40",  "/Users/jo/Tick/GER40_merged.h1.csv",  0.5},
        {"NSXUSD", "/Users/jo/Tick/NSXUSD_merged.h1.csv",  0.6},
        {"BCOUSD", "/Users/jo/Tick/BCOUSD_merged.h1.csv",  3.0},
    };
    printf("PER-SYMBOL RE-TUNE  GER40 / NSXUSD / BCOUSD  (widened grids, WF + 6-block, bull-LB swept)\n");
    printf("deploy-grade = *** ROBUST AND survives 3x cost AND has a param-plateau neighbour\n\n");

    for(auto& sy : syms){
        auto B=load_h1(sy.path); int N=B.size();
        if(N<400){ printf("%-8s skip (%d)\n",sy.name,N); continue; }
        TS_MIN=B.front().ts; TS_MAX=B.back().ts; SPLIT_TS=B[N/2].ts;
        auto atr=mkatr(B,N);
        double px0=B.front().c, px1=B.back().c; HS=sy.hs_bps/10000.0*((px0+px1)*0.5);
        printf("================ %-7s  %d bars  px %.2f->%.2f  ret %+.0f%%  HS=%.5f ================\n",
               sy.name,N,px0,px1,(px1/px0-1)*100,HS);

        // sweep bull-LB too: maybe these indices need a different trend filter than gold's 120.
        for(int lb : {60,120,200}){
            BULL_LB=lb; COST_MULT=1.0;
            printf("  --- bull_LB=%d : VOLDON (N x sl) ---\n", lb);
            for(int dn:{20,40,55}) for(double sl:{2.0,2.5,3.0}){ char nm[40]; snprintf(nm,40,"voldon_N%d_sl%.1f",dn,sl); report(nm,voldon(B,atr,N,dn,sl)); }
            printf("  --- bull_LB=%d : PULLBACK (ema x pb) ---\n", lb);
            for(int ep:{20,50}) for(double pb:{0.5,1.0}){ char nm[40]; snprintf(nm,40,"pullback_e%d_pb%.1f",ep,pb); report(nm,pullback(B,atr,N,ep,pb,3.0)); }
            printf("  --- bull_LB=%d : KELTNER (ema x k) ---\n", lb);
            for(int ep:{20,50}) for(double k:{1.5,2.0,2.5}){ char nm[40]; snprintf(nm,40,"keltner_e%d_k%.1f",ep,k); report(nm,keltner(B,atr,N,ep,k,3.0)); }
        }
        BULL_LB=120;
        printf("\n");
    }
    printf("DONE\n");
    return 0;
}
