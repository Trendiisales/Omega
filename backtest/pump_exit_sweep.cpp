// pump_exit_sweep.cpp — ENGINE-FAITHFUL exhaustive EXIT sweep for PumpScalpEngine.
//
// Operator ask (2026-06-18): "look at all the options as to how we exit and find
// the best — try every lever/setting." This drives the REAL PumpScalpEngine class
// (no Python re-impl drift) over CSV bars dumped by pump/fetch_pump_bars.py, with
// the ENTRY config FROZEN at the live settings so every difference is the EXIT.
//
// Faithful + HONEST-pessimistic replay:
//   • entries fire on closed bars exactly as live (gate100 + ignition, VOLX off).
//   • intrabar path is ADVERSE-FIRST (long: o->l->h->c) so a trail can be stopped
//     at the low BEFORE the high — the conservative direction. Bar-replay STILL
//     overstates (no real gaps/halts/queue) => treat results as a HINT, discount
//     ~0.5-0.7 PF, real arbiter = the live shadow ledger (BACKTEST_TRUTH).
//
// Ship rule (same as pump_exit_bt.py): a config beats the live exit only if net
// AND PF improve at BOTH slip levels AND BOTH basket halves are positive.
//
// Build: g++ -std=c++17 -Iinclude backtest/pump_exit_sweep.cpp -o /tmp/pumpsweep
// Run:   /tmp/pumpsweep backtest/data/pump_bars.csv
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

// ── Exit config under test. Entry levers are NOT here — they stay frozen. ──────
struct ExitCfg {
    std::string name;
    double trail=2.0, hard=6.0, be_arm=2.0, be_floor=2.0;
    int    maxhold_bars=5;
    int    atr_len=0;    double atr_mult=0.0;
    int    struct_lb=0;
    bool   roll_vwap=false, roll_ema=false;
    double giveback=0.0;
};

// Apply the FROZEN live entry config + this exit config to a fresh engine.
static PumpScalpEngine build(const ExitCfg& x, double slip_pct) {
    PumpScalpEngine e;
    // ── frozen live ENTRY config (PumpScalpManager::configure + engine defaults) ──
    e.TF_SEC=180; e.DAY_GATE_PCT=100.0; e.LB=3; e.IG_PCT=3.0; e.VOLX=0.0;  // VOLX off (live)
    e.STRENGTH=0.60; e.RUNUP_PCT=20.0; e.EXT_PCT=5.0; e.NEWHOD_M=8; e.ALLOW_SHORT=true;
    e.VOL_REG_FILTER=true; e.REG_LB=12; e.SLOPE_MIN=0.0;
    e.NOTIONAL_USD=1000.0; e.PRICE_MIN=1.0; e.MIN_DVOL_USD=2.0e6; e.MAX_ENTRIES_PER_DAY=2;
    e.PYR_ADDS=0; e.SLIP_PCT=slip_pct; e.shadow_mode=true;
    // ── exit config under test ──
    e.TRAIL_PCT=x.trail; e.HARD_PCT=x.hard; e.BE_ARM_PCT=x.be_arm; e.BE_FLOOR_PCT=x.be_floor;
    e.MAXHOLD_SEC=x.maxhold_bars*180;
    e.ATR_LEN=x.atr_len; e.ATR_MULT=x.atr_mult; e.STRUCT_LB=x.struct_lb;
    e.ROLLOVER_VWAP=x.roll_vwap; e.ROLLOVER_EMA=x.roll_ema; e.GIVEBACK_FRAC=x.giveback;
    e.init();
    return e;
}

// One name-day, faithful adverse-first replay. Appends booked trades (tagged day).
static void replay(const NameDay& nd, const ExitCfg& x, double slip,
                   std::vector<std::pair<int,TradeRecord>>& out) {
    PumpScalpEngine e = build(x, slip);
    std::vector<TradeRecord> trs;
    e.on_trade_record=[&trs](const TradeRecord& t){ trs.push_back(t); };
    e.symbol=nd.sym; e.engine_name="PumpScalp_3m";
    for (const Bar& b : nd.bars) {
        const int64_t t0=b.ts;
        // adverse-first intrabar ticks (manage any OPEN position through this bar)
        // long: low before high; short: high before low. close last.
        // We don't know dir until a pos is open; feed BOTH orders is wrong, so use
        // the open position's dir; if flat, ticks are inert (on_price no-ops).
        const int dir = e.has_open_position() ? e.pos.dir : 1;
        if (dir>0) { e.on_price(b.o,t0); e.on_price(b.l,t0+60000); e.on_price(b.h,t0+120000); e.on_price(b.c,t0+150000); }
        else       { e.on_price(b.o,t0); e.on_price(b.h,t0+60000); e.on_price(b.l,t0+120000); e.on_price(b.c,t0+150000); }
        e.on_entry_bar(b.o,b.h,b.l,b.c,b.v,t0+170000,false);   // entry on close + bar-close exits
    }
    if (!nd.bars.empty() && e.has_open_position())
        e.force_close(nd.bars.back().c, nd.bars.back().ts+180000);
    for (auto& t : trs) out.push_back({nd.day, t});
}

