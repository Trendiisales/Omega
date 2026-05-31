// =============================================================================
// fx_combined_sleeve.cpp -- can 3 THIN uncorrelated FX edges combine into ONE
// robust sleeve? (S43, the "stack small edges" test.)
//   sleeve A: Friday-long seasonality (D1, 9 pairs)
//   sleeve B: COT positioning-fade   (D1, 9 pairs, W104/z1.5, 6d lag)
//   sleeve C: London-session breakout(EURUSD M1, buf0.20/tp2R/compress40)
// Each -> daily bp series. Equal-RISK combine (divide each by its full-sample
// daily sd). Report per-sleeve + combined Sharpe/WF-halves/6-block + the
// cross-sleeve correlation (low corr = real diversification). HONEST: all
// signals no-lookahead + real cost (already in each sleeve's bp).
//
// VERDICT RULE: combined viable iff Sharpe>0.40 AND both halves+ AND >=5/6 blocks
// AND sleeves genuinely uncorrelated (|corr|<0.3). Else CULL.
//
// BUILD: c++ -std=c++17 -O2 backtest/fx_combined_sleeve.cpp -o backtest/fx_combined_sleeve
// =============================================================================
#include "../include/FxRateTable.hpp"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>
using namespace omega;

struct DBar{int64_t day;double o,h,l,c;};
static int wday(int64_t day_sec){int64_t dd=day_sec/86400;return (int)(((dd%7)+4+7)%7);} // Sat=6,Fri=5,Mon=1
static std::vector<DBar> load_d1(const char* p){
    std::vector<DBar> v;FILE* f=fopen(p,"r");if(!f)return v;char ln[256];bool fst=true;
    while(fgets(ln,sizeof ln,f)){if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
        double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
        int64_t d=(int64_t)(ts/1000.0)/86400;if(wday(d*86400)==6)continue;v.push_back({d*86400,o,h,l,c});}
    fclose(f);return v;}

struct PP{const char*name,*path;double hs;int psign;const char*cot;};
static std::vector<PP> PAIRS={
 {"EURUSD","/Users/jo/Omega/download/eurusd-d1-bid-2019-01-01-2026-05-31.csv",0.5,+1,"\"EURO FX - CHICAGO"},
 {"GBPUSD","/Users/jo/Omega/download/gbpusd-d1-bid-2019-01-01-2026-05-31.csv",0.6,+1,"\"BRITISH POUND - CHICAGO"},
 {"USDJPY","/Users/jo/Omega/download/usdjpy-d1-bid-2019-01-01-2026-05-31.csv",0.7,-1,"\"JAPANESE YEN - CHICAGO"},
 {"AUDUSD","/Users/jo/Omega/download/audusd-d1-bid-2019-01-01-2026-05-31.csv",0.8,+1,"\"AUSTRALIAN DOLLAR - CHICAGO"},
 {"NZDUSD","/Users/jo/Omega/download/nzdusd-d1-bid-2019-01-01-2026-05-31.csv",1.0,+1,"\"NEW ZEALAND DOLLAR - CHICAGO"},
 {"USDCAD","/Users/jo/Omega/download/usdcad-d1-bid-2019-01-01-2026-05-31.csv",0.8,-1,"\"CANADIAN DOLLAR - CHICAGO"},
 {"USDCHF","/Users/jo/Omega/download/usdchf-d1-bid-2019-01-01-2026-05-31.csv",0.9,-1,"\"SWISS FRANC - CHICAGO"},
 {"EURGBP","/Users/jo/Omega/download/eurgbp-d1-bid-2019-01-01-2026-05-31.csv",0.8,+1,"\"EURO FX/BRITISH POUND XRATE - CHICAGO"},
 {"EURJPY","/Users/jo/Omega/download/eurjpy-d1-bid-2019-01-01-2026-05-31.csv",1.0,+1,"\"EURO FX/JAPANESE YEN XRATE - CHICAGO"},
};

