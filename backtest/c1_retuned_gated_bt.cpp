// =============================================================================
// c1_retuned_gated_bt.cpp -- C1RetunedPortfolio bear-tape + trend-gate BT
// (S-2026-07-08c tombstone valid-use follow-up).
//
// Tombstone (engine_init.hpp ~L1074, S-2026-06-11): corpus was 2024-03..2026-04
// gold BULL only; long-only BBand dip-buyer with no trend/regime gate catches
// falling knives in a downtrend. "Do NOT re-enable without a bear-tape backtest
// + trend gate." This IS that backtest: drives the REAL C1RetunedPortfolio over
// 2022-2026 XAU H1 (certified, incl the 2022 bear), gate = the live
// gold_regime() brain (price core + optional macro-hostile overlay) blocking
// NEW entries via max_concurrent=0 while blocked (management unaffected).
//
// Cost: IBKR spot-gold reality (2*0.00015*price + 0.30 spread) * lot * 100oz.
// Portfolio halt logic (cluster-day / -7.5% DD) left ACTIVE = faithful.
//
// build: clang++ -O3 -std=c++17 -Iinclude backtest/c1_retuned_gated_bt.cpp -o /tmp/c1_gated
// run:   /tmp/c1_gated /Users/jo/Tick/XAUUSD_2022_2026.h1.csv
// =============================================================================
#include "C1RetunedPortfolio.hpp"
#include "RegimeState.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
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

static std::map<int64_t,int> g_hostile;
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

struct Tr { int64_t entry_ts; double usd_net; std::string cell; };

// Wilder ATR14
struct Atr { double atr=0; double pc=0; int n=0;
    void on(double h,double l,double c){
        double tr = n==0 ? (h-l) : std::max({h-l, std::fabs(h-pc), std::fabs(l-pc)});
        if(n<14){ atr=(atr*n+tr)/(n+1); } else atr=(atr*13+tr)/14.0;
        pc=c; ++n; }
    bool ready() const { return n>=15; } };

// BB20(2) + ATR14 over H4
struct H4Ind { std::deque<double> cl; Atr atr;
    double up=0, mid=0, lo=0;
    void on(double h,double l,double c){
        atr.on(h,l,c);
        cl.push_back(c); if(cl.size()>20) cl.pop_front();
        if(cl.size()==20){
            double m=0; for(double x:cl)m+=x; m/=20;
            double v=0; for(double x:cl)v+=(x-m)*(x-m); v=std::sqrt(v/20);
            mid=m; up=m+2*v; lo=m-2*v;
        } }
    bool ready() const { return cl.size()==20 && atr.ready(); } };

static std::vector<Tr> run(const std::vector<B>& bars, int gate_mode){
    // gate_mode: 0=off (as-tombstoned config), 1=price core, 2=price+macro overlay
    omega::gold_regime().reset();
    omega::gold_regime().set_macro_hostile(false);
    omega::C1RetunedPortfolio p;
    p.shadow_mode=true; p.enabled=true;
    p.init();
    const int MAXC = p.max_concurrent;

    std::vector<Tr> out;
    auto cb=[&](const omega::TradeRecord& tr){
        double cost=(2.0*0.00015*tr.entryPrice + 0.30) * tr.size * 100.0;
        out.push_back({tr.entryTs, tr.pnl*100.0 - cost, tr.engine});
    };

    Atr h1atr; H4Ind h4;
    // H4 aggregation from H1
    int64_t h4b=-1; double o4=0,h4h=0,l4=0,c4=0;
    const size_t WARM=250;
    for(size_t i=0;i<bars.size();++i){
        const auto& b=bars[i];
        omega::gold_regime().on_h1_bar(b.o,b.h,b.l,b.c);
        if(gate_mode==2){
            auto it=g_hostile.find(b.ts/86400 - 1);
            omega::gold_regime().set_macro_hostile(it!=g_hostile.end() && it->second==1);
        }
        // gate: block NEW entries while long_blocked (management continues)
        p.max_concurrent = (gate_mode>0 && omega::gold_regime().long_blocked()) ? 0 : MAXC;

        int64_t ms=b.ts*1000;
        auto rcb = i<WARM ? omega::C1RetunedPortfolio::CloseCallback{} : omega::C1RetunedPortfolio::CloseCallback(cb);
        // intrabar ticks first (manage SL/TP on open positions): o,h,l,c
        p.on_tick(b.o,b.o,ms+0,rcb);
        p.on_tick(b.h,b.h,ms+250,rcb);
        p.on_tick(b.l,b.l,ms+500,rcb);
        p.on_tick(b.c,b.c,ms+750,rcb);

        h1atr.on(b.h,b.l,b.c);
        omega::C1Bar cb1; cb1.bar_start_ms=ms; cb1.open=b.o; cb1.high=b.h; cb1.low=b.l; cb1.close=b.c;
        int64_t now=ms+3600LL*1000;
        if(h1atr.ready())
            p.on_h1_bar(cb1, b.c, b.c, h1atr.atr, now, rcb);

        // H4 aggregate + dispatch on completed H4
        int64_t hb = b.ts - (b.ts % 14400);
        if(h4b<0){ h4b=hb; o4=b.o; h4h=b.h; l4=b.l; c4=b.c; }
        else if(hb!=h4b){
            h4.on(h4h,l4,c4);
            if(h4.ready()){
                omega::C1Bar b4; b4.bar_start_ms=h4b*1000; b4.open=o4; b4.high=h4h; b4.low=l4; b4.close=c4;
                p.on_h4_bar(b4, b.c, b.c, h4.up, h4.mid, h4.lo, h4.atr.atr, now, rcb);
            }
            h4b=hb; o4=b.o; h4h=b.h; l4=b.l; c4=b.c;
        } else { h4h=std::max(h4h,b.h); l4=std::min(l4,b.l); c4=b.c; }
    }
    std::printf("[run gate=%d] halt=%s (%s) equity=%.0f maxDD=%.1f%%\n",
        gate_mode, p.halt_tripped_?"YES":"no", p.halt_reason_.c_str(),
        p.equity_, p.max_dd_pct_*100.0);
    return out;
}

