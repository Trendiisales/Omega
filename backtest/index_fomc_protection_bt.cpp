// =============================================================================
// index_fomc_protection_bt.cpp -- IndexFomc adverse-protection verdict BT
// (S-2026-07-08c; owed since backfill S-2026-06-24n: "verdict owed").
//
// Drives the REAL IndexFomcEngine (built-in FOMC table 2019-2026) per symbol on
// certified daily tapes, then grids a cold loss-cut applied ADVERSE-FIRST
// (pessimistic for the engine, generous to the stop): if the FOMC-day low
// breaches entry - X*ATR14(entry), the trade books exactly -X*ATR -- even when
// the close recovered. If the stop still loses to the naked time-exit under
// this stop-favoring fill, the time-stop-only design is validated.
//
// Cost: 3bp round-trip base + 2x stress, applied in-harness (bid=ask=close fed
// to the engine so its internal spread-cost is 0 and the ExecutionCostGuard
// entry gate passes on every scheduled day -- we want ALL FOMC events sampled).
//
// build: clang++ -O3 -std=c++17 -Iinclude backtest/index_fomc_protection_bt.cpp -o /tmp/fomc_prot
// run:   /tmp/fomc_prot
// =============================================================================
#include "IndexFomcEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

struct B { int64_t ts; double o,h,l,c; };

static std::vector<B> load(const std::string& p){
    std::vector<B> v; std::ifstream f(p); std::string ln;
    while(std::getline(f,ln)){
        if(ln.empty()||(ln[0]<'0'||ln[0]>'9')) continue;
        B b; const char* s=ln.c_str(); char* e;
        b.ts=std::strtoll(s,&e,10); if(*e!=',')continue; s=e+1;
        b.o=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.h=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.l=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        b.c=std::strtod(s,&e); v.push_back(b);
    }
    return v;
}
static int year_of(int64_t sec){ std::time_t t=(std::time_t)sec; std::tm g{}; gmtime_r(&t,&g); return g.tm_year+1900; }

struct Tr { int64_t entry_ts; double entry, exitpx, mae_px, atr_entry; };

struct Agg { int n=0,w=0; double net=0,gw=0,gl=0,worst=0; };
static void add(Agg&a,double p){ a.n++; a.net+=p; if(p>=0){a.w++;a.gw+=p;} else {a.gl+=-p; if(p<a.worst)a.worst=p;} }
static double pf(const Agg&a){ return a.gl>0? a.gw/a.gl : (a.gw>0?99.0:0.0); }

int main(){
    struct Sym { const char* name; const char* path; };
    const Sym syms[] = {
        {"US500.F","/Users/jo/Tick/SPX_daily_2016_2026.csv"},
        {"USTEC.F","/Users/jo/Tick/NDX_daily_2016_2026.csv"},
        {"DJ30.F", "/Users/jo/Tick/DJ30_daily_2016_2026.csv"},
    };
    std::vector<Tr> all;
    for(const auto& S: syms){
        auto bars=load(S.path);
        omega::IndexFomcEngine e(S.name);
        e.enabled=true; e.shadow_mode=true; e.lot=0.01;
        e.p.target_vol_bps=60.0; e.p.usd_per_pt=1.0;
        std::vector<Tr> trades;
        // parallel ATR14 (same Wilder update the engine runs) captured at entry
        double atr=0,pc=0; int n=0; double atr_at_open=0; bool open=false;
        auto cb=[&](const omega::TradeRecord& tr){
            all.push_back({tr.entryTs, tr.entryPrice, tr.exitPrice, tr.mae, atr_at_open});
        };
        for(const auto& b: bars){
            bool was_open=e.has_open_position();
            e.on_d1_bar(b.h,b.l,b.c,b.c,b.c,b.ts*1000,cb);
            if(!was_open && e.has_open_position()) atr_at_open=atr;   // captured on entry bar
            double tr_= n==0?(b.h-b.l):std::fmax(b.h-b.l,std::fmax(std::fabs(b.h-pc),std::fabs(b.l-pc)));
            if(n<14) atr=(atr*n+tr_)/(n+1); else atr=(atr*13+tr_)/14.0;
            pc=b.c; ++n; (void)open;
        }
        std::printf("[%s] bars=%zu\n",S.name,bars.size());
    }
    std::printf("total FOMC trades (3 syms, 2019-2026 table window): %zu\n",all.size());

    { // per-year, live config @3bp
        std::printf("\nper-year (time-exit only, 3bp):\n");
        for(int y=2019;y<=2026;y++){ Agg a;
            for(const auto& t: all){ if(year_of(t.entry_ts)!=y) continue;
                add(a,(t.exitpx-t.entry)/t.entry*10000.0-3.0);}
            if(a.n) std::printf("  %d n=%2d net=%+8.1fbp PF=%.2f\n",y,a.n,a.net,pf(a)); }
        std::printf("per-year (stop 0.5xATR, 3bp):\n");
        for(int y=2019;y<=2026;y++){ Agg a;
            for(const auto& t: all){ if(year_of(t.entry_ts)!=y) continue;
                double r=(t.atr_entry>0&&t.mae_px>=0.5*t.atr_entry)?(-0.5*t.atr_entry/t.entry*10000.0):((t.exitpx-t.entry)/t.entry*10000.0);
                add(a,r-3.0);}
            if(a.n) std::printf("  %d n=%2d net=%+8.1fbp PF=%.2f\n",y,a.n,a.net,pf(a)); }
    }
    for(double cost_bp : {3.0, 6.0}){
        std::printf("\n================ cost %.0fbp round-trip ================\n",cost_bp);
        std::printf("%-28s %4s %6s %6s %9s %9s %9s %9s %8s\n",
            "variant","n","WR%","PF","net_bp","bear22bp","H1bp","H2bp","worst_bp");
        for(double X : {0.0, 0.5, 1.0, 1.5, 2.0}){
            Agg a,bear,h1,h2;
            int64_t tmin=INT64_MAX,tmax=0;
            for(const auto&t:all){ tmin=std::min(tmin,t.entry_ts); tmax=std::max(tmax,t.entry_ts);}
            int64_t mid=(tmin+tmax)/2;
            for(const auto& t: all){
                double ret_bp;
                if(X>0.0 && t.atr_entry>0.0 && t.mae_px >= X*t.atr_entry)
                    ret_bp = -X*t.atr_entry/t.entry*10000.0;            // stop fills adverse-first
                else
                    ret_bp = (t.exitpx-t.entry)/t.entry*10000.0;        // engine time-exit
                ret_bp -= cost_bp;
                add(a,ret_bp);
                if(year_of(t.entry_ts)==2022) add(bear,ret_bp);
                if(t.entry_ts<mid) add(h1,ret_bp); else add(h2,ret_bp);
            }
            char nm[64]; if(X==0.0) snprintf(nm,64,"time-exit only (LIVE cfg)"); else snprintf(nm,64,"cold stop %.1fxATR (adv-first)",X);
            std::printf("%-28s %4d %6.1f %6.2f %+9.1f %+9.1f %+9.1f %+9.1f %+8.1f\n",
                nm,a.n,a.n?100.0*a.w/a.n:0,pf(a),a.net,bear.net,h1.net,h2.net,a.worst);
        }
    }
    return 0;
}
