// bb_l2ab.cpp -- A/B the L2 profit-protect on the data we HAVE.
//
// The 2yr tick file is top-of-book only (no depth), so we cannot test the real
// order-book imbalance. Instead we drive the engine's L2 input with a PROXY
// derived from price action -- fast adverse velocity = "ask-heavy / hostile" --
// and compare three modes against the validated baseline:
//
//   A. baseline       USE_L2_PROTECT off (the PF 1.67 config)
//   B. giveback-only  protect on, imbalance forced hostile (0.0) -> tests the
//                     pure "lock when price gives back >=GIVEBACK_ATR after 1R"
//                     with no flow filter (upper bound on how often it fires)
//   C. velocity-proxy protect on, imbalance = 0.5 + clamp(vel/SCALE) over a
//                     ~30s window -> lock only when price is ALSO dropping fast
//
// This tells us whether the mechanism helps PF/DD or just shaves winners. Real
// L2 (data to come) replaces the proxy with the true imbalance; this is the
// sanity check that the LOCK LOGIC itself is sound.
//
// Build: g++ -O3 -std=c++17 -I../../include bb_l2ab.cpp -o bbl2ab
// Run:   ./bbl2ab <ticks.csv>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "BreakBounceEngine.hpp"

struct Tick { int64_t ts; double bid; double ask; };

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2; const int era=(y>=0?y:y-399)/400; const unsigned yoe=(unsigned)(y-era*400);
    const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1; const unsigned doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+(int)doe-719468;
}
static bool parse_line(const char* s, int64_t& ts_ms, double& bid, double& ask) {
    if (s[0]>='0'&&s[0]<='9') { char* end=nullptr; double f0=std::strtod(s,&end);
        if (end&&*end==',') { if (f0>=1e11) { char* e2=nullptr; bid=std::strtod(end+1,&e2);
            if(!e2||*e2!=',')return false; ask=std::strtod(e2+1,nullptr); ts_ms=(int64_t)f0;
            if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; } } else return false; }
    if (std::strlen(s)<19) return false;
    int y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0'); int mo=(s[4]-'0')*10+(s[5]-'0');
    int da=(s[6]-'0')*10+(s[7]-'0'); if(s[8]!=',')return false;
    int hh=(s[9]-'0')*10+(s[10]-'0'); int mi=(s[12]-'0')*10+(s[13]-'0'); int se=(s[15]-'0')*10+(s[16]-'0');
    if(y<1971||mo<1||mo>12||da<1||da>31)return false; char* e=nullptr; bid=std::strtod(s+18,&e);
    if(!e||*e!=',')return false; ask=std::strtod(e+1,nullptr);
    ts_ms=(days_from_civil(y,(unsigned)mo,(unsigned)da)*86400+hh*3600+mi*60+se)*1000LL;
    if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0;
}

struct Stat { int n=0,w=0; double net=0,gw=0,gl=0,mdd=0,peak=0,eq=0; void add(double p){
    n++; net+=p; eq+=p; if(p>0){w++;gw+=p;}else gl+=-p; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{return gl>0?gw/gl:(gw>0?999:0);} double wr()const{return n?100.0*w/n:0;} };

enum Mode { BASELINE, GIVEBACK, PROXY };

static Stat run(const std::vector<Tick>& ticks, Mode mode) {
    omega::BreakBounceEngine e;
    e.shadow_mode = true;
    e.USE_L2_PROTECT = (mode != BASELINE);
    e.init();
    Stat st;
    e.on_trade_record = [&](const omega::TradeRecord& tr){ st.add(tr.pnl); };

    // 30s-lagged mid for the velocity proxy. ~$1.5 maps to a full hostile read.
    const double SCALE = 1.5;
    std::deque<std::pair<int64_t,double>> win;   // (ts, mid)
    for (const auto& t : ticks) {
        const double mid = (t.bid + t.ask) * 0.5;
        if (mode == GIVEBACK) {
            e.set_l2_imbalance(0.0);                 // always "hostile" -> pure giveback lock
        } else if (mode == PROXY) {
            win.emplace_back(t.ts, mid);
            while (!win.empty() && t.ts - win.front().first > 30000) win.pop_front();
            const double vel = mid - win.front().second;        // + rising, - falling
            double imb = 0.5 + std::max(-0.5, std::min(0.5, vel / SCALE));
            e.set_l2_imbalance(imb);
        }
        e.on_tick(t.bid, t.ask, t.ts);
    }
    return st;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: bbl2ab <ticks.csv>\n"); return 1; }
    std::printf("loading ticks...\n");
    std::vector<Tick> ticks; ticks.reserve(160000000);
    { std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
      std::string line; while(std::getline(in,line)){ if(line.empty())continue; Tick t;
        if(parse_line(line.c_str(),t.ts,t.bid,t.ask)) ticks.push_back(t); } }
    std::printf("ticks=%zu\n\n", ticks.size());

    const char* names[3] = {"A baseline (off)","B giveback-only","C velocity-proxy"};
    Mode modes[3] = {BASELINE, GIVEBACK, PROXY};
    std::printf("%-20s  %6s %6s %5s %8s %7s\n","mode","trades","WR%","PF","net","maxDD");
    std::printf("%s\n", std::string(60,'-').c_str());
    for (int i=0;i<3;i++){
        Stat s = run(ticks, modes[i]);
        std::printf("%-20s  %6d %6.1f %5.2f %8.1f %7.1f\n",
            names[i], s.n, s.wr(), s.pf(), s.net, s.mdd);
    }
    return 0;
}
