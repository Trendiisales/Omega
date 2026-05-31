// =============================================================================
// index_macro_regime.cpp (S44 hunt, post data-pull) -- do rates/credit/dollar
// regimes add TIMING to the index book? Tests the edges unlocked by the new
// feeds (download/macro/: tnx10y, irx3m, hyg, lqd, tlt, dxy) on the 6-index
// equal-weight basket, all vs buy&hold (the beta benchmark). No lookahead:
// every regime read uses prior-close (day i-1). Real index cost.
//   A) CREDIT gate   : long only when HYG/LQD (credit) 20d momentum >= 0
//   B) RATES gate    : long only when 10Y yield NOT rising (20d change <= 0)
//   C) DOLLAR gate   : long only when DXY 20d momentum <= 0
//   D) RISK-ON combo : credit-ok AND dollar-not-up
//   E) CONDITIONED TSMOM : long-trend(c>c[-60]) AND risk-on -- does conditioning
//        rescue the killed (just-beta) trend?  vs unconditional trend + hold
//   F) MONTH-END REBALANCE: last-3 trading days, direction by prior equity-vs-bond
//        (TLT) month move -- sell-tilt after strong-equity months [research 8.3]
// BUILD: c++ -std=c++17 -O2 backtest/index_macro_regime.cpp -o backtest/index_macro_regime
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <unordered_map>

struct Bar{int64_t day;double c;int mon;};
struct Series{std::unordered_map<int64_t,double> v; // day-key -> close
    bool load(const char* path){FILE*f=fopen(path,"r");if(!f){printf("MISS %s\n",path);return false;}char ln[128];bool fr=true;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;continue;}double ts,c;if(sscanf(ln,"%lf,%lf",&ts,&c)!=2)continue;v[(int64_t)ts/86400]=c;}fclose(f);return !v.empty();}
    double at(int64_t day_s)const{auto it=v.find(day_s/86400);return it!=v.end()?it->second:-1;}
    // value N calendar days earlier (approx via scanning back up to N+5 days)
    double back(int64_t day_s,int nd)const{for(int k=nd;k<nd+6;k++){auto it=v.find(day_s/86400-k);if(it!=v.end())return it->second;}return -1;}
};
struct Sym{std::string name;const char*path;double hs;std::vector<Bar> b;
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool fr=true;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0);struct tm g;gmtime_r(&t,&g);if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb;bb.day=(int64_t)(ts/1000.0);bb.c=c;bb.mon=g.tm_mon+1;b.push_back(bb);}fclose(f);return b.size()>260;}};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}}

