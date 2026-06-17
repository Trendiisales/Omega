// =============================================================================
// index_fomc_faithful.cpp -- ENGINE-FAITHFUL tick/D1 backtest for IndexFomcEngine
//
// Drives the REAL omega::IndexFomcEngine class (#include) with the PRODUCTION
// config from engine_init.hpp (idx_fomc_boot): lot=0.01, target_vol_bps=60,
// usd_per_pt per symbol. Feeds D1 bars EXACTLY as the engine's own
// seed_from_d1_csv loop does (same spread, same on_d1_bar path), but with
// enabled=true and a trade-capturing callback.
//
// Cost-honest: (1) injects a realistic bid/ask spread on each bar (the engine
// subtracts it in net_pnl), and (2) reports an additional commission/slippage
// stress matching OmegaCostGuard's index cost table.
//
// Risk-off gate: production feeds index_market_regime() from H1 NAS ticks. We
// can optionally feed it from D1 closes (EMA200/PERSIST100 trend proxy) so the
// bear-block (2022/2020) is reproduced. --gate=on feeds it from the symbol's
// own D1; --gate=off leaves it cold (pure seed-replay behaviour).
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/index_fomc_faithful.cpp -o /tmp/idx_fomc
// RUN:   /tmp/idx_fomc <SYMBOL> <csv> <usd_per_pt> [spread_pts] [gate=on|off] [comm_mult]
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>
#include "IndexFomcEngine.hpp"
#include "RegimeState.hpp"

struct BarRow { int64_t day_ms; double o,h,l,c; };

