// =============================================================================
// fx_cot_fade.cpp -- CFTC COT positioning-fade FX edge (S43, NEW DATA).
// Top FX traders fade crowded speculative positioning. CFTC COT (free, weekly)
// gives CME-FX-futures non-commercial (spec) net positions. When specs are at a
// net-long/short EXTREME (z-score over trailing window), fade it (contrarian).
//
// HONEST / NO-LOOKAHEAD: COT is as-of Tuesday, RELEASED Friday ~15:30 ET. A COT
// reading is only tradeable AFTER release -> we lag it 6 days (apply from the
// Monday after). z-score uses only prior COT prints. Real per-pair spread cost
// on weekly position change. WF 50% split + 6-block + 3x cost stress. Portfolio
// daily-MTM (vol-targeted) across 9 pairs, same accounting as the carry harness.
//
// Mapping CME currency future -> FX pair (+ sign: +1 if currency is the BASE of
// the pair, -1 if the QUOTE). Fade dir in pair terms = -sign(spec_z) * pair_sign.
//
// BUILD: c++ -std=c++17 -O2 backtest/fx_cot_fade.cpp -o backtest/fx_cot_fade
// =============================================================================
#include "../include/FxRateTable.hpp"   // for omega::ep(y,m,d)
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

using namespace omega;

struct CotPt { int64_t ts; double net; };          // weekly net spec (already signed for pair)
struct PairD { std::string name,path; double hs_bps; int pair_sign; const char* cot_prefix; };

struct DBar { int64_t day; double o,h,l,c; };
static std::vector<DBar> load_d1(const char* path){
    std::vector<DBar> v; FILE* f=fopen(path,"r"); if(!f) return v; char ln[256]; bool first=true;
    while(fgets(ln,sizeof ln,f)){ if(first){first=false;if(ln[0]<'0'||ln[0]>'9')continue;}
        double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue; if(c<=0)continue;
        int64_t dd=(int64_t)(ts/1000.0)/86400; if(((dd%7)+4+7)%7==6) continue;   // drop Sat artifact
        v.push_back({dd*86400,o,h,l,c}); }
    fclose(f); return v;
}

// parse one COT line's date(col2 YYYY-MM-DD) + OI(7) ncLong(8) ncShort(9) [0-indexed]
static bool parse_cot(const char* line,int64_t& ts,double& net){
    // split by comma into up to 12 fields (names have no internal comma for our markets)
    const char* p=line; std::vector<std::string> f; std::string cur;
    for(; *p && f.size()<12; ++p){ if(*p==','){f.push_back(cur);cur.clear();} else if(*p!='"'&&*p!='\n'&&*p!='\r') cur+=*p; }
    f.push_back(cur);
    if(f.size()<10) return false;
    int y,m,d; if(sscanf(f[2].c_str(),"%d-%d-%d",&y,&m,&d)!=3) return false;
    double ncL=atof(f[8].c_str()), ncS=atof(f[9].c_str());
    ts=ep(y,m,d); net=ncL-ncS; return true;
}

