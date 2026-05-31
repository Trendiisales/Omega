// =============================================================================
// index_vix_gate.cpp (S44) -- does a VIX term-structure gate / vol-target sizing
// improve the best-2 day-of-week seasonal sleeve? Honest Sharpe/DD/vs-baseline.
// Sleeve = long the session entered at Tue-close + Fri-close (the two strong
// weekday sessions). Gate decided on PRIOR-close VIX ratio (no lookahead).
//   ratio = VIX/VIX3M ; <1 contango (risk-on) ; >=1 backwardation (risk-off).
// Variants: ungated baseline / trade-only-contango / trade-only-backwardation /
//   threshold sweep / inverse-VIX vol-target sizing.
// BUILD: c++ -std=c++17 -O2 backtest/index_vix_gate.cpp -o backtest/index_vix_gate
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <unordered_map>

struct Bar{int64_t day;double o,c;int wday;};
static std::unordered_map<int64_t,double> VIXR; // day-key -> ratio
static std::unordered_map<int64_t,double> VIXL; // day-key -> VIX level

static void load_vix(){
    FILE* f=fopen("/Users/jo/Omega/download/vix_term_d1.csv","r"); if(!f){printf("NO VIX\n");return;}
    char ln[256]; bool first=true;
    while(fgets(ln,sizeof ln,f)){ if(first){first=false;continue;}
        double ts,v,v3,r; if(sscanf(ln,"%lf,%lf,%lf,%lf",&ts,&v,&v3,&r)!=4)continue;
        int64_t k=((int64_t)ts/86400); VIXR[k]=r; VIXL[k]=v; }
    fclose(f);
}
struct Sym{std::string name;const char*path;double hs;std::vector<Bar> b;
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool first=true;
        while(fgets(ln,sizeof ln,f)){if(first){first=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0);struct tm g;gmtime_r(&t,&g);if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb;bb.day=(int64_t)(ts/1000.0);bb.o=o;bb.c=c;bb.wday=g.tm_wday;b.push_back(bb);}
        fclose(f);return b.size()>120;}
};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}
}
int main(){
    load_vix();
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P={{"SPX500",DK("usa500idxusd"),1.0},{"NAS100",DK("usatechidxusd"),1.5},
        {"GER40",DK("deuidxeur"),1.5},{"US30",DK("usa30idxusd"),1.0},
        {"UK100",DK("gbridxgbp"),1.5},{"ESTX50",DK("eusidxeur"),1.5}};
    for(auto it=P.begin();it!=P.end();){if(!it->load()){it=P.erase(it);}else ++it;}
    // strong sessions: weekday(i)==Mon(1) [entered Fri-close] or Wed(3) [entered Tue-close]
    auto isStrong=[](int wd){return wd==1||wd==3;};

    printf("VIX TERM-STRUCTURE GATE on best-2 seasonal sleeve (ratio=VIX/VIX3M, prior-close, no lookahead)\n");
    printf("  %-34s netbp   Sharpe  maxDD  %%days  n\n","variant");
    auto run=[&](const char* name,int mode,double thr){
        // mode: 0 ungated | 1 only ratio<thr (contango) | 2 only ratio>=thr (backwardation)
        //       3 vol-target: size = clamp(target/VIX,0.25,2.0)
        std::vector<double> series;int n=0;double tgt=18.0;
        for(auto&p:P)for(size_t i=1;i<p.b.size();i++){
            int wd=p.b[i].wday;if(wd<1||wd>5){continue;}
            if(!isStrong(wd)){series.push_back(0);continue;}
            int64_t k=(p.b[i-1].day/86400);auto itr=VIXR.find(k);
            double ratio= itr!=VIXR.end()?itr->second:-1;
            double vix= VIXL.count(k)?VIXL[k]:tgt;
            double sz=1.0;
            if(mode==1){ if(ratio<0||ratio>=thr){series.push_back(0);continue;} }
            else if(mode==2){ if(ratio<0||ratio<thr){series.push_back(0);continue;} }
            else if(mode==3){ sz=tgt/vix; if(sz<0.25)sz=0.25; if(sz>2.0)sz=2.0; }
            else if(mode==4){ if(ratio<0||ratio>=thr){series.push_back(0);continue;} sz=tgt/vix; if(sz<0.25)sz=0.25; if(sz>2.0)sz=2.0; }
            else if(mode==5){ if(vix>=thr){series.push_back(0);continue;} }  // VIX LEVEL gate (live-available)
            double r=((p.b[i].c/p.b[i-1].c-1)*1e4 - 2.0*p.hs)*sz;
            series.push_back(r/1e4); n++;
        }
        double sh,dd,tt;stats(series,sh,dd,tt);
        printf("  %-34s %+7.0f  %+5.2f  %6.0f  %4.0f  %d\n",name,tt*1e4,sh,dd*1e4,100.0*n/series.size(),n);
    };
    run("ungated baseline",0,0);
    run("only contango (ratio<1.00)",1,1.00);
    run("only contango (ratio<0.95)",1,0.95);
    run("only contango (ratio<0.90)",1,0.90);
    run("only backwardation (ratio>=1.00)",2,1.00);
    run("only backwardation (ratio>=0.95)",2,0.95);
    run("vol-target sizing (18/VIX)",3,0);
    run("contango<1.00 + vol-target",4,1.00);
    run("VIX level < 20 (live proxy)",5,20.0);
    run("VIX level < 25 (live proxy)",5,25.0);
    run("VIX level < 30 (live proxy)",5,30.0);
    printf("\n  Higher Sharpe AND/OR lower maxDD than ungated = gate adds. Else seasonal edge already self-sufficient.\n");
    return 0;
}
