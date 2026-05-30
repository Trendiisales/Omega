// =============================================================================
// xsym_edge_scan.cpp -- CROSS-SYMBOL edge scan (S41 2026-05-30).
//
// Tests whether the 3 robust XAU trend families (vol-target Donchian N40,
// pullback-continuation EMA20, Keltner EMA50) generalize to indices + FX + oil.
// Derived from backtest/gold_regime_edges.cpp, with the gold-only hardcoded
// HS=0.15 replaced by a per-symbol half-spread expressed in BPS OF PRICE so the
// same cost model is fair across instruments of wildly different scale
// (FX ~1.0, indices ~5k-20k, JPY ~157, oil ~75).
//
// FIDELITY (same as gold_regime_edges): cross-spread fills (long@level+HS in,
// @level-HS out); cost 2*HS per RT; SL checked FIRST (conservative); all
// signal/regime state uses bars <= i (no lookahead); WF split at 50% of series;
// 6 equal-count blocks for regime-consistency.
//
// SIZING: vol-target sz = VT_UNIT_BPS*price/10000 / ATR  (so the per-trade $
// risk is normalised across price scales; PF/robustness are scale-free anyway).
//
// BUILD: c++ -std=c++17 -O2 backtest/xsym_edge_scan.cpp -o backtest/xsym_edge_scan
// RUN:   ./backtest/xsym_edge_scan            (scans the built-in symbol table)
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; };

// Per-symbol: name, CSV path, half-spread in bps of price.
struct Sym { const char* name; const char* path; double hs_bps; };

static double HS = 0.0;          // absolute half-spread for the current symbol
static double COST_MULT = 1.0;
static int64_t SPLIT_TS = 0;
static int64_t TS_MIN=0, TS_MAX=1;
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
    double blk_g[6]={0,0,0,0,0,0}; int blk_n[6]={0,0,0,0,0,0};
    void add(double pnl, int64_t ts){
        int half = (ts<SPLIT_TS)?1:2;
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

// PnL in "R-normalised" units: (move/price)*10000 = bps, times size. Keeps
// cross-symbol totals comparable. Cost = 2*HS(bps)*COST_MULT per RT.
static double pl_long_bps(double e,double x,double price){ return ((x-e)/price*10000.0) - 2.0*(HS/price*10000.0)*COST_MULT; }

static void report(const char* name, const Stats& s){
    printf("    %-22s n=%-4d WR=%4.1f PF=%.2f g=%+8.1f mdd=%6.1f | H1 PF=%.2f | H2 PF=%.2f | blk+=%d/6 %s\n",
        name,s.n[0],s.wr(0),s.pf(0),s.g[0],s.mdd,s.pf(1),s.pf(2),s.blocks_pos(),
        s.robust()?"*** ROBUST":(s.wfpos()?"** WF+":""));
}

// ---- families (ported verbatim from gold_regime_edges.cpp, bps-cost) --------
static Stats voldon(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int dn,double slatr){
    Stats s; bool in=false; double e=0,slx=0,sz=1; int ei=0;
    auto cback=[&](int i,int k){ return i-k>=0?B[i-k].c:B[0].c; };
    for(int i=dn+1;i<N;i++){
        double hi=0,lo=1e18; for(int k=i-dn;k<i;k++){hi=std::max(hi,B[k].h);lo=std::min(lo,B[k].l);}
        bool bull=B[i].c>cback(i,120);
        if(!in){ if(bull&&B[i].c>hi&&atr[i]>0){ e=B[i].c+HS; slx=e-slatr*atr[i]; in=true; ei=i; } }
        else { double x=0; bool ex=false;
            if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
            if(ex){ s.add(pl_long_bps(e,x,B[ei].c)*sz,B[ei].ts); in=false; } }
    }
    return s;
}
static Stats pullback(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double pb_atr,double slatr){
    Stats s; std::vector<double> ema(N,0); double k=2.0/(ep+1); ema[0]=B[0].c;
    for(int i=1;i<N;i++) ema[i]=k*B[i].c+(1-k)*ema[i-1];
    auto cback=[&](int i,int kk){ return i-kk>=0?B[i-kk].c:B[0].c; };
    bool in=false; double e=0,slx=0,sz=1; int ei=0;
    for(int i=130;i<N;i++){
        double don_lo=1e18; for(int x=i-40;x<i;x++) don_lo=std::min(don_lo,B[x].l);
        bool bull=B[i].c>cback(i,120) && ema[i]>ema[i-24];
        if(!in){
            bool nearema = B[i].l <= ema[i]+pb_atr*atr[i] && B[i].c>ema[i];
            if(bull&&nearema&&atr[i]>0){ e=B[i].c+HS; slx=e-slatr*atr[i]; in=true; ei=i; }
        } else { double x=0;bool ex=false;
            if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<don_lo){x=B[i].c-HS;ex=true;}
            if(ex){ s.add(pl_long_bps(e,x,B[ei].c)*sz,B[ei].ts); in=false; } }
    }
    return s;
}
static Stats keltner(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double kmult,double slatr){
    Stats s; std::vector<double> ema(N,0); double a2=2.0/(ep+1); ema[0]=B[0].c;
    for(int i=1;i<N;i++) ema[i]=a2*B[i].c+(1-a2)*ema[i-1];
    auto cback=[&](int i,int kk){ return i-kk>=0?B[i-kk].c:B[0].c; };
    bool in=false; double e=0,slx=0,sz=1; int ei=0;
    for(int i=130;i<N;i++){
        bool bull=B[i].c>cback(i,120);
        if(!in){ if(bull&&B[i].c>ema[i]+kmult*atr[i]&&atr[i]>0){ e=B[i].c+HS; slx=e-slatr*atr[i]; in=true; ei=i; } }
        else { double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<ema[i]){x=B[i].c-HS;ex=true;}
            if(ex){ s.add(pl_long_bps(e,x,B[ei].c)*sz,B[ei].ts); in=false; } }
    }
    return s;
}

