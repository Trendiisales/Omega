// =============================================================================
// index_d1_edges.cpp -- broad HONEST index edge scan on D1 (S44). Ported from
// fx_d1_edges.cpp. Indices DIFFER from FX: they have drift (equity risk premium)
// and trend, so TSMOM/momentum is the PRIME candidate (it failed on FX).
// NO LIES policy (same as FX scan):
//   * signals use only CLOSED prior bars (no lookahead).
//   * realistic per-symbol half-spread cost in bps (2*hs round trip).
//   * WF: 50% time split, both halves must be positive.
//   * 6 equal-time blocks, >=5 positive for robustness.
//   * COST STRESS 3x printed for every family.
//   * EVERY family printed (winners AND losers) -- no cherry-picking.
//   * WEEKDAY DISTRIBUTION printed per symbol (lesson #1: catch flat-Sat artifact).
// D1 bars are resampled from the h1 merged CSVs (ts in seconds, o,h,l,c).
// Families: TSMOM(20/40/60/120 hold-to-flip), TurnOfMonth, day-of-week,
//   weekly-range breakout, RSI2 reversion, Bollinger reversion, RegimeMR.
//
// BUILD: c++ -std=c++17 -O2 backtest/index_d1_edges.cpp -o backtest/index_d1_edges
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <map>

struct Bar { int64_t day; double o,h,l,c; int wday, mday, dim; };  // wday 0=Sun

static int days_in_month(int y,int m){
    static const int d[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(m==2 && ((y%4==0&&y%100!=0)||y%400==0)) return 29;
    return d[m-1];
}

struct Sym {
    std::string name; const char* path; double hs_bps;
    std::vector<Bar> b;
    int wcount[7]={0};
    // Load Dukascopy D1 CSV (header: timestamp,open,high,low,close; ts in ms).
    // Already proper trading-day bars -> no Sunday-stub artifact, no resampling.
    bool load(){
        FILE* f=fopen(path,"r"); if(!f){return false;}
        char ln[256]; bool first=true;
        while(fgets(ln,sizeof ln,f)){
            if(first){first=false; if(ln[0]<'0'||ln[0]>'9') continue;}
            double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
            if(c<=0) continue;
            time_t t=(time_t)(ts/1000.0); struct tm g; gmtime_r(&t,&g);
            wcount[g.tm_wday]++;
            if(g.tm_wday==6 || g.tm_wday==0) continue;  // drop weekend stubs (lesson #1)
            Bar bb; bb.day=(int64_t)(ts/1000.0); bb.o=o;bb.h=h;bb.l=l;bb.c=c;
            bb.wday=g.tm_wday; bb.mday=g.tm_mday; bb.dim=days_in_month(g.tm_year+1900,g.tm_mon+1);
            b.push_back(bb);
        }
        fclose(f); return b.size()>120;
    }
};

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
    printf("  %-28s n=%-5d WR=%4.1f PF=%.2f net=%+8.0fbp [3x:%+7.0f] H1=%+6.0f H2=%+6.0f blk=%d/6%s\n",
           fam,a.n,a.wr(),a.pf(),a.g,cost3_net,a.h1g,a.h2g,a.blkpos(),fl);
}