// ---- sleeve A: Friday-long seasonality -> daily bp keyed by entry(Friday) day
static std::map<int64_t,double> sleeveFriday(){
    std::map<int64_t,double> out;
    for(auto&pr:PAIRS){ auto v=load_d1(pr.path); for(size_t i=1;i+1<v.size();i++){
        if(wday(v[i].day)!=5) continue; double e=v[i].c,x=v[i+1].c;
        out[v[i].day]+=(x-e)/e*1e4 - 2*pr.hs; }}
    return out;
}
// ---- sleeve B: COT positioning-fade portfolio -> daily bp
static std::map<int64_t,double> sleeveCOT(){
    // load COT per pair
    std::map<std::string,std::vector<std::pair<int64_t,double>>> cot;
    for(int y=2019;y<=2026;y++){char p[64];snprintf(p,sizeof p,"cot/cot_%d.txt",y);FILE*f=fopen(p,"r");if(!f)continue;char ln[1024];
        while(fgets(ln,sizeof ln,f)){for(auto&pr:PAIRS){if(strncmp(ln,pr.cot,strlen(pr.cot))==0){
            std::vector<std::string> fl;std::string cur;for(const char*q=ln;*q&&fl.size()<12;++q){if(*q==','){fl.push_back(cur);cur.clear();}else if(*q!='"'&&*q!='\n'&&*q!='\r')cur+=*q;}fl.push_back(cur);
            if(fl.size()>=10){int Y,M,D;if(sscanf(fl[2].c_str(),"%d-%d-%d",&Y,&M,&D)==3)cot[pr.name].push_back({ep(Y,M,D),atof(fl[8].c_str())-atof(fl[9].c_str())});}
            break;}}}fclose(f);}
    for(auto&kv:cot)std::sort(kv.second.begin(),kv.second.end());
    const int64_t LAG=6*86400;const int W=104;const double ZTH=1.5;
    std::map<std::string,std::vector<DBar>> px;std::map<std::string,std::unordered_map<int64_t,int>> idx;
    for(auto&pr:PAIRS){auto v=load_d1(pr.path);for(size_t i=0;i<v.size();i++)idx[pr.name][v[i].day]=(int)i;px[pr.name]=v;}
    auto fdir=[&](const PP&pr,int64_t day)->double{auto&c=cot[pr.name];if((int)c.size()<W+2)return 0;int last=-1;for(int i=0;i<(int)c.size();i++){if(c[i].first+LAG<=day)last=i;else break;}if(last<W)return 0;
        double s=0;for(int i=last-W+1;i<=last;i++)s+=c[i].second;double m=s/W;double v=0;for(int i=last-W+1;i<=last;i++){double d=c[i].second-m;v+=d*d;}double sd=std::sqrt(v/(W-1));if(sd<=0)return 0;double z=(c[last].second-m)/sd;if(std::fabs(z)<ZTH)return 0;return -(z>0?1.0:-1.0)*pr.psign;};
    std::map<int64_t,double> out;std::map<std::string,double> dir,sz;auto&days=px["EURUSD"];
    for(size_t di=1;di<days.size();di++){int64_t day=days[di].day,pd=days[di-1].day;double dd=0;
        for(auto&pr:PAIRS){auto&I=idx[pr.name];auto it=I.find(day),ip=I.find(pd);if(it==I.end()||ip==I.end())continue;if(dir[pr.name]==0)continue;
            dd+=dir[pr.name]*sz[pr.name]*(px[pr.name][it->second].c-px[pr.name][ip->second].c)/px[pr.name][ip->second].c*1e4;}
        out[day]+=dd;
        if((int)(di%5)!=0)continue;
        for(auto&pr:PAIRS){auto&I=idx[pr.name];auto it=I.find(day);if(it==I.end())continue;int i=it->second;if(i<20)continue;double want=fdir(pr,day);
            double atr=0;int cn=0;for(int k=i-14;k<i&&k>0;k++){atr+=px[pr.name][k].h-px[pr.name][k].l;cn++;}atr=cn?atr/cn:0;double ab=atr/px[pr.name][i].c*1e4;double s=ab>0?50/ab:0;
            double old=dir[pr.name];if(want!=old){if(old!=0)out[day]-=pr.hs*sz[pr.name];if(want!=0)out[day]-=pr.hs*s;}dir[pr.name]=want;sz[pr.name]=want!=0?s:0;}}
    return out;
}
// ---- sleeve C: EURUSD London-session breakout (M1) -> daily bp keyed by entry day
static std::map<int64_t,double> sleeveSession(){
    std::map<int64_t,double> out;
    auto load=[&](const char*p){std::unordered_map<int64_t,double> m;m.reserve(5000000);FILE*f=fopen(p,"r");if(!f)return m;char ln[256];bool fst=true;
        while(fgets(ln,sizeof ln,f)){if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c>0)m[(int64_t)(ts/1000.0)]=c;}fclose(f);return m;};
    auto bid=load("/Users/jo/Omega/download/eurusd-m1-bid-2019-01-01-2026-05-31.csv");
    auto ask=load("/Users/jo/Omega/download/eurusd-m1-ask-2019-01-01-2026-05-31.csv");
    if(bid.empty()||ask.empty()){printf("  [sleeveC] no EURUSD M1 -> session sleeve EMPTY\n");return out;}
    struct M{int64_t ts;double b,a;};std::vector<M> v;for(auto&kv:bid){auto it=ask.find(kv.first);if(it!=ask.end()&&it->second>kv.second)v.push_back({kv.first,kv.second,it->second});}
    std::sort(v.begin(),v.end(),[](const M&a,const M&b){return a.ts<b.ts;});
    auto uh=[&](int64_t ts){int h=(int)((ts/3600)%24);return h<0?h+24:h;};
    size_t i=0;while(i<v.size()){int64_t d0=(v[i].ts/86400)*86400;size_t j=i;while(j<v.size()&&(v[j].ts/86400)*86400==d0)j++;
        double hi=-1e18,lo=1e18;for(size_t k=i;k<j;k++){int h=uh(v[k].ts);if(h<7){double m=(v[k].b+v[k].a)*0.5;if(m>hi)hi=m;if(m<lo)lo=m;}}
        if(hi>lo){double rng=hi-lo;if(rng/lo*1e4<=40.0){double buf=rng*0.20;bool in=false,lng=false;double e=0,sl=0,tp=0;
            for(size_t k=i;k<j;k++){int h=uh(v[k].ts);if(!in){if(h<7||h>=12)continue;double m=(v[k].b+v[k].a)*0.5;
                if(m>hi+buf){in=true;lng=true;e=v[k].a;sl=lo;tp=e+2.0*rng;}else if(m<lo-buf){in=true;lng=false;e=v[k].b;sl=hi;tp=e-2.0*rng;}}
                else{double xb=v[k].b,xa=v[k].a;bool ex=false;double xp=0;if(lng){if(xb<=sl||xb>=tp){ex=true;xp=xb;}}else{if(xa>=sl||xa<=tp){ex=true;xp=xa;}}if(!ex&&h>=21){ex=true;xp=lng?xb:xa;}
                    if(ex){out[d0]+=(lng?(xp-e):(e-xp))/e*1e4;break;}}}}}
        i=j;}
    return out;
}

