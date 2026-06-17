// =============================================================================
// fx_xrev_tf_sweep.cpp -- EURGBP FxCrossRevEngine across intraday timeframes.
// Aggregates real Dukascopy M1 bid+ask (2019-2026) to M1/M5/M10/M15/M30/H1/H4/D1,
// runs the PRODUCTION FxCrossRevEngine over a full lever grid per TF. Real spread
// from data (closing ask-bid); net_pnl already subtracts round-trip spread cost.
// Cost gate bypassed to expose the raw edge gradient across TFs.
//
// stdout = engine chatter -> /dev/null. Results emitted to STDERR as "ROW,..." CSV.
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/fx_xrev_tf_sweep.cpp -o backtest/fx_xrev_tf_sweep
// RUN:   ./backtest/fx_xrev_tf_sweep 2> sweep.csv   (stdout discarded)
// =============================================================================
#include "FxCrossRevEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
using namespace omega;

struct M1 { int64_t ts; double bc, ac; };
struct Bar { int64_t ts; double h,l,c,bid,ask; };

static std::vector<M1> load_m1(const char* bidp, const char* askp){
    std::vector<M1> v; v.reserve(4000000);
    FILE* fb=fopen(bidp,"r"); FILE* fa=fopen(askp,"r");
    if(!fb||!fa){ perror("open"); return v; }
    char lb[256], la[256]; bool first=true;
    while(fgets(lb,sizeof lb,fb) && fgets(la,sizeof la,fa)){
        if(first){first=false; if(lb[0]<'0'||lb[0]>'9') continue;}
        double tb,bo,bh,bl,bc, ta,ao,ah,al,ac;
        if(sscanf(lb,"%lf,%lf,%lf,%lf,%lf",&tb,&bo,&bh,&bl,&bc)!=5) continue;
        if(sscanf(la,"%lf,%lf,%lf,%lf,%lf",&ta,&ao,&ah,&al,&ac)!=5) continue;
        if(tb!=ta || bc<=0 || ac<=0) continue;
        v.push_back({(int64_t)tb,bc,ac});
    }
    fclose(fb); fclose(fa); return v;
}

static std::vector<Bar> agg(const std::vector<M1>& m, int tf_min){
    std::vector<Bar> out; const int64_t W=(int64_t)tf_min*60000LL;
    int64_t cur=-1; Bar b{};
    for(const auto& r: m){
        double mid=(r.bc+r.ac)*0.5;
        int64_t bk=(r.ts/W)*W;
        if(bk!=cur){ if(cur>=0) out.push_back(b); cur=bk; b={bk,mid,mid,mid,r.bc,r.ac}; }
        else { if(mid>b.h)b.h=mid; if(mid<b.l)b.l=mid; b.c=mid; b.bid=r.bc; b.ask=r.ac; }
    }
    if(cur>=0) out.push_back(b);
    return out;
}

struct Res { int n,win; double gw,gl,net,spread_bps,avg_bars; };
static Res run(const std::vector<Bar>& bars,int w,double zin,double zout,int hold,bool hook){
    FxCrossRevEngine eng("EURGBP");
    eng.enabled=true; eng.shadow_mode=true; eng.bypass_cost_gate=true; eng.lot=0.01;
    eng.p.z_window=w; eng.p.z_in=zin; eng.p.z_out=zout; eng.p.z_stop=3.5;
    eng.p.hold_timeout=hold; eng.p.require_hook=hook;
    Res R{0,0,0,0,0,0,0}; double spr=0;
    auto cb=[&](const omega::TradeRecord& t){
        R.n++; if(t.net_pnl>0){R.win++;R.gw+=t.net_pnl;} else R.gl+=-t.net_pnl;
        R.net+=t.net_pnl; spr+=t.spreadAtEntry/t.entryPrice*10000.0;
    };
    for(const auto& b: bars) eng.on_d1_bar(b.h,b.l,b.c,b.bid,b.ask,b.ts,cb);
    eng.force_close(bars.empty()?0:bars.back().ts,cb);
    if(R.n) R.spread_bps=spr/R.n;
    return R;
}

int main(){
    const char* B="/Users/jo/Omega/download/eurgbp-m1-bid-2019-01-01-2026-05-31.csv";
    const char* A="/Users/jo/Omega/download/eurgbp-m1-ask-2019-01-01-2026-05-31.csv";
    fprintf(stderr,"loading M1...\n");
    auto m1=load_m1(B,A);
    fprintf(stderr,"loaded %zu M1 rows\n",m1.size());
    // silence engine printf chatter
    if(!freopen("/dev/null","w",stdout)) { /* best effort */ }

    struct TF{const char*name;int min;} tfs[]={
        {"M1",1},{"M5",5},{"M10",10},{"M15",15},{"M30",30},{"H1",60},{"H4",240},{"D1",1440}};
    int wins[]={20,40,60,120};
    double zins[]={1.5,2.0,2.5,3.0};
    double zouts[]={0.3,0.5};
    int holds[]={10,20,40};
    bool hooks[]={false,true};

    fprintf(stderr,"ROW,tf,tf_min,window,zin,zout,hold,hook,n,wr,pf,net,avg_net,med_spread_bps\n");
    for(auto& tf: tfs){
        auto bars=agg(m1,tf.min);
        fprintf(stderr,"# %s: %zu bars\n",tf.name,bars.size());
        for(int w:wins)for(double zi:zins)for(double zo:zouts)for(int h:holds)for(bool hk:hooks){
            Res R=run(bars,w,zi,zo,h,hk);
            double pf = R.gl>0? R.gw/R.gl : (R.gw>0?99:0);
            double wr = R.n? 100.0*R.win/R.n : 0;
            double an = R.n? R.net/R.n : 0;
            fprintf(stderr,"ROW,%s,%d,%d,%.1f,%.1f,%d,%d,%d,%.1f,%.2f,%.2f,%.4f,%.2f\n",
                tf.name,tf.min,w,zi,zo,h,hk?1:0,R.n,wr,pf,R.net,an,R.spread_bps);
        }
    }
    return 0;
}