// ---- signals (NO lookahead: use bars <= i) ----
static double TOM(const std::vector<Bar>&b,int i){
    int md=b[i].mday, dim=b[i].dim;
    if(md>=dim-1 || md<=1) return +1.0;
    return 0.0;
}
static double MON(const std::vector<Bar>&b,int i){ return b[i].wday==1? +1.0:0.0; }
static double TUE(const std::vector<Bar>&b,int i){ return b[i].wday==2? +1.0:0.0; }
static double FRI(const std::vector<Bar>&b,int i){ return b[i].wday==5? +1.0:0.0; }
static double WRB(const std::vector<Bar>&b,int i){
    double hh=b[i-1].h, ll=b[i-1].l; for(int k=i-5;k<i;k++){if(b[k].h>hh)hh=b[k].h;if(b[k].l<ll)ll=b[k].l;}
    if(b[i].c>hh) return +1.0; if(b[i].c<ll) return -1.0; return 0.0;
}
static double RSI2(const std::vector<Bar>&b,int i){
    double au=0,ad=0; for(int k=i-1;k>=i-2;k--){ double d=b[k+1].c-b[k].c; if(d>0)au+=d;else ad+=-d; }
    double rs= ad>0? au/ad : (au>0?99:1); double rsi=100-100/(1+rs);
    if(rsi<10) return +1.0; if(rsi>90) return -1.0; return 0.0;
}
static double BBrev(const std::vector<Bar>&b,int i){
    double s=0; for(int k=i-20;k<i;k++)s+=b[k].c; double m=s/20;
    double v=0; for(int k=i-20;k<i;k++){double d=b[k].c-m;v+=d*d;} double sd=std::sqrt(v/19);
    if(sd<=0)return 0; double z=(b[i].c-m)/sd;
    if(z<-2.0)return +1.0; if(z>2.0)return -1.0; return 0.0;
}
static double effratio(const std::vector<Bar>&b,int i,int N){
    double net=std::fabs(b[i].c-b[i-N].c), tot=0; for(int k=i-N+1;k<=i;k++) tot+=std::fabs(b[k].c-b[k-1].c);
    return tot>0? net/tot : 0.0;
}
static double RegimeMR(const std::vector<Bar>&b,int i){
    if(effratio(b,i,10)>=0.35) return 0.0;
    return BBrev(b,i);
}
// TSMOM: sign of N-day return -> dir. (hold handled by caller via lookback param)
template<int N> static double TSMOM(const std::vector<Bar>&b,int i){
    if(i<N) return 0.0;
    double r=b[i].c-b[i-N].c;
    if(r>0) return +1.0; if(r<0) return -1.0; return 0.0;
}
// Long-only TSMOM variants (equity drift -> long bias may beat symmetric)
template<int N> static double TSMOM_L(const std::vector<Bar>&b,int i){
    if(i<N) return 0.0;
    return b[i].c>b[i-N].c ? +1.0 : 0.0;
}

