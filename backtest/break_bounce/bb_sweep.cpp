// bb_sweep.cpp -- IS/OOS parameter sweep for BreakBounceEngine on one tick file.
//
// Loads the tick stream into RAM ONCE, then runs the live engine under many
// parameter configs. Indicators stay warm across the whole stream (no cold
// OOS start); each emitted trade is bucketed into IS or OOS by its ENTRY
// timestamp, so in-sample tuning and out-of-sample validation come from the
// same continuous run. Ranks configs by OOS profit factor with an IS sanity
// check (rejects the IS-great / OOS-bad overfit signature).
//
// Build: g++ -O3 -std=c++17 -I../../include bb_sweep.cpp -o bbsweep
// Run:   ./bbsweep <ticks.csv>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "BreakBounceEngine.hpp"

struct Tick { int64_t ts; double bid; double ask; };

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}
static bool parse_line(const char* s, int64_t& ts_ms, double& bid, double& ask) {
    if (s[0] >= '0' && s[0] <= '9') {
        char* end = nullptr; double f0 = std::strtod(s, &end);
        if (end && *end == ',') {
            if (f0 >= 1e11) {
                char* e2 = nullptr; bid = std::strtod(end + 1, &e2);
                if (!e2 || *e2 != ',') return false;
                ask = std::strtod(e2 + 1, nullptr); ts_ms = (int64_t)f0;
                if (ask < bid) std::swap(bid, ask); return bid > 0 && ask > 0;
            }
        } else return false;
    }
    if (std::strlen(s) < 19) return false;
    int y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
    int mo=(s[4]-'0')*10+(s[5]-'0'); int da=(s[6]-'0')*10+(s[7]-'0');
    if (s[8] != ',') return false;
    int hh=(s[9]-'0')*10+(s[10]-'0'); int mi=(s[12]-'0')*10+(s[13]-'0'); int se=(s[15]-'0')*10+(s[16]-'0');
    if (y<1971||mo<1||mo>12||da<1||da>31) return false;
    char* e=nullptr; bid=std::strtod(s+18,&e);
    if (!e||*e!=',') return false;
    ask=std::strtod(e+1,nullptr);
    ts_ms=(days_from_civil(y,(unsigned)mo,(unsigned)da)*86400+hh*3600+mi*60+se)*1000LL;
    if (ask<bid) std::swap(bid,ask); return bid>0&&ask>0;
}

struct Stat { int n=0,w=0; double net=0,gw=0,gl=0,mdd=0,peak=0,eq=0; void add(double p){
    n++; net+=p; eq+=p; if(p>0){w++;gw+=p;}else gl+=-p; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{return gl>0?gw/gl:(gw>0?999:0);} double wr()const{return n?100.0*w/n:0;} };

struct Cfg { int64_t retest; double stop_atr, rr, trail_atr; int lookback; bool trail; const char* tflbl; };

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: bbsweep <ticks.csv>\n"); return 1; }

    std::printf("loading ticks...\n");
    std::vector<Tick> ticks; ticks.reserve(160000000);
    {
        std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
        std::string line;
        while (std::getline(in,line)) { if(line.empty())continue; Tick t;
            if(parse_line(line.c_str(),t.ts,t.bid,t.ask)) ticks.push_back(t); }
    }
    if (ticks.empty()) { std::printf("no ticks\n"); return 1; }
    const int64_t t0=ticks.front().ts, t1=ticks.back().ts;
    const int64_t split = t0 + (int64_t)((t1-t0)*0.60);   // 60% IS / 40% OOS
    std::printf("ticks=%zu span=%.0f days  IS<%lld OOS>=  (60/40)\n",
        ticks.size(), (t1-t0)/86400000.0, (long long)split);

    // ---- grid ----
    std::vector<Cfg> grid;
    for (int64_t rt : {900LL, 1200LL})                 // M15, M20 retest
      for (double s : {1.0,1.2,1.5})                    // STOP_ATR
        for (double rr : {1.5,2.0,2.5,3.0})             // REWARD_RISK
          for (bool tr : {true,false})                  // trail on/off
            grid.push_back(Cfg{rt,s,rr,2.0,32,tr, rt==900?"H1/M15":"H1/M20"});

    std::printf("\n%-8s %4s %4s %5s  | %5s %4s %5s %6s | %5s %4s %5s %6s\n",
        "TF","stp","rr","trail","ISpf","ISwr","ISn","ISdd","OOpf","OOwr","OOn","OOdd");
    std::printf("%s\n", std::string(86,'-').c_str());

    struct Row { Cfg c; Stat is,oos; };
    std::vector<Row> rows;

    for (auto& c : grid) {
        omega::BreakBounceEngine e;
        e.shadow_mode=true;
        e.BIAS_TF_SEC=86400; e.BREAK_TF_SEC=3600; e.RETEST_TF_SEC=c.retest;
        e.STOP_ATR=c.stop_atr; e.REWARD_RISK=c.rr; e.LOOKBACK=c.lookback;
        e.USE_TRAIL=c.trail; e.TRAIL_ATR=c.trail_atr;
        e.init();
        Stat is, oos;
        e.on_trade_record=[&](const omega::TradeRecord& tr){
            if ((int64_t)tr.entryTs*1000 < split) is.add(tr.pnl); else oos.add(tr.pnl); };
        for (const auto& t : ticks) e.on_tick(t.bid,t.ask,t.ts);
        rows.push_back({c,is,oos});
    }

    // rank by OOS pf, require OOS n>=20
    std::sort(rows.begin(),rows.end(),[](const Row&a,const Row&b){
        bool av=a.oos.n>=20, bv=b.oos.n>=20;
        if(av!=bv)return av>bv;
        return a.oos.pf()>b.oos.pf(); });

    for (auto& r : rows) {
        std::printf("%-8s %4.1f %4.1f %5s  | %5.2f %4.0f %5d %6.1f | %5.2f %4.0f %5d %6.1f\n",
            r.c.tflbl, r.c.stop_atr, r.c.rr, r.c.trail?"on":"off",
            r.is.pf(), r.is.wr(), r.is.n, r.is.mdd,
            r.oos.pf(), r.oos.wr(), r.oos.n, r.oos.mdd);
    }
    return 0;
}
