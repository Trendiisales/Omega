// =============================================================================
// xau_edge_deep.cpp -- S41 DEEP test of the 3 robust XAU families across the
// dimensions the gold rethink (gold_regime_edges.cpp) never swept:
//   AXIS 1  TIMEFRAME   : H1 (validated) + H4 + D1  -- does the edge survive
//                          coarser bars, or is it H1-specific?
//   AXIS 2  SHORT-SIDE  : bear-regime mirror of each family -- the engines are
//                          long-only; is there a tradeable down-leg edge?
//   AXIS 3  MEAN-REVERT : range-regime counter-trend -- when NOT trending, does
//                          fading extremes work (opposite mechanic)?
//
// Same prod fidelity as gold_regime_edges: cross-spread fills (HS), SL-first,
// no-lookahead (state uses bars <= i), WF split at series midpoint, 6-block.
// PnL in bps of price. XAU half-spread 0.5bp.
//
// BUILD: c++ -std=c++17 -O2 backtest/xau_edge_deep.cpp -o backtest/xau_edge_deep
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; };
static double HS=0.0, COST_MULT=1.0;
static int64_t SPLIT_TS=0, TS_MIN=0, TS_MAX=1;
static inline int blockOf(int64_t ts){ int b=(int)(6.0*(ts-TS_MIN)/(double)(TS_MAX-TS_MIN+1)); return b<0?0:(b>5?5:b); }

// loader handles BOTH epoch-seconds (h1/h4) and YYYYMMDD (daily) first column.
static std::vector<Bar> load(const char* path){
    std::vector<Bar> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        Bar b{}; double t,o,h,l,c;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&t,&o,&h,&l,&c)!=5) continue;
        int64_t ts=(int64_t)t;
        if(ts < 20000000 || ts > 99999999){ /* epoch */ }
        else { // YYYYMMDD -> epoch
            struct tm tm{}; int y=(int)t/10000, m=((int)t/100)%100, d=(int)t%100;
            tm.tm_year=y-1900; tm.tm_mon=m-1; tm.tm_mday=d; tm.tm_hour=12;
            ts=(int64_t)timegm(&tm);
        }
        b.ts=ts; b.o=o; b.h=h; b.l=l; b.c=c; v.push_back(b);
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
static double pl_long (double e,double x,double px){ return ((x-e)/px*10000.0) - 2.0*(HS/px*10000.0)*COST_MULT; }
static double pl_short(double e,double x,double px){ return ((e-x)/px*10000.0) - 2.0*(HS/px*10000.0)*COST_MULT; }
static void report(const char* name, const Stats& s){
    printf("    %-28s n=%-4d WR=%4.1f PF=%.2f g=%+8.1f mdd=%6.1f | H1 PF=%.2f | H2 PF=%.2f | blk+=%d/6 %s\n",
        name,s.n[0],s.wr(0),s.pf(0),s.g[0],s.mdd,s.pf(1),s.pf(2),s.blocks_pos(),
        s.robust()?"*** ROBUST":(s.wfpos()?"** WF+":""));
}
static std::vector<double> mkatr(const std::vector<Bar>&B,int N){
    std::vector<double> atr(N,0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a=i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; atr[i]=a; }
    return atr;
}
static std::vector<double> mkema(const std::vector<Bar>&B,int N,int ep){
    std::vector<double> e(N,0); double k=2.0/(ep+1); e[0]=B[0].c;
    for(int i=1;i<N;i++) e[i]=k*B[i].c+(1-k)*e[i-1]; return e;
}

