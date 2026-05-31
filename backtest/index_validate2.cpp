// =============================================================================
// index_validate2.cpp (S44) -- validate the remaining shortlist before building:
//   A) PROPER residual momentum: regress each index's daily returns on the market
//      factor (EW of 6) over a rolling window, momentum on the CUMULATIVE RESIDUAL,
//      long top / short bottom (Blitz-Huij-Martens 2011). vs raw-return L/S.
//   B) PRE-FOMC drift (Lucca-Moench 2015): mean return on the day BEFORE + the day
//      OF scheduled FOMC announcements; split 2019-22 vs 2023-26 to see decay.
//   C) VOL-TARGET sizing on the EW basket: size = target/realized_vol; Sharpe/DD
//      vs unsized hold (research #1).
// Honest: no-lookahead, real cost, Sharpe/DD. 6-index duka D1.
// BUILD: c++ -std=c++17 -O2 backtest/index_validate2.cpp -o backtest/index_validate2
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

struct Bar{int64_t day;double c;int y,mon,mday;};
struct Sym{std::string name;const char*path;double hs;std::vector<Bar> b;
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool fr=true;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0);struct tm g;gmtime_r(&t,&g);if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb;bb.day=(int64_t)(ts/1000.0);bb.c=c;bb.y=g.tm_year+1900;bb.mon=g.tm_mon+1;bb.mday=g.tm_mday;b.push_back(bb);}
        fclose(f);return b.size()>260;}};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}}

// FOMC scheduled announcement dates (YYYYMMDD) 2019-2026 (8/yr; 2026 partial/known).
static const int FOMC[]={
 20190130,20190320,20190501,20190619,20190731,20190918,20191030,20191211,
 20200129,20200318,20200429,20200610,20200729,20200916,20201105,20201216,
 20210127,20210317,20210428,20210616,20210728,20210922,20211103,20211215,
 20220126,20220316,20220504,20220615,20220727,20220921,20221102,20221214,
 20230201,20230322,20230503,20230614,20230726,20230920,20231101,20231213,
 20240131,20240320,20240501,20240612,20240731,20240918,20241107,20241218,
 20250129,20250319,20250507,20250618,20250730,20250917,20251029,20251210,
 20260128,20260318,20260429};
static bool is_fomc(int ymd){for(int x:FOMC)if(x==ymd)return true;return false;}
static int ymd_of(const Bar&b){return b.y*10000+b.mon*100+b.mday;}

