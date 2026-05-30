// =============================================================================
// ger40_deepen.cpp -- hunt a SECOND robust GER40 (DAX) trend edge (S42, 2026-05-31).
//
// Context: the S41 cross-symbol scan found GER40 carries trend structure but the
// DEFAULT XAU configs (keltner e50, voldon N40) do NOT fire robust on it -- the
// one validated GER40 edge (Ger40KeltnerH1Engine) needed a SLOWER tune: LB200
// trend filter + EMA20 + k2.0. Only the keltner family was validated; voldon and
// pullback on GER40 were never gridded, and no timeframe check was done.
//
// This harness grids all 3 families on GER40 across slow lookbacks, H1 + H4, and
// cost-stresses survivors. Goal: a 2nd GER40 cell to diversify the (currently
// ~all-XAU) shadow book. A real edge must (a) be ROBUST (both halves +, n>=15 ea,
// >=5/6 blocks), (b) survive 3x cost, (c) persist to H4 -- the bar the BCOUSD oil
// ember failed (see omega-bco-voldon-deadend).
//
// FIDELITY: families ported VERBATIM from xsym_edge_scan.cpp (cross-spread fills,
// cost 2*HS/RT, SL-first, bull-gate close>close[-tl], no-lookahead, WF50%, 6-block,
// bps PnL). NEW levers vs the single baseline: tl (trend lookback), xn (voldon/
// pullback exit lookback decoupled), ep (EMA period). Same warm-up discipline for
// all cells (start at max-window+1) so comparisons are apples-to-apples.
//
// BUILD: c++ -std=c++17 -O2 backtest/ger40_deepen.cpp -o backtest/ger40_deepen
// RUN:   ./backtest/ger40_deepen
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

struct Bar { int64_t ts; double o,h,l,c; };
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
    int hn[3]={0,0,0}; double hg[3]={0,0,0};
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
    bool robust()const{ return hg[1]>0&&hg[2]>0&&hn[1]>=15&&hn[2]>=15&&blocks_pos()>=5; }
    bool wfpos()const{  return hg[1]>0&&hg[2]>0&&hn[1]>=15&&hn[2]>=15; }
};
static double pl_long_bps(double e,double x,double price){ return ((x-e)/price*10000.0) - 2.0*(HS/price*10000.0)*COST_MULT; }

static std::vector<double> wilder_atr(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> atr(N,0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a=i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; atr[i]=a; }
    return atr;
}
static std::vector<double> ema_series(const std::vector<Bar>&B,int ep){
    int N=B.size(); std::vector<double> e(N,0); double k=2.0/(ep+1); e[0]=B[0].c;
    for(int i=1;i<N;i++) e[i]=k*B[i].c+(1-k)*e[i-1];
    return e;
}

// ---- families (bull-gate cback(i,tl); SL-first; bps) ------------------------
static Stats voldon(const std::vector<Bar>&B,const std::vector<double>&atr,int N,int tl,int dn,int xn,double slatr){
    Stats s; bool in=false; double e=0,slx=0; int ei=0; int warm=std::max({tl,dn,xn})+1;
    auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    for(int i=warm;i<N;i++){
        double hi=0; for(int k=i-dn;k<i;k++) hi=std::max(hi,B[k].h);
        bool bull=B[i].c>cb(i,tl);
        if(!in){ if(bull&&B[i].c>hi&&atr[i]>0){e=B[i].c+HS;slx=e-slatr*atr[i];in=true;ei=i;} }
        else { double lo=1e18; for(int k=i-xn;k<i;k++) lo=std::min(lo,B[k].l);
            double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
            if(ex){s.add(pl_long_bps(e,x,B[ei].c),B[ei].ts);in=false;} }
    }
    return s;
}
static Stats pullback(const std::vector<Bar>&B,const std::vector<double>&atr,const std::vector<double>&ema,int N,int tl,int xn,double pb_atr,double slatr){
    Stats s; bool in=false; double e=0,slx=0; int ei=0; int warm=std::max({tl,xn,24})+1;
    auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    for(int i=warm;i<N;i++){
        bool bull=B[i].c>cb(i,tl)&&ema[i]>ema[i-24];
        if(!in){ bool nearema=B[i].l<=ema[i]+pb_atr*atr[i]&&B[i].c>ema[i];
            if(bull&&nearema&&atr[i]>0){e=B[i].c+HS;slx=e-slatr*atr[i];in=true;ei=i;} }
        else { double lo=1e18; for(int k=i-xn;k<i;k++) lo=std::min(lo,B[k].l);
            double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
            if(ex){s.add(pl_long_bps(e,x,B[ei].c),B[ei].ts);in=false;} }
    }
    return s;
}
static Stats keltner(const std::vector<Bar>&B,const std::vector<double>&atr,const std::vector<double>&ema,int N,int tl,double kmult,double slatr){
    Stats s; bool in=false; double e=0,slx=0; int ei=0; int warm=tl+1;
    auto cb=[&](int i,int k){return i-k>=0?B[i-k].c:B[0].c;};
    for(int i=warm;i<N;i++){
        bool bull=B[i].c>cb(i,tl);
        if(!in){ if(bull&&B[i].c>ema[i]+kmult*atr[i]&&atr[i]>0){e=B[i].c+HS;slx=e-slatr*atr[i];in=true;ei=i;} }
        else { double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<ema[i]){x=B[i].c-HS;ex=true;}
            if(ex){s.add(pl_long_bps(e,x,B[ei].c),B[ei].ts);in=false;} }
    }
    return s;
}

