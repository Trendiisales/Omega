// =============================================================================
// index_seasonal_sharpe.cpp (S44) -- pin down the day-of-week seasonality edge
// with UNAMBIGUOUS labeling + Sharpe/DD/vs-buy&hold + overnight/intraday split.
//
// Convention (standard, no ambiguity): the return ON weekday d is
//   r_d = close_d / close_{d-1} - 1   (entry at prior close, exit at close_d).
// Monday's r_d spans the weekend (Fri close -> Mon close). We further split:
//   overnight = open_d/close_{d-1} - 1   (gap, carries the documented night drift)
//   intraday  = close_d/open_d - 1       (regular session)
//   r_d = (1+overnight)(1+intraday)-1
// Then build sleeves (long on the strong weekdays), report annualized Sharpe,
// max drawdown, %time-in-market, and the SAME-bars buy&hold benchmark.
// Honest: real bps cost per round trip, WF 50% split, 6-block, all printed.
//
// BUILD: c++ -std=c++17 -O2 backtest/index_seasonal_sharpe.cpp -o backtest/index_seasonal_sharpe
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>

struct Bar { int64_t day; double o,h,l,c; int wday, mday, dim; };
static int dim_(int y,int m){static const int d[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(m==2&&((y%4==0&&y%100!=0)||y%400==0))return 29; return d[m-1];}

struct Sym {
    std::string name; const char* path; double hs_bps; std::vector<Bar> b;
    bool load(){ FILE* f=fopen(path,"r"); if(!f)return false; char ln[256]; bool first=true;
        while(fgets(ln,sizeof ln,f)){ if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue; if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0); struct tm g; gmtime_r(&t,&g);
            if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb; bb.day=(int64_t)(ts/1000.0); bb.o=o;bb.h=h;bb.l=l;bb.c=c;
            bb.wday=g.tm_wday; bb.mday=g.tm_mday; bb.dim=dim_(g.tm_year+1900,g.tm_mon+1); b.push_back(bb);
        } fclose(f); return b.size()>120; }
};

// annualized Sharpe + maxDD from a daily PnL-fraction series (0 on flat days)
static void stats(const std::vector<double>& r, double& sharpe, double& maxdd, double& tot){
    double m=0; for(double x:r)m+=x; m/= (r.empty()?1:r.size());
    double v=0; for(double x:r){double d=x-m; v+=d*d;} v/=(r.size()>1?r.size()-1:1);
    double sd=std::sqrt(v); sharpe= sd>0? m/sd*std::sqrt(252.0):0;
    double eq=0,peak=0; maxdd=0; tot=0; for(double x:r){ eq+=x; tot+=x; if(eq>peak)peak=eq; double dd=peak-eq; if(dd>maxdd)maxdd=dd; }
}

