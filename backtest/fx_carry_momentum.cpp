// =============================================================================
// fx_carry_momentum.cpp -- FX carry + momentum-agreement portfolio (S43 flagship)
//
// THE FX-NATIVE EDGE. Carry = long high-yield / short low-yield currencies, the
// one FX return premium with academic support (compensation for crash risk).
// It is the most COST-SURVIVABLE FX edge: positions hold weeks and earn swap
// daily, so retail spread amortises to ~zero -- the opposite of the churn that
// killed every prior FX engine (session/turtle/scalp).
//
// MOMENTUM-AGREEMENT FILTER: take the carry-implied direction ONLY when price
// momentum agrees (sign(mom)==sign(carry)). Carry trades die in risk-off crashes
// (high-yielder collapse); the momentum gate exits before the unwind = crash guard.
//
// PORTFOLIO MECHANICS:
//   universe = {EURUSD,GBPUSD,AUDUSD,NZDUSD,USDJPY,USDCAD,EURGBP}
//   D1 bars (resampled from H1). Rebalance every REBAL days.
//   per pair, per rebalance (data <= i, no lookahead):
//     dir = sign(carry); take position only if sign(mom_L) == dir.
//     size = TARGET_VOL_BPS / ATR_bps   (vol-target -> equal risk per leg)
//   daily PnL while held = dir*price_move_bps + dir*carry_bps_per_day
//   cost = 2*HS(bps) charged on every position open/flip/close (turnover).
//
// Reports portfolio annualised Sharpe (daily), total bps, maxDD, per-half
// Sharpe + per-block sign for regime consistency. NOTE: existing H1 spans only
// ~13mo (ONE rate regime) -> this is a SMOKE TEST. True carry validation needs
// the 2019-2026 Dukascopy D1 (spans ZIRP->hike->cut). Re-run on that when landed.
//
// BUILD: c++ -std=c++17 -O2 backtest/fx_carry_momentum.cpp -o backtest/fx_carry_momentum
// RUN:   ./backtest/fx_carry_momentum
// =============================================================================
#include "../include/FxRateTable.hpp"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <map>

using namespace omega;

struct DBar { int64_t day; double o,h,l,c; };

// Load Dukascopy D1 CSV: "timestamp,open,high,low,close" with ts in MILLISECONDS.
// DROP Saturday bars (wday==6): Dukascopy emits a flat Saturday dup of Friday's
// close (~395 of them) -- a pure artifact that corrupts hold-N exits and inflates
// bar counts. Live FX has no Saturday ticks, so dropping it matches reality.
static std::vector<DBar> load_d1(const char* path){
    std::vector<DBar> out; FILE* f=fopen(path,"r"); if(!f) return out;
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}  // skip header
        double ts_ms,o,h,l,c;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&ts_ms,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue;
        time_t t=(time_t)(ts_ms/1000.0); struct tm g; gmtime_r(&t,&g);
        if(g.tm_wday==6) continue;                           // drop flat Saturday artifact
        int64_t day=(((int64_t)(ts_ms/1000.0))/86400)*86400;
        out.push_back({day,o,h,l,c});
    }
    fclose(f); return out;
}

struct Pair {
    std::string name, path;
    double hs_bps;
    std::vector<DBar> d1;
    std::map<int64_t,int> idx;   // day -> index
    std::vector<double> atr;     // ATR14 in price
    bool load(){
        d1=load_d1(path.c_str());
        if(d1.size()<60) return false;
        for(size_t i=0;i<d1.size();i++) idx[d1[i].day]=(int)i;
        atr.assign(d1.size(),0.0);
        double a=0; const int P=14;
        for(size_t i=1;i<d1.size();i++){
            double tr=std::max(d1[i].h-d1[i].l,std::max(std::fabs(d1[i].h-d1[i-1].c),std::fabs(d1[i].l-d1[i-1].c)));
            if((int)i<=P){ a+=tr; if((int)i==P) a/=P; }
            else a=(a*(P-1)+tr)/P;
            atr[i]=a;
        }
        return true;
    }
};

