// =============================================================================
// index_xsec_momentum.cpp (S44 hunt) -- is there BETA-STRIPPED cross-sectional
// alpha across the 6 indices? Directly tests the answer to the killed TSMOM
// (which was just beta). Three constructs, all vs equal-weight buy&hold:
//   A) long-only rotation : long top-K by trailing-LB return (carries beta)
//   B) dollar-neutral L/S : long top-K, short bottom-K (beta-stripped)  [AMP 2013]
//   C) residual momentum  : strip common factor (mean of 6), momentum on the
//      residual cum-return, L/S  [Blitz-Huij-Martens 2011]
// Honest: no-lookahead (rank as of i-1), real cost both legs, Sharpe/DD/WF,
// + market-beta of each sleeve (corr to EW) to prove neutrality.
// BUILD: c++ -std=c++17 -O2 backtest/index_xsec_momentum.cpp -o backtest/index_xsec_momentum
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

struct Bar{int64_t day;double c;};
struct Sym{std::string name;const char*path;double hs;std::vector<Bar> b;
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool fr=true;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0);struct tm g;gmtime_r(&t,&g);if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb;bb.day=(int64_t)(ts/1000.0);bb.c=c;b.push_back(bb);}fclose(f);return b.size()>260;}};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}}
static double corr_off(const std::vector<double>&a,const std::vector<double>&b,size_t boff){
    size_t n=a.size(); if(n<2)return 0;double ma=0,mb=0;for(size_t i=0;i<n;i++)ma+=a[i];ma/=n;
    double mbb=0;for(size_t i=0;i<n;i++)mbb+=b[boff+i];mbb/=n; (void)mb; mb=mbb;
    double cov=0,va=0,vb=0;for(size_t i=0;i<n;i++){double da=a[i]-ma,db=b[boff+i]-mb;cov+=da*db;va+=da*da;vb+=db*db;}
    return (va>0&&vb>0)?cov/std::sqrt(va*vb):0;}

int main(){
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P={{"SPX500",DK("usa500idxusd"),1.0},{"NAS100",DK("usatechidxusd"),1.5},
        {"GER40",DK("deuidxeur"),1.5},{"US30",DK("usa30idxusd"),1.0},
        {"UK100",DK("gbridxgbp"),1.5},{"ESTX50",DK("eusidxeur"),1.5}};
    for(auto it=P.begin();it!=P.end();){if(!it->load())it=P.erase(it);else ++it;}
    size_t L=1e9; for(auto&p:P)L=std::min(L,p.b.size()); int N=P.size();
    printf("CROSS-SECTIONAL INDEX MOMENTUM -- is there BETA-STRIPPED alpha? (vs equal-weight hold)\n");
    printf("  N=%d indices, %zu aligned D1 bars. cost=2*hs per leg traded.\n\n",N,L);

    // equal-weight hold series (the beta benchmark)
    std::vector<double> ew;
    for(size_t i=1;i<L;i++){double e=0;for(auto&p:P)e+=(p.b[i].c/p.b[i-1].c-1);ew.push_back(e/N);}
    double seh,deh,teh;stats(ew,seh,deh,teh);
    printf("  EQUAL-WEIGHT HOLD: Sharpe=%+.2f net=%+.0fbp maxDD=%.0f (beta benchmark)\n\n",seh,teh*1e4,deh*1e4);

    printf("  %-30s Sharpe  net(bp)  maxDD  corr_to_EW(beta)\n","construct");
    for(int LB : {60,120,200}){
        // A) long-only top-1 & top-2
        for(int K : {1,2}){
            std::vector<double> s;
            for(size_t i=LB+1;i<L;i++){std::vector<std::pair<double,int>> rk;
                for(int x=0;x<N;x++)rk.push_back({P[x].b[i-1].c/P[x].b[i-1-LB].c-1,x});
                std::sort(rk.begin(),rk.end(),[](auto&a,auto&b){return a.first>b.first;});
                double r=0;for(int k=0;k<K;k++){int x=rk[k].second;r+=(P[x].b[i].c/P[x].b[i-1].c-1)-2.0*P[x].hs/1e4/5.0;}
                s.push_back(r/K);}
            double sh,dd,tt;stats(s,sh,dd,tt);char nm[48];snprintf(nm,sizeof nm,"LB%d long-only top%d",LB,K);
            printf("  %-30s %+5.2f  %+7.0f  %5.0f   %+.2f\n",nm,sh,tt*1e4,dd*1e4,corr_off(s,ew,LB-1));
        }
        // B) dollar-neutral L/S: long top-1, short bottom-1
        {
            std::vector<double> s;
            for(size_t i=LB+1;i<L;i++){std::vector<std::pair<double,int>> rk;
                for(int x=0;x<N;x++)rk.push_back({P[x].b[i-1].c/P[x].b[i-1-LB].c-1,x});
                std::sort(rk.begin(),rk.end(),[](auto&a,auto&b){return a.first>b.first;});
                int hi=rk.front().second,lo=rk.back().second;
                double rl=(P[hi].b[i].c/P[hi].b[i-1].c-1)-2.0*P[hi].hs/1e4/5.0;
                double rs=-((P[lo].b[i].c/P[lo].b[i-1].c-1))-2.0*P[lo].hs/1e4/5.0;
                s.push_back((rl+rs)/2.0);}
            double sh,dd,tt;stats(s,sh,dd,tt);char nm[48];snprintf(nm,sizeof nm,"LB%d L/S top1-bot1 ($-neutral)",LB);
            printf("  %-30s %+5.2f  %+7.0f  %5.0f   %+.2f\n",nm,sh,tt*1e4,dd*1e4,corr_off(s,ew,LB-1));
        }
        // C) residual momentum L/S: factor=EW cum return; residual = own cum - factor cum over LB
        {
            std::vector<double> s;
            for(size_t i=LB+1;i<L;i++){
                double fac=0;for(int x=0;x<N;x++)fac+=(P[x].b[i-1].c/P[x].b[i-1-LB].c-1);fac/=N;
                std::vector<std::pair<double,int>> rk;
                for(int x=0;x<N;x++){double own=P[x].b[i-1].c/P[x].b[i-1-LB].c-1;rk.push_back({own-fac,x});}
                std::sort(rk.begin(),rk.end(),[](auto&a,auto&b){return a.first>b.first;});
                int hi=rk.front().second,lo=rk.back().second;
                double rl=(P[hi].b[i].c/P[hi].b[i-1].c-1)-2.0*P[hi].hs/1e4/5.0;
                double rs=-((P[lo].b[i].c/P[lo].b[i-1].c-1))-2.0*P[lo].hs/1e4/5.0;
                s.push_back((rl+rs)/2.0);}
            double sh,dd,tt;stats(s,sh,dd,tt);char nm[48];snprintf(nm,sizeof nm,"LB%d RESIDUAL-mom L/S",LB);
            printf("  %-30s %+5.2f  %+7.0f  %5.0f   %+.2f\n",nm,sh,tt*1e4,dd*1e4,corr_off(s,ew,LB-1));
        }
    }
    printf("\n  corr_to_EW near 0 = beta-stripped (real RV alpha). Positive L/S Sharpe + low corr = the prize.\n");
    return 0;
}