int main(){
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P={{"SPX500",DK("usa500idxusd"),1.0},{"NAS100",DK("usatechidxusd"),1.5},
        {"GER40",DK("deuidxeur"),1.5},{"US30",DK("usa30idxusd"),1.0},
        {"UK100",DK("gbridxgbp"),1.5},{"ESTX50",DK("eusidxeur"),1.5}};
    for(auto it=P.begin();it!=P.end();){if(!it->load())it=P.erase(it);else ++it;}
    int N=P.size(); size_t L=1e9; for(auto&p:P)L=std::min(L,p.b.size());

    // ---- A) PROPER residual momentum (beta-regression) L/S ----
    printf("=== A) RESIDUAL MOMENTUM (beta-regressed) L/S vs raw-return L/S ===\n");
    for(int LB:{120,200}){
        std::vector<double> resid, raw;
        for(size_t i=LB+1;i<L;i++){
            // market factor daily returns over the window
            // for each index: regress r_x on r_mkt -> alpha+beta; residual cum = sum(r_x - beta*r_mkt)
            std::vector<double> score(N), rawscore(N);
            // precompute market daily returns in window
            std::vector<double> mkt(LB);
            for(int k=0;k<LB;k++){double e=0;for(int x=0;x<N;x++)e+=(P[x].b[i-LB+k].c/P[x].b[i-LB+k-1].c-1);mkt[k]=e/N;}
            double mm=0;for(double v:mkt)mm+=v;mm/=LB; double vm=0;for(double v:mkt){double d=v-mm;vm+=d*d;}
            for(int x=0;x<N;x++){
                double cov=0,sumr=0; std::vector<double> rx(LB);
                for(int k=0;k<LB;k++){rx[k]=P[x].b[i-LB+k].c/P[x].b[i-LB+k-1].c-1;sumr+=rx[k];}
                double mr=sumr/LB;
                for(int k=0;k<LB;k++)cov+=(rx[k]-mr)*(mkt[k]-mm);
                double beta=vm>0?cov/vm:1.0;
                double rescum=0;for(int k=0;k<LB;k++)rescum+=(rx[k]-beta*mkt[k]);
                score[x]=rescum; rawscore[x]=P[x].b[i-1].c/P[x].b[i-1-LB].c-1;
            }
            auto ls=[&](std::vector<double>&sc){int hi=0,lo=0;for(int x=1;x<N;x++){if(sc[x]>sc[hi])hi=x;if(sc[x]<sc[lo])lo=x;}
                double rl=(P[hi].b[i].c/P[hi].b[i-1].c-1)-2.0*P[hi].hs/1e4/5.0;
                double rs=-((P[lo].b[i].c/P[lo].b[i-1].c-1))-2.0*P[lo].hs/1e4/5.0;return (rl+rs)/2.0;};
            resid.push_back(ls(score)); raw.push_back(ls(rawscore));
        }
        double sr,dr,tr;stats(resid,sr,dr,tr);double s2,d2,t2;stats(raw,s2,d2,t2);
        printf("  LB%-3d residual-L/S Sharpe=%+.2f net=%+.0fbp maxDD=%.0f | raw-L/S Sharpe=%+.2f net=%+.0fbp\n",
            LB,sr,tr*1e4,dr*1e4,s2,t2*1e4);
    }
    // ---- B) PRE-FOMC drift ----
    printf("\n=== B) PRE-FOMC drift (mean bp; day-before & FOMC-day), split for decay ===\n");
    { double pre[2]={0},day[2]={0};int pn[2]={0},dn[2]={0}; double allpre=0,allday=0;int apn=0,adn=0;
      for(auto&p:P)for(size_t i=2;i<p.b.size();i++){
        int ymd=ymd_of(p.b[i]); if(!is_fomc(ymd))continue;
        int era=(p.b[i].y<=2022)?0:1;
        double dayr=(p.b[i].c/p.b[i-1].c-1)*1e4;      // FOMC-day return (entry prior close)
        double prer=(p.b[i-1].c/p.b[i-2].c-1)*1e4;    // day-before return
        day[era]+=dayr;dn[era]++;pre[era]+=prer;pn[era]++; allday+=dayr;adn++;allpre+=prer;apn++;
      }
      printf("  ALL: pre-FOMC-day avg=%+.2fbp (n=%d)  FOMC-day avg=%+.2fbp (n=%d)\n",apn?allpre/apn:0,apn,adn?allday/adn:0,adn);
      printf("  2019-22: pre=%+.2f day=%+.2f | 2023-26: pre=%+.2f day=%+.2f  (decay if 23-26 << 19-22)\n",
        pn[0]?pre[0]/pn[0]:0,dn[0]?day[0]/dn[0]:0,pn[1]?pre[1]/pn[1]:0,dn[1]?day[1]/dn[1]:0);
    }
    // ---- C) VOL-TARGET sizing on EW basket ----
    printf("\n=== C) VOL-TARGET sizing on EW basket (size=target/realized_vol) vs unsized ===\n");
    { std::vector<double> plain,vt; double tgt=0.0;
      // build EW daily returns
      std::vector<double> ew(L-1); for(size_t i=1;i<L;i++){double e=0;for(auto&p:P)e+=(p.b[i].c/p.b[i-1].c-1);ew[i-1]=e/N;}
      // target vol = full-sample daily vol (so avg leverage ~1)
      double m=0;for(double x:ew)m+=x;m/=ew.size();double v=0;for(double x:ew){double d=x-m;v+=d*d;}tgt=std::sqrt(v/ew.size());
      for(size_t i=21;i<ew.size();i++){double rv=0,mm=0;for(int k=0;k<20;k++)mm+=ew[i-1-k];mm/=20;
        for(int k=0;k<20;k++){double d=ew[i-1-k]-mm;rv+=d*d;}rv=std::sqrt(rv/20);
        double sz=rv>0?tgt/rv:1;if(sz>3)sz=3;if(sz<0.2)sz=0.2;
        plain.push_back(ew[i]); vt.push_back(ew[i]*sz);}
      double sp,dp,tp;stats(plain,sp,dp,tp);double sv,dv,tv;stats(vt,sv,dv,tv);
      printf("  unsized  Sharpe=%+.2f net=%+.0fbp maxDD=%.0f\n",sp,tp*1e4,dp*1e4);
      printf("  vol-targ Sharpe=%+.2f net=%+.0fbp maxDD=%.0f  (avg lev~1)\n",sv,tv*1e4,dv*1e4);
    }
    printf("\n  Build if: residual-L/S Sharpe>0 & beats raw; pre-FOMC alive in 2023-26; vol-target Sharpe>unsized.\n");
    return 0;
}