int main(){
    printf("FX COT POSITIONING-FADE (S43, CFTC weekly) -- honest: 6d release lag, real cost, WF+6block+3x\n");
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<PairD> P = {
        {"EURUSD",DK("eurusd"),0.5,+1,"\"EURO FX - CHICAGO"},
        {"GBPUSD",DK("gbpusd"),0.6,+1,"\"BRITISH POUND - CHICAGO"},
        {"USDJPY",DK("usdjpy"),0.7,-1,"\"JAPANESE YEN - CHICAGO"},
        {"AUDUSD",DK("audusd"),0.8,+1,"\"AUSTRALIAN DOLLAR - CHICAGO"},
        {"NZDUSD",DK("nzdusd"),1.0,+1,"\"NEW ZEALAND DOLLAR - CHICAGO"},
        {"USDCAD",DK("usdcad"),0.8,-1,"\"CANADIAN DOLLAR - CHICAGO"},
        {"USDCHF",DK("usdchf"),0.9,-1,"\"SWISS FRANC - CHICAGO"},
        {"EURGBP",DK("eurgbp"),0.8,+1,"\"EURO FX/BRITISH POUND XRATE - CHICAGO"},
        {"EURJPY",DK("eurjpy"),1.0,+1,"\"EURO FX/JAPANESE YEN XRATE - CHICAGO"},
    };
    // load COT per pair
    std::map<std::string,std::vector<CotPt>> cot;
    for(int y=2019;y<=2026;y++){
        char path[64]; snprintf(path,sizeof path,"cot/cot_%d.txt",y);
        FILE* f=fopen(path,"r"); if(!f) continue; char ln[1024];
        while(fgets(ln,sizeof ln,f)){
            for(auto&pr:P){ if(strncmp(ln,pr.cot_prefix,strlen(pr.cot_prefix))==0){
                int64_t ts; double net; if(parse_cot(ln,ts,net)) cot[pr.name].push_back({ts,net}); break; } }
        }
        fclose(f);
    }
    for(auto&kv:cot) std::sort(kv.second.begin(),kv.second.end(),[](const CotPt&a,const CotPt&b){return a.ts<b.ts;});
    printf("COT series loaded: "); for(auto&pr:P) printf("%s=%zu ",pr.name.c_str(),cot[pr.name].size()); printf("\n");

    // load prices
    std::map<std::string,std::vector<DBar>> px; std::map<std::string,std::map<int64_t,int>> idx;
    int64_t TMIN=0,TMAX=0;
    for(auto&pr:P){ auto v=load_d1(pr.path.c_str()); if(v.size()<260) continue; for(size_t i=0;i<v.size();i++) idx[pr.name][v[i].day]=(int)i; px[pr.name]=v;
        if(TMIN==0||v.front().day<TMIN)TMIN=v.front().day; if(v.back().day>TMAX)TMAX=v.back().day; }
    int64_t SPLIT=TMIN+(TMAX-TMIN)/2;
    const int64_t LAG=6*86400;   // COT release lag (Tue as-of -> Fri release -> trade after)

    // signal at day for a pair: z-score of net over trailing W cot prints (using only
    // prints with ts+LAG <= day). fade dir = -sign(z)*pair_sign if |z|>=zth.
    auto faded_dir=[&](const PairD& pr,int64_t day,int W,double zth)->double{
        auto& c=cot[pr.name]; if((int)c.size()<W+2) return 0;
        // find last index whose ts+LAG <= day
        int last=-1; for(int i=0;i<(int)c.size();i++){ if(c[i].ts+LAG<=day) last=i; else break; }
        if(last<W) return 0;
        double sum=0; for(int i=last-W+1;i<=last;i++) sum+=c[i].net; double m=sum/W;
        double v=0; for(int i=last-W+1;i<=last;i++){double d=c[i].net-m;v+=d*d;} double sd=std::sqrt(v/(W-1));
        if(sd<=0) return 0; double z=(c[last].net-m)/sd;
        if(std::fabs(z)<zth) return 0;
        return -(z>0?1.0:-1.0)*pr.pair_sign;     // fade the crowd, in pair terms
    };

    struct Res{double sharpe,eq,mdd,sh1,sh2;int trades,blk;};
    auto run=[&](int W,double zth,int rebal,double cm)->Res{
        std::map<std::string,double> dir,sz; std::vector<double> dr; std::vector<int64_t> dts;
        double eq=0,peak=0,mdd=0; int trades=0;
        // master day axis = EURUSD
        auto& days=px["EURUSD"];
        for(size_t di=1;di<days.size();di++){
            int64_t day=days[di].day,pday=days[di-1].day; double d=0;
            for(auto&pr:P){ auto&I=idx[pr.name]; auto it=I.find(day),ip=I.find(pday); if(it==I.end()||ip==I.end())continue;
                if(dir[pr.name]==0)continue; double mv=(px[pr.name][it->second].c-px[pr.name][ip->second].c)/px[pr.name][ip->second].c*1e4;
                d+=dir[pr.name]*sz[pr.name]*mv; }
            dr.push_back(d); dts.push_back(day); eq+=d; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq;
            if((int)(di%rebal)!=0) continue;
            for(auto&pr:P){ auto&I=idx[pr.name]; auto it=I.find(day); if(it==I.end())continue; int i=it->second; if(i<20)continue;
                double want=faded_dir(pr,day,W,zth);
                // vol-target via 14-bar ATR proxy (high-low)
                double atr=0;int cnt=0; for(int k=i-14;k<i&&k>0;k++){atr+=px[pr.name][k].h-px[pr.name][k].l;cnt++;} atr=cnt?atr/cnt:0;
                double atrbp=atr/px[pr.name][i].c*1e4; double s=atrbp>0?50.0/atrbp:0;
                double old=dir[pr.name];
                if(want!=old){ if(old!=0)eq-=pr.hs_bps*sz[pr.name]*cm; if(want!=0)eq-=pr.hs_bps*s*cm; if(want!=0||old!=0)trades++; }
                dir[pr.name]=want; sz[pr.name]=(want!=0)?s:0; }
        }
        Res r{}; int n=dr.size(); if(n<60)return r; double me=0;for(double x:dr)me+=x;me/=n;
        double va=0;for(double x:dr){double q=x-me;va+=q*q;}va/=(n-1);double sd=std::sqrt(va);
        auto hS=[&](int a,int b){double mm=0;int c=0;for(int i=a;i<b;i++){mm+=dr[i];c++;}if(c<10)return 0.0;mm/=c;double vv=0;for(int i=a;i<b;i++){double q=dr[i]-mm;vv+=q*q;}vv/=(c-1);double s=std::sqrt(vv);return s>0?mm/s*std::sqrt(252.0):0.0;};
        int blk=0;for(int b=0;b<6;b++){double g=0;int a=b*n/6,e=(b+1)*n/6;for(int i=a;i<e;i++)g+=dr[i];if(g>0)blk++;}
        r.sharpe=sd>0?me/sd*std::sqrt(252.0):0;r.eq=eq;r.mdd=mdd;r.sh1=hS(0,n/2);r.sh2=hS(n/2,n);r.trades=trades;r.blk=blk; return r;
    };
    auto show=[&](const char*t,Res r,double net3){ const char* fl=(r.sharpe>0.35&&r.sh1>0&&r.sh2>0&&r.blk>=5)?" *** ROBUST":((r.sh1>0&&r.sh2>0)?" ** WF+":(r.eq>0?" (1-sided)":""));
        printf("  %-22s Sh=%5.2f tot=%+7.0f [3x %+7.0f] mdd=%6.0f tr=%4d | H1=%5.2f H2=%5.2f blk=%d/6%s\n",t,r.sharpe,r.eq,net3,r.mdd,r.trades,r.sh1,r.sh2,r.blk,fl); };

    printf("\nzwindow x zthresh sweep (rebal weekly=5d):\n");
    for(int W:{52,104,156}) for(double z:{1.0,1.5,2.0}){
        char t[32]; snprintf(t,sizeof t,"W%d z%.1f",W,z);
        Res r1=run(W,z,5,1.0); Res r3=run(W,z,5,3.0); show(t,r1,r3.eq);
    }
    printf("\n*** ROBUST = Sharpe>0.35 + both halves + 5/6 blocks. Honest: 6d release lag, real cost.\n");
    return 0;
}
