// =============================================================================
// index_final_sweep.cpp (S44) -- last cheap sweep of D1-testable UNTESTED families
// before declaring the index opportunity set exhausted. Honest: no-lookahead,
// real cost, Sharpe/DD. 6-index duka D1 + macro feeds.
//   A) LEAD-LAG  : does US (SPX) day-D return predict EU index day-D+1 return?
//   B) PAIR MR   : rolling-z fade of NAS/SPX (and US-vs-EU) ratio (detrended)
//   C) NFP-FRIDAY: mean return on/around first-Friday (payroll) days
//   D) SEASONAL x VOL: does the live Tue/Fri edge concentrate by realized-vol tercile?
// BUILD: c++ -std=c++17 -O2 backtest/index_final_sweep.cpp -o backtest/index_final_sweep
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

struct Bar{int64_t day;double c;int wd,mon,mday;};
struct Sym{std::string name;const char*path;double hs;std::vector<Bar> b;
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool fr=true;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0);struct tm g;gmtime_r(&t,&g);if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb;bb.day=(int64_t)(ts/1000.0);bb.c=c;bb.wd=g.tm_wday;bb.mon=g.tm_mon+1;bb.mday=g.tm_mday;b.push_back(bb);}
        fclose(f);return b.size()>260;}};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}}

