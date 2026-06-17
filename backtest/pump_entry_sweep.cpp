// pump_entry_sweep.cpp — ENGINE-FAITHFUL ENTRY sweep for PumpScalpEngine.
//
// The exhaustive EXIT sweep (pump_exit_sweep.cpp) proved no exit setting rescues
// the engine: on 47 name-days the LIVE exit is net-NEG (PF 0.35) and ZERO exit
// configs clear the ship rule. The leak is UPSTREAM — entry selectivity. This
// sweeps every ENTRY knob (regime gate, liquidity floors, ignition quality, the
// anti-late-chase ENTRY_MAX_EXT_PCT filter, re-entry cap) with the EXIT FROZEN at
// the live config, to answer: can entry selectivity flip it robust? If nothing
// clears the ship rule (net+PF up both slips, both halves +), the engine is culled.
//
// Build: g++ -std=c++17 -Iinclude backtest/pump_entry_sweep.cpp -o /tmp/pumpentry
// Run:   /tmp/pumpentry backtest/data/pump_bars_big.csv
#include "PumpScalpEngine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using omega::PumpScalpEngine;
using omega::TradeRecord;

struct Bar { int64_t ts; double o,h,l,c,v; };
struct NameDay { std::string sym; int day; std::vector<Bar> bars; };

struct EntryCfg {
    std::string name;
    double day_gate=100.0, ig_pct=3.0, strength=0.60, slope_min=0.0;
    int    lb=3;
    double min_dvol=2.0e6, price_min=1.0, max_ext=0.0;
    int    maxent=2;
    bool   vol_reg=true;
};

static PumpScalpEngine build(const EntryCfg& x, double slip) {
    PumpScalpEngine e;
    // entry config under test
    e.DAY_GATE_PCT=x.day_gate; e.IG_PCT=x.ig_pct; e.LB=x.lb; e.STRENGTH=x.strength;
    e.VOL_REG_FILTER=x.vol_reg; e.SLOPE_MIN=x.slope_min; e.MIN_DVOL_USD=x.min_dvol;
    e.PRICE_MIN=x.price_min; e.MAX_ENTRIES_PER_DAY=x.maxent; e.ENTRY_MAX_EXT_PCT=x.max_ext;
    e.TF_SEC=180; e.VOLX=0.0; e.RUNUP_PCT=20.0; e.EXT_PCT=5.0; e.NEWHOD_M=8; e.ALLOW_SHORT=true; e.REG_LB=12;
    // EXIT frozen = live deployed
    e.TRAIL_PCT=2.0; e.HARD_PCT=6.0; e.BE_ARM_PCT=2.0; e.BE_FLOOR_PCT=2.0; e.MAXHOLD_SEC=5*180;
    e.NOTIONAL_USD=1000.0; e.SLIP_PCT=slip; e.PYR_ADDS=0; e.shadow_mode=true;
    e.init();
    return e;
}

static void replay(const NameDay& nd, const EntryCfg& x, double slip,
                   std::vector<std::pair<int,TradeRecord>>& out) {
    PumpScalpEngine e = build(x, slip);
    std::vector<TradeRecord> trs;
    e.on_trade_record=[&trs](const TradeRecord& t){ trs.push_back(t); };
    e.symbol=nd.sym; e.engine_name="PumpScalp_3m";
    for (const Bar& b : nd.bars) {
        const int64_t t0=b.ts;
        const int dir = e.has_open_position() ? e.pos.dir : 1;   // adverse-first intrabar path
        if (dir>0) { e.on_price(b.o,t0); e.on_price(b.l,t0+60000); e.on_price(b.h,t0+120000); e.on_price(b.c,t0+150000); }
        else       { e.on_price(b.o,t0); e.on_price(b.h,t0+60000); e.on_price(b.l,t0+120000); e.on_price(b.c,t0+150000); }
        e.on_entry_bar(b.o,b.h,b.l,b.c,b.v,t0+170000,false);
    }
    if (!nd.bars.empty() && e.has_open_position())
        e.force_close(nd.bars.back().c, nd.bars.back().ts+180000);
    for (auto& t : trs) out.push_back({nd.day, t});
}

struct Metric { int n=0,wins=0; double net=0,gw=0,gl=0,maxdd=0,half_a=0,half_b=0; };
static Metric score(std::vector<std::pair<int,TradeRecord>> trs, int split) {
    Metric m; std::sort(trs.begin(),trs.end(),[](auto&a,auto&b){return a.second.exitTs<b.second.exitTs;});
    double cum=0,peak=0;
    for (auto& p : trs){ double v=p.second.pnl; m.n++; m.net+=v; if(v>0){m.gw+=v;m.wins++;}else m.gl+=-v;
        cum+=v; peak=std::max(peak,cum); m.maxdd=std::max(m.maxdd,peak-cum);
        if(p.first<split) m.half_a+=v; else m.half_b+=v; }
    return m;
}
static double pf(const Metric& m){ return m.gl>1e-9?m.gw/m.gl:(m.gw>0?999.0:0.0); }

