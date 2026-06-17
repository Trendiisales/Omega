// =============================================================================
// index_seasonal_revalidate.cpp -- faithful real-class BT for IndexSeasonalEngine
// (S44 day-of-week Tue+Fri long). Drives the REAL omega::IndexSeasonalEngine via
// its own on_d1_bar() path on Dukascopy D1 OHLC files (download/*idx*-d1).
//
// Production config (engine_init.hpp idx_seas_boot):
//   shadow_mode=true, enabled=true, lot=0.01, target_vol_bps=60, usd_per_pt=<sym>,
//   gate_by_vix=true, vix_gate_ratio=1.05, vix_ratio_path="data/vix_term_ratio.txt"
//     -> file ABSENT locally (not committed) => engine degrades to UNGATED, which
//        engine_init explicitly calls "the proven 0.69 edge". Faithful = ungated.
//   index_risk_off(): no macro feed => g_index_regime_valid=false => price-bear
//        fallback index_market_regime().is_bear(). We warm that proxy from the
//        USTEC D1 close series (NAS bellwether) so the bear-suppression is honored.
//
// Data: D1 OHLC, cols ts,o,h,l,c ; ts in ms ; prices NOT x1000 (verified medians).
// Cost: synthetic bid/ask from a per-index CFD spread (pts) so the engine's own
//   exit-at-bid + spread-cost path runs; plus a commission/slip stress in pts.
//
// Build: clang++ -O3 -std=c++17 -I include backtest/index_seasonal_revalidate.cpp \
//        -o backtest/index_seasonal_revalidate
// Run:   ./backtest/index_seasonal_revalidate
// =============================================================================
#include "IndexSeasonalEngine.hpp"
#include "RegimeState.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

struct Bar { int64_t ts_ms; double o,h,l,c; };