static Series TNX,IRX,HYG,LQD,TLT,DXY;
int main(){
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P={{"SPX500",DK("usa500idxusd"),1.0},{"NAS100",DK("usatechidxusd"),1.5},
        {"GER40",DK("deuidxeur"),1.5},{"US30",DK("usa30idxusd"),1.0},
        {"UK100",DK("gbridxgbp"),1.5},{"ESTX50",DK("eusidxeur"),1.5}};
    for(auto it=P.begin();it!=P.end();){if(!it->load())it=P.erase(it);else ++it;}
    TNX.load("/Users/jo/Omega/download/macro/tnx10y_d1.csv");HYG.load("/Users/jo/Omega/download/macro/hyg_d1.csv");
    LQD.load("/Users/jo/Omega/download/macro/lqd_d1.csv");TLT.load("/Users/jo/Omega/download/macro/tlt_d1.csv");
    DXY.load("/Users/jo/Omega/download/macro/dxy_d1.csv");
    int N=P.size();
    printf("INDEX MACRO-REGIME GATING (6-index EW basket, prior-close regimes, vs buy&hold)\n\n");

    // regime helpers at day d (prior close): momentum sign over ~20 cal days
    auto credit_ok=[&](int64_t d){double h=HYG.at(d),hb=HYG.back(d,20),l=LQD.at(d),lb=LQD.back(d,20);
        if(h<0||hb<0||l<0||lb<0)return 1; double cr=(h/hb-1)+(l/lb-1); return cr>=0?1:0;};      // credit healthy
    auto rates_ok=[&](int64_t d){double t=TNX.at(d),tb=TNX.back(d,20); if(t<0||tb<0)return 1; return (t-tb)<=0.0?1:0;}; // yields not rising
    auto dollar_ok=[&](int64_t d){double x=DXY.at(d),xb=DXY.back(d,20); if(x<0||xb<0)return 1; return (x/xb-1)<=0.0?1:0;}; // dollar not strengthening

    auto runEW=[&](const char* nm, int mode){
        // mode: 0 hold | 1 credit | 2 rates | 3 dollar | 4 riskon(credit&dollar) | 5 condTSMOM | 6 uncondTSMOM
        std::vector<double> s; int inmkt=0,tot=0;
        size_t L=1e9; for(auto&p:P)L=std::min(L,p.b.size());
        for(size_t i=70;i<L;i++){
            int64_t d=P[0].b[i-1].day;       // regime as of prior close
            double er=0; for(auto&p:P)er+=(p.b[i].c/p.b[i-1].c-1); er/=N;  // EW next-day return
            double pos=1; bool on=true;
            if(mode==1) on=credit_ok(d);
            else if(mode==2) on=rates_ok(d);
            else if(mode==3) on=dollar_ok(d);
            else if(mode==4) on=credit_ok(d)&&dollar_ok(d);
            else if(mode==5){ // conditioned trend: EW above its 60d ago AND risk-on
                double tr=0;for(auto&p:P)tr+=(p.b[i-1].c>p.b[i-1-60].c)?1:0; bool up=tr>=N/2.0;
                on= up && credit_ok(d) && dollar_ok(d); }
            else if(mode==6){ double tr=0;for(auto&p:P)tr+=(p.b[i-1].c>p.b[i-1-60].c)?1:0; on=tr>=N/2.0; }
            tot++;
            if(!on){s.push_back(0);continue;}
            s.push_back(er - 2.0*1.2/1e4*0.2); inmkt++;  // small daily cost proxy when in mkt
        }
        double sh,dd,tt;stats(s,sh,dd,tt);
        printf("  %-28s Sharpe=%+.2f net=%+7.0fbp maxDD=%5.0f  in-mkt=%.0f%%\n",nm,sh,tt*1e4,dd*1e4,100.0*inmkt/tot);
    };
    runEW("buy&hold EW",0);
    runEW("credit-gate (HYG/LQD up)",1);
    runEW("rates-gate (10Y not rising)",2);
    runEW("dollar-gate (DXY not up)",3);
    runEW("risk-on (credit&dollar)",4);
    runEW("uncond trend (60d)",6);
    runEW("CONDITIONED trend",5);

    // ---- F) MONTH-END REBALANCE: last-3 trading days, dir by equity-vs-bond month move ----
    printf("\n=== MONTH-END REBALANCE (last-3 trading days; tilt by prior equity-vs-bond move) ===\n");
    {
        // build trading-day-of-month / remaining via SPX calendar
        auto&p=P[0]; std::vector<int> trem(p.b.size()); int rd=0,pm=-1;
        for(int i=(int)p.b.size()-1;i>=0;i--){time_t t=(time_t)p.b[i].day;struct tm g;gmtime_r(&t,&g);if(g.tm_mon!=pm){rd=0;pm=g.tm_mon;}trem[i]=rd++;}
        std::vector<double> plain,cond;
        for(size_t i=22;i<p.b.size();i++){
            if(trem[i]>2) {plain.push_back(0);cond.push_back(0);continue;}    // only last-3 trading days
            double er=0;for(auto&q:P)er+=(q.b[i].c/q.b[i-1].c-1);er/=N;
            plain.push_back(er-2.0*1.2/1e4*0.2);
            // prior ~20d equity vs bond (TLT): if equity outperformed bonds -> rebalancers SELL equity -> short tilt
            int64_t d=p.b[i-1].day; double eq=p.b[i-1].c/p.b[i-1-20].c-1; double tb=TLT.at(d),tbk=TLT.back(d,20);
            double bond=(tb>0&&tbk>0)?(tb/tbk-1):0; double dir=(eq-bond)>0?-1:+1;   // fade strong-equity-vs-bond
            cond.push_back(dir*er - 2.0*1.2/1e4*0.2);
        }
        double s1,d1,t1;stats(plain,s1,d1,t1); double s2,d2,t2;stats(cond,s2,d2,t2);
        printf("  ToM last-3 LONG (plain):       Sharpe=%+.2f net=%+.0fbp\n",s1,t1*1e4);
        printf("  ToM last-3 equity-vs-bond tilt: Sharpe=%+.2f net=%+.0fbp  (dir=-1 fade strong-equity months)\n",s2,t2*1e4);
    }
    // ---- validate credit/dollar gate on the SEASONAL SLEEVE (the engine we'd gate) ----
    printf("\n=== CREDIT/DOLLAR gate on the day-of-week SEASONAL sleeve (enter Tue/Fri close) ===\n");
    {
        auto run=[&](const char* nm,int mode){
            std::vector<double> s; int inmkt=0,tot=0;
            for(auto&p:P){
                for(size_t i=61;i<p.b.size();i++){
                    time_t t=(time_t)p.b[i-1].day;struct tm g;gmtime_r(&t,&g);int wd=g.tm_wday;
                    if(wd!=2&&wd!=5)continue;                       // seasonal entry days only
                    tot++; int64_t d=p.b[i-1].day; bool on=true;
                    if(mode==1)on=credit_ok(d);
                    else if(mode==2)on=credit_ok(d)&&dollar_ok(d);
                    if(!on){s.push_back(0);continue;}
                    s.push_back((p.b[i].c/p.b[i-1].c-1) - 2.0*p.hs/1e4); inmkt++;
                }
            }
            double sh,dd,tt;stats(s,sh,dd,tt);
            printf("  %-26s Sharpe=%+.2f net=%+7.0fbp maxDD=%5.0f in-mkt=%.0f%%\n",nm,sh,tt*1e4,dd*1e4,100.0*inmkt/(tot?tot:1));
        };
        run("seasonal ungated",0);
        run("seasonal + credit-gate",1);
        run("seasonal + credit&dollar",2);
    }
    printf("\n  A gate EARNS ITS KEEP if Sharpe>hold AND maxDD<<hold. Conditioned-trend earns it if it beats uncond+hold.\n");
    return 0;
}
