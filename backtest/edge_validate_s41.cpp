// =============================================================================
// edge_validate_s41.cpp -- S41 PRE-BUILD validation of the 2 candidate edges
// before committing engine code to them:
//   (A) GER40 keltner EMA20 k2.0, bull_LB=200   (cross-symbol, from xsym_retune)
//   (B) XAUUSD keltner EMA50 k2.0 on H4         (new-TF, from xau_edge_deep)
//
// Validation gates (deploy-grade):
//   1. WF: both halves PF>1, n>=15 each
//   2. 6-block: >=5/6 positive
//   3. COST-STRESS: still WF+ at 2x and 3x half-spread
//   4. PARAM PLATEAU: neighbours (k, sl) agree -> not a lone spike
//
// Same fidelity as gold_regime_edges: cross-spread, SL-first, no-lookahead,
// WF split at midpoint, 6-block, bps PnL.
//
// BUILD: c++ -std=c++17 -O2 backtest/edge_validate_s41.cpp -o backtest/edge_validate_s41
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; };
static double HS=0.0, COST_MULT=1.0;
static int64_t SPLIT_TS=0, TS_MIN=0, TS_MAX=1;
static inline int blockOf(int64_t ts){ int b=(int)(6.0*(ts-TS_MIN)/(double)(TS_MAX-TS_MIN+1)); return b<0?0:(b>5?5:b); }

static std::vector<Bar> load(const char* path){
    std::vector<Bar> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        Bar b{}; double t,o,h,l,c;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&t,&o,&h,&l,&c)!=5) continue;
        b.ts=(int64_t)t; b.o=o;b.h=h;b.l=l;b.c=c; v.push_back(b);
    }
    fclose(f); return v;
}
struct Stats {
    int n[3]={0,0,0}, win[3]={0,0,0};
    double gw[3]={0,0,0}, gl[3]={0,0,0}, g[3]={0,0,0}, peak=0, eq=0, mdd=0;
    double blk_g[6]={0}; int blk_n[6]={0};
    void add(double pnl, int64_t ts){ int half=(ts<SPLIT_TS)?1:2;
        for(int k:{0,half}){ n[k]++; g[k]+=pnl; if(pnl>0){win[k]++;gw[k]+=pnl;} else gl[k]+=-pnl; }
        int b=blockOf(ts); blk_g[b]+=pnl; blk_n[b]++;
        eq+=pnl; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf(int k)const{ return gl[k]>0? gw[k]/gl[k] : (gw[k]>0?99:0); }
    double wr(int k)const{ return n[k]? 100.0*win[k]/n[k]:0; }
    int blocks_pos()const{ int p=0; for(int i=0;i<6;i++) if(blk_g[i]>0)p++; return p; }
    bool robust()const{ return g[1]>0&&g[2]>0&&n[1]>=15&&n[2]>=15&&blocks_pos()>=5; }
    bool wfpos()const{  return g[1]>0&&g[2]>0&&n[1]>=15&&n[2]>=15; }
};
static double pl_long(double e,double x,double px){ return ((x-e)/px*10000.0) - 2.0*(HS/px*10000.0)*COST_MULT; }
static void report(const char* name, const Stats& s){
    printf("    %-26s n=%-4d WR=%4.1f PF=%.2f g=%+8.1f | H1 PF=%.2f | H2 PF=%.2f | blk+=%d/6 %s\n",
        name,s.n[0],s.wr(0),s.pf(0),s.g[0],s.pf(1),s.pf(2),s.blocks_pos(),
        s.robust()?"*** ROBUST":(s.wfpos()?"** WF+":""));
}
static std::vector<double> mkatr(const std::vector<Bar>&B,int N){
    std::vector<double> atr(N,0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a=i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; atr[i]=a; } return atr;
}
static std::vector<double> mkema(const std::vector<Bar>&B,int N,int ep){
    std::vector<double> e(N,0); double k=2.0/(ep+1); e[0]=B[0].c;
    for(int i=1;i<N;i++) e[i]=k*B[i].c+(1-k)*e[i-1]; return e;
}
static Stats keltner_L(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double km,double slatr,int lb){
    Stats s; auto ema=mkema(B,N,ep); auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    bool in=false; double e=0,slx=0; int ei=0;
    for(int i=130;i<N;i++){ bool bull=B[i].c>cb(i,lb);
        if(!in){ if(bull&&B[i].c>ema[i]+km*atr[i]&&atr[i]>0){e=B[i].c+HS;slx=e-slatr*atr[i];in=true;ei=i;} }
        else{ double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<ema[i]){x=B[i].c-HS;ex=true;}
            if(ex){s.add(pl_long(e,x,B[ei].c),B[ei].ts);in=false;} } }
    return s;
}