int main(int argc, char** argv) {
    const char* path = argc>1?argv[1]:"backtest/data/pump_bars_big.csv";
    std::ifstream f(path); if(!f){ fprintf(stderr,"cannot open %s\n",path); return 2; }
    std::map<std::string,NameDay> book; std::string line; std::getline(f,line);
    while (std::getline(f,line)){ if(line.empty())continue; std::stringstream ss(line); std::string cl; std::vector<std::string> col;
        while(std::getline(ss,cl,','))col.push_back(cl); if(col.size()<8)continue;
        NameDay& nd=book[col[0]+":"+col[1]]; nd.sym=col[0]; nd.day=std::stoi(col[1]);
        nd.bars.push_back({std::stoll(col[2]),std::stod(col[3]),std::stod(col[4]),std::stod(col[5]),std::stod(col[6]),std::stod(col[7])}); }
    std::vector<NameDay> nds; std::vector<int> days;
    for(auto&kv:book){ std::sort(kv.second.bars.begin(),kv.second.bars.end(),[](auto&a,auto&b){return a.ts<b.ts;}); nds.push_back(kv.second); days.push_back(kv.second.day); }
    std::sort(days.begin(),days.end()); days.erase(std::unique(days.begin(),days.end()),days.end());
    int split = days.empty()?0:days[days.size()/2];
    printf("Loaded %zu name-days, %zu days, half-split %d (EXIT frozen=live trail2/BE2/h6/mh5)\n\n", nds.size(), days.size(), split);

    std::vector<EntryCfg> cfgs;
    auto add=[&](EntryCfg c){ cfgs.push_back(c); };
    add({"LIVE_gate100_ig3_dvol2M_px1_me2"});
    // Stage A — SELECTIVITY: regime gate x liquidity x price floor x re-entry cap
    for (double g : {100.0,150.0,200.0,300.0})
      for (double dv : {2.0e6,5.0e6,1.0e7,2.0e7})
        for (double px : {1.0,2.0,5.0})
          for (int me : {1,2}) {
            char nm[96]; snprintf(nm,sizeof nm,"A_g%.0f_dv%.0fM_px%.0f_me%d",g,dv/1e6,px,me);
            EntryCfg c; c.name=nm; c.day_gate=g; c.min_dvol=dv; c.price_min=px; c.maxent=me; add(c);
          }
    // Stage B — IGNITION QUALITY + ANTI-LATE-CHASE (at a selective base g150/dv5M/px2/me2)
    for (double ig : {2.0,3.0,4.0,5.0})
      for (int lb : {2,3})
        for (double st : {0.5,0.6,0.7})
          for (double ext : {0.0,5.0,10.0,20.0}) {
            char nm[96]; snprintf(nm,sizeof nm,"B_ig%.0f_lb%d_st%.0f_ext%.0f",ig,lb,st*100,ext);
            EntryCfg c; c.name=nm; c.day_gate=150.0; c.min_dvol=5.0e6; c.price_min=2.0; c.maxent=2;
            c.ig_pct=ig; c.lb=lb; c.strength=st; c.max_ext=ext; add(c);
          }
    // Stage C — anti-chase x maxent1 at strict selectivity (the "fewest, cleanest" thesis)
    for (double ext : {5.0,10.0,20.0})
      for (double g : {150.0,200.0,300.0}) {
        char nm[96]; snprintf(nm,sizeof nm,"C_g%.0f_ext%.0f_me1_dv5M",g,ext);
        EntryCfg c; c.name=nm; c.day_gate=g; c.max_ext=ext; c.maxent=1; c.min_dvol=5.0e6; c.price_min=2.0; add(c);
      }

    struct Row { EntryCfg c; Metric m1,m2; };
    std::vector<Row> rows;
    for (const EntryCfg& c : cfgs){ std::vector<std::pair<int,TradeRecord>> t1,t2;
        for(const NameDay& nd:nds){ replay(nd,c,1.0,t1); replay(nd,c,2.0,t2);} rows.push_back({c,score(t1,split),score(t2,split)}); }
    std::sort(rows.begin(),rows.end(),[](const Row&a,const Row&b){ if(std::fabs(a.m2.net-b.m2.net)>1e-6)return a.m2.net>b.m2.net; return pf(a.m2)>pf(b.m2); });

    printf("%-30s | %4s | net@1%%  PF | net@2%%  PF  WR maxDD | hA   hB  | ROBUST\n","CONFIG","n");
    printf("%s\n", std::string(108,'-').c_str());
    auto pr=[&](const Row& r){ bool rob=r.m1.net>0&&r.m2.net>0&&r.m2.half_a>0&&r.m2.half_b>0;
        printf("%-30s | %4d | %6.0f %5.2f | %6.0f %5.2f %3.0f %5.0f | %4.0f %4.0f | %s\n",
            r.c.name.c_str(), r.m2.n, r.m1.net, pf(r.m1), r.m2.net, pf(r.m2),
            r.m2.n?100.0*r.m2.wins/r.m2.n:0.0, r.m2.maxdd, r.m2.half_a, r.m2.half_b, rob?"YES":""); };
    int s=0; for(const Row& r:rows){ if(s++<400) pr(r); }
    printf("%s\n", std::string(108,'-').c_str());
    for(const Row& r:rows) if(r.c.name.rfind("LIVE",0)==0){ printf("BASELINE -> "); pr(r); }
    const Row* best=nullptr; for(const Row& r:rows){ if(r.m1.net>0&&r.m2.net>0&&r.m2.half_a>0&&r.m2.half_b>0){best=&r;break;} }
    printf("\nBEST ROBUST entry (net+PF up both slips, both halves +): %s\n",
        best?best->c.name.c_str():"<none — NO entry config clears the ship rule -> CULL>");
    return 0;
}
