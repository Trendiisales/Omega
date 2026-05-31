// =============================================================================
// fx_d1_momentum.cpp -- FX D1 time-series momentum / trend (S43, carry's twin)
//
// S41 "killed FX trend" but that was H1 + ported GOLD families. This is a clean
// FX-native D1 test: time-series momentum (CTA-style) + Donchian breakout, long
// AND short, vol-targeted, cost-stressed, WF + 6-block. Carry and momentum are
// the two academically-robust FX style premia -- carry validated; this tests the
// other. Portfolio MTM across 11 Dukascopy D1 pairs, same accounting as the
// carry harness so the two are directly comparable / combinable as sleeves.
//
// BUILD: c++ -std=c++17 -O2 backtest/fx_d1_momentum.cpp -o backtest/fx_d1_momentum
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <map>

struct DBar { int64_t day; double o,h,l,c; };

static std::vector<DBar> load_d1(const char* path){
    std::vector<DBar> v; FILE* f=fopen(path,"r"); if(!f){return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        double ts,o,h,l,c;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue;
        int64_t day=(((int64_t)(ts/1000.0))/86400)*86400;
        v.push_back({day,o,h,l,c});
    }
    fclose(f); return v;
}

struct Pair {
    std::string name, path; double hs_bps;
    std::vector<DBar> d1; std::map<int64_t,int> idx; std::vector<double> atr;
    bool load(){
        d1=load_d1(path.c_str()); if(d1.size()<260) return false;
        for(size_t i=0;i<d1.size();i++) idx[d1[i].day]=(int)i;
        atr.assign(d1.size(),0.0); double a=0; const int P=14;
        for(size_t i=1;i<d1.size();i++){
            double tr=std::fmax(d1[i].h-d1[i].l,std::fmax(std::fabs(d1[i].h-d1[i-1].c),std::fabs(d1[i].l-d1[i-1].c)));
            if((int)i<=P){ a+=tr; if((int)i==P)a/=P; } else a=(a*(P-1)+tr)/P;
            atr[i]=a;
        }
        return true;
    }
};

