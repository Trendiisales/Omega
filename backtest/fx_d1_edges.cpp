// =============================================================================
// fx_d1_edges.cpp -- broad HONEST FX edge scan on D1 (S43). Tests untested
// FX-native angles beyond carry/cross-RV/trend. NO LIES policy:
//   * signals use only CLOSED prior bars (no lookahead) — entry at bar i close,
//     exit at a later bar close.
//   * realistic per-pair retail spread cost (2*hs_bps round trip).
//   * WF: 50% time split, both halves must be positive.
//   * 6 equal blocks, >=5 positive for robustness.
//   * COST STRESS 3x printed for every family.
//   * EVERY family printed (winners AND losers) — no cherry-picking.
// Families: turn-of-month, day-of-week, weekly-range breakout, RSI(2) reversion,
//   Bollinger(20,2) reversion. Aggregated across 11 Dukascopy D1 pairs.
//
// BUILD: c++ -std=c++17 -O2 backtest/fx_d1_edges.cpp -o backtest/fx_d1_edges
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>

struct Bar { int64_t day; double o,h,l,c; int wday, mday, dim; };  // wday 0=Sun, mday=day-of-month, dim=days-in-month

static int days_in_month(int y,int m){
    static const int d[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(m==2 && ((y%4==0&&y%100!=0)||y%400==0)) return 29;
    return d[m-1];
}

struct Pair {
    std::string name; const char* path; double hs_bps;
    std::vector<Bar> b;
    bool load(){
        FILE* f=fopen(path,"r"); if(!f) return false;
        char ln[256]; bool first=true;
        while(fgets(ln,sizeof ln,f)){
            if(first){first=false; if(ln[0]<'0'||ln[0]>'9') continue;}
            double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
            if(c<=0) continue;
            time_t t=(time_t)(ts/1000.0); struct tm g; gmtime_r(&t,&g);
            if(g.tm_wday==6) continue;   // drop flat Saturday artifact (corrupts hold-N exits)
            Bar bb; bb.day=(int64_t)(ts/1000.0); bb.o=o;bb.h=h;bb.l=l;bb.c=c;
            bb.wday=g.tm_wday; bb.mday=g.tm_mday; bb.dim=days_in_month(g.tm_year+1900,g.tm_mon+1);
            b.push_back(bb);
        }
        fclose(f); return b.size()>260;
    }
};

// trade record for aggregation
struct Agg {
    int n=0,win=0; double gw=0,gl=0,g=0;
    double h1g=0,h2g=0; int h1n=0,h2n=0;
    double blk[6]={0}; int64_t tmin=0,tmax=1;
    void setspan(int64_t a,int64_t b){tmin=a;tmax=b;}
    void add(double pnl,int64_t ts,int64_t split){
        n++; g+=pnl; if(pnl>0){win++;gw+=pnl;}else gl+=-pnl;
        if(ts<split){h1g+=pnl;h1n++;}else{h2g+=pnl;h2n++;}
        int bi=(int)(6.0*(ts-tmin)/(double)(tmax-tmin+1)); if(bi<0)bi=0;if(bi>5)bi=5; blk[bi]+=pnl;
    }
    double pf()const{return gl>0?gw/gl:(gw>0?99:0);}
    double wr()const{return n?100.0*win/n:0;}
    int blkpos()const{int p=0;for(int i=0;i<6;i++)if(blk[i]>0)p++;return p;}
    bool robust()const{return h1g>0&&h2g>0&&h1n>=10&&h2n>=10&&blkpos()>=5&&n>=30;}
};

static void report(const char* fam,const Agg& a,double cost3_net){
    const char* fl=a.robust()?" *** ROBUST":((a.h1g>0&&a.h2g>0)?" ** WF+":(a.g>0?" (1-sided)":""));
    printf("  %-26s n=%-5d WR=%4.1f PF=%.2f net=%+8.0fbp [3x:%+7.0f] H1=%+6.0f H2=%+6.0f blk=%d/6%s\n",
           fam,a.n,a.wr(),a.pf(),a.g,cost3_net,a.h1g,a.h2g,a.blkpos(),fl);
}

// ---- signal functions (NO lookahead: use bars <= i) ----
// Turn-of-month: long in last 2 / first 2 calendar days (USD rebalance flow).
static double TOM(const std::vector<Bar>&b,int i){
    int md=b[i].mday, dim=b[i].dim;
    if(md>=dim-1 || md<=1) return +1.0;
    return 0.0;
}
static double MON(const std::vector<Bar>&b,int i){ return b[i].wday==1? +1.0:0.0; }
static double FRI(const std::vector<Bar>&b,int i){ return b[i].wday==5? +1.0:0.0; }
// Weekly-range breakout: close > 5-bar high -> long; < 5-bar low -> short.
static double WRB(const std::vector<Bar>&b,int i){
    double hh=b[i-1].h, ll=b[i-1].l; for(int k=i-5;k<i;k++){if(b[k].h>hh)hh=b[k].h;if(b[k].l<ll)ll=b[k].l;}
    if(b[i].c>hh) return +1.0; if(b[i].c<ll) return -1.0; return 0.0;
}
// RSI(2) reversion: RSI2<10 long, >90 short (Connors).
static double RSI2(const std::vector<Bar>&b,int i){
    double au=0,ad=0; for(int k=i-1;k>=i-2;k--){ double d=b[k+1].c-b[k].c; if(d>0)au+=d;else ad+=-d; }
    double rs= ad>0? au/ad : (au>0?99:1); double rsi=100-100/(1+rs);
    if(rsi<10) return +1.0; if(rsi>90) return -1.0; return 0.0;
}
// Bollinger(20,2) reversion: close < lower -> long, > upper -> short.
static double BBrev(const std::vector<Bar>&b,int i){
    double s=0; for(int k=i-20;k<i;k++)s+=b[k].c; double m=s/20;
    double v=0; for(int k=i-20;k<i;k++){double d=b[k].c-m;v+=d*d;} double sd=std::sqrt(v/19);
    if(sd<=0)return 0; double z=(b[i].c-m)/sd;
    if(z<-2.0)return +1.0; if(z>2.0)return -1.0; return 0.0;
}
// Kaufman efficiency ratio over N (0=pure noise/range, 1=pure trend).
static double effratio(const std::vector<Bar>&b,int i,int N){
    double net=std::fabs(b[i].c-b[i-N].c), tot=0; for(int k=i-N+1;k<=i;k++) tot+=std::fabs(b[k].c-b[k-1].c);
    return tot>0? net/tot : 0.0;
}
// Vol-compression breakout: if PRIOR bar's range is the narrowest of last 7
// (coil), trade the breakout of that bar's high/low (FX coils then expands).
static double VCB(const std::vector<Bar>&b,int i){
    double r1=b[i-1].h-b[i-1].l, mn=1e18; for(int k=i-7;k<i;k++){double r=b[k].h-b[k].l; if(r<mn)mn=r;}
    if(r1>mn+1e-12) return 0.0;                     // prior bar not the narrowest -> no coil
    if(b[i].c>b[i-1].h) return +1.0; if(b[i].c<b[i-1].l) return -1.0; return 0.0;
}
// Regime-conditional MR: Bollinger reversion ONLY when ranging (ER<0.35).
// (plain BB rev was dead because it also fired in trends — top traders gate MR by regime.)
static double RegimeMR(const std::vector<Bar>&b,int i){
    if(effratio(b,i,10)>=0.35) return 0.0;          // trending -> no MR
    return BBrev(b,i);
}

int main(){
    printf("FX D1 BROAD EDGE SCAN (S43) -- honest: no-lookahead, real cost, WF+6block, 3x-stress, ALL printed\n");
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Pair> P = {
        {"EURUSD",DK("eurusd"),0.5},{"GBPUSD",DK("gbpusd"),0.6},{"AUDUSD",DK("audusd"),0.8},
        {"NZDUSD",DK("nzdusd"),1.0},{"USDJPY",DK("usdjpy"),0.7},{"USDCAD",DK("usdcad"),0.8},
        {"USDCHF",DK("usdchf"),0.9},{"EURGBP",DK("eurgbp"),0.8},{"AUDNZD",DK("audnzd"),1.5},
        {"EURJPY",DK("eurjpy"),1.0},{"GBPJPY",DK("gbpjpy"),1.3},
    };
    for(auto it=P.begin();it!=P.end();){ if(!it->load()){printf("[skip %s]\n",it->name.c_str()); it=P.erase(it);} else ++it; }
    if(P.empty()){printf("no data\n");return 1;}
    int64_t TMIN=P[0].b.front().day,TMAX=P[0].b.back().day;
    for(auto&p:P){ if(p.b.front().day<TMIN)TMIN=p.b.front().day; if(p.b.back().day>TMAX)TMAX=p.b.back().day; }
    int64_t SPLIT=TMIN+(TMAX-TMIN)/2;
    printf("pairs=%zu span=%ld..%ld\n\n",P.size(),(long)TMIN,(long)TMAX);

    auto bp=[&](double e,double x,double dir,double hs){ return dir*(x-e)/e*1e4 - 2.0*hs; };

    // helper: run a family that, per pair, opens dir at bar i close and exits at bar i+hold close.
    // sigfn returns dir (+1 long / -1 short / 0 none) given pair history up to & incl i (closed).
    auto run_holdN=[&](const char* name, int hold, double cm,
                       double(*sig)(const std::vector<Bar>&,int)){
        Agg a; a.setspan(TMIN,TMAX);
        for(auto&p:P){
            double hs=p.hs_bps;
            for(int i=20;i+hold<(int)p.b.size();){
                double dir=sig(p.b,i);
                if(dir==0){++i;continue;}
                double e=p.b[i].c, x=p.b[i+hold].c;
                a.add(bp(e,x,dir,hs*cm),p.b[i].day,SPLIT);
                i+=hold; // non-overlapping
            }
        }
        return a;
    };

    printf("EDGE                     trades  WR   PF   net(1x)    (3x)     H1     H2    blocks\n");
    struct F{const char*name;int hold;double(*fn)(const std::vector<Bar>&,int);};
    F fams[]={
        {"TurnOfMonth (long, h3)",3,TOM},
        {"Monday (long, h1)",1,MON},
        {"Friday (long, h1)",1,FRI},
        {"WeeklyRangeBreak (h5)",5,WRB},
        {"RSI2 reversion (h3)",3,RSI2},
        {"Bollinger2 reversion (h5)",5,BBrev},
        {"VolCompressBreakout (h3)",3,VCB},
        {"RegimeMR (ER<.35, h5)",5,RegimeMR},
    };
    for(auto&f:fams){
        Agg a1=run_holdN(f.name,f.hold,1.0,f.fn);
        Agg a3=run_holdN(f.name,f.hold,3.0,f.fn);
        report(f.name,a1,a3.g);
    }
    printf("\n*** ROBUST = both halves + 5/6 blocks + n>=30, survives at 1x (check 3x col stays +).\n");
    printf("Honest note: aggregate across 11 pairs. A '(1-sided)' or blank flag = NOT a real edge.\n");
    return 0;
}
