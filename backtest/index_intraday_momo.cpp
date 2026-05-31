// =============================================================================
// index_intraday_momo.cpp (S44 follow-up #3) -- market intraday momentum
// (Gao-Han-Li-Zhou, JFE 2018): the FIRST half-hour return predicts the LAST
// half-hour return. Strategy: at 15:30 (ET) enter dir=sign(first30), exit at
// 16:00 close. Tested on m5 index CFD bars with PROPER cash-session + US DST.
//   first30 = close@10:00 / open@09:30 - 1
//   last30  = close@16:00 / close@15:30 - 1   (the tradeable window)
// Honest: real m5 half-spread cost, WF 50% split, sign-hit-rate, all printed.
// CAVEAT: m5 data is 2024-2026 (bull only) -> lower confidence; research says
// this edge is decaying/regime-sensitive. Treat as exploratory.
// BUILD: c++ -std=c++17 -O2 backtest/index_intraday_momo.cpp -o backtest/index_intraday_momo
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <map>

// US DST: 2nd Sunday March 02:00 -> 1st Sunday November 02:00 (date-level approx).
static bool us_dst(int y,int mon,int mday){
    if(mon<3||mon>11) return false; if(mon>3&&mon<11) return true;
    // find day-of-week of the 1st of the month via Zeller-ish (use mktime-free)
    // compute weekday of given date: days since 1970-01-01 (Thu=4)
    auto wd=[&](int Y,int M,int D){ static int t[]={0,3,2,5,0,3,5,1,4,6,2,4}; if(M<3)Y--; return (Y+Y/4-Y/100+Y/400+t[M-1]+D)%7; };
    if(mon==3){ int firstSunMar = 1 + ((7 - wd(y,3,1))%7); int secondSun=firstSunMar+7; return mday>=secondSun; }
    if(mon==11){ int firstSun = 1 + ((7 - wd(y,11,1))%7); return mday<firstSun; }
    return false;
}
struct Bar{int64_t ts;double o,c;int et_day;int et_hm;};
struct Sym{
    std::string name;const char*path;double hs;
    std::map<int,std::vector<Bar>> byday;  // et_day -> bars
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool fr=true;size_t n=0;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)ts;struct tm g;gmtime_r(&t,&g);
            bool dst=us_dst(g.tm_year+1900,g.tm_mon+1,g.tm_mday);
            time_t et=(time_t)ts-(dst?4:5)*3600;struct tm e;gmtime_r(&et,&e);
            if(e.tm_wday==0||e.tm_wday==6)continue;
            Bar b;b.ts=(int64_t)ts;b.o=o;b.c=c;b.et_day=(int)(et/86400);b.et_hm=e.tm_hour*60+e.tm_min;
            byday[b.et_day].push_back(b);n++;}
        fclose(f);return n>1000;}
};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}}

int main(int argc,char**argv){
    std::vector<Sym> P={
        {"NAS100","/Users/jo/Tick/NSXUSD_merged.m5.csv",1.5},
        {"GER40", "/Users/jo/Tick/GER40_merged.m5.csv", 1.5},  // (GER40 cash is CET; ET windows approximate -> caveat)
    };
    printf("INTRADAY MOMENTUM (Gao et al.) -- first-30m sign trades last-30m. m5, ET cash session, US DST.\n");
    printf("  CAVEAT: m5 = 2024-2026 bull only; GER40 uses ET windows (its cash is CET) -> NAS100 is the clean test.\n\n");
    for(auto&s:P){
        if(!s.load()){printf("[skip %s]\n",s.name.c_str());continue;}
        // session marks (ET minutes): open 9:30=570, +30m=600, last30 start 15:30=930, close 16:00=960
        std::vector<double> sgn,lng,both; // signed, long-only, also r1 vs last corr
        double r1sum=0,lsum=0,r1l=0,r1sq=0,lsq=0;int hit=0,nn=0;
        double h1=0,h2=0;int firstday=s.byday.begin()->first,lastday=s.byday.rbegin()->first,mid=(firstday+lastday)/2;
        for(auto&kv:s.byday){
            auto&v=kv.second; double o930=0,c1000=0,c1530=0,c1600=0; bool f930=false,f1000=false,f1530=false,f1600=false;
            for(auto&b:v){
                if(!f930 && b.et_hm>=570){o930=b.o;f930=true;}
                if(b.et_hm<=600){c1000=b.c;f1000=true;}
                if(b.et_hm<=930){c1530=b.c;f1530=true;}
                if(b.et_hm<=960){c1600=b.c;f1600=true;}
            }
            if(!(f930&&f1000&&f1530&&f1600))continue; if(o930<=0||c1530<=0)continue;
            double first30=c1000/o930-1.0; double last30=c1600/c1530-1.0;
            if(first30==0)continue;
            double dir = first30>0?1:-1;
            double rs=(dir*last30 - 2.0*s.hs/1e4); sgn.push_back(rs);
            double rl=(first30>0?(last30 - 2.0*s.hs/1e4):0); lng.push_back(rl);
            // stats
            r1sum+=first30;lsum+=last30;r1l+=first30*last30;r1sq+=first30*first30;lsq+=last30*last30;
            if((first30>0)==(last30>0))hit++; nn++;
            if(kv.first<mid)h1+=rs*1e4;else h2+=rs*1e4;
        }
        double corr=0; if(nn>1){double cov=r1l/nn-(r1sum/nn)*(lsum/nn);double s1=std::sqrt(r1sq/nn-(r1sum/nn)*(r1sum/nn)),s2=std::sqrt(lsq/nn-(lsum/nn)*(lsum/nn));corr=(s1>0&&s2>0)?cov/(s1*s2):0;}
        double sh,dd,tt; stats(sgn,sh,dd,tt); double shl,ddl,ttl; stats(lng,shl,ddl,ttl);
        printf("%s (n=%d days)\n",s.name.c_str(),nn);
        printf("  corr(first30,last30)=%+.3f  sign-hit-rate=%.1f%%  (>52%% = predictive)\n",corr,100.0*hit/nn);
        printf("  signed   net=%+7.0fbp Sharpe=%+.2f maxDD=%.0f  H1=%+.0f H2=%+.0f\n",tt*1e4,sh,dd*1e4,h1,h2);
        printf("  long-only net=%+7.0fbp Sharpe=%+.2f maxDD=%.0f\n\n",ttl*1e4,shl,ddl*1e4);
    }
    printf("Honest: positive corr + hit>52%% + Sharpe>0.5 surviving cost = worth building. Else folklore on this sample.\n");
    return 0;
}