int main(){
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P = {
        {"SPX500",DK("usa500idxusd"),1.0},{"NAS100",DK("usatechidxusd"),1.5},
        {"GER40",DK("deuidxeur"),1.5},{"US30",DK("usa30idxusd"),1.0},
        {"UK100",DK("gbridxgbp"),1.5},{"ESTX50",DK("eusidxeur"),1.5},
    };
    for(auto it=P.begin();it!=P.end();){ if(!it->load()){printf("[skip %s]\n",it->name.c_str());it=P.erase(it);}else ++it; }
    printf("INDEX DAY-OF-WEEK SEASONALITY -- unambiguous (return ON day d) + overnight/intraday split\n\n");

    // ---- 1. weekday return decomposition, aggregate over symbols (bp, gross, no cost) ----
    const char* NM[6]={"","Mon","Tue","Wed","Thu","Fri"};
    double tot_r[6]={0},tot_on[6]={0},tot_id[6]={0}; int cnt[6]={0};
    for(auto&p:P) for(size_t i=1;i<p.b.size();i++){
        int wd=p.b[i].wday; if(wd<1||wd>5)continue;
        double r=(p.b[i].c/p.b[i-1].c-1)*1e4, on=(p.b[i].o/p.b[i-1].c-1)*1e4, id=(p.b[i].c/p.b[i].o-1)*1e4;
        tot_r[wd]+=r; tot_on[wd]+=on; tot_id[wd]+=id; cnt[wd]++;
    }
    printf("=== mean return ON weekday d (gross bp), entry prior close -> exit close_d ===\n");
    printf("  day   total_r   overnight   intraday    n     (Mon spans weekend)\n");
    for(int wd=1;wd<=5;wd++) printf("  %-4s %+8.2f  %+8.2f   %+8.2f   %d\n",
        NM[wd], tot_r[wd]/cnt[wd], tot_on[wd]/cnt[wd], tot_id[wd]/cnt[wd], cnt[wd]);
    printf("  >> the STRONG sessions and whether the edge is overnight (gap) or intraday\n\n");

    // ---- 2. sleeves: daily PnL series (0 on flat), real cost on trade days ----
    // candidate sleeves defined by which weekdays we go long (1) and short (-1).
    struct Sleeve{const char* name; int dir[6];}; // index by wday 1..5
    Sleeve sleeves[]={
        {"AlwaysLong (buy&hold)", {0, 1,1,1,1,1}},
        {"StrongDays-long",       {0, 0,1,0,0,1}}, // placeholder, set after we see table
        {"Long all but weak",     {0, 1,1,0,1,1}},
        {"Wed-short overlay",     {0, 1,1,-1,1,1}},
    };
    // We'll also auto-build "long the 2 best weekdays" from the table.
    int best1=1,best2=2; { double mx1=-1e9,mx2=-1e9;
        for(int wd=1;wd<=5;wd++){double m=tot_r[wd]/cnt[wd]; if(m>mx1){mx2=mx1;best2=best1;mx1=m;best1=wd;}else if(m>mx2){mx2=m;best2=wd;}}}
    sleeves[1].dir[best1]=1; sleeves[1].dir[best2]=1; for(int wd=1;wd<=5;wd++) if(wd!=best1&&wd!=best2) sleeves[1].dir[wd]=0;
    int worst=1; { double mn=1e9; for(int wd=1;wd<=5;wd++){double m=tot_r[wd]/cnt[wd]; if(m<mn){mn=m;worst=wd;}} }
    char s1name[64]; snprintf(s1name,sizeof s1name,"Long best-2 (%s+%s)",NM[best1],NM[best2]); sleeves[1].name=s1name;

    printf("=== SLEEVES: per-day equity, real cost, vs buy&hold. (best2=%s,%s worst=%s) ===\n",NM[best1],NM[best2],NM[worst]);
    printf("  %-26s netbp   Sharpe  maxDD(bp) %%time  H1bp   H2bp  blocks\n","sleeve");
    int64_t TMIN=P[0].b.front().day,TMAX=0; for(auto&p:P){if(p.b.front().day<TMIN)TMIN=p.b.front().day; if(p.b.back().day>TMAX)TMAX=p.b.back().day;}
    int64_t SPLIT=TMIN+(TMAX-TMIN)/2;
    int sleeve_idx=-1;
    for(auto&S:sleeves){ ++sleeve_idx;
        // PORT_DUMP: per-trade stream for the best-2 sleeve (sleeve index 1) on SPX500.
        FILE* pd=nullptr; if(sleeve_idx==1 && getenv("PORT_DUMP")) pd=fopen(getenv("PORT_DUMP"),"w");
        // build per-symbol daily PnL fraction series, then pool by aligning on index (equal-weight)
        std::vector<double> series; double h1=0,h2=0; double blk[6]={0};
        for(auto&p:P){
            for(size_t i=1;i<p.b.size();i++){
                int wd=p.b[i].wday; if(wd<1||wd>5)continue; int d=S.dir[wd]; if(d==0){series.push_back(0);continue;}
                double r=(p.b[i].c/p.b[i-1].c-1)*1e4*d - 2.0*p.hs_bps; // bp net
                series.push_back(r/1e4); // fraction for sharpe
                if(pd && p.name=="SPX500") fprintf(pd,"%lld,%.4f\n",(long long)p.b[i].day,(r/1e4)*1000.0); // $1000 notional/trade
                if(p.b[i].day<SPLIT)h1+=r; else h2+=r;
                int bi=(int)(6.0*(p.b[i].day-TMIN)/(double)(TMAX-TMIN+1)); if(bi<0)bi=0;if(bi>5)bi=5; blk[bi]+=r;
            }
        }
        if(pd){fclose(pd);}
        double sh,dd,tt; stats(series,sh,dd,tt);
        int bp=0; for(int i=0;i<6;i++)if(blk[i]>0)bp++;
        int trades=0; for(double x:series)if(x!=0)trades++;
        printf("  %-26s %+7.0f  %+5.2f  %8.0f  %4.0f  %+6.0f %+6.0f  %d/6\n",
            S.name, tt*1e4, sh, dd*1e4, 100.0*trades/series.size(), h1,h2,bp);
    }
    printf("\n  Sharpe annualized (252d), on per-day series incl flat days (honest time-in-market).\n");
    printf("  A seasonal sleeve EARNS ITS KEEP if Sharpe > buy&hold AND maxDD << buy&hold.\n");

    // ---- 2b. TURN-OF-MONTH sleeve + COMBINED with day-of-week (research Tier-1) ----
    // ToM window = last 4 + first 3 TRADING days of each month (Xu-McConnell).
    // Tag each bar with trading-day-of-month and trading-days-remaining-in-month.
    for(auto&p:P){
        // forward pass: tdom (1-based); detect month change via mday rollover
        std::vector<int> tdom(p.b.size()), trem(p.b.size());
        int td=0; int prev_mon=-1;
        for(size_t i=0;i<p.b.size();i++){
            time_t t=(time_t)p.b[i].day; struct tm g; gmtime_r(&t,&g);
            if(g.tm_mon!=prev_mon){td=0;prev_mon=g.tm_mon;}
            tdom[i]=++td;
        }
        // backward pass: days remaining in month
        int rd=0; prev_mon=-1;
        for(int i=(int)p.b.size()-1;i>=0;i--){
            time_t t=(time_t)p.b[i].day; struct tm g; gmtime_r(&t,&g);
            if(g.tm_mon!=prev_mon){rd=0;prev_mon=g.tm_mon;}
            trem[i]=rd++;
        }
        // Store ToM flag by overwriting mday: 1 if in last-3/first-3 window else 0 (mday unused downstream).
        for(size_t i=0;i<p.b.size();i++) p.b[i].mday = (tdom[i]<=3 || trem[i]<=3) ? 1 : 0;
    }
    {
        struct Sl{const char* name; bool tom; bool dow;};
        Sl sl[]={{"TurnOfMonth-long",true,false},{"DayOfWeek best-2",false,true},{"COMBINED (ToM OR DoW)",true,true}};
        printf("\n=== ToM + COMBINED sleeves (Sharpe/DD/vs-hold) ===\n");
        printf("  %-26s netbp   Sharpe  maxDD(bp) %%time  H1bp   H2bp  blocks\n","sleeve");
        for(auto&S:sl){
            std::vector<double> series; double h1=0,h2=0,blk[6]={0};
            for(auto&p:P) for(size_t i=1;i<p.b.size();i++){
                int wd=p.b[i].wday; if(wd<1||wd>5)continue;
                bool inT = S.tom && p.b[i].mday==1;
                bool inD = S.dow && sleeves[1].dir[wd]==1;
                if(!(inT||inD)){series.push_back(0);continue;}
                double r=(p.b[i].c/p.b[i-1].c-1)*1e4 - 2.0*p.hs_bps;
                series.push_back(r/1e4);
                if(p.b[i].day<SPLIT)h1+=r;else h2+=r;
                int bi=(int)(6.0*(p.b[i].day-TMIN)/(double)(TMAX-TMIN+1));if(bi<0)bi=0;if(bi>5)bi=5;blk[bi]+=r;
            }
            double sh,dd,tt;stats(series,sh,dd,tt);int bp2=0;for(int i=0;i<6;i++)if(blk[i]>0)bp2++;
            int tr=0;for(double x:series)if(x!=0)tr++;
            printf("  %-26s %+7.0f  %+5.2f  %8.0f  %4.0f  %+6.0f %+6.0f  %d/6\n",S.name,tt*1e4,sh,dd*1e4,100.0*tr/series.size(),h1,h2,bp2);
        }
    }

    // ---- 3. REGIME conditioning: is the best-2 seasonal edge a bull artifact? ----
    // Split the best-2 sleeve's trade returns by (a) price vs own 200d SMA at entry,
    // (b) 20d realized-vol tercile. A real edge should persist in BOTH regimes.
    printf("\n=== REGIME ROBUSTNESS of best-2 seasonal sleeve (%s+%s long) ===\n",NM[best1],NM[best2]);
    {
        double up_g=0,dn_g=0; int up_n=0,dn_n=0;          // 200d SMA regime
        double lv_g=0,hv_g=0; int lv_n=0,hv_n=0;          // vol regime (median split)
        std::vector<double> vols;
        // first pass collect vols for median
        for(auto&p:P) for(size_t i=220;i<p.b.size();i++){ double v=0,m=0; for(int k=0;k<20;k++)m+=p.b[i-k].c; m/=20;
            for(int k=0;k<20;k++){double d=p.b[i-k].c-m;v+=d*d;} vols.push_back(std::sqrt(v/19)/p.b[i].c);}
        std::vector<double> sv=vols; size_t mid=sv.size()/2; std::nth_element(sv.begin(),sv.begin()+mid,sv.end());
        double vmed= sv.empty()?0:sv[mid];
        for(auto&p:P) for(size_t i=220;i<p.b.size();i++){
            int wd=p.b[i].wday; if(wd<1||wd>5)continue; int d=sleeves[1].dir[wd]; if(d==0)continue;
            double r=(p.b[i].c/p.b[i-1].c-1)*1e4*d - 2.0*p.hs_bps;
            double sma=0; for(int k=0;k<200;k++)sma+=p.b[i-1-k].c; sma/=200;
            if(p.b[i-1].c>sma){up_g+=r;up_n++;}else{dn_g+=r;dn_n++;}
            double vv=0,mm=0; for(int k=0;k<20;k++)mm+=p.b[i-k].c; mm/=20; for(int k=0;k<20;k++){double dd=p.b[i-k].c-mm;vv+=dd*dd;} double rv=std::sqrt(vv/19)/p.b[i].c;
            if(rv<vmed){lv_g+=r;lv_n++;}else{hv_g+=r;hv_n++;}
        }
        printf("  above 200d SMA (bull): net=%+7.0fbp  avg=%+.2fbp/trade  n=%d\n",up_g,up_n?up_g/up_n:0,up_n);
        printf("  below 200d SMA (bear): net=%+7.0fbp  avg=%+.2fbp/trade  n=%d  %s\n",dn_g,dn_n?dn_g/dn_n:0,dn_n, dn_g>0?"<-- edge SURVIVES in downtrend":"<-- edge is bull-only (FRAGILE)");
        printf("  low-vol  regime:       net=%+7.0fbp  avg=%+.2fbp/trade  n=%d\n",lv_g,lv_n?lv_g/lv_n:0,lv_n);
        printf("  high-vol regime:       net=%+7.0fbp  avg=%+.2fbp/trade  n=%d  %s\n",hv_g,hv_n?hv_g/hv_n:0,hv_n, hv_g>0?"(persists in high vol)":"(dies in high vol)");
    }
    return 0;
}
