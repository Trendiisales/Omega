// bigcap_sweep.cpp — ENGINE-FAITHFUL full ENTRY x EXIT sweep of BigCapMomo.
//
// Drives the REAL PumpScalpEngine class (the g_bigcap_momo instance) over Yahoo 5m
// big-cap bars. Sweeps every entry + exit lever to find the best robust settings,
// or prove there are none -> cull. Robust = net+ at BOTH slip levels (15+30bps)
// AND both walk-forward halves + AND net-ex-top5 + (not one-fat-tail-name driven).
//
// Build: g++ -std=c++17 -Iinclude backtest/bigcap_sweep.cpp -o /tmp/bcsweep
// Run:   /tmp/bcsweep backtest/data/bigcap_5m.csv
#include "PumpScalpEngine.hpp"
#include <algorithm>
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

struct Cfg {
    std::string name;
    double gate=4.0, ig=3.0, strength=0.60, ext=0.0;     // entry
    int    lb=3, maxent=2; bool allow_short=false;
    double trail=5.0; int atr_len=0; double atr_mult=0.0; int maxhold=48;   // exit
    // ride/ratchet levers (2026-06-18 — the fixes the prior grid never tested):
    double be_arm=0.0, be_floor=0.0, giveback=0.0; bool skip_mh_profit=false;
};

static PumpScalpEngine build(const Cfg& x, double slip) {
    PumpScalpEngine e;
    e.TF_SEC=300; e.DAY_GATE_PCT=x.gate; e.LB=x.lb; e.IG_PCT=x.ig; e.VOLX=0.0; e.STRENGTH=x.strength;
    e.ENTRY_MAX_EXT_PCT=x.ext; e.ALLOW_SHORT=x.allow_short; e.MAX_ENTRIES_PER_DAY=x.maxent;
    e.TRAIL_PCT=x.trail; e.ATR_LEN=x.atr_len; e.ATR_MULT=x.atr_mult; e.HARD_PCT=6.0;
    e.BE_ARM_PCT=x.be_arm; e.BE_FLOOR_PCT=x.be_floor; e.GIVEBACK_FRAC=x.giveback;
    e.MAXHOLD_SKIP_IF_PROFIT=x.skip_mh_profit; e.MAXHOLD_SEC=x.maxhold*300;
    e.RUNUP_PCT=20.0; e.EXT_PCT=5.0; e.NEWHOD_M=8; e.VOL_REG_FILTER=true; e.REG_LB=12; e.SLOPE_MIN=0.0;
    e.NOTIONAL_USD=1000.0; e.MIN_DVOL_USD=0.0; e.PRICE_MIN=0.0; e.PYR_ADDS=0; e.SLIP_PCT=slip; e.shadow_mode=true;
    e.init(); return e;
}

static void replay(const NameDay& nd, const Cfg& x, double slip, std::vector<std::pair<int,TradeRecord>>& out) {
    PumpScalpEngine e = build(x, slip);
    std::vector<TradeRecord> trs; e.on_trade_record=[&trs](const TradeRecord& t){ trs.push_back(t); };
    e.symbol=nd.sym; e.engine_name="BigCapMomo";
    for (const Bar& b : nd.bars) {
        const int64_t t0=b.ts; const int dir = e.has_open_position()? e.pos.dir : 1;
        if (dir>0){ e.on_price(b.o,t0); e.on_price(b.l,t0+75000); e.on_price(b.h,t0+150000); e.on_price(b.c,t0+225000); }
        else      { e.on_price(b.o,t0); e.on_price(b.h,t0+75000); e.on_price(b.l,t0+150000); e.on_price(b.c,t0+225000); }
        e.on_entry_bar(b.o,b.h,b.l,b.c,b.v,t0+290000,false);
    }
    if (!nd.bars.empty() && e.has_open_position()) e.force_close(nd.bars.back().c, nd.bars.back().ts+300000);
    for (auto& t : trs) out.push_back({nd.day, t});
}