static void stats(const char* tag,const std::vector<std::pair<int64_t,double>>& s,int64_t split,double ppy){
    int n=s.size();if(n<60){printf("  %-12s n=%d too few\n",tag,n);return;}
    double me=0;for(auto&x:s)me+=x.second;me/=n;double va=0;for(auto&x:s){double q=x.second-me;va+=q*q;}va/=(n-1);double sd=std::sqrt(va);
    double h1=0,h2=0;for(auto&x:s){if(x.first<split){h1+=x.second;}else{h2+=x.second;}}
    int64_t tmin=s.front().first,tmax=s.back().first;double blk[6]={0};for(auto&x:s){int b=(int)(6.0*(x.first-tmin)/(double)(tmax-tmin+1));if(b<0)b=0;if(b>5)b=5;blk[b]+=x.second;}
    int bp=0;for(int i=0;i<6;i++)if(blk[i]>0)bp++;double tot=0;for(auto&x:s)tot+=x.second;
    double sh=sd>0?me/sd*std::sqrt(ppy):0;   // ppy = periods/yr (weekly=52, daily=252) -- frequency-honest
    const char* fl=(sh>0.40&&h1>0&&h2>0&&bp>=5)?" *** ROBUST":((h1>0&&h2>0)?" ** WF+":"");
    printf("  %-12s Sharpe=%5.2f tot=%+7.0f H1=%+6.0f H2=%+6.0f blk=%d/6 n=%d%s\n",tag,sh,tot,h1,h2,bp,n,fl);
}