static void setup(const std::vector<Bar>&B,int N,double hs_bps){
    TS_MIN=B.front().ts; TS_MAX=B.back().ts; SPLIT_TS=B[N/2].ts;
    HS = hs_bps/10000.0 * ((B.front().c+B.back().c)*0.5);
}

int main(){
    printf("S41 PRE-BUILD EDGE VALIDATION (cost-stress + param plateau)\n\n");

    // ===== (A) GER40 keltner EMA20 k2.0 LB200 =====
    {
        auto B=load("/Users/jo/Tick/GER40_merged.h1.csv"); int N=B.size(); auto atr=mkatr(B,N);
        printf("==== (A) GER40 H1  keltner EMA20 LB200  %d bars ====\n",N);
        printf("  -- param PLATEAU (k x sl) at 1x cost --\n");
        for(double k:{1.5,2.0,2.5}) for(double sl:{2.5,3.0,3.5}){
            setup(B,N,0.5); COST_MULT=1.0; char nm[40]; snprintf(nm,40,"kelt_e20_k%.1f_sl%.1f",k,sl);
            report(nm, keltner_L(B,atr,N,20,k,sl,200));
        }
        printf("  -- COST-STRESS on k2.0 sl3.0 (1x/2x/3x) --\n");
        for(double cm:{1.0,2.0,3.0}){ setup(B,N,0.5); COST_MULT=cm; char nm[40]; snprintf(nm,40,"kelt_e20_k2.0_sl3.0_%.0fx",cm);
            report(nm, keltner_L(B,atr,N,20,2.0,3.0,200)); }
        printf("\n");
    }

    // ===== (B) XAU keltner EMA50 k2.0 on H4 =====
    {
        auto B=load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv"); int N=B.size(); auto atr=mkatr(B,N);
        printf("==== (B) XAUUSD H4  keltner EMA50  %d bars  bull_LB=30 ====\n",N);
        printf("  -- param PLATEAU (k x sl) at 1x cost --\n");
        for(double k:{1.5,2.0,2.5}) for(double sl:{2.5,3.0,3.5}){
            setup(B,N,0.5); COST_MULT=1.0; char nm[40]; snprintf(nm,40,"kelt_e50_k%.1f_sl%.1f",k,sl);
            report(nm, keltner_L(B,atr,N,50,k,sl,30));
        }
        printf("  -- COST-STRESS on k2.0 sl3.0 (1x/2x/3x) --\n");
        for(double cm:{1.0,2.0,3.0}){ setup(B,N,0.5); COST_MULT=cm; char nm[40]; snprintf(nm,40,"kelt_e50_k2.0_sl3.0_%.0fx",cm);
            report(nm, keltner_L(B,atr,N,50,2.0,3.0,30)); }
        printf("  -- also EMA20 variant on H4 (compare to existing 4h cell) --\n");
        setup(B,N,0.5); COST_MULT=1.0; report("kelt_e20_k2.0_sl3.0", keltner_L(B,atr,N,20,2.0,3.0,30));
        printf("\n");
    }
    printf("DONE\n");
    return 0;
}