int main(int argc,char**argv){
    // name, path, half-spread bps. Spreads = realistic-to-conservative retail.
    std::vector<Sym> syms = {
        {"XAUUSD", "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv", 0.5},
        {"GER40",  "/Users/jo/Tick/GER40_merged.h1.csv",  0.5},
        {"NSXUSD", "/Users/jo/Tick/NSXUSD_merged.h1.csv",  0.6},
        {"SPXUSD", "/Users/jo/Tick/SPXUSD_merged.h1.csv",  1.0},
        {"BCOUSD", "/Users/jo/Tick/BCOUSD_merged.h1.csv",  3.0},
        {"EURUSD", "/Users/jo/Tick/EURUSD_merged.h1.csv",  0.5},
        {"GBPUSD", "/Users/jo/Tick/GBPUSD_merged.h1.csv",  0.6},
        {"USDJPY", "/Users/jo/Tick/USDJPY_merged.h1.csv",  0.6},
        {"AUDUSD", "/Users/jo/Tick/AUDUSD_merged.h1.csv",  0.8},
        {"NZDUSD", "/Users/jo/Tick/NZDUSD_merged.h1.csv",  1.0},
        {"USDCAD", "/Users/jo/Tick/USDCAD_merged.h1.csv",  0.8},
        {"EURGBP", "/Users/jo/Tick/EURGBP_merged.h1.csv",  0.8},
    };
    (void)argc;(void)argv;
    printf("CROSS-SYMBOL EDGE SCAN -- 3 validated XAU trend families, bps-cost, WF + 6-block\n");
    printf("(PnL in bps; cost = 2*HS_bps per RT; long-only bull-gated; SL-first)\n\n");

    int robust_total=0;
    for(auto& sy : syms){
        auto B = load_h1(sy.path); int N=B.size();
        if(N<400){ printf("%-8s  (only %d bars, skip)\n",sy.name,N); continue; }
        TS_MIN=B.front().ts; TS_MAX=B.back().ts; SPLIT_TS = B[N/2].ts;
        // ATR(14) Wilder
        std::vector<double> atr(N,0); double a=0;
        for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
            a = i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; atr[i]=a; }
        double px0=B.front().c, px1=B.back().c;
        HS = sy.hs_bps/10000.0 * ( (px0+px1)*0.5 );   // bps -> absolute, at mid price level
        printf("== %-7s  %d bars  px %.4f->%.4f  ret %+.0f%%  HS=%.5f (%.1fbp) ==\n",
               sy.name,N,px0,px1,(px1/px0-1)*100,HS,sy.hs_bps);
        Stats vd  = voldon  (B,atr,N,40,2.5);
        Stats pb  = pullback(B,atr,N,20,0.5,3.0);
        Stats kel = keltner (B,atr,N,50,2.0,3.0);
        report("voldon_N40_sl2.5", vd);
        report("pullback_e20_pb0.5",pb);
        report("keltner_e50_k2.0", kel);
        robust_total += vd.robust()+pb.robust()+kel.robust();
        printf("\n");
    }
    printf("TOTAL ROBUST (family x symbol) cells: %d / %d\n", robust_total, (int)syms.size()*3);
    printf("DONE\n");
    return 0;
}