int main(){
    printf("FX D1 MOMENTUM / TREND SCAN (S43) -- long+short, vol-target, cost-stressed, WF+6block\n");
    #define DK(p) "/Users/jo/Omega/download/" p "-d1-bid-2019-01-01-2026-05-31.csv"
    std::vector<Pair> U = {
        {"EURUSD",DK("eurusd"),0.5},{"GBPUSD",DK("gbpusd"),0.6},{"AUDUSD",DK("audusd"),0.8},
        {"NZDUSD",DK("nzdusd"),1.0},{"USDJPY",DK("usdjpy"),0.7},{"USDCAD",DK("usdcad"),0.8},
        {"USDCHF",DK("usdchf"),0.9},{"EURGBP",DK("eurgbp"),0.8},{"AUDNZD",DK("audnzd"),1.5},
        {"EURJPY",DK("eurjpy"),1.0},{"GBPJPY",DK("gbpjpy"),1.3},
    };
    std::vector<Pair*> P; for(auto&p:U){ if(p.load()) P.push_back(&p); }
    if(P.empty()){ printf("no data\n"); return 1; }
    Pair* m=P[0]; for(auto*p:P) if(p->d1.size()>m->d1.size()) m=p;
    const std::vector<DBar>& days=m->d1;
    int64_t TSMIN=days.front().day, TSMAX=days.back().day, SPLIT=days[days.size()/2].day;
    printf("days=%zu pairs=%zu span=%ld..%ld\n",days.size(),P.size(),(long)TSMIN,(long)TSMAX);

    const double VOL=50.0, costmult=1.0;
    // signal: kind 0=TSMOM(close vs close[L]), 1=Donchian breakout(L)
    auto strat=[&](int kind,int L,double cm)->std::vector<double>{
        // returns {sharpe,eq,mdd,sh1,sh2,blkpos,trades}
        std::map<std::string,double> dir,sz; std::vector<double> dr; std::vector<int64_t> dts;
        double eq=0,peak=0,mdd=0; int trades=0;
        for(size_t di=1; di<days.size(); ++di){
            int64_t day=days[di].day, pday=days[di-1].day; double d=0;
            for(auto*p:P){ auto it=p->idx.find(day),ip=p->idx.find(pday);
                if(it==p->idx.end()||ip==p->idx.end()) continue;
                if(dir[p->name]==0) continue;
                double mv=(p->d1[it->second].c-p->d1[ip->second].c)/p->d1[ip->second].c*10000.0;
                d+=dir[p->name]*sz[p->name]*mv; }
            dr.push_back(d); dts.push_back(day); eq+=d; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq;
            // re-evaluate every bar (trend signals change slowly anyway)
            for(auto*p:P){ auto it=p->idx.find(day); if(it==p->idx.end()) continue;
                int i=it->second; if(i<L+1||p->atr[i]<=0) continue;
                double want=0;
                if(kind==0){ double r=p->d1[i].c-p->d1[i-L].c; want=(r>0)?1.0:((r<0)?-1.0:0.0); }
                else { // Donchian: highest high / lowest low over [i-L, i-1]
                    double hh=p->d1[i-1].h, ll=p->d1[i-1].l;
                    for(int k=i-L;k<i;k++){ if(p->d1[k].h>hh)hh=p->d1[k].h; if(p->d1[k].l<ll)ll=p->d1[k].l; }
                    if(p->d1[i].c>hh) want=1.0; else if(p->d1[i].c<ll) want=-1.0; else want=dir[p->name]; // hold
                }
                double atr_bps=p->atr[i]/p->d1[i].c*10000.0; double s=(atr_bps>0)?VOL/atr_bps:0.0;
                double old=dir[p->name];
                if(want!=old){ if(old!=0)eq-=p->hs_bps*sz[p->name]*cm; if(want!=0)eq-=p->hs_bps*s*cm; if(want!=0||old!=0)trades++; }
                dir[p->name]=want; sz[p->name]=(want!=0)?s:0.0;
            }
        }
        // pack stats into the returned vector
        int n=(int)dr.size(); std::vector<double> out(7,0);
        if(n<60) return out;
        double me=0; for(double v:dr)me+=v; me/=n; double va=0; for(double v:dr){double q=v-me;va+=q*q;} va/=(n-1);
        double sd=std::sqrt(va);
        auto hS=[&](int a,int b){ double mm=0;int c=0;for(int i=a;i<b;i++){mm+=dr[i];c++;}if(c<10)return 0.0;mm/=c;
            double vv=0;for(int i=a;i<b;i++){double q=dr[i]-mm;vv+=q*q;}vv/=(c-1);double s=std::sqrt(vv);return s>0?mm/s*std::sqrt(252.0):0.0; };
        int blk=0; for(int b=0;b<6;b++){double g=0;int a=b*n/6,e=(b+1)*n/6;for(int i=a;i<e;i++)g+=dr[i]; if(g>0)blk++;}
        out[0]=(sd>0)?me/sd*std::sqrt(252.0):0.0; out[1]=eq; out[2]=mdd; out[3]=hS(0,n/2); out[4]=hS(n/2,n); out[5]=blk; out[6]=trades;
        return out;
    };
    auto show=[&](const char* tag,std::vector<double> r){
        const char* fl=(r[0]>0.35&&r[3]>0&&r[4]>0&&r[5]>=5)?" *** ROBUST":((r[3]>0&&r[4]>0)?" ** WF+":"");
        printf("%-22s Sh=%5.2f tot=%+7.0f mdd=%6.0f tr=%5.0f | H1=%5.2f H2=%5.2f | blk=%.0f/6%s\n",
               tag,r[0],r[1],r[2],r[6],r[3],r[4],r[5],fl);
    };

    printf("\n--- TSMOM (close vs close[L]) ---\n");
    for(int L:{20,50,100,150,200}){ char t[32]; snprintf(t,sizeof t,"TSMOM L%d",L); show(t,strat(0,L,costmult)); }
    printf("\n--- Donchian breakout (L) ---\n");
    for(int L:{20,50,100,200}){ char t[32]; snprintf(t,sizeof t,"Donch L%d",L); show(t,strat(1,L,costmult)); }
    printf("\n--- cost stress on best TSMOM (L100) ---\n");
    show("TSMOM L100 1x",strat(0,100,1.0)); show("TSMOM L100 3x",strat(0,100,3.0));
    return 0;
}
