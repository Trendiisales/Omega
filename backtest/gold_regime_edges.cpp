// =============================================================================
// gold_regime_edges.cpp -- S39 gold edge rethink, prod-fidelity validation (v2).
//
// v2 adds: finer vol-target-Donchian grid, COST-STRESS (1x/2x/3x), 6-BLOCK regime
// consistency, and new hunt families (multi-TF confluence, vol-expansion entry,
// Monday-gap, Donchian + day/session overlay).
//
// FIDELITY: fills cross spread (long@ask in/@bid out = level -/+ HS); cost 2*HS
// per RT * COST_MULT; same-bar SL checked FIRST (conservative); ALL signal/regime
// state uses bars <= i only (no lookahead); WF split 2025-04-01; 6 equal-time blocks.
// Build: c++ -std=c++17 -O2 backtest/gold_regime_edges.cpp -o backtest/gold_regime_edges
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>

struct Bar { int64_t ts; double o,h,l,c; int hr,dow,yday; };
static const double HS = 0.15;
static const int64_t SPLIT_TS = 1743465600;        // 2025-04-01 UTC
static double COST_MULT = 1.0;
static int64_t TS_MIN=0, TS_MAX=1;
static inline int blockOf(int64_t ts){ int b=(int)(6.0*(ts-TS_MIN)/(double)(TS_MAX-TS_MIN+1)); return b<0?0:(b>5?5:b); }