struct Agg { int n=0,w=0; double net=0,gw=0,gl=0; std::vector<double> v; };
static void add(Agg&a,double p){ a.n++; a.net+=p; a.v.push_back(p); if(p>=0){a.w++;a.gw+=p;} else a.gl+=-p; }
static double pf(const Agg&a){ return a.gl>0? a.gw/a.gl : (a.gw>0?99.0:0.0); }
static double top3(Agg a){ if(a.v.empty()||a.net<=0) return 0; std::sort(a.v.begin(),a.v.end(),std::greater<double>()); double s=0; for(int i=0;i<3&&i<(int)a.v.size();++i)s+=a.v[i]; return 100.0*s/a.net; }

static void report(const char* tag, const std::vector<Tr>& tr, int64_t mid_s){
    Agg all,bull,bear,h1,h2; std::map<std::string,Agg> cells;
    for(const auto& t: tr){
        add(all,t.usd_net);
        int y=year_of(t.entry_ts);
        if(y==2022) add(bear,t.usd_net);
        if(y>=2024) add(bull,t.usd_net);
        if(t.entry_ts<mid_s) add(h1,t.usd_net); else add(h2,t.usd_net);
        add(cells[t.cell],t.usd_net);
    }
    std::printf("\n===== %s =====\n", tag);
    std::printf("ALL n=%d WR=%.1f%% PF=%.2f net=$%+.0f | BULL n=%d PF=%.2f %+.0f | BEAR22 n=%d PF=%.2f %+.0f | H1 %+.0f / H2 %+.0f both+=%s | top3=%.0f%%\n",
        all.n, all.n?100.0*all.w/all.n:0, pf(all), all.net,
        bull.n, pf(bull), bull.net, bear.n, pf(bear), bear.net,
        h1.net, h2.net, (h1.net>0&&h2.net>0)?"YES":"NO", top3(all));
    for(auto& kv: cells)
        std::printf("    %-32s n=%3d WR=%5.1f%% PF=%5.2f net=$%+8.0f\n",
            kv.first.c_str(), kv.second.n, kv.second.n?100.0*kv.second.w/kv.second.n:0,
            pf(kv.second), kv.second.net);
}

int main(int argc,char**argv){
    if(argc<2){ std::fprintf(stderr,"usage: %s h1.csv\n",argv[0]); return 1; }
    auto bars=load(argv[1]);
    if(bars.empty()){ std::fprintf(stderr,"no bars\n"); return 1; }
    load_hostile("/tmp/macro_gold_hostile_daily.csv");
    std::printf("bars=%zu hostile_days=%zu\n",bars.size(),g_hostile.size());
    int64_t mid=(bars.front().ts+bars.back().ts)/2;
    auto g0=run(bars,0);
    auto g1=run(bars,1);
    auto g2=run(bars,2);
    report("GATE=OFF (as-tombstoned: naked long-only dip-buyer)", g0, mid);
    report("GATE=PRICE (gold_regime price core blocks new entries)", g1, mid);
    report("GATE=FULL (price core + macro-hostile overlay)", g2, mid);
    return 0;
}