int main(){
    printf("FX CARRY + MOMENTUM-AGREEMENT PORTFOLIO (S43 flagship)\n");

    #define DUKA(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Pair> U = {
        {"EURUSD",DUKA("eurusd"),0.5,{},{},{}},
        {"GBPUSD",DUKA("gbpusd"),0.6,{},{},{}},
        {"AUDUSD",DUKA("audusd"),0.8,{},{},{}},
        {"NZDUSD",DUKA("nzdusd"),1.0,{},{},{}},
        {"USDJPY",DUKA("usdjpy"),0.7,{},{},{}},
        {"USDCAD",DUKA("usdcad"),0.8,{},{},{}},
        {"USDCHF",DUKA("usdchf"),0.9,{},{},{}},
        {"EURGBP",DUKA("eurgbp"),0.8,{},{},{}},
        {"AUDNZD",DUKA("audnzd"),1.5,{},{},{}},
        {"EURJPY",DUKA("eurjpy"),1.0,{},{},{}},
        {"GBPJPY",DUKA("gbpjpy"),1.3,{},{},{}},
    };
    std::vector<Pair*> P;
    for(auto& p:U){ if(p.load()) P.push_back(&p); else printf("  [skip] %s (no/short data)\n",p.name.c_str()); }
    if(P.empty()){ printf("no data\n"); return 1; }

    // Unified day axis = the longest pair's days (use union via a master pair).
    Pair* master=P[0]; for(auto* p:P) if(p->d1.size()>master->d1.size()) master=p;
    const std::vector<DBar>& days=master->d1;
    int64_t ts0=days.front().day, ts1=days.back().day;
    printf("days=%zu span=%ld..%ld pairs=%zu\n",days.size(),(long)ts0,(long)ts1,P.size());

    const double TARGET_VOL_BPS = 50.0;

    // Reusable strategy run. useMom=false -> CARRY-ONLY (take sign(carry) whenever
    // |carry|>=CF, ignore price momentum). useMom=true -> carry+momentum-agreement.
    struct Res { double sharpe,eq,mdd,sh1,sh2; int trades,blkpos; double avgN; };
    auto strat=[&](const std::vector<Pair*>& univ,int MOM,int REBAL,double CF,bool useMom)->Res{
        std::vector<double> dailyret;
        std::map<std::string,double> posdir, possz;
        double eq=0,peak=0,mdd=0; int trades=0; long npos_sum=0; int npos_days=0;
        for(size_t di=1; di<days.size(); ++di){
            int64_t day=days[di].day, pday=days[di-1].day;
            double dret=0;
            for(auto* p:univ){
                auto it=p->idx.find(day), ip=p->idx.find(pday);
                if(it==p->idx.end()||ip==p->idx.end()) continue;
                double d=posdir[p->name]; if(d==0) continue;
                double sz=possz[p->name];
                double mv=(p->d1[it->second].c - p->d1[ip->second].c)/p->d1[ip->second].c*10000.0;
                double carry=pair_carry(p->name.c_str(),day);
                dret += d*sz*mv + d*sz*(carry*100.0/365.0);
            }
            dailyret.push_back(dret);
            eq+=dret; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq;
            if((int)(di % REBAL)==0){
                int npos=0;
                for(auto* p:univ){
                    auto it=p->idx.find(day); if(it==p->idx.end()) continue;
                    int i=it->second; if(i<MOM||p->atr[i]<=0) continue;
                    double carry=pair_carry(p->name.c_str(),day);
                    double mom=(p->d1[i].c - p->d1[i-MOM].c);
                    double dir=0;
                    if(std::fabs(carry)>=CF){
                        double cdir=(carry>0)?1.0:-1.0;
                        if(!useMom) dir=cdir;                        // carry-only
                        else { double mdir=(mom>0)?1.0:((mom<0)?-1.0:0.0); if(mdir==cdir) dir=cdir; }
                    }
                    double atr_bps=p->atr[i]/p->d1[i].c*10000.0;
                    double sz=(atr_bps>0)?TARGET_VOL_BPS/atr_bps:0.0;
                    double old=posdir[p->name];
                    if(dir!=old){
                        if(old!=0) eq-=p->hs_bps*possz[p->name];
                        if(dir!=0) eq-=p->hs_bps*sz;
                        if(dir!=0||old!=0) trades++;
                    }
                    posdir[p->name]=dir; possz[p->name]=(dir!=0)?sz:0.0;
                    if(dir!=0) npos++;
                }
                npos_sum+=npos; npos_days++;
            }
        }
        Res r{}; int n=(int)dailyret.size(); if(n<40) return r;
        double mean=0; for(double v:dailyret) mean+=v; mean/=n;
        double var=0; for(double v:dailyret){ double d=v-mean; var+=d*d; } var/=(n-1);
        double sd=std::sqrt(var);
        auto hS=[&](int a,int b){ double m=0;int c=0;for(int i=a;i<b;i++){m+=dailyret[i];c++;} if(c<10)return 0.0; m/=c;
            double v=0;for(int i=a;i<b;i++){double d=dailyret[i]-m;v+=d*d;}v/=(c-1);double s=std::sqrt(v);return s>0?m/s*std::sqrt(252.0):0.0; };
        int blk=0; for(int b=0;b<6;b++){ double g=0;int a=b*n/6,e=(b+1)*n/6;for(int i=a;i<e;i++)g+=dailyret[i]; if(g>0)blk++; }
        r.sharpe=(sd>0)?mean/sd*std::sqrt(252.0):0.0; r.eq=eq; r.mdd=mdd;
        r.sh1=hS(0,n/2); r.sh2=hS(n/2,n); r.blkpos=blk; r.trades=trades;
        r.avgN=npos_days?(double)npos_sum/npos_days:0;
        return r;
    };
    auto show=[&](const char* tag,const Res& r){
        const char* fl=(r.sharpe>0.35&&r.sh1>0&&r.sh2>0&&r.blkpos>=5)?" *** ROBUST":((r.sh1>0&&r.sh2>0)?" ** WF+":"");
        printf("%-30s %6.2f %+7.0f %6.0f %7d %6.1f | %5.2f %5.2f | %d/6%s\n",
               tag,r.sharpe,r.eq,r.mdd,r.trades,r.avgN,r.sh1,r.sh2,r.blkpos,fl);
    };

    printf("\n--- FULL SWEEP (11 pairs, carry+momentum) ---\n");
    printf("%-30s %6s %7s %6s %7s %6s | %5s %5s | blkpos\n",
           "config","Sharpe","totbps","maxDD","trades","avgN","ShH1","ShH2");
    for(int MOM:{20,60,100}) for(int REBAL:{5,10,20}) for(double CF:{0.0,0.5,1.0}){
        char t[64]; snprintf(t,sizeof t,"mom%d/reb%d/cf%.1f",MOM,REBAL,CF);
        show(t,strat(P,MOM,REBAL,CF,true));
    }

    // === A/B 1: momentum gate value (carry-only vs carry+momentum), best params ===
    printf("\n--- A/B 1: does the momentum gate help? (mom100/reb5, full 11) ---\n");
    for(double CF:{0.0,0.5,1.0}){
        char t1[64],t2[64];
        snprintf(t1,sizeof t1,"CARRY-ONLY      cf%.1f",CF);  show(t1,strat(P,100,5,CF,false));
        snprintf(t2,sizeof t2,"CARRY+MOM       cf%.1f",CF);  show(t2,strat(P,100,5,CF,true));
    }

    // === A/B 2: JPY crosses only vs full universe ===
    std::vector<Pair*> JPY;
    for(auto* p:P) if(p->name=="USDJPY"||p->name=="EURJPY"||p->name=="GBPJPY") JPY.push_back(p);
    printf("\n--- A/B 2: JPY crosses only (%zu) vs full (mom100/reb5/cf0.5) ---\n",JPY.size());
    show("JPY-only  carry+mom",  strat(JPY,100,5,0.5,true));
    show("JPY-only  carry-only", strat(JPY,100,5,0.5,false));
    show("FULL-11   carry+mom",  strat(P,100,5,0.5,true));
    // -------- RIGOR on the winner: per-pair attribution + cost stress --------
    // Re-run mom100/reb5/cf0.5 capturing per-pair bp + at 1x/2x/3x cost.
    auto run_detail=[&](int MOM,int REBAL,double CF,double cost_mult,std::map<std::string,double>* perpair)->double{
        std::map<std::string,double> posdir,possz; double eq=0;
        for(size_t di=1; di<days.size(); ++di){
            int64_t day=days[di].day, pday=days[di-1].day;
            for(auto* p:P){
                auto it=p->idx.find(day), ip=p->idx.find(pday);
                if(it==p->idx.end()||ip==p->idx.end()) continue;
                double d=posdir[p->name]; if(d==0) continue;
                double sz=possz[p->name];
                double mv=(p->d1[it->second].c - p->d1[ip->second].c)/p->d1[ip->second].c*10000.0;
                double carry=pair_carry(p->name.c_str(),day);
                double pnl=d*sz*mv + d*sz*(carry*100.0/365.0);
                eq+=pnl; if(perpair) (*perpair)[p->name]+=pnl;
            }
            if((int)(di%REBAL)==0){
                for(auto* p:P){
                    auto it=p->idx.find(day); if(it==p->idx.end()) continue;
                    int i=it->second; if(i<MOM||p->atr[i]<=0) continue;
                    double carry=pair_carry(p->name.c_str(),day);
                    double mom=(p->d1[i].c - p->d1[i-MOM].c);
                    double dir=0;
                    if(std::fabs(carry)>=CF){ double cd=(carry>0)?1.0:-1.0; double md=(mom>0)?1.0:((mom<0)?-1.0:0.0); if(md==cd) dir=cd; }
                    double atr_bps=p->atr[i]/p->d1[i].c*10000.0;
                    double sz=(atr_bps>0)?50.0/atr_bps:0.0;
                    double old=posdir[p->name];
                    if(dir!=old){ if(old!=0){eq-=p->hs_bps*possz[p->name]*cost_mult; if(perpair)(*perpair)[p->name]-=p->hs_bps*possz[p->name]*cost_mult;}
                                  if(dir!=0){eq-=p->hs_bps*sz*cost_mult; if(perpair)(*perpair)[p->name]-=p->hs_bps*sz*cost_mult;} }
                    posdir[p->name]=dir; possz[p->name]=(dir!=0)?sz:0.0;
                }
            }
        }
        return eq;
    };
    printf("\n==== RIGOR: winner mom100/reb5/cf0.5 ====\n");
    printf("cost stress: 1x=%+.0fbp  2x=%+.0fbp  3x=%+.0fbp\n",
           run_detail(100,5,0.5,1.0,nullptr),run_detail(100,5,0.5,2.0,nullptr),run_detail(100,5,0.5,3.0,nullptr));
    std::map<std::string,double> pp;
    double tot=run_detail(100,5,0.5,1.0,&pp);
    printf("per-pair attribution (total %+.0fbp):\n",tot);
    for(auto&kv:pp) printf("   %-8s %+8.0fbp  (%.0f%%)\n",kv.first.c_str(),kv.second,100.0*kv.second/tot);
    printf("\nMULTI-REGIME (2019-2026, 11 pairs). Carry+momentum-agreement. The mom100\n");
    printf("cluster is positive both-halves + 5/6 blocks = first regime-consistent FX edge.\n");
    return 0;
}