// ---- LONG families (bull-gated), as validated -------------------------------
static Stats voldon_L(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int dn,double slatr,int lb){
    Stats s; bool in=false; double e=0,slx=0; int ei=0;
    auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    for(int i=dn+1;i<N;i++){ double hi=0,lo=1e18; for(int k=i-dn;k<i;k++){hi=std::max(hi,B[k].h);lo=std::min(lo,B[k].l);}
        bool bull=B[i].c>cb(i,lb);
        if(!in){ if(bull&&B[i].c>hi&&atr[i]>0){e=B[i].c+HS;slx=e-slatr*atr[i];in=true;ei=i;} }
        else{ double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
            if(ex){s.add(pl_long(e,x,B[ei].c),B[ei].ts);in=false;} } }
    return s;
}
static Stats pullback_L(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double pb,double slatr,int lb){
    Stats s; auto ema=mkema(B,N,ep); auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    bool in=false; double e=0,slx=0; int ei=0;
    for(int i=130;i<N;i++){ double dl=1e18; for(int x=i-40;x<i;x++) dl=std::min(dl,B[x].l);
        bool bull=B[i].c>cb(i,lb)&&ema[i]>ema[i-24];
        if(!in){ bool near=B[i].l<=ema[i]+pb*atr[i]&&B[i].c>ema[i];
            if(bull&&near&&atr[i]>0){e=B[i].c+HS;slx=e-slatr*atr[i];in=true;ei=i;} }
        else{ double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<dl){x=B[i].c-HS;ex=true;}
            if(ex){s.add(pl_long(e,x,B[ei].c),B[ei].ts);in=false;} } }
    return s;
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

// ---- SHORT families (bear-gated mirror) -------------------------------------
static Stats voldon_S(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int dn,double slatr,int lb){
    Stats s; bool in=false; double e=0,slx=0; int ei=0;
    auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    for(int i=dn+1;i<N;i++){ double hi=0,lo=1e18; for(int k=i-dn;k<i;k++){hi=std::max(hi,B[k].h);lo=std::min(lo,B[k].l);}
        bool bear=B[i].c<cb(i,lb);
        if(!in){ if(bear&&B[i].c<lo&&atr[i]>0){e=B[i].c-HS;slx=e+slatr*atr[i];in=true;ei=i;} }       // break DOWN
        else{ double x=0;bool ex=false; if(B[i].h>=slx){x=slx+HS;ex=true;} else if(B[i].c>hi){x=B[i].c+HS;ex=true;}
            if(ex){s.add(pl_short(e,x,B[ei].c),B[ei].ts);in=false;} } }
    return s;
}
static Stats keltner_S(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double km,double slatr,int lb){
    Stats s; auto ema=mkema(B,N,ep); auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    bool in=false; double e=0,slx=0; int ei=0;
    for(int i=130;i<N;i++){ bool bear=B[i].c<cb(i,lb);
        if(!in){ if(bear&&B[i].c<ema[i]-km*atr[i]&&atr[i]>0){e=B[i].c-HS;slx=e+slatr*atr[i];in=true;ei=i;} } // break BELOW lower channel
        else{ double x=0;bool ex=false; if(B[i].h>=slx){x=slx+HS;ex=true;} else if(B[i].c>ema[i]){x=B[i].c+HS;ex=true;}
            if(ex){s.add(pl_short(e,x,B[ei].c),B[ei].ts);in=false;} } }
    return s;
}

// ---- MEAN-REVERSION (range regime): fade extremes when NOT trending ----------
// Regime gate: |c - c_lb| / (atr) small => range. Entry: long when close pokes
// below EMA - km*ATR (oversold dip in range) -> expect snap back to EMA; exit at
// EMA (target) or stop slatr*ATR below entry. Short mirror at upper band.
static Stats meanrev(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int ep,double km,double slatr,double range_atr,bool longside){
    Stats s; auto ema=mkema(B,N,ep); auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    bool in=false; double e=0,slx=0,tp=0; int ei=0;
    for(int i=130;i<N;i++){
        double drift=std::fabs(B[i].c-cb(i,120));
        bool range = atr[i]>0 && drift < range_atr*atr[i];     // not strongly trending
        if(!in){
            if(longside){ if(range&&B[i].c<ema[i]-km*atr[i]&&atr[i]>0){e=B[i].c+HS;slx=e-slatr*atr[i];tp=ema[i];in=true;ei=i;} }
            else        { if(range&&B[i].c>ema[i]+km*atr[i]&&atr[i]>0){e=B[i].c-HS;slx=e+slatr*atr[i];tp=ema[i];in=true;ei=i;} }
        } else {
            double x=0;bool ex=false;
            if(longside){ if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].h>=tp){x=tp-HS;ex=true;}
                if(ex){s.add(pl_long(e,x,B[ei].c),B[ei].ts);in=false;} }
            else        { if(B[i].h>=slx){x=slx+HS;ex=true;} else if(B[i].l<=tp){x=tp+HS;ex=true;}
                if(ex){s.add(pl_short(e,x,B[ei].c),B[ei].ts);in=false;} }
        }
    }
    return s;
}

