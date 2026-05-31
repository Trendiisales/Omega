// =============================================================================
// index_edge_battery.cpp (S44 opportunity hunt) -- broad HONEST scan of UNTESTED
// index edge families on Dukascopy D1 (6 indices, 7.4yr incl 2020/2022). All
// drift/beta-aware, real cost, WF+blocks where relevant. Families:
//   1. MONTH-OF-YEAR  (Sell-in-May / Halloween, Santa, September effect)
//   2. GAP            (overnight gap follow vs fade; open vs prior close)
//   3. OVERNIGHT vs INTRADAY  (night-effect: close->open vs open->close)
//   4. RELATIVE-STRENGTH ROTATION (long top-K of 6 by trailing return, vs EW hold)
//   5. DAY-OF-WEEK x MONTH (does the confirmed Tue/Fri edge concentrate?)
// BUILD: c++ -std=c++17 -O2 backtest/index_edge_battery.cpp -o backtest/index_edge_battery
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

struct Bar{int64_t day;double o,h,l,c;int wd,mon,mday,year;};
struct Sym{std::string name;const char*path;double hs;std::vector<Bar> b;
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool fr=true;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0);struct tm g;gmtime_r(&t,&g);if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb;bb.day=(int64_t)(ts/1000.0);bb.o=o;bb.h=h;bb.l=l;bb.c=c;bb.wd=g.tm_wday;bb.mon=g.tm_mon+1;
            bb.mday=g.tm_mday;bb.year=g.tm_year+1900;b.push_back(bb);}fclose(f);return b.size()>260;}};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}}

