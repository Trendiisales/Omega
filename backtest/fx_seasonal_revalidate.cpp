// =============================================================================
// fx_seasonal_revalidate.cpp -- faithful real-class BT for g_fx_seas_* engines
// (FxSeasonalEngine, Friday-long FX). Drives the REAL engine class via its real
// signal path on_d1_bar() (the same path live tick aggregation lands in, and the
// same path seed_from_d1_csv replays). PROD config from engine_init.hpp seas_boot:
//   shadow_mode=true; enabled=true; lot=0.01; p.target_vol_bps=50.0
//   (defaults: hold_bars=1, atr_period=14, max_lot=0.10, usd_per_pt=100000)
//
// D1 data: download/<pair>-d1-bid-2019-01-01-2026-05-31.csv  (timestamp_ms,o,h,l,c)
// BID series. We synthesize bid/ask around each close using a per-pair realistic
// half-spread (IBKR-ish majors), so the engine's own cost accounting (entry@ask,
// exit@bid, cost=spread/entry*notional) + its cost gate run faithfully.
//
// The engine's PnL is reported via net_pnl (its spread cost already deducted).
// We additionally apply a commission stress (per round-trip, in USD) on top, and
// a wider-spread stress, to test cost-robustness per BACKTEST_TRUTH.
//
// Build: clang++ -O3 -std=c++17 -I include backtest/fx_seasonal_revalidate.cpp \
//        -o backtest/fx_seasonal_revalidate
// =============================================================================
#include "FxSeasonalEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

struct DBar { int64_t day_ms; double o,h,l,c; };