struct Cell { std::string desc; int tl,ep; double sl; char fam; Stats s; };

int main(){
    struct TF{ const char* tag; const char* path; double hs_bps; };
    std::vector<TF> tfs = {
        {"H1","/Users/jo/Tick/GER40_merged.h1.csv",0.5},
        {"H4","/Users/jo/Tick/GER40_merged.h4.csv",0.5},
    };
    printf("GER40 DEEPEN -- 3 families, slow lookbacks, H1+H4, cost-stress. Hunt 2nd non-gold edge.\n");
    printf("(bps PnL, cross-spread, SL-first, bull-gated, WF50%% + 6-block; xsym fidelity)\n");

    int tls[]={120,160,200,260};
    int dns[]={20,30,40,55}, xns[]={20,40,55}; double sls[]={2.0,2.5,3.0,3.5};
    int eps[]={20,50}; double pbs[]={0.3,0.5,0.8}; double kms[]={1.5,2.0,2.5};

    for(auto&tf:tfs){
        auto B=load(tf.path); int N=B.size();
        if(N<400){ printf("%s only %d bars, skip\n",tf.tag,N); continue; }
        TS_MIN=B.front().ts; TS_MAX=B.back().ts; SPLIT_TS=B[N/2].ts;
        auto atr=wilder_atr(B);
        double px0=B.front().c,px1=B.back().c;
        HS=tf.hs_bps/10000.0*((px0+px1)*0.5);
        printf("\n===== GER40 %s  %d bars  px %.1f->%.1f  ret %+.0f%%  HS=%.3f(%.1fbp) =====\n",
               tf.tag,N,px0,px1,(px1/px0-1)*100,HS,tf.hs_bps);
        std::vector<Cell> rob, wf;
        int total=0;
        COST_MULT=1.0;
        // voldon grid
        for(int tl:tls)for(int dn:dns)for(int xn:xns)for(double sl:sls){
            Stats s=voldon(B,atr,N,tl,dn,xn,sl); total++; if(s.n<10)continue;
            char buf[96]; snprintf(buf,sizeof buf,"voldon tl%d dn%d xn%d sl%.1f",tl,dn,xn,sl);
            Cell c{buf,tl,0,sl,'v',s};
            if(s.robust())rob.push_back(c); else if(s.wfpos())wf.push_back(c);
        }
        // pullback grid
        for(int ep:eps){ auto ema=ema_series(B,ep);
            for(int tl:tls)for(int xn:xns)for(double pb:pbs)for(double sl:sls){
                Stats s=pullback(B,atr,ema,N,tl,xn,pb,sl); total++; if(s.n<10)continue;
                char buf[96]; snprintf(buf,sizeof buf,"pullback tl%d ep%d xn%d pb%.1f sl%.1f",tl,ep,xn,pb,sl);
                Cell c{buf,tl,ep,sl,'p',s};
                if(s.robust())rob.push_back(c); else if(s.wfpos())wf.push_back(c);
            } }
        // keltner grid
        for(int ep:eps){ auto ema=ema_series(B,ep);
            for(int tl:tls)for(double km:kms)for(double sl:sls){
                Stats s=keltner(B,atr,ema,N,tl,km,sl); total++; if(s.n<10)continue;
                char buf[96]; snprintf(buf,sizeof buf,"keltner tl%d ep%d k%.1f sl%.1f",tl,ep,km,sl);
                Cell c{buf,tl,ep,sl,'k',s};
                if(s.robust())rob.push_back(c); else if(s.wfpos())wf.push_back(c);
            } }
        auto byg=[](const Cell&a,const Cell&b){return a.s.g>b.s.g;};
        std::sort(rob.begin(),rob.end(),byg); std::sort(wf.begin(),wf.end(),byg);
        printf("  cells=%d  ROBUST=%zu  WF+only=%zu\n",total,rob.size(),wf.size());
        auto pr=[&](const char* m,const Cell&c){
            printf("  %s %-34s | n=%-3d WR=%4.1f PF=%.2f g=%+8.1f mdd=%6.1f | H1 g=%+7.1f H2 g=%+7.1f | blk+=%d/6\n",
                m,c.desc.c_str(),c.s.n,c.s.wr(),c.s.pf(),c.s.g,c.s.mdd,c.s.hg[1],c.s.hg[2],c.s.blocks_pos());
        };
        int sh=0; for(auto&c:rob){ if(sh++>=12)break; pr("ROBUST",c);}
        sh=0; for(auto&c:wf){ if(sh++>=6)break; pr("  wf+ ",c);}
    }
    printf("\nDONE\n");
    return 0;
}