int main(){
    struct TF { const char* name; const char* path; int lb; };
    // bull-LB scaled per TF: H1=120(~5d), H4=30(~5d), D1=20(~1mo)
    std::vector<TF> tfs = {
        {"H1", "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv", 120},
        {"H4", "/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv",  30},
        {"D1", "/Users/jo/Tick/2yr_XAUUSD_daily.csv",          20},
    };
    const double XAU_HS_BPS=0.5;
    printf("XAU DEEP EDGE TEST -- 3 families x {H1,H4,D1} x {long, short, mean-rev}\n");
    printf("(prod fidelity: cross-spread, SL-first, no-lookahead, WF + 6-block; PnL bps)\n\n");

    int robust=0;
    for(auto& tf : tfs){
        auto B=load(tf.path); int N=B.size();
        if(N<300){ printf("== %s only %d bars, skip ==\n",tf.name,N); continue; }
        TS_MIN=B.front().ts; TS_MAX=B.back().ts; SPLIT_TS=B[N/2].ts;
        auto atr=mkatr(B,N);
        HS = XAU_HS_BPS/10000.0 * ((B.front().c+B.back().c)*0.5);
        // mean-rev EMA lookback differs by TF
        int mr_ep = (tf.lb>=120)?50:20;
        printf("================ XAUUSD %s  %d bars  px %.1f->%.1f  bull_lb=%d ================\n",
               tf.name,N,B.front().c,B.back().c,tf.lb);
        printf("  --- LONG (bull-gated, validated families) ---\n");
        { Stats a=voldon_L (B,atr,N,40,2.5,tf.lb); report("voldon_N40_sl2.5_L",a); robust+=a.robust(); }
        { Stats a=pullback_L(B,atr,N,20,0.5,3.0,tf.lb); report("pullback_e20_pb0.5_L",a); robust+=a.robust(); }
        { Stats a=keltner_L (B,atr,N,50,2.0,3.0,tf.lb); report("keltner_e50_k2.0_L",a); robust+=a.robust(); }
        printf("  --- SHORT (bear-gated mirror) ---\n");
        { Stats a=voldon_S (B,atr,N,40,2.5,tf.lb); report("voldon_N40_sl2.5_S",a); robust+=a.robust(); }
        { Stats a=keltner_S (B,atr,N,50,2.0,3.0,tf.lb); report("keltner_e50_k2.0_S",a); robust+=a.robust(); }
        printf("  --- MEAN-REVERSION (range regime, fade band to EMA) ---\n");
        { Stats a=meanrev(B,atr,N,mr_ep,1.0,2.0,1.5,true ); report("meanrev_k1.0_long",a); robust+=a.robust(); }
        { Stats a=meanrev(B,atr,N,mr_ep,1.5,2.0,1.5,true ); report("meanrev_k1.5_long",a); robust+=a.robust(); }
        { Stats a=meanrev(B,atr,N,mr_ep,1.0,2.0,1.5,false); report("meanrev_k1.0_short",a); robust+=a.robust(); }
        printf("\n");
    }
    printf("TOTAL ROBUST cells: %d\nDONE\n", robust);
    return 0;
}