static std::vector<Bar> load_h1(const char* path){
    std::vector<Bar> v; FILE* f=fopen(path,"r"); if(!f){perror("open");return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        Bar b{}; double ts;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        b.ts=(int64_t)ts; time_t t=b.ts; struct tm tm{}; gmtime_r(&t,&tm);
        b.hr=tm.tm_hour; b.dow=tm.tm_wday; b.yday=tm.tm_yday; v.push_back(b);
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
};
static void report(const char* name, const Stats& s){
    bool wf = s.g[1]>0&&s.g[2]>0&&s.n[1]>=20&&s.n[2]>=20;
    printf("%-20s ALL n=%-5d WR=%4.1f PF=%.2f g=%+8.1f mdd=%6.1f | H1 g=%+7.1f PF=%.2f | H2 g=%+7.1f PF=%.2f | blk+=%d/6 %s\n",
        name,s.n[0],s.wr(0),s.pf(0),s.g[0],s.mdd,s.g[1],s.pf(1),s.g[2],s.pf(2),s.blocks_pos(),
        wf&&s.blocks_pos()>=5?"*** ROBUST":(wf?"** WF+":""));
}
static inline double pl_long(double e,double x){ return (x-e)-2*HS*COST_MULT; }
static inline double pl_short(double e,double x){ return (e-x)-2*HS*COST_MULT; }

int main(int argc,char**argv){
    const char* path=argc>1?argv[1]:"/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h1.csv";
    auto B=load_h1(path); int N=B.size();
    if(N<500){printf("not enough bars\n");return 1;}
    TS_MIN=B.front().ts; TS_MAX=B.back().ts;
    printf("loaded %d H1 bars  px %.0f->%.0f  blocks of ~%.0f days each\n",N,B.front().c,B.back().c,(TS_MAX-TS_MIN)/6.0/86400);

    std::vector<double> atr(N,0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a = i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; atr[i]=a; }
    auto cback=[&](int i,int k){ return i-k>=0?B[i-k].c:B[0].c; };

    // ---- E2 VOL-TARGET DONCHIAN (the prize) ---------------------------------
    auto voldon=[&](int dn,double slatr)->Stats{
        Stats s; bool in=false; double e=0,slx=0,sz=1; int ei=0;
        for(int i=dn+1;i<N;i++){
            double hi=0,lo=1e9; for(int k=i-dn;k<i;k++){hi=std::max(hi,B[k].h);lo=std::min(lo,B[k].l);}
            bool bull=B[i].c>cback(i,120);
            if(!in){ if(bull&&B[i].c>hi&&atr[i]>0){ e=B[i].c+HS; sz=10.0/atr[i]; slx=e-slatr*atr[i]; in=true; ei=i; } }
            else { double x=0; bool ex=false;
                if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
                if(ex){ s.add(pl_long(e,x)*sz,B[ei].ts); in=false; } }
        }
        return s;
    };

    // ---- multi-TF confluence: H4 trend (close>4*30 bar ago) + H1 Donchian entry
    auto mtf=[&](int dn,double slatr)->Stats{
        Stats s; bool in=false; double e=0,slx=0,sz=1; int ei=0;
        for(int i=dn+1;i<N;i++){
            double hi=0,lo=1e9; for(int k=i-dn;k<i;k++){hi=std::max(hi,B[k].h);lo=std::min(lo,B[k].l);}
            bool h4up = B[i].c>cback(i,120) && B[i].c>cback(i,40);   // multi-horizon up
            if(!in){ if(h4up&&B[i].c>hi&&atr[i]>0){ e=B[i].c+HS; sz=10.0/atr[i]; slx=e-slatr*atr[i]; in=true; ei=i;} }
            else{ double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
                if(ex){s.add(pl_long(e,x)*sz,B[ei].ts);in=false;} }
        }
        return s;
    };

    // ---- vol-expansion entry: enter long when ATR jumps > X*median-ATR AND up
    auto volexp=[&](double mult,double slatr,int hold)->Stats{
        Stats s; int i=200;
        while(i<N){
            double med=0; { std::vector<double> w(atr.begin()+i-100,atr.begin()+i); std::nth_element(w.begin(),w.begin()+50,w.end()); med=w[50]; }
            bool bull=B[i].c>cback(i,120);
            if(bull && med>0 && atr[i]>mult*med && B[i].c>B[i-1].c){
                double e=B[i].c+HS, slx=e-slatr*atr[i]; double x=0; int j;
                for(j=i+1;j<N&&j<=i+hold;j++){ if(B[j].l<=slx){x=slx-HS;break;} }
                if(!x){ j=std::min(N-1,i+hold); x=B[j].c-HS; }
                s.add(pl_long(e,x),B[i].ts); i=j+1;
            } else i++;
        }
        return s;
    };

    // ---- Monday-gap: long at Mon first-bar open, exit Mon close (weekend gap continuation)
    auto mongap=[&]()->Stats{
        Stats s; for(int i=1;i<N;i++){
            if(B[i].dow==1 && B[i-1].dow!=1){   // first Monday bar
                double e=B[i].o+HS; int j=i; while(j<N&&B[j].dow==1)j++;
                s.add(pl_long(e,B[std::min(j-1,N-1)].c-HS),B[i].ts);
            }
        }
        return s;
    };

    // ---- OVERLAY: vol-target Donchian but ONLY enter on Mon/Tue (day-gated trend)
    auto don_daygated=[&](int dn,double slatr)->Stats{
        Stats s; bool in=false; double e=0,slx=0,sz=1; int ei=0;
        for(int i=dn+1;i<N;i++){
            double hi=0,lo=1e9; for(int k=i-dn;k<i;k++){hi=std::max(hi,B[k].h);lo=std::min(lo,B[k].l);}
            bool bull=B[i].c>cback(i,120); bool earlywk=(B[i].dow==1||B[i].dow==2);
            if(!in){ if(bull&&earlywk&&B[i].c>hi&&atr[i]>0){ e=B[i].c+HS; sz=10.0/atr[i]; slx=e-slatr*atr[i]; in=true; ei=i;} }
            else{ double x=0;bool ex=false; if(B[i].l<=slx){x=slx-HS;ex=true;} else if(B[i].c<lo){x=B[i].c-HS;ex=true;}
                if(ex){s.add(pl_long(e,x)*sz,B[ei].ts);in=false;} }
        }
        return s;
    };

    // ===================== A. DEEPEN VOL-TARGET DONCHIAN =====================
    printf("\n===== A. VOL-TARGET DONCHIAN -- finer grid + 6-block (1x cost) =====\n");
    COST_MULT=1.0;
    for(int dn:{15,20,30,40,55,70}) for(double sl:{1.5,2.0,2.5,3.0}){ char nm[40]; snprintf(nm,40,"vd%d_sl%.1f",dn,sl); report(nm,voldon(dn,sl)); }
    printf("\n----- COST-STRESS on vd40_sl2.0 and vd55_sl2.5 -----\n");
    for(double cm:{1.0,2.0,3.0}){ COST_MULT=cm; char nm[40];
        snprintf(nm,40,"vd40_sl2.0_%.0fx",cm); report(nm,voldon(40,2.0));
        snprintf(nm,40,"vd55_sl2.5_%.0fx",cm); report(nm,voldon(55,2.5)); }
    COST_MULT=1.0;

    // ===================== overlays (#2/#3 day & session on trend) ===========
    printf("\n===== OVERLAY: Donchian gated to Mon/Tue (day x trend) =====\n");
    for(int dn:{20,40}){ char nm[40]; snprintf(nm,40,"don_montue_vd%d",dn); report(nm,don_daygated(dn,2.0)); }

    // ===================== B. WIDEN THE HUNT =================================
    printf("\n===== B. NEW EDGE HUNT =====\n");
    printf("--- multi-TF confluence (multi-horizon up + Donchian) ---\n");
    for(int dn:{20,40}){ char nm[40]; snprintf(nm,40,"mtf_vd%d",dn); report(nm,mtf(dn,2.0)); }
    printf("--- vol-expansion entry (ATR jump + up) ---\n");
    for(double m:{1.3,1.6,2.0}){ char nm[40]; snprintf(nm,40,"volexp_%.1fx",m); report(nm,volexp(m,2.0,24)); }
    printf("--- Monday-gap (long Mon open->close) ---\n");
    report("monday_gap",mongap());
    printf("\nDONE\n");
    return 0;
}