static std::vector<Bar> load(const std::string& path){
    std::vector<Bar> v; std::ifstream f(path);
    if(!f){ std::fprintf(stderr,"NO FILE %s\n",path.c_str()); return v; }
    std::string ln; std::getline(f,ln); // header
    while(std::getline(f,ln)){
        Bar b{}; double ts;
        if(std::sscanf(ln.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        if(b.c<=0) continue;
        b.ts_ms = (ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
        b.ts_ms = (b.ts_ms/86400000LL)*86400000LL;
        int wd=(int)((((b.ts_ms/86400000LL)%7)+4+7)%7);
        if(wd==6||wd==0) continue;          // drop weekend stubs (engine does too)
        v.push_back(b);
    }
    return v;
}

struct Trade { double price_bp; int64_t exit_ts; };

// Run one index through the REAL engine. spread_pts = synthetic CFD spread.
static std::vector<Trade> run_index(const char* sym, const std::string& path,
                                    double usd_per_pt, double spread_pts){
    auto bars = load(path);
    if(bars.empty()) return {};
    omega::IndexSeasonalEngine e(sym);
    e.shadow_mode=true; e.enabled=true; e.lot=0.01;
    e.p.target_vol_bps=60.0; e.p.usd_per_pt=usd_per_pt;
    e.gate_by_vix=true; e.vix_gate_ratio=1.05;
    e.vix_ratio_path="data/vix_term_ratio.txt";   // absent -> ungated (graceful)

    std::vector<Trade> trades;
    // engine pnl is computed in price_bp internally; we recover price_bp from the
    // TradeRecord (entry/exit) so cost modeling is independent of usd_per_pt.
    auto cb=[&](const omega::TradeRecord& tr){
        double bp=(tr.exitPrice-tr.entryPrice)/tr.entryPrice*10000.0;
        trades.push_back({bp, tr.exitTs});
    };
    for(const auto& b: bars){
        double bid = b.c - spread_pts*0.5;
        double ask = b.c + spread_pts*0.5;
        e.on_d1_bar(b.h,b.l,b.c,bid,ask,b.ts_ms,cb);
    }
    return trades;
}

int main(){
    // Warm the NAS bear-proxy from USTEC D1 closes (each D1 close as a regime bar).
    // This honors index_risk_off()'s price-bear fallback (the production gate when
    // the macro feed is absent). RegimeState is H1-tuned; feeding D1 closes makes it
    // a ~200-trading-day EMA spine, a reasonable D1 bear proxy.
    {
        auto nas = load("download/usatechidxusd-d1-bid-2019-01-01-2026-05-31.csv");
        for(const auto& b: nas) omega::index_market_regime().on_h1_bar(b.o,b.h,b.l,b.c);
        std::printf("[regime-proxy] warmed from USTEC D1: regime=%s warm=%d\n",
            omega::index_market_regime().regime_name(),(int)omega::index_market_regime().warm());
    }

    struct Idx { const char* sym; const char* file; double upp; double spread; };
    // usd_per_pt from engine_init; spread_pts: typical index-CFD spreads (pts).
    std::vector<Idx> idx = {
        {"US500",  "download/usa500idxusd-d1-bid-2019-01-01-2026-05-31.csv", 50.0, 0.5},
        {"USTEC",  "download/usatechidxusd-d1-bid-2019-01-01-2026-05-31.csv",20.0, 2.0},
        {"GER40",  "download/deuidxeur-d1-bid-2019-01-01-2026-05-31.csv",    25.0, 2.0},
        {"DJ30",   "download/usa30idxusd-d1-bid-2019-01-01-2026-05-31.csv",   5.0, 3.0},
        {"UK100",  "download/gbridxgbp-d1-bid-2019-01-01-2026-05-31.csv",    10.0, 1.5},
        {"ESTX50", "download/eusidxeur-d1-bid-2019-01-01-2026-05-31.csv",    10.0, 1.0},
    };

    std::vector<Trade> pooled;
    int64_t gmin=0,gmax=0;

    auto report=[&](const char* nm, std::vector<double>& bp, double cost_bp){
        long n=0,w=0; double g=0,ws=0,ls=0,eq=0,pk=0,mdd=0;
        for(double raw:bp){ double x=raw-cost_bp; ++n; g+=x; if(x>0){++w;ws+=x;}else ls+=-x;
            eq+=x; if(eq>pk)pk=eq; if(pk-eq>mdd)mdd=pk-eq; }
        double pf=ls>0?ws/ls:(ws>0?99:0);
        std::printf("  %-14s cost=%4.1fbp n=%-4ld WR=%5.1f%% net=%+9.1fbp PF=%5.2f mdd=%7.1fbp\n",
            nm,cost_bp,n,n?100.0*w/n:0,g,pf,mdd);
    };

    for(auto& ix: idx){
        auto tr = run_index(ix.sym, ix.file, ix.upp, ix.spread);
        if(tr.empty()){ std::printf("[%s] no trades\n",ix.sym); continue; }
        std::vector<double> bp; std::vector<int64_t> ts;
        for(auto&t:tr){ bp.push_back(t.price_bp); ts.push_back(t.exit_ts); pooled.push_back(t);
            if(!gmin||t.exit_ts<gmin)gmin=t.exit_ts; if(t.exit_ts>gmax)gmax=t.exit_ts; }
        // per-index WF halves split by trade index midpoint of time
        int64_t mid=(ts.front()+ts.back())/2;
        std::vector<double> h1,h2; for(size_t k=0;k<bp.size();++k)(ts[k]<mid?h1:h2).push_back(bp[k]);
        std::printf("[%s] usd_per_pt=%.0f spread=%.1fpt\n",ix.sym,ix.upp,ix.spread);
        // index seasonal trades are net of the engine's own spread cost already in
        // price terms (entry@ask exit@bid). cost_bp below = ADDITIONAL commission/slip.
        report("FULL@0",  bp, 0.0);
        report("FULL@2bp",bp, 2.0);
        report("FULL@4bp",bp, 4.0);
        report("H1@2bp",  h1, 2.0);
        report("H2@2bp",  h2, 2.0);
    }

    // pooled
    std::printf("\n==== POOLED (6 indices, real engine) range %lld..%lld ====\n",(long long)gmin,(long long)gmax);
    std::vector<double> pb; std::vector<int64_t> pts;
    for(auto&t:pooled){ pb.push_back(t.price_bp); pts.push_back(t.exit_ts); }
    // sort by time for halves
    std::vector<size_t> ord(pb.size()); for(size_t i=0;i<ord.size();++i)ord[i]=i;
    std::sort(ord.begin(),ord.end(),[&](size_t a,size_t b){return pts[a]<pts[b];});
    std::vector<double> sb; std::vector<int64_t> sts; for(size_t i:ord){sb.push_back(pb[i]);sts.push_back(pts[i]);}
    int64_t pmid=(sts.front()+sts.back())/2;
    std::vector<double> H1,H2; for(size_t k=0;k<sb.size();++k)(sts[k]<pmid?H1:H2).push_back(sb[k]);
    report("FULL@0",  sb, 0.0);
    report("FULL@2bp",sb, 2.0);
    report("FULL@4bp",sb, 4.0);
    report("H1@2bp",  H1, 2.0);
    report("H2@2bp",  H2, 2.0);
    // fat-tail top3 share (on @2bp net)
    std::vector<double> net; for(double x:sb)net.push_back(x-2.0);
    std::vector<double> s=net; std::sort(s.rbegin(),s.rend());
    double tot=0; for(double x:net)tot+=x; double ex3=0; for(size_t i=3;i<s.size();++i)ex3+=s[i];
    std::printf("  net_ex_top3=%+.1fbp (of %+.1fbp)  top3_share=%.0f%%\n",
        ex3,tot, tot!=0?100.0*(tot-ex3)/tot:0.0);
    return 0;
}
