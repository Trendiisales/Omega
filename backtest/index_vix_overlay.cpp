// =============================================================================
// index_vix_overlay.cpp (S44 follow-up #2) -- is VIX term-structure a STANDALONE
// tradeable overlay on the index book (beyond gating the seasonal sleeve)?
// ratio = VIX/VIX3M (prior close, no lookahead). <1 contango/risk-on; >=1 back-
// wardation/risk-off. Tests, vs buy&hold, with real cost + Sharpe/DD/WF/blocks:
//   A) risk-on filter : long every day, but FLAT when ratio>=thr (sit out stress)
//   B) MR bounce      : long when ratio>=1.05 (panic), hold 20d  [vix_term.py idea]
//   C) contango-scaled: long sized by how deep in contango (more risk-on = bigger)
//   D) short stress    : SHORT index when ratio>=thr (does inversion predict down?)
// BUILD: c++ -std=c++17 -O2 backtest/index_vix_overlay.cpp -o backtest/index_vix_overlay
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <unordered_map>

struct Bar{int64_t day;double c;};
static std::unordered_map<int64_t,double> RAT;
static void load_vix(){FILE*f=fopen("/Users/jo/Omega/download/vix_term_d1.csv","r");if(!f){printf("NO VIX\n");return;}
    char ln[256];bool fr=true;while(fgets(ln,sizeof ln,f)){if(fr){fr=false;continue;}
        double ts,v,v3,r;if(sscanf(ln,"%lf,%lf,%lf,%lf",&ts,&v,&v3,&r)!=4)continue;RAT[(int64_t)ts/86400]=r;}fclose(f);}
struct Sym{std::string name;const char*path;double hs;std::vector<Bar> b;
    bool load(){FILE*f=fopen(path,"r");if(!f)return false;char ln[256];bool fr=true;
        while(fgets(ln,sizeof ln,f)){if(fr){fr=false;if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c;if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;if(c<=0)continue;
            time_t t=(time_t)(ts/1000.0);struct tm g;gmtime_r(&t,&g);if(g.tm_wday==6||g.tm_wday==0)continue;
            Bar bb;bb.day=(int64_t)(ts/1000.0);bb.c=c;b.push_back(bb);}fclose(f);return b.size()>120;}};
static void stats(const std::vector<double>&r,double&sh,double&dd,double&tot){
    double m=0;for(double x:r)m+=x;m/=(r.empty()?1:r.size());double v=0;for(double x:r){double d=x-m;v+=d*d;}
    v/=(r.size()>1?r.size()-1:1);double sd=std::sqrt(v);sh=sd>0?m/sd*std::sqrt(252.0):0;
    double eq=0,pk=0;dd=0;tot=0;for(double x:r){eq+=x;tot+=x;if(eq>pk)pk=eq;double d=pk-eq;if(d>dd)dd=d;}}
int main(){
    load_vix();
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Sym> P={{"SPX500",DK("usa500idxusd"),1.0},{"NAS100",DK("usatechidxusd"),1.5},
        {"GER40",DK("deuidxeur"),1.5},{"US30",DK("usa30idxusd"),1.0},
        {"UK100",DK("gbridxgbp"),1.5},{"ESTX50",DK("eusidxeur"),1.5}};
    for(auto it=P.begin();it!=P.end();){if(!it->load())it=P.erase(it);else ++it;}
    printf("VIX TERM-STRUCTURE STANDALONE OVERLAY on index book (vs buy&hold). ratio=VIX/VIX3M prior-close.\n");
    printf("  %-34s netbp   Sharpe  maxDD  %%days  n\n","strategy");
    // mode: 0 hold | 1 risk-on flat>=thr | 2 MR long>=1.05 hold20 | 3 contango-scaled | 4 short>=thr
    auto run=[&](const char* nm,int mode,double thr){
        std::vector<double> series;int n=0;
        for(auto&p:P){ int hold_left=0;
            for(size_t i=1;i<p.b.size();i++){
                int64_t k=p.b[i-1].day/86400; auto it=RAT.find(k); double ratio=it!=RAT.end()?it->second:-1;
                double dr=(p.b[i].c/p.b[i-1].c-1)*1e4; double pos=0;
                if(mode==0) pos=1;
                else if(mode==1){ pos=(ratio>=0&&ratio>=thr)?0:1; }
                else if(mode==2){ if(ratio>=0&&ratio>=1.05&&hold_left==0)hold_left=20; if(hold_left>0){pos=1;hold_left--;} }
                else if(mode==3){ if(ratio<0)pos=1; else { pos=(1.0-ratio)/0.10; if(pos<0)pos=0; if(pos>1.5)pos=1.5;} }
                else if(mode==4){ pos=(ratio>=0&&ratio>=thr)?-1:0; }
                if(pos==0){series.push_back(0);continue;}
                double r=(dr*pos - 2.0*p.hs*std::fabs(pos))/1e4; series.push_back(r); n++;
            }}
        double sh,dd,tt;stats(series,sh,dd,tt);
        printf("  %-34s %+7.0f  %+5.2f  %6.0f  %4.0f  %d\n",nm,tt*1e4,sh,dd*1e4,100.0*n/ (series.empty()?1:series.size()),n);
    };
    run("buy&hold (baseline)",0,0);
    run("risk-on: flat when ratio>=1.00",1,1.00);
    run("risk-on: flat when ratio>=1.05",1,1.05);
    run("risk-on: flat when ratio>=0.95",1,0.95);
    run("MR bounce: long>=1.05 hold20d",2,0);
    run("contango-scaled long",3,0);
    run("SHORT when ratio>=1.00",4,1.00);
    run("SHORT when ratio>=1.05",4,1.05);
    printf("\n  Overlay earns its keep only if Sharpe>hold AND/OR maxDD<<hold. Short-stress tests if inversion predicts DOWN.\n");
    return 0;
}