struct Metric { int n=0,wins=0; double net=0,gw=0,gl=0,maxdd=0,ha=0,hb=0,top5=0; };
static Metric score(std::vector<std::pair<int,TradeRecord>> trs, int split) {
    Metric m; std::sort(trs.begin(),trs.end(),[](auto&a,auto&b){return a.second.exitTs<b.second.exitTs;});
    double cum=0,peak=0; std::vector<double> pos;
    for (auto& p:trs){ double v=p.second.pnl; m.n++; m.net+=v; if(v>0){m.gw+=v;m.wins++;pos.push_back(v);} else m.gl+=-v;
        cum+=v; peak=std::max(peak,cum); m.maxdd=std::max(m.maxdd,peak-cum); if(p.first<split)m.ha+=v; else m.hb+=v; }
    std::sort(pos.rbegin(),pos.rend()); double t5=0; for(size_t i=0;i<pos.size()&&i<5;i++)t5+=pos[i]; m.top5=m.net-t5;
    return m;
}
static double pf(const Metric& m){ return m.gl>1e-9?m.gw/m.gl:(m.gw>0?999.0:0.0); }

int main(int argc, char** argv) {
    const char* path = argc>1?argv[1]:"backtest/data/bigcap_5m.csv";
    std::ifstream f(path); if(!f){ fprintf(stderr,"cannot open %s\n",path); return 2; }
    std::map<std::string,NameDay> book; std::string line; std::getline(f,line);
    while (std::getline(f,line)){ if(line.empty())continue; std::stringstream ss(line); std::string cl; std::vector<std::string> col;
        while(std::getline(ss,cl,','))col.push_back(cl); if(col.size()<8)continue;
        try { NameDay& nd=book[col[0]+":"+col[1]]; nd.sym=col[0]; nd.day=std::stoi(col[1]);
            nd.bars.push_back({std::stoll(col[2]),std::stod(col[3]),std::stod(col[4]),std::stod(col[5]),std::stod(col[6]),std::stod(col[7])}); }
        catch(...){ continue; } }
    std::vector<NameDay> nds; std::vector<int> days;
    for(auto&kv:book){ std::sort(kv.second.bars.begin(),kv.second.bars.end(),[](auto&a,auto&b){return a.ts<b.ts;}); nds.push_back(kv.second); days.push_back(kv.second.day); }
    std::sort(days.begin(),days.end()); days.erase(std::unique(days.begin(),days.end()),days.end());
    int split = days.empty()?0:days[days.size()/2];
    printf("Loaded %zu name-days, %zu days (5m Yahoo 60d), WF split %d. slip 15bps live / 30bps stress.\n\n", nds.size(), days.size(), split);
    if (nds.empty()){ fprintf(stderr,"no data\n"); return 2; }

    std::vector<Cfg> cfgs;
    auto add=[&](Cfg c){ cfgs.push_back(c); };
    add({"LIVE_g4_trail5_short_me2", 4,3,0.60,0, 3,2,true, 5.0,0,0,48});
    // Stage E — EXIT (long-only, entry g4/ig3/ext0/me2): trail x maxhold x ATR-trail
    for (double tr : {3.0,4.0,5.0,6.0,8.0})
      for (int mh : {24,48,96}) {
        char nm[80]; snprintf(nm,sizeof nm,"E_tr%.0f_mh%d",tr,mh);
        add({nm, 4,3,0.60,0, 3,2,false, tr,0,0,mh});
      }
    for (int al : {10,14,20,30}) for (double am : {3.0,4.0,5.0,6.0}) for (int mh : {48,96}) {
        char nm[80]; snprintf(nm,sizeof nm,"E_ATR%d_x%.0f_mh%d",al,am,mh);
        add({nm, 4,3,0.60,0, 3,2,false, 0.0,al,am,mh});
      }
    // Stage N — ENTRY (exit trail5/mh48 long-only): gate x anti-chase ext x maxent
    for (double g : {3.0,4.0,5.0,6.0,8.0})
      for (double ex : {0.0,5.0,10.0,20.0})
        for (int me : {1,2}) {
          char nm[80]; snprintf(nm,sizeof nm,"N_g%.0f_ext%.0f_me%d",g,ex,me);
          add({nm, g,3,0.60,ex, 3,me,false, 5.0,0,0,48});
        }
    // Stage X — best-guess combos (anti-chase + wider trail + selective gate)
    add({"X_g5_ext10_trail6_me1", 5,3,0.60,10, 3,1,false, 6.0,0,0,48});
    add({"X_g5_ext10_trail5_me2", 5,3,0.60,10, 3,2,false, 5.0,0,0,48});
    add({"X_g6_ext10_trail5_me1", 6,3,0.60,10, 3,1,false, 5.0,0,0,48});
    // Stage R — RIDE/RATCHET (2026-06-18, the ledger-diagnosed fix): let in-profit
    // winners ride past the clock (skip_mh_profit) + lock gains with BE-ratchet /
    // give-back instead of the loose trail that gave back ~$451 live.
    // fields: name,gate,ig,strength,ext, lb,maxent,short, trail,atr_len,atr_mult,maxhold, be_arm,be_floor,giveback,skip_mh_profit
    // R1: ride-until-fade — skip time-stop while in profit, vary trail width
    for (double tr : {4.0,5.0,6.0,8.0}) {
        char nm[96]; snprintf(nm,sizeof nm,"R_ride_tr%.0f_mh96",tr);
        add({nm, 4,3,0.60,0, 3,2,false, tr,0,0,96, 0,0,0,true});
    }
    // R2: ride + BE-ratchet (arm at +X%, floor at net-BE) — lock then ride
    for (double ba : {3.0,5.0,8.0}) for (double bf : {1.0,2.0}) {
        char nm[96]; snprintf(nm,sizeof nm,"R_ride_be%.0f_fl%.0f",ba,bf);
        add({nm, 4,3,0.60,0, 3,2,false, 6.0,0,0,96, ba,bf,0,true});
    }
    // R3: ride + give-back lock (close on X% retrace of peak gain)
    for (double gb : {0.30,0.40,0.50}) {
        char nm[96]; snprintf(nm,sizeof nm,"R_ride_gb%.0f",gb*100);
        add({nm, 4,3,0.60,0, 3,2,false, 8.0,0,0,96, 0,0,gb,true});
    }
    // R4: anti-chase entry + ride + ratchet (best-guess robust combo)
    add({"R_g5_ext10_ride_be5_fl2", 5,3,0.60,10, 3,1,false, 6.0,0,0,96, 5,2,0,true});
    add({"R_g6_ext10_ride_gb40",    6,3,0.60,10, 3,1,false, 8.0,0,0,96, 0,0,0.40,true});
    // R5 — BEST winner + gain-protection combo: ATR-trail (best exit) + BE-ratchet (lock gains)
    // + ride-in-profit (don't clock out winners). The deploy candidate.
    add({"R_BEST_atr30x4_be3fl2_ride", 4,3,0.60,0, 3,2,false, 0.0,30,4.0,96, 3,2,0,true});
    add({"R_BEST_atr30x4_be5fl2_ride", 4,3,0.60,0, 3,2,false, 0.0,30,4.0,96, 5,2,0,true});

    struct Row { Cfg c; Metric m15,m30; };
    std::vector<Row> rows;
    for (const Cfg& c : cfgs){ std::vector<std::pair<int,TradeRecord>> t15,t30;
        for(const NameDay& nd:nds){ replay(nd,c,0.15,t15); replay(nd,c,0.30,t30); } rows.push_back({c,score(t15,split),score(t30,split)}); }
    std::sort(rows.begin(),rows.end(),[](const Row&a,const Row&b){ if(std::abs(a.m30.net-b.m30.net)>1e-6)return a.m30.net>b.m30.net; return pf(a.m30)>pf(b.m30); });

    auto robust=[](const Row& r){ return r.m15.net>0&&r.m30.net>0&&r.m30.ha>0&&r.m30.hb>0&&r.m30.top5>0; };
    printf("%-24s | %5s | net15 PF15 WR | net30 PF30 | hA   hB(30) | exTop5 | maxDD | ROB\n","CONFIG","n");
    printf("%s\n", std::string(104,'-').c_str());
    auto prn=[&](const Row& r){ printf("%-24s | %5d | %5.0f %4.2f %3.0f | %5.0f %4.2f | %4.0f %4.0f | %5.0f | %5.0f | %s\n",
        r.c.name.c_str(), r.m15.n, r.m15.net, pf(r.m15), r.m15.n?100.0*r.m15.wins/r.m15.n:0.0,
        r.m30.net, pf(r.m30), r.m30.ha, r.m30.hb, r.m30.top5, r.m30.maxdd, robust(r)?"YES":""); };
    int s=0; for(const Row& r:rows){ if(s++<26) prn(r); }
    printf("%s\n", std::string(104,'-').c_str());
    for(const Row& r:rows) if(r.c.name.rfind("LIVE",0)==0){ printf("BASELINE -> "); prn(r); }
    const Row* best=nullptr; for(const Row& r:rows){ if(robust(r)){best=&r;break;} }
    printf("\nBEST ROBUST: %s\n", best?best->c.name.c_str():"<none robust -> CULL>");
    return 0;
}