int main(){
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P={{"SPX500",DK("usa500idxusd"),1.0},{"NAS100",DK("usatechidxusd"),1.5},
        {"GER40",DK("deuidxeur"),1.5},{"US30",DK("usa30idxusd"),1.0},
        {"UK100",DK("gbridxgbp"),1.5},{"ESTX50",DK("eusidxeur"),1.5}};
    for(auto it=P.begin();it!=P.end();){if(!it->load())it=P.erase(it);else ++it;}

    // ---- 1. MONTH-OF-YEAR (mean next-day return aggregated by calendar month) ----
    printf("=== 1. MONTH-OF-YEAR: mean daily return by month (bp, gross), all 6 indices ===\n");
    { double s[13]={0};int n[13]={0};
      for(auto&p:P)for(size_t i=1;i<p.b.size();i++){double r=(p.b[i].c/p.b[i-1].c-1)*1e4;s[p.b[i].mon]+=r;n[p.b[i].mon]++;}
      const char* mn[13]={"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
      double smr=0,wmr=0;int sn=0,wn=0; // summer May-Oct vs winter Nov-Apr (Sell-in-May)
      for(int m=1;m<=12;m++){printf("  %s %+6.2f bp/d (n=%d)\n",mn[m],n[m]?s[m]/n[m]:0,n[m]);
        if(m>=5&&m<=10){smr+=s[m];sn+=n[m];}else{wmr+=s[m];wn+=n[m];}}
      printf("  Sell-in-May check: summer(May-Oct)=%+.2fbp/d  winter(Nov-Apr)=%+.2fbp/d  -> %s\n",
        sn?smr/sn:0,wn?wmr/wn:0,(wn&&sn&&wmr/wn>smr/sn)?"WINTER > summer (Halloween effect present)":"no clear Halloween");
    }
    // ---- 2. GAP follow vs fade (open vs prior close -> intraday open->close) ----
    printf("\n=== 2. GAP: after an overnight gap, does the session FOLLOW or FADE it? ===\n");
    { double folg=0,fadg=0;int fn=0; double up_id=0,dn_id=0;int un=0,dnn=0;
      std::vector<double> fade,follow;
      for(auto&p:P)for(size_t i=1;i<p.b.size();i++){
        double gap=(p.b[i].o/p.b[i-1].c-1)*1e4; double intraday=(p.b[i].c/p.b[i].o-1)*1e4;
        if(std::fabs(gap)<5)continue; // only meaningful gaps
        double follow_r=(gap>0?1:-1)*intraday - 2.0*p.hs; follow.push_back(follow_r/1e4);
        fade.push_back((-(gap>0?1:-1)*intraday - 2.0*p.hs)/1e4);
        if(gap>0){up_id+=intraday;un++;}else{dn_id+=intraday;dnn++;} fn++;
      }
      double sf,df,tf; stats(follow,sf,df,tf); double sd2,dd2,td2; stats(fade,sd2,dd2,td2);
      printf("  after UP-gap: intraday avg=%+.2fbp (n=%d)  after DN-gap: intraday avg=%+.2fbp (n=%d)\n",un?up_id/un:0,un,dnn?dn_id/dnn:0,dnn);
      printf("  FOLLOW gap: net=%+.0fbp Sharpe=%+.2f | FADE gap: net=%+.0fbp Sharpe=%+.2f  (n=%d gaps>5bp)\n",tf*1e4,sf,td2*1e4,sd2,fn);
    }
    // ---- 3. OVERNIGHT vs INTRADAY (night effect) ----
    printf("\n=== 3. OVERNIGHT (close->open) vs INTRADAY (open->close) cumulative, per index ===\n");
    printf("  (CAVEAT: duka D1 open/close are UTC-midnight bounded, NOT cash-session -> directional only)\n");
    for(auto&p:P){double on=0,id=0;for(size_t i=1;i<p.b.size();i++){on+=(p.b[i].o/p.b[i-1].c-1)*1e4;id+=(p.b[i].c/p.b[i].o-1)*1e4;}
      printf("  %-7s overnight=%+8.0fbp  intraday=%+8.0fbp\n",p.name.c_str(),on,id);}
    // ---- 4. RELATIVE-STRENGTH ROTATION (long top-K by trailing LB, weekly rebal) ----
    printf("\n=== 4. RELATIVE-STRENGTH ROTATION: long top-K of 6 by trailing return vs equal-weight hold ===\n");
    { // align by index position (all 6 share duka calendar -> same length & dates mostly)
      size_t L=1e9; for(auto&p:P)L=std::min(L,p.b.size());
      for(int LB : {20,60,120}) for(int K : {1,2,3}){
        std::vector<double> rot,ew;
        for(size_t i=LB+1;i<L;i++){
          // rank by trailing LB return as of i-1 (no lookahead)
          std::vector<std::pair<double,int>> rk;
          for(size_t s=0;s<P.size();s++){double tr=P[s].b[i-1].c/P[s].b[i-1-LB].c-1;rk.push_back({tr,(int)s});}
          std::sort(rk.begin(),rk.end(),[](auto&a,auto&b){return a.first>b.first;});
          double rsum=0; for(int k=0;k<K;k++){int s=rk[k].second;rsum+=(P[s].b[i].c/P[s].b[i-1].c-1)*1e4 - 2.0*P[s].hs/5.0;}
          rot.push_back(rsum/K/1e4);
          double e=0;for(auto&p:P)e+=(p.b[i].c/p.b[i-1].c-1)*1e4; ew.push_back(e/P.size()/1e4);
        }
        double sr,dr,tr2;stats(rot,sr,dr,tr2); double se,de,te;stats(ew,se,de,te);
        printf("  LB=%-3d top-%d: rot net=%+7.0fbp Sharpe=%+.2f maxDD=%.0f | EW-hold Sharpe=%+.2f  %s\n",
          LB,K,tr2*1e4,sr,dr*1e4,se, sr>se?"<-- rotation beats EW":"");
      }
    }
    // ---- 5. DAY-OF-WEEK x MONTH: does the Tue/Fri (entry) seasonal concentrate? ----
    printf("\n=== 5. Day-of-week (entry Tue+Fri = strong sessions) net by month -- concentration check ===\n");
    { double s[13]={0};int n[13]={0};
      for(auto&p:P)for(size_t i=1;i<p.b.size();i++){int wd=p.b[i-1].wd; if(wd!=2&&wd!=5)continue;
        double r=(p.b[i].c/p.b[i-1].c-1)*1e4-2.0*p.hs;s[p.b[i].mon]+=r;n[p.b[i].mon]++;}
      const char* mn[13]={"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
      for(int m=1;m<=12;m++)printf("  %s %+7.0fbp (n=%d)\n",mn[m],s[m],n[m]);
    }
    return 0;
}