int main(){
    printf("FX COMBINED SLEEVE -- stack 3 thin edges (Friday + COT + London-session). Honest WF+blocks.\n");
    auto A=sleeveFriday(); auto B=sleeveCOT(); auto C=sleeveSession();
    printf("sleeve days: Friday=%zu COT=%zu Session=%zu\n",A.size(),B.size(),C.size());
    // union of dates
    std::map<int64_t,int> all; for(auto&kv:A)all[kv.first]=1; for(auto&kv:B)all[kv.first]=1; for(auto&kv:C)all[kv.first]=1;
    // per-sleeve full-sample sd for equal-risk scaling
    auto sdof=[&](std::map<int64_t,double>&m){double s=0;int n=0;for(auto&kv:m){s+=kv.second;n++;}double me=n?s/n:0;double v=0;for(auto&kv:m){double q=kv.second-me;v+=q*q;}return n>1?std::sqrt(v/(n-1)):1.0;};
    double sA=sdof(A),sB=sdof(B),sC=sdof(C); if(sA<=0)sA=1;if(sB<=0)sB=1;if(sC<=0)sC=1;
    std::vector<std::pair<int64_t,double>> vA,vB,vC,comb;
    for(auto&kv:all){int64_t d=kv.first;double a=A.count(d)?A[d]:0,b=B.count(d)?B[d]:0,c=C.count(d)?C[d]:0;
        if(A.count(d))vA.push_back({d,a}); if(B.count(d))vB.push_back({d,b}); if(C.count(d))vC.push_back({d,c});
        comb.push_back({d,a/sA+b/sB+c/sC});}   // equal-risk sum (unit-vol each)
    int64_t TMIN=comb.front().first,TMAX=comb.back().first,SPLIT=TMIN+(TMAX-TMIN)/2;
    printf("\n--- individual sleeves (raw bp) ---\n");
    stats("Friday(wk)",vA,SPLIT,52.0); stats("COT",vB,SPLIT,252.0); stats("Session",vC,SPLIT,252.0);
    printf("\n--- COMBINED (equal-risk, unit-vol each) ---\n");
    stats("COMBINED",comb,SPLIT,252.0);
    // pairwise correlation on common dates (diversification check)
    auto corr=[&](std::map<int64_t,double>&X,std::map<int64_t,double>&Y){double sx=0,sy=0,sxy=0,sxx=0,syy=0;int n=0;
        for(auto&kv:X){if(!Y.count(kv.first))continue;double x=kv.second,y=Y[kv.first];sx+=x;sy+=y;sxy+=x*y;sxx+=x*x;syy+=y*y;n++;}
        if(n<30)return 0.0;double cov=sxy/n-(sx/n)*(sy/n),vx=sxx/n-(sx/n)*(sx/n),vy=syy/n-(sy/n)*(sy/n);return (vx>0&&vy>0)?cov/std::sqrt(vx*vy):0.0;};
    printf("\ncorrelations: Friday-COT=%.2f Friday-Session=%.2f COT-Session=%.2f  (|corr|<0.3 = real diversification)\n",
           corr(A,B),corr(A,C),corr(B,C));
    printf("\nVERDICT: combined viable iff Sharpe>0.40 + both halves + 5/6 blocks + low corr. Else CULL -> indices.\n");
    return 0;
}