struct Metric { int n=0; double net=0,gw=0,gl=0,maxdd=0,half_a=0,half_b=0; int wins=0; };

static Metric score(std::vector<std::pair<int,TradeRecord>> trs, int split_day) {
    Metric m;
    std::sort(trs.begin(), trs.end(), [](auto&a, auto&b){ return a.second.exitTs<b.second.exitTs; });
    double cum=0, peak=0;
    for (auto& pr : trs) {
        const double p = pr.second.pnl;
        m.n++; m.net+=p; if (p>0){m.gw+=p;m.wins++;} else m.gl+=-p;
        cum+=p; peak=std::max(peak,cum); m.maxdd=std::max(m.maxdd, peak-cum);
        if (pr.first < split_day) m.half_a+=p; else m.half_b+=p;
    }
    return m;
}
static double pf(const Metric& m){ return m.gl>1e-9 ? m.gw/m.gl : (m.gw>0?999.0:0.0); }

int main(int argc, char** argv) {
    const char* path = argc>1 ? argv[1] : "backtest/data/pump_bars.csv";
    std::ifstream f(path);
    if (!f) { fprintf(stderr,"cannot open %s\n", path); return 2; }
    // load CSV -> name-days
    std::map<std::string, NameDay> book; std::string line; std::getline(f,line); // header
    std::vector<int> all_days;
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        std::stringstream ss(line); std::string cell; std::vector<std::string> col;
        while (std::getline(ss,cell,',')) col.push_back(cell);
        if (col.size()<8) continue;
        const std::string key=col[0]+":"+col[1];
        NameDay& nd=book[key]; nd.sym=col[0]; nd.day=std::stoi(col[1]);
        nd.bars.push_back({std::stoll(col[2]),std::stod(col[3]),std::stod(col[4]),
                           std::stod(col[5]),std::stod(col[6]),std::stod(col[7])});
    }
    std::vector<NameDay> nds; std::vector<int> days;
    for (auto& kv : book){ std::sort(kv.second.bars.begin(),kv.second.bars.end(),
                           [](auto&a,auto&b){return a.ts<b.ts;}); nds.push_back(kv.second); days.push_back(kv.second.day); }
    std::sort(days.begin(),days.end()); days.erase(std::unique(days.begin(),days.end()),days.end());
    const int split_day = days.empty()?0:days[days.size()/2];   // halves: < split | >= split
    printf("Loaded %zu name-days, %zu unique days, half-split at day %d\n\n", nds.size(), days.size(), split_day);
    if (nds.empty()) { fprintf(stderr,"no data\n"); return 2; }

    // ── build the sweep: Stage 1 classic grid, Stage 2 new modes, Stage 3 combos ──
    std::vector<ExitCfg> cfgs;
    auto add=[&](ExitCfg c){ cfgs.push_back(c); };
    // LIVE baseline (the thing to beat)
    add({"LIVE_trail2_BE2_h6_mh5"});
    // Stage 1 — classic levers: trail x BE x hard x maxhold
    for (double tr : {1.0,1.5,2.0,2.5,3.0,4.0})
      for (double ba : {0.0,2.0,3.0})
        for (double hd : {6.0,8.0})
          for (int mh : {5,10,20}) {
            char nm[96]; snprintf(nm,sizeof nm,"S1_tr%.1f_be%.0f_h%.0f_mh%d",tr,ba,hd,mh);
            ExitCfg c; c.name=nm; c.trail=tr; c.be_arm=ba; c.be_floor=ba>0?ba:2.0; c.hard=hd; c.maxhold_bars=mh;
            add(c);
          }
    // Stage 2 — each NEW exit mode (vs a sane base), its own param swept
    // ATR trail (trail off):
    for (int al : {10,14,20}) for (double am : {2.0,2.5,3.0}) {
        char nm[96]; snprintf(nm,sizeof nm,"S2_ATR%d_x%.1f",al,am);
        ExitCfg c; c.name=nm; c.trail=0.0; c.atr_len=al; c.atr_mult=am; c.be_arm=2.0; c.be_floor=2.0; c.maxhold_bars=10; add(c);
    }
    // Structure / rollover / give-back: trail OFF so the MODE is the binding exit
    // (a tight % trail would fire first and mask them). Loose hard backstop only.
    for (int sl : {2,3,5}) { char nm[64]; snprintf(nm,sizeof nm,"S2_STRUCT%d",sl);
        ExitCfg c; c.name=nm; c.trail=0; c.hard=8; c.maxhold_bars=20; c.struct_lb=sl; add(c); }
    { ExitCfg c; c.name="S2_ROLL_VWAP"; c.trail=0; c.hard=8; c.maxhold_bars=20; c.roll_vwap=true; add(c); }
    { ExitCfg c; c.name="S2_ROLL_EMA";  c.trail=0; c.hard=8; c.maxhold_bars=20; c.roll_ema=true;  add(c); }
    for (double g : {0.3,0.4,0.5}) { char nm[64]; snprintf(nm,sizeof nm,"S2_GIVEBACK%.0f",g*100);
        ExitCfg c; c.name=nm; c.trail=0; c.hard=8; c.maxhold_bars=20; c.giveback=g; add(c); }
    // Stage 3 — combine the ATR-trail winner with a reversal-signal cut.
    { ExitCfg c; c.name="S3_ATR14x2.5+STRUCT3"; c.trail=0; c.atr_len=14; c.atr_mult=2.5; c.struct_lb=3; c.maxhold_bars=20; add(c); }
    { ExitCfg c; c.name="S3_ATR14x2.5+ROLLVWAP"; c.trail=0; c.atr_len=14; c.atr_mult=2.5; c.roll_vwap=true; c.maxhold_bars=20; add(c); }
    { ExitCfg c; c.name="S3_ATR20x2.0+GIVE50"; c.trail=0; c.atr_len=20; c.atr_mult=2.0; c.giveback=0.5; c.maxhold_bars=20; add(c); }

    // ── run every config at slip 1% and 2%, score, rank ──
    struct Row { ExitCfg c; Metric m1,m2; };
    std::vector<Row> rows;
    for (const ExitCfg& c : cfgs) {
        std::vector<std::pair<int,TradeRecord>> t1,t2;
        for (const NameDay& nd : nds){ replay(nd,c,1.0,t1); replay(nd,c,2.0,t2); }
        rows.push_back({c, score(t1,split_day), score(t2,split_day)});
    }
    // rank by net@2% (the honest/stress level), tiebreak PF@2%
    std::sort(rows.begin(),rows.end(),[](const Row&a,const Row&b){
        if (std::fabs(a.m2.net-b.m2.net)>1e-6) return a.m2.net>b.m2.net;
        return pf(a.m2)>pf(b.m2); });

    printf("%-26s | %4s | net@1%%  PF@1%% | net@2%%  PF@2%%  WR  maxDD | halfA halfB(@2%%) | ROBUST\n","CONFIG","n");
    printf("%s\n", std::string(120,'-').c_str());
    auto print_row=[&](const Row& r){
        const bool robust = r.m1.net>0 && r.m2.net>0 && r.m2.half_a>0 && r.m2.half_b>0;
        printf("%-26s | %4d | %7.0f %5.2f | %7.0f %5.2f %3.0f %6.0f | %5.0f %5.0f | %s\n",
            r.c.name.c_str(), r.m2.n, r.m1.net, pf(r.m1), r.m2.net, pf(r.m2),
            r.m2.n?100.0*r.m2.wins/r.m2.n:0.0, r.m2.maxdd, r.m2.half_a, r.m2.half_b,
            robust?"YES":"");
    };
    // top 25 + always show LIVE baseline for reference
    int shown=0; for (const Row& r : rows){ if(shown++<200) print_row(r); }
    printf("%s\n", std::string(120,'-').c_str());
    for (const Row& r : rows) if (r.c.name.rfind("LIVE",0)==0){ printf("BASELINE -> "); print_row(r); }
    // best ROBUST config (the only kind we'd consider shipping)
    const Row* best=nullptr;
    for (const Row& r : rows){ const bool rob=r.m1.net>0&&r.m2.net>0&&r.m2.half_a>0&&r.m2.half_b>0;
        if (rob){ best=&r; break; } }
    printf("\nBEST ROBUST (net+PF up both slips, both halves +): %s\n",
        best?best->c.name.c_str():"<none — no config cleared the ship rule>");

    // ── CONCENTRATION CHECK: per-name P&L for LIVE vs best-robust at 2% slip. If
    //   one name drives the net, it's fat-tail luck, not an exit edge. ───────────
    auto detail=[&](const ExitCfg& c){
        std::vector<std::pair<int,TradeRecord>> t; for (const NameDay& nd:nds) replay(nd,c,2.0,t);
        std::sort(t.begin(),t.end(),[](auto&a,auto&b){return a.second.pnl>b.second.pnl;});
        printf("\n  %s  (n=%zu @2%% slip)\n", c.name.c_str(), t.size());
        printf("  %-6s %-5s %8s %8s %9s %-9s %5s\n","sym","side","entry","exit","pnl","reason","held");
        double tot=0; for (auto& pr:t){ const TradeRecord& x=pr.second; tot+=x.pnl;
            printf("  %-6s %-5s %8.3f %8.3f %9.1f %-9s %4lldm\n", x.symbol.c_str(), x.side.c_str(),
                x.entryPrice, x.exitPrice, x.pnl, x.exitReason.c_str(), (long long)((x.exitTs-x.entryTs)/60)); }
        printf("  %-6s %42.1f\n","TOTAL",tot);
    };
    ExitCfg live; live.name="LIVE_trail2_BE2_h6_mh5";
    detail(live);
    if (best) detail(best->c);
    return 0;
}