static std::vector<BarRow> load_csv(const char* path){
    std::vector<BarRow> v; FILE* f=fopen(path,"r"); if(!f){fprintf(stderr,"cannot open %s\n",path);return v;}
    char ln[512]; bool first=true;
    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9') continue;}
        double ts,o,h,l,c;
        if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue;
        int64_t day_ms=(ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
        int wd=(int)((((day_ms/86400000LL)%7)+4+7)%7); if(wd==6||wd==0) continue; // skip weekend
        day_ms=(day_ms/86400000LL)*86400000LL;
        v.push_back({day_ms,o,h,l,c});
    }
    fclose(f); return v;
}

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"usage: %s SYM csv usd_per_pt [spread_pts] [gate=on|off] [comm_mult]\n",argv[0]); return 1; }
    const char* sym=argv[1]; const char* csv=argv[2];
    double upp=atof(argv[3]);
    double spread_pts = (argc>4)? atof(argv[4]) : -1.0;   // -1 => use engine default (0.0001*price)
    bool gate_on = (argc>5)? (strcmp(argv[5],"on")==0||strcmp(argv[5],"gate=on")==0) : false;
    double comm_mult = (argc>6)? atof(argv[6]) : 1.0;     // commission/slippage stress multiplier (faithful add-on)

    auto bars = load_csv(csv);
    if(bars.size()<260){ fprintf(stderr,"too few bars (%zu)\n",bars.size()); return 1; }

    omega::IndexFomcEngine eng(sym);
    eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01;
    eng.p.target_vol_bps=60.0; eng.p.usd_per_pt=upp;

    struct T { int64_t entry_ts,exit_ts; double bp,net_pnl,gross_pnl,lot,entry,exit,mfe,mae; };
    std::vector<T> trades;
    auto cb=[&](const omega::TradeRecord& tr){
        double bp=(tr.exitPrice-tr.entryPrice)/tr.entryPrice*1e4;
        trades.push_back({tr.entryTs,tr.exitTs,bp,tr.net_pnl,tr.pnl,tr.size,tr.entryPrice,tr.exitPrice,tr.mfe,tr.mae});
    };

    auto& reg = omega::index_market_regime();

    for(auto& b: bars){
        if(gate_on) reg.on_h1_bar(b.o,b.h,b.l,b.c);  // D1-as-trend-bar proxy feed for the bear gate
        double sp = (spread_pts>=0.0)? spread_pts : b.c*0.00010;
        double bid=b.c-sp*0.5, ask=b.c+sp*0.5;
        eng.on_d1_bar(b.h,b.l,b.c,bid,ask,b.day_ms,cb);
    }
    eng.force_close(bars.back().day_ms, cb);

    if(trades.empty()){ printf("[%s] NO TRADES (gate_on=%d spread=%.4g) -- cost-gate blocked all?\n",sym,gate_on,spread_pts); return 0; }

    // ---- metrics on bp (regime-independent economic truth for 1-day hold) ----
    // and on net_pnl (engine shadow units). Commission stress applied in bp terms:
    // index commission ~0 (CFD), slippage in the cost table is the real friction.
    // We add a per-trade bp haircut = slippage_pts/price (two ticks of slip).
    double slip_pts_tbl = 0.0;
    { std::string s(sym);
      if(s=="US500.F")slip_pts_tbl=1.50; else if(s=="USTEC.F")slip_pts_tbl=2.00;
      else if(s=="DJ30.F")slip_pts_tbl=3.00; else slip_pts_tbl=1.5; }

    auto report=[&](const char* label, double extra_bp_haircut){
        int n=trades.size(), wins=0; double gp=0,gl=0,net_bp=0,net_pnl=0;
        std::vector<double> netbps;
        for(auto&t:trades){
            // entry at ask, exit at bid already in engine; add stress haircut in bp
            double price=t.entry>0?t.entry:1.0;
            double hb = extra_bp_haircut>0? (extra_bp_haircut/price*1e4):0.0;
            double bp = t.bp - hb;
            netbps.push_back(bp);
            net_bp+=bp;
            // scale pnl proportionally to the bp change for the haircut
            double pnl_adj = t.net_pnl + (bp - t.bp)/1e4 * (t.lot*upp);
            net_pnl += pnl_adj;
            if(bp>0){wins++;gp+=bp;}else gl+=-bp;
        }
        double pf = gl>0? gp/gl : (gp>0?999:0);
        // halves
        int half=n/2; double h1=0,h2=0; for(int i=0;i<n;i++){ if(i<half)h1+=netbps[i]; else h2+=netbps[i]; }
        // fat tail top3
        std::vector<double> sorted=netbps; std::sort(sorted.begin(),sorted.end(),std::greater<double>());
        double top3=0; for(int i=0;i<3&&i<(int)sorted.size();i++) top3+=sorted[i];
        double net_pos_sum=0; for(double x:netbps) if(x>0) net_pos_sum+=x;
        printf("  [%-22s] n=%d WR=%.0f%% PF=%.2f net=%+.1fbp avg=%+.2fbp | H1=%+.1f H2=%+.1f | net_pnl(shadow)=%+.2f | top3=%+.1fbp(%.0f%% of gross+)\n",
               label,n,100.0*wins/n,pf,net_bp,net_bp/n,h1,h2,net_pnl,top3, net_pos_sum>0?100.0*top3/net_pos_sum:0);
    };

    // year-split decay view
    printf("\n=== IndexFomc FAITHFUL %s  (lot=%.2f vol_tgt=%.0f upp=%.0f gate=%s)  n=%zu ===\n",
           sym,eng.lot,eng.p.target_vol_bps,upp, gate_on?"ON":"OFF", trades.size());
    report("base (engine spread)", 0.0);
    report("+IBKR slip stress",    slip_pts_tbl);
    report("+2x slip stress",      slip_pts_tbl*2.0);

    // per-trade dump (compact) + era split
    double pre23=0,post23=0; int npre=0,npost=0;
    for(auto&t:trades){ time_t et=t.exit_ts; struct tm g; gmtime_r(&et,&g); int y=g.tm_year+1900;
        if(y<=2022){pre23+=t.bp;npre++;} else {post23+=t.bp;npost++;} }
    printf("  era: 2019-22 sum=%+.1fbp (n=%d, avg %+.2f) | 2023-26 sum=%+.1fbp (n=%d, avg %+.2f)\n",
        pre23,npre, npre?pre23/npre:0, post23,npost, npost?post23/npost:0);
    return 0;
}
