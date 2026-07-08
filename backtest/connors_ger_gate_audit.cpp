// connors_ger_gate_audit.cpp — DRIVE THE REAL ConnorsRSI2Engine on GER40 daily.
// Tombstone valid-use follow-up (S-2026-07-08c): g_connors_ger was culled 2026-06-24 on the
// fleet-audit PROXY figure (freq_dd_frontier.py next-open, "bear n=4"). Per the ConnorsNas
// RETRACTION precedent (proxy said asym-veto fails; REAL engine said PF4.17, 2022 POSITIVE),
// nobody has driven the real class on GER40 with REGIME_GATE=1. This does exactly that:
// exact live config (engine_init.hpp g_connors_ger: RSI2<10 dip-buy, close>SMA200 trend,
// enhanced close>SMA5 exit, MAXHOLD 10, no scale-in, CET session), REGIME_GATE 0 vs 1,
// cost-included, bull(24-26)/bear(2022) + WF-half split.
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include backtest/connors_ger_gate_audit.cpp -o /tmp/connors_ger_audit
// run:   /tmp/connors_ger_audit /Users/jo/Tick/GER40_daily_2016_2026.csv <cost_pts>
#include "ConnorsRSI2Engine.hpp"
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <ctime>
#include <cstdlib>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const std::string& p){
    std::vector<Bar> v; std::ifstream f(p); std::string ln;
    long long ts; double o,h,l,c;
    while(std::getline(f,ln)){
        if(std::sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)==5 && c>0)
            v.push_back({ts,o,h,l,c});
    }
    return v;
}

static int year_of(int64_t sec){ std::time_t t=(std::time_t)sec; std::tm g{}; gmtime_r(&t,&g); return g.tm_year+1900; }

struct Stat { int n=0,w=0; double gp=0,gl=0,net=0,peak=0,dd=0,eq=0; };
static void rec(Stat&s,double pnl){ s.n++; if(pnl>0){s.w++;s.gp+=pnl;}else s.gl+=-pnl; s.net+=pnl; s.eq+=pnl; if(s.eq>s.peak)s.peak=s.eq; double d=s.peak-s.eq; if(d>s.dd)s.dd=d; }
static double pf(const Stat&s){ return s.gl>0? s.gp/s.gl : (s.gp>0?999.0:0.0); }

struct Closed { int64_t entry_ts; double pnl_pts; };

static std::vector<Closed> run(const std::vector<Bar>& bars, int regime_gate, double cost_pts){
    omega::ConnorsRSI2Engine e;
    // EXACT live GER config (engine_init.hpp:5147-5166). SCALEIN stays engine-default false
    // ("GER scale-in was neutral in the sweep; base enhanced is the win").
    e.symbol="GER40"; e.engine_name="ConnorsRSI2_GER";
    e.TREND_SMA=200; e.RSI_IN=10.0; e.SHORT_SMA=5; e.MAXHOLD=10;
    e.SESS_OPEN_HM=900; e.SESS_CLOSE_HM=1730; e.TZ_STD_OFF_MIN=60; e.TZ_EU_DST=true;
    e.lot=1.0; e.enabled=true; e.shadow_mode=true;
    e.REGIME_GATE=regime_gate; e.BEAR_VETO_K=20;
    e.init();
    std::vector<Closed> out;
    e.on_trade_record=[&](const omega::TradeRecord& tr){
        double pnl = tr.pnl - cost_pts*tr.size;
        out.push_back({tr.entryTs, pnl});
    };
    // Drive one in-session tick + one after-close tick per daily bar.
    // 12:00 UTC = 13:00 CET / 14:00 CEST -> inside 09:00-17:30 local, both DST states.
    // 19:00 UTC = 20:00 CET / 21:00 CEST -> after 17:30 local close, both DST states.
    for(const auto& b: bars){
        std::time_t t=(std::time_t)b.ts; std::tm g{}; gmtime_r(&t,&g);
        g.tm_hour=12; g.tm_min=0; g.tm_sec=0;
        int64_t in_utc = (int64_t)timegm(&g);
        std::tm g2=g; g2.tm_hour=19;
        int64_t out_utc=(int64_t)timegm(&g2);
        double px=b.c;
        e.on_tick(px,px,in_utc*1000);
        e.on_tick(px,px,out_utc*1000);
    }
    e.force_close(bars.back().c, bars.back().c, bars.back().ts*1000);
    return out;
}

static void report(const char* tag,const std::vector<Closed>& tr){
    Stat bull,bear,all,h1,h2;
    int total=(int)tr.size();
    for(int i=0;i<total;i++){
        int y=year_of(tr[i].entry_ts); double p=tr[i].pnl_pts;
        rec(all,p);
        if(y==2022) rec(bear,p);
        if(y>=2024) rec(bull,p);
        if(i<total/2) rec(h1,p); else rec(h2,p);
    }
    bool both_halves = h1.net>0 && h2.net>0;
    printf("\n===== %s =====\n",tag);
    printf("ALL : n=%d WR=%.1f%% PF=%.2f net=%.1fpt maxDD=%.1f\n",
        all.n, all.n? 100.0*all.w/all.n:0, pf(all), all.net, all.dd);
    printf("BULL(24-26): n=%d WR=%.1f%% PF=%.2f net=%.1fpt maxDD=%.1f\n",
        bull.n, bull.n?100.0*bull.w/bull.n:0, pf(bull), bull.net, bull.dd);
    printf("BEAR(2022) : n=%d WR=%.1f%% PF=%.2f net=%.1fpt maxDD=%.1f\n",
        bear.n, bear.n?100.0*bear.w/bear.n:0, pf(bear), bear.net, bear.dd);
    printf("HALVES: H1 net=%.1f (PF%.2f) | H2 net=%.1f (PF%.2f) -> both+=%s\n",
        h1.net,pf(h1),h2.net,pf(h2), both_halves?"YES":"NO");
}

int main(int argc,char**argv){
    std::string path = argc>1? argv[1] : "/Users/jo/Tick/GER40_daily_2016_2026.csv";
    double cost = argc>2? atof(argv[2]) : 3.0;   // ~1.5pt spread each way on GER40 CFD
    auto bars=load(path);
    printf("Loaded %zu GER40 daily bars, cost=%.1fpt round-trip\n",bars.size(),cost);
    auto g0=run(bars,0,cost);
    auto g1=run(bars,1,cost);
    report("REGIME_GATE=0  (close>SMA200, deployed-as-culled basis)", g0);
    report("REGIME_GATE=1  (asym sustained-bear veto, ConnorsNas-winning gate)", g1);
    return 0;
}
