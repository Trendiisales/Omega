// =============================================================================
// xau_sess_nypm_gated_bt.cpp -- g_xau_sess_nypm gated re-validate (S-2026-07-08c)
//
// Tombstone valid-use follow-up. TOMBSTONES.tsv 2026-06-26: bull PF1.86 / bear
// PF0.85, both 2022 WF halves NEG -> bull-beta. The engine ALREADY carries the
// gold_regime().long_blocked() bear gate in its entry path
// (SessionMomentumEngine.hpp ~L298) -- added post-cull, never backtested.
// This drives the REAL engine class at prod config over 2022-2026 XAU H1 with
// the REAL omega::gold_regime() brain fed the same H1 bars (its exact live
// mechanism), vs the brain left cold (gate never arms = as-culled behavior).
//
// Cost: IBKR spot-gold reality per memory project-ibkr-cost-basis --
// round-trip = 2*0.00015*price + spread(0.30pt), price-proportional per trade.
// (The old flat 0.37pt figure is ~4x too low.)
//
// build: clang++ -O3 -std=c++17 -Iinclude backtest/xau_sess_nypm_gated_bt.cpp -o /tmp/nypm_gated
// run:   /tmp/nypm_gated /Users/jo/Tick/XAUUSD_2022_2026.h1.csv
// =============================================================================
#include "SessionMomentumEngine.hpp"
#include "RegimeState.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

struct B { int64_t ts; double o,h,l,c; };

static std::vector<B> load(const std::string& p){
    std::vector<B> v; std::ifstream f(p); if(!f) return v;
    std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){
        B b; const char* s=ln.c_str(); char* e;
        b.ts=std::strtoll(s,&e,10); if(*e!=',')continue; s=e+1;
        b.o=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.h=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.l=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.c=std::strtod(s,&e);
        v.push_back(b);
    }
    return v;
}

static int year_of(int64_t sec){ std::time_t t=(std::time_t)sec; std::tm g{}; gmtime_r(&t,&g); return g.tm_year+1900; }

struct Tr { int64_t entry_ts; double pts; double entry_px; };

#include <map>
// daily macro-hostile flags (date -> 0/1), applied with a +1-day lag (producer
// runs 23:00 UTC; the flag protects the NEXT trading day -- no lookahead).
static std::map<int64_t,int> g_hostile;   // key = UTC day number
static void load_hostile(const char* p){
    std::ifstream f(p); std::string ln;
    while(std::getline(f,ln)){
        int y,mo,d,h;
        if(std::sscanf(ln.c_str(),"%d-%d-%d,%d",&y,&mo,&d,&h)==4){
            std::tm t{}; t.tm_year=y-1900; t.tm_mon=mo-1; t.tm_mday=d;
            g_hostile[timegm(&t)/86400]=h;
        }
    }
}

static std::vector<Tr> run(const std::vector<B>& bars, bool feed_regime, bool feed_macro=false){
    omega::gold_regime().reset();
    omega::SessionMomentumEngine e;
    e.symbol="XAUUSD"; e.entry_hour=16; e.hold_hours=4;
    e.use_trend_filter=true; e.ema_period=200; e.sl_atr=0.0;
    e.skip_dow_mask=(1<<5); e.shadow_mode=true; e.enabled=true;
    e.lot=0.01; e.max_spread=2.0; e.init();

    std::vector<Tr> out; double half=0.15; double last_entry_px=0;
    auto cb=[&](const omega::TradeRecord& tr){
        out.push_back({tr.entryTs, tr.pnl/e.lot, last_entry_px});
    };
    const int WARM=250;
    for(size_t i=0;i<bars.size();++i){
        const auto&b=bars[i];
        if(feed_regime) omega::gold_regime().on_h1_bar(b.o,b.h,b.l,b.c);
        if(feed_macro){
            auto it=g_hostile.find(b.ts/86400 - 1);          // yesterday's flag
            omega::gold_regime().set_macro_hostile(it!=g_hostile.end() && it->second==1);
        }
        omega::SessBar sb; sb.bar_start_ms=b.ts*1000; sb.open=b.o; sb.high=b.h; sb.low=b.l; sb.close=b.c;
        int64_t now_ms=b.ts*1000 + 3600LL*1000;
        last_entry_px=b.c;
        e.on_h1_bar(sb, b.c-half, b.c+half, 0.0, now_ms,
                    (int)i<WARM ? omega::SessionMomentumEngine::OnCloseFn{} : cb);
    }
    return out;
}

struct Agg { int n=0,w=0; double net=0,gw=0,gl=0; std::vector<double> v; };
static void add(Agg&a,double p){ a.n++; a.net+=p; a.v.push_back(p); if(p>=0){a.w++;a.gw+=p;} else a.gl+=-p; }
static double pf(const Agg&a){ return a.gl>0? a.gw/a.gl : (a.gw>0?99.0:0.0); }
static double top3(Agg a){ if(a.v.empty()||a.net<=0) return 0; std::sort(a.v.begin(),a.v.end(),std::greater<double>()); double s=0; for(int i=0;i<3&&i<(int)a.v.size();++i)s+=a.v[i]; return 100.0*s/a.net; }

static void report(const char* tag, const std::vector<Tr>& tr, int64_t mid_s){
    std::printf("\n===== %s =====\n", tag);
    for(double m : {1.0, 2.0}){
        Agg all,bull,bear,h1,h2;
        for(const auto& t: tr){
            double cost=(2.0*0.00015*t.entry_px + 0.30)*m;   // IBKR rt, price-proportional
            double x=t.pts-cost;
            add(all,x);
            int y=year_of(t.entry_ts);
            if(y==2022) add(bear,x);
            if(y>=2024) add(bull,x);
            if(t.entry_ts<mid_s) add(h1,x); else add(h2,x);
        }
        std::printf("cost x%.0f: ALL n=%d WR=%.1f%% PF=%.2f net=%+.1fpt | BULL n=%d PF=%.2f %+.1f | BEAR22 n=%d PF=%.2f %+.1f | H1 %+.1f / H2 %+.1f both+=%s | top3=%.0f%%\n",
            m, all.n, all.n?100.0*all.w/all.n:0, pf(all), all.net,
            bull.n, pf(bull), bull.net, bear.n, pf(bear), bear.net,
            h1.net, h2.net, (h1.net>0&&h2.net>0)?"YES":"NO", top3(all));
    }
}

int main(int argc,char**argv){
    if(argc<2){ std::fprintf(stderr,"usage: %s h1.csv\n",argv[0]); return 1; }
    auto bars=load(argv[1]);
    if(bars.empty()){ std::fprintf(stderr,"no bars\n"); return 1; }
    std::printf("bars=%zu range %lld..%lld\n",bars.size(),
        (long long)bars.front().ts,(long long)bars.back().ts);
    int64_t mid=(bars.front().ts+bars.back().ts)/2;
    load_hostile("/tmp/macro_gold_hostile_daily.csv");
    std::printf("hostile days loaded: %zu\n", g_hostile.size());
    auto cold = run(bars,false);
    auto hot  = run(bars,true);
    auto full = run(bars,true,true);
    report("GATE COLD (gold_regime never fed -> long_blocked always false, as-culled)", cold, mid);
    report("GATE HOT  (real gold_regime brain fed same H1 tape, live mechanism)",       hot,  mid);
    report("GATE FULL (price core + macro-hostile overlay, the complete live gate)",    full, mid);
    return 0;
}