int main(){
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    Sym SPX{"SPX500",DK("usa500idxusd"),1.0}, NAS{"NAS100",DK("usatechidxusd"),1.5},
        GER{"GER40",DK("deuidxeur"),1.5}, EST{"ESTX50",DK("eusidxeur"),1.5}, UK{"UK100",DK("gbridxgbp"),1.5};
    for(Sym* s:{&SPX,&NAS,&GER,&EST,&UK}) if(!s->load()){printf("load fail %s\n",s->name.c_str());return 1;}

    // ---- A) LEAD-LAG: SPX day-D return -> EU index day-D+1 return ----
    printf("=== A) LEAD-LAG: US (SPX) day-D predicts EU day-D+1? (corr + long-EU-if-US-up) ===\n");
    for(Sym* eu:{&GER,&EST,&UK}){
        size_t L=std::min(SPX.b.size(),eu->b.size());
        double cov=0,msx=0,meu=0,vsx=0,veu=0;int n=0; std::vector<double> strat;
        // align by index (same duka calendar)
        for(size_t i=1;i+1<L;i++){double rs=SPX.b[i].c/SPX.b[i-1].c-1; double re=eu->b[i+1].c/eu->b[i].c-1; msx+=rs;meu+=re;n++;}
        msx/=n;meu/=n;
        for(size_t i=1;i+1<L;i++){double rs=SPX.b[i].c/SPX.b[i-1].c-1; double re=eu->b[i+1].c/eu->b[i].c-1;
            cov+=(rs-msx)*(re-meu);vsx+=(rs-msx)*(rs-msx);veu+=(re-meu)*(re-meu);
            double dir=rs>0?1:-1; strat.push_back(dir*re - 2.0*eu->hs/1e4);}
        double c=(vsx>0&&veu>0)?cov/std::sqrt(vsx*veu):0; double sh,dd,tt;stats(strat,sh,dd,tt);
        printf("  SPX->%-7s corr=%+.3f  follow-strat Sharpe=%+.2f net=%+.0fbp\n",eu->name.c_str(),c,sh,tt*1e4);
    }
    // ---- B) PAIR MR: rolling-z fade of NAS/SPX ratio ----
    printf("\n=== B) PAIR-SPREAD MR: rolling-z(60) fade of log-ratio, exit at 0 (detrended) ===\n");
    auto pairmr=[&](Sym&A,Sym&B,double zin){
        size_t L=std::min(A.b.size(),B.b.size()); std::vector<double> s; int pos=0;double entry=0;
        std::vector<double> lr(L); for(size_t i=0;i<L;i++)lr[i]=std::log(A.b[i].c/B.b[i].c);
        for(size_t i=61;i<L;i++){double m=0;for(int k=1;k<=60;k++)m+=lr[i-k];m/=60;double v=0;for(int k=1;k<=60;k++){double d=lr[i-k]-m;v+=d*d;}double sd=std::sqrt(v/59);
            if(sd<=0){s.push_back(0);continue;} double z=(lr[i-1]-m)/sd;   // signal as of prior close
            // position: short-A/long-B when z>zin (A rich), long-A/short-B when z<-zin; exit when |z|<0.5
            int want=pos; if(pos==0){ if(z>zin)want=-1; else if(z<-zin)want=+1; } else { if(std::fabs(z)<0.5)want=0; }
            // realize today's return of the spread position (long spread = long A short B)
            double ra=A.b[i].c/A.b[i-1].c-1, rb=B.b[i].c/B.b[i-1].c-1; double spr=ra-rb;
            double pnl= pos*spr; if(want!=pos) pnl-= (2.0*A.hs+2.0*B.hs)/1e4; // leg costs on change
            s.push_back(pnl); pos=want;}
        double sh,dd,tt;stats(s,sh,dd,tt);
        printf("  %s/%s zin=%.1f: Sharpe=%+.2f net=%+.0fbp maxDD=%.0f\n",A.name.c_str(),B.name.c_str(),zin,sh,tt*1e4,dd*1e4);
    };
    pairmr(NAS,SPX,2.0); pairmr(NAS,SPX,1.5); pairmr(SPX,EST,2.0); pairmr(GER,EST,2.0);

    // ---- C) NFP-FRIDAY (first Friday of month) ----
    printf("\n=== C) NFP-FRIDAY (first Friday = payroll day): mean return + day-before ===\n");
    { double nfp=0,pre=0;int nn=0; for(Sym* s:{&SPX,&NAS,&GER,&EST,&UK}){
        for(size_t i=2;i<s->b.size();i++){ if(s->b[i].wd==5 && s->b[i].mday<=7){ // first Friday
            nfp+=(s->b[i].c/s->b[i-1].c-1)*1e4; pre+=(s->b[i-1].c/s->b[i-2].c-1)*1e4; nn++; }}}
      printf("  first-Friday(NFP) avg=%+.2fbp  day-before avg=%+.2fbp  (n=%d)\n",nn?nfp/nn:0,nn?pre/nn:0,nn);
    }
    // ---- D) SEASONAL x VOL tercile (sharpen the live Tue/Fri edge) ----
    printf("\n=== D) Tue/Fri seasonal entry net by realized-vol tercile (concentration) ===\n");
    { for(Sym* s:{&SPX,&NAS,&GER,&EST,&UK}){ /* pooled below */ }
      // pooled across 6: collect (vol20, seasonal_ret)
      std::vector<std::pair<double,double>> v;
      for(Sym* s:{&SPX,&NAS,&GER,&EST,&UK}){ for(size_t i=21;i+1<s->b.size();i++){ int wd=s->b[i].wd; if(wd!=2&&wd!=5)continue;
            double m=0;for(int k=0;k<20;k++)m+=s->b[i-k].c;m/=20;double rv=0;for(int k=0;k<20;k++){double d=s->b[i-k].c-m;rv+=d*d;}rv=std::sqrt(rv/20)/s->b[i].c;
            double r=(s->b[i+1].c/s->b[i].c-1)*1e4-2.0*s->hs; v.push_back({rv,r}); } }
      std::vector<double> vols;for(auto&x:v)vols.push_back(x.first);std::sort(vols.begin(),vols.end());
      double t1=vols[vols.size()/3],t2=vols[2*vols.size()/3];
      double lo=0,mid=0,hi=0;int ln=0,mn=0,hn=0;
      for(auto&x:v){ if(x.first<=t1){lo+=x.second;ln++;} else if(x.first<=t2){mid+=x.second;mn++;} else {hi+=x.second;hn++;} }
      printf("  low-vol  net=%+7.0fbp avg=%+.2f (n=%d)\n",lo,ln?lo/ln:0,ln);
      printf("  mid-vol  net=%+7.0fbp avg=%+.2f (n=%d)\n",mid,mn?mid/mn:0,mn);
      printf("  high-vol net=%+7.0fbp avg=%+.2f (n=%d)\n",hi,hn?hi/hn:0,hn);
    }
    printf("\n  Build only if Sharpe>0.5 survives cost AND mechanism is sound. Else D1 set is exhausted.\n");
    return 0;
}