static std::vector<DBar> load_d1(const char* p){
    std::vector<DBar> v; std::ifstream f(p); if(!f.is_open()) return v;
    std::string ln; std::getline(f,ln); // header
    while(std::getline(f,ln)){
        double ts,o,h,l,c;
        if(std::sscanf(ln.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue;
        int64_t ms = (ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
        v.push_back({ms,o,h,l,c});
    }
    return v;
}

struct Pair { const char* sym; const char* file; double half_spread_pts; };
// Realistic IBKR majors half-spread (price points). EURUSD ~0.1 pip => 0.00001.
// These are tight (favourable) majors spreads; engine cost gate uses ask-bid.
static std::vector<Pair> PAIRS = {
 {"EURUSD","/Users/jo/Omega/download/eurusd-d1-bid-2019-01-01-2026-05-31.csv", 0.000010},
 {"GBPUSD","/Users/jo/Omega/download/gbpusd-d1-bid-2019-01-01-2026-05-31.csv", 0.000015},
 {"USDJPY","/Users/jo/Omega/download/usdjpy-d1-bid-2019-01-01-2026-05-31.csv", 0.0015  },
 {"AUDUSD","/Users/jo/Omega/download/audusd-d1-bid-2019-01-01-2026-05-31.csv", 0.000015},
 {"NZDUSD","/Users/jo/Omega/download/nzdusd-d1-bid-2019-01-01-2026-05-31.csv", 0.000025},
 {"USDCAD","/Users/jo/Omega/download/usdcad-d1-bid-2019-01-01-2026-05-31.csv", 0.000020},
 {"USDCHF","/Users/jo/Omega/download/usdchf-d1-bid-2019-01-01-2026-05-31.csv", 0.000020},
 {"EURGBP","/Users/jo/Omega/download/eurgbp-d1-bid-2019-01-01-2026-05-31.csv", 0.000020},
 {"EURJPY","/Users/jo/Omega/download/eurjpy-d1-bid-2019-01-01-2026-05-31.csv", 0.0020  },
};

struct Tr { double net_pnl; double gross_pnl; double lot; int64_t exitTs; double entry; };

int main(int argc,char**argv){
    double spread_mult = (argc>1)?atof(argv[1]):1.0;     // spread stress multiplier
    double comm_usd_rt = (argc>2)?atof(argv[2]):0.0;     // extra commission per RT (USD, total)

    std::vector<Tr> all;
    std::vector<std::pair<std::string,std::vector<Tr>>> per_pair;

    for(auto& pr : PAIRS){
        auto bars = load_d1(pr.file);
        if(bars.empty()){ std::fprintf(stderr,"NO DATA %s\n",pr.file); continue; }
        // median price sanity (x1000 trap)
        std::vector<double> px; for(auto&b:bars) px.push_back(b.c);
        std::nth_element(px.begin(),px.begin()+px.size()/2,px.end());
        double med=px[px.size()/2];

        omega::FxSeasonalEngine e(pr.sym);
        e.shadow_mode=true; e.enabled=true; e.lot=0.01; e.p.target_vol_bps=50.0; // PROD

        std::vector<Tr> trs;
        double hs = pr.half_spread_pts * spread_mult;
        auto cb=[&](const omega::TradeRecord& tr){
            trs.push_back({tr.net_pnl, tr.pnl, tr.size, tr.exitTs, tr.entryPrice});
        };
        // drive the REAL signal path: on_d1_bar per UTC-day bar, Sat dropped (engine
        // entry only fires Fri; on_d1_bar handles weekday internally).
        for(auto& b : bars){
            int wd=(int)((((b.day_ms/86400000LL)%7)+4+7)%7);
            if(wd==6) continue; // drop Sat artifact (matches engine seed + live)
            double bid=b.c-hs, ask=b.c+hs;
            e.on_d1_bar(b.h,b.l,b.c,bid,ask,b.day_ms,cb);
        }
        // apply extra commission stress on top of engine's own spread cost
        for(auto& t : trs){ t.net_pnl -= comm_usd_rt; }
        std::printf("  %-7s med=%.4f spread=%.6fx n=%zu\n", pr.sym, med, hs*2, trs.size());
        per_pair.push_back({pr.sym,trs});
        for(auto&t:trs) all.push_back(t);
    }

    auto report=[&](const char* nm, std::vector<Tr>& v){
        if(v.empty()){ std::printf("%-22s (no trades)\n",nm); return; }
        std::sort(v.begin(),v.end(),[](const Tr&a,const Tr&b){return a.exitTs<b.exitTs;});
        int64_t n=0,w=0; double g=0,ws=0,ls=0,eq=0,pk=0,mdd=0;
        std::vector<double> wins;
        for(auto& t:v){ double x=t.net_pnl; ++n; g+=x; if(x>0){++w;ws+=x;wins.push_back(x);} else ls+=-x;
            eq+=x; if(eq>pk)pk=eq; if(pk-eq>mdd)mdd=pk-eq; }
        double pf=ls>0?ws/ls:(ws>0?99:0);
        // both-halves (by trade count, time-ordered)
        double h1=0,h2=0; size_t mid=v.size()/2;
        for(size_t k=0;k<v.size();++k)(k<mid?h1:h2)+= v[k].net_pnl;
        // fat-tail: top-3 winners share of gross positive
        std::sort(wins.begin(),wins.end(),std::greater<double>());
        double top3=0; for(size_t k=0;k<wins.size()&&k<3;++k) top3+=wins[k];
        double top3share = ws>0? top3/ws*100 : 0;
        std::printf("%-22s n=%-4lld WR=%5.1f%% net=$%+9.2f PF=%5.2f mdd=$%7.2f  H1=$%+8.2f H2=$%+8.2f  top3=%.0f%%\n",
            nm,(long long)n,n?100.0*w/n:0,g,pf,mdd,h1,h2,top3share);
    };

    std::printf("\n=== FxSeasonal faithful (real class, on_d1_bar) spread_mult=%.1f comm_rt=$%.2f ===\n",
                spread_mult, comm_usd_rt);
    std::printf("--- per-pair ---\n");
    for(auto& pp : per_pair) report(pp.first.c_str(), pp.second);
    std::printf("--- portfolio ---\n");
    report("ALL_9_PAIRS", all);
    return 0;
}