int main(){
    printf("INDEX D1 BROAD EDGE SCAN (S44) -- honest: no-lookahead, real cost, WF+6block, 3x-stress, ALL printed\n");
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P = {
        {"SPX500", DK("usa500idxusd"),  1.0},
        {"NAS100", DK("usatechidxusd"), 1.5},
        {"GER40",  DK("deuidxeur"),     1.5},
        {"US30",   DK("usa30idxusd"),   1.0},
        {"UK100",  DK("gbridxgbp"),     1.5},
        {"ESTX50", DK("eusidxeur"),     1.5},
    };
    for(auto it=P.begin();it!=P.end();){ if(!it->load()){printf("[skip %s]\n",it->name.c_str()); it=P.erase(it);} else ++it; }
    if(P.empty()){printf("no data\n");return 1;}
    printf("\n=== WEEKDAY DISTRIBUTION (D1 bars, lesson #1; wday 0=Sun..6=Sat, Sat dropped) ===\n");
    for(auto&p:P){
        printf("  %-7s bars=%-5zu  Sun=%d Mon=%d Tue=%d Wed=%d Thu=%d Fri=%d Sat=%d(dropped)\n",
            p.name.c_str(),p.b.size(),p.wcount[0],p.wcount[1],p.wcount[2],p.wcount[3],p.wcount[4],p.wcount[5],p.wcount[6]);
    }
    int64_t TMIN=P[0].b.front().day,TMAX=P[0].b.back().day;
    for(auto&p:P){ if(p.b.front().day<TMIN)TMIN=p.b.front().day; if(p.b.back().day>TMAX)TMAX=p.b.back().day; }
    int64_t SPLIT=TMIN+(TMAX-TMIN)/2;
    printf("\nsymbols=%zu span=%ld..%ld (%.1f yrs)\n\n",P.size(),(long)TMIN,(long)TMAX,(TMAX-TMIN)/(365.25*86400));

    auto bp=[&](double e,double x,double dir,double hs){ return dir*(x-e)/e*1e4 - 2.0*hs; };

    auto run_holdN=[&](int hold, double cm, double(*sig)(const std::vector<Bar>&,int)){
        Agg a; a.setspan(TMIN,TMAX);
        for(auto&p:P){
            double hs=p.hs_bps;
            for(int i=120;i+hold<(int)p.b.size();){
                double dir=sig(p.b,i);
                if(dir==0){++i;continue;}
                double e=p.b[i].c, x=p.b[i+hold].c;
                a.add(bp(e,x,dir,hs*cm),p.b[i].day,SPLIT);
                i+=hold; // non-overlapping
            }
        }
        return a;
    };

    printf("EDGE                          trades  WR   PF   net(1x)    (3x)     H1     H2    blocks\n");
    struct F{const char*name;int hold;double(*fn)(const std::vector<Bar>&,int);};
    F fams[]={
        {"TSMOM20 sym (h20)",     20, TSMOM<20>},
        {"TSMOM40 sym (h40)",     40, TSMOM<40>},
        {"TSMOM60 sym (h60)",     60, TSMOM<60>},
        {"TSMOM120 sym (h120)",  120, TSMOM<120>},
        {"TSMOM20 long (h20)",    20, TSMOM_L<20>},
        {"TSMOM40 long (h40)",    40, TSMOM_L<40>},
        {"TSMOM60 long (h60)",    60, TSMOM_L<60>},
        {"TurnOfMonth (long, h3)", 3, TOM},
        {"Monday (long, h1)",      1, MON},
        {"Tuesday (long, h1)",     1, TUE},
        {"Friday (long, h1)",      1, FRI},
        {"WeeklyRangeBreak (h5)",  5, WRB},
        {"RSI2 reversion (h3)",    3, RSI2},
        {"Bollinger2 rev (h5)",    5, BBrev},
        {"RegimeMR (ER<.35, h5)",  5, RegimeMR},
    };
    for(auto&f:fams){
        Agg a1=run_holdN(f.hold,1.0,f.fn);
        Agg a3=run_holdN(f.hold,3.0,f.fn);
        report(f.name,a1,a3.g);
    }
    printf("\n*** ROBUST = both halves + 5/6 blocks + n>=30, survives 1x (check 3x col stays +).\n");
    printf("Honest: aggregate across %zu indices. '(1-sided)'/blank = NOT a real edge.\n",P.size());
    // per-symbol breakdown for the headline TSMOM families
    printf("\n=== PER-SYMBOL (TSMOM40 sym, TSMOM60 sym) ===\n");
    for(auto&p:P){
        for(int N : {40,60}){
            Agg a; a.setspan(TMIN,TMAX); int hold=N;
            double(*sig)(const std::vector<Bar>&,int) = (N==40)?TSMOM<40>:TSMOM<60>;
            for(int i=120;i+hold<(int)p.b.size();){ double dir=sig(p.b,i); if(dir==0){++i;continue;}
                a.add(bp(p.b[i].c,p.b[i+hold].c,dir,p.hs_bps),p.b[i].day,SPLIT); i+=hold; }
            char nm[48]; snprintf(nm,sizeof nm,"%s TSMOM%d",p.name.c_str(),N);
            Agg a3; a3.setspan(TMIN,TMAX);
            for(int i=120;i+hold<(int)p.b.size();){ double dir=sig(p.b,i); if(dir==0){++i;continue;}
                a3.add(bp(p.b[i].c,p.b[i+hold].c,dir,p.hs_bps*3),p.b[i].day,SPLIT); i+=hold; }
            report(nm,a,a3.g);
        }
    }
    // ---- DRIFT CONTROL: per-weekday mean 1-day fwd return vs daily baseline ----
    // If "Tuesday edge" were just equity drift, EVERY weekday would be positive
    // and equal. The EDGE is the excess over the all-day mean. Monday<baseline,
    // Tue/Fri>baseline => genuine day-of-week structure, not beta.
    printf("\n=== DRIFT CONTROL: weekday mean next-day return (bp, net of 1x cost), excess vs daily baseline ===\n");
    {
        double wsum[6]={0}; int wn[6]={0}; double allsum=0; int alln=0;
        for(auto&p:P){
            for(int i=0;i+1<(int)p.b.size();i++){
                double r=(p.b[i+1].c-p.b[i].c)/p.b[i].c*1e4 - 2.0*p.hs_bps; // hold 1 day, cost
                int wd=p.b[i].wday; if(wd<1||wd>5) continue;
                wsum[wd]+=r; wn[wd]++; allsum+=r; alln++;
            }
        }
        double base=alln?allsum/alln:0;
        const char* nm[6]={"","Mon","Tue","Wed","Thu","Fri"};
        printf("  baseline (any weekday) mean = %+.2f bp/day  (n=%d)\n",base,alln);
        for(int wd=1;wd<=5;wd++){
            double m=wn[wd]?wsum[wd]/wn[wd]:0;
            printf("  %-4s mean=%+6.2f bp  excess=%+6.2f bp  n=%d  %s\n",
                   nm[wd],m,m-base,wn[wd], (m-base)>0?"<-- above drift":"");
        }
        printf("  (Monday below baseline + Tue/Fri above = real day-of-week structure, not pure drift)\n");
        // per-symbol breadth: is Tue/Fri-long broad or one-symbol-driven?
        printf("  --- per-symbol weekday mean next-day return (bp, net 1x), Mon..Fri ---\n");
        for(auto&p:P){
            double s[6]={0}; int c[6]={0};
            for(int i=0;i+1<(int)p.b.size();i++){
                double r=(p.b[i+1].c-p.b[i].c)/p.b[i].c*1e4 - 2.0*p.hs_bps;
                int wd=p.b[i].wday; if(wd<1||wd>5)continue; s[wd]+=r; c[wd]++;
            }
            printf("  %-7s Mon=%+6.2f Tue=%+6.2f Wed=%+6.2f Thu=%+6.2f Fri=%+6.2f\n",
                   p.name.c_str(), c[1]?s[1]/c[1]:0,c[2]?s[2]/c[2]:0,c[3]?s[3]/c[3]:0,c[4]?s[4]/c[4]:0,c[5]?s[5]/c[5]:0);
        }
    }
    // ---- BETA CONTROL: long-only trend vs always-long (buy&hold) per time-in-market ----
    printf("\n=== BETA CONTROL: long-trend (TSMOM60) capture vs buy&hold, same bars ===\n");
    {
        for(auto&p:P){
            double bh=0; int bhn=0;            // buy&hold: every day long
            double tr=0; int trn=0; int inmkt=0; // trend: long only when c>c[-60]
            for(int i=60;i+1<(int)p.b.size();i++){
                double r=(p.b[i+1].c-p.b[i].c)/p.b[i].c*1e4;
                bh+=r; bhn++;
                if(p.b[i].c>p.b[i-60].c){ tr+=r-2.0*p.hs_bps/60.0; trn++; inmkt++; }
            }
            printf("  %-7s buy&hold=%+8.0fbp (%d d)   trend-long=%+8.0fbp (in-mkt %d d, %.0f%%)   %s\n",
                   p.name.c_str(),bh,bhn,tr,trn,100.0*inmkt/bhn,
                   tr> bh*inmkt/(double)bhn ? "trend BEATS hold/time" : "trend < hold/time (just beta)");
        }
        printf("  (trend earns its keep only if it beats buy&hold scaled by its time-in-market)\n");
    }
    return 0;
}
