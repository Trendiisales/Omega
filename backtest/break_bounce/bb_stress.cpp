// bb_stress.cpp -- bear + chop stress tests + profit-lock A/B for BreakBounce.
//
// The 2yr XAU file is all-bull, so we MANUFACTURE the missing regimes:
//
//   MIRROR-BEAR: feed each tick mirrored about a midpoint M (bid'=2M-ask,
//   ask'=2M-bid). Identical spread/vol microstructure, but a DOWNtrend -- so
//   the D1 bias flips to -1 and the engine must trade the SHORT side (which
//   never fired in the bull). A symmetric engine should post similar metrics.
//
//   CHOP via ADX buckets: every trade is tagged with ADX-at-entry (captured
//   via on_l2_sample); we bucket realized PnL by ADX to see how the engine
//   does in low-ADX (choppy) vs high-ADX (trending) conditions that exist
//   inside the bull.
//
//   PROFIT-LOCK A/B: a pure price give-back lock (the L2-protect mechanism with
//   imbalance forced hostile -> no order book needed) at several give-back
//   thresholds, on NORMAL and MIRROR, to find a profit protection that helps
//   bear/chop WITHOUT bleeding the trend edge.
//
// Build: g++ -O3 -std=c++17 -I../../include bb_stress.cpp -o bbstress
// Run:   ./bbstress <ticks.csv>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "BreakBounceEngine.hpp"

struct Tick { int64_t ts; double bid; double ask; };
static int64_t days_from_civil(int y, unsigned m, unsigned d){ y-=m<=2; const int era=(y>=0?y:y-399)/400;
    const unsigned yoe=(unsigned)(y-era*400); const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    const unsigned doe=yoe*365+yoe/4-yoe/100+doy; return era*146097+(int)doe-719468; }
static bool parse_line(const char* s,int64_t& ts,double& bid,double& ask){
    if(s[0]>='0'&&s[0]<='9'){ char* e=nullptr; double f0=std::strtod(s,&e);
        if(e&&*e==','){ if(f0>=1e11){ char* e2=nullptr; bid=std::strtod(e+1,&e2);
            if(!e2||*e2!=',')return false; ask=std::strtod(e2+1,nullptr); ts=(int64_t)f0;
            if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; } } else return false; }
    if(std::strlen(s)<19)return false;
    int y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0'); int mo=(s[4]-'0')*10+(s[5]-'0');
    int da=(s[6]-'0')*10+(s[7]-'0'); if(s[8]!=',')return false;
    int hh=(s[9]-'0')*10+(s[10]-'0'); int mi=(s[12]-'0')*10+(s[13]-'0'); int se=(s[15]-'0')*10+(s[16]-'0');
    if(y<1971||mo<1||mo>12||da<1||da>31)return false; char* e=nullptr; bid=std::strtod(s+18,&e);
    if(!e||*e!=',')return false; ask=std::strtod(e+1,nullptr);
    ts=(days_from_civil(y,(unsigned)mo,(unsigned)da)*86400+hh*3600+mi*60+se)*1000LL;
    if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; }

struct Trade { double pnl; double adx; int is_long; };

// Run the engine; collect (pnl, adx-at-entry, side). plock_give<=0 => protect off.
static std::vector<Trade> run(const std::vector<Tick>& ticks, bool mirror, double plock_give) {
    const double M = 3377.0;   // mirror midpoint (gold spanned ~2044..4710)
    omega::BreakBounceEngine e; e.shadow_mode=true;
    e.USE_L2_CAPTURE = true; e.L2_CAPTURE_SEC = 1;
    if (plock_give > 0) { e.USE_L2_PROTECT = true; e.L2_GIVEBACK_ATR = plock_give; e.L2_LOCK_ATR = 0.5; }
    e.init();

    std::map<int64_t,double> adx_at_entry;   // entry_ms -> first captured adx
    e.on_l2_sample = [&](int64_t em,int64_t,double,double,double,double,double,double,double,double,double adx,bool){
        adx_at_entry.emplace(em, adx); };
    std::vector<Trade> out;
    e.on_trade_record = [&](const omega::TradeRecord& tr){
        auto it = adx_at_entry.find((int64_t)tr.entryTs*1000);
        out.push_back({tr.pnl, it!=adx_at_entry.end()?it->second:0.0, tr.side=="LONG"?1:0}); };

    // Hostile-proxy for the give-back lock: LONG hostile = imb low (0.0);
    // SHORT (mirror) hostile = imb high (1.0). Forcing it makes the lock a pure
    // price give-back test (no order book).
    const double host = mirror ? 1.0 : 0.0;
    for (const auto& t : ticks) {
        if (plock_give > 0) e.set_l2_imbalance(host);
        if (mirror) e.on_tick(2*M - t.ask, 2*M - t.bid, t.ts);
        else        e.on_tick(t.bid, t.ask, t.ts);
    }
    return out;
}

struct Met { int n=0,L=0,S=0,w=0; double net=0,gw=0,gl=0,mdd=0,peak=0,eq=0,sum=0,sum2=0; void add(double p,int isl){
    n++; if(isl)L++;else S++; net+=p; eq+=p; sum+=p; sum2+=p*p; if(p>0){w++;gw+=p;}else gl+=-p;
    if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{return gl>0?gw/gl:(gw>0?99:0);} double wr()const{return n?100.0*w/n:0;}
    double sharpe(double tpy)const{ if(n<2)return 0; double m=sum/n,v=(sum2-sum*sum/n)/(n-1); return v>0?(m/std::sqrt(v))*std::sqrt(tpy):0; } };

static Met agg(const std::vector<Trade>& t){ Met m; for(auto&x:t)m.add(x.pnl,x.is_long); return m; }

int main(int argc,char** argv){
    if(argc<2){ std::printf("usage: bbstress <ticks.csv>\n"); return 1; }
    std::printf("loading ticks...\n");
    std::vector<Tick> ticks; ticks.reserve(160000000);
    { std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
      std::string line; while(std::getline(in,line)){ if(line.empty())continue; Tick t;
        if(parse_line(line.c_str(),t.ts,t.bid,t.ask)) ticks.push_back(t); } }
    if(ticks.empty()){std::printf("no ticks\n");return 1;}
    const double tpy = ticks.size()? 92.0 : 0;  // ~92 trades/yr (engine cadence); for sharpe scale

    auto row=[&](const char* lbl,const Met& m){
        std::printf("%-22s | %5d | %3d/%-3d | %5.1f | %5.2f | %6.2f | %7.1f | %6.1f\n",
            lbl, m.n, m.L, m.S, m.wr(), m.pf(), m.sharpe(tpy), m.net, m.mdd); };
    auto hdr=[&](){ std::printf("\n%-22s | %5s | long/sh | %5s | %5s | %6s | %7s | %6s\n",
            "scenario","trd","WR%","PF","Sharpe","net","maxDD");
        std::printf("-----------------------+-------+---------+-------+-------+--------+---------+-------\n"); };

    // ---- 1) BEAR test: NORMAL vs MIRROR baseline ----
    std::printf("\n=== 1) BEAR TEST (mirror-bear vs bull baseline) ===");
    std::vector<Trade> bull = run(ticks,false,0.0);
    std::vector<Trade> bear = run(ticks,true ,0.0);
    hdr(); row("NORMAL (bull)",agg(bull)); row("MIRROR (bear)",agg(bear));

    // ---- 2) CHOP test: ADX buckets on the bull trades ----
    std::printf("\n=== 2) CHOP TEST (bull trades bucketed by ADX-at-entry) ===\n");
    struct B{const char* l;double lo,hi;}; B bk[]={{"ADX<18 (chop)  ",0,18},{"ADX 18-25 (mod)",18,25},{"ADX 25-35 (trend)",25,35},{"ADX>=35 (strong)",35,1e9}};
    std::printf("%-18s | %5s | %5s | %5s | %7s\n","bucket","trd","WR%","PF","net");
    std::printf("-------------------+-------+-------+-------+--------\n");
    for(auto& b:bk){ Met m; for(auto&x:bull) if(x.adx>=b.lo&&x.adx<b.hi) m.add(x.pnl,x.is_long);
        std::printf("%-18s | %5d | %5.1f | %5.2f | %7.1f\n", b.l, m.n, m.wr(), m.pf(), m.net); }

    // ---- 3) PROFIT-LOCK A/B (give-back lock, pure price) ----
    std::printf("\n=== 3) PROFIT-LOCK A/B (price give-back lock; arm>=1R) ===");
    hdr();
    row("BULL  off", agg(bull));
    for(double g : {0.3,0.5,0.8}){ char l[40]; snprintf(l,sizeof(l),"BULL  give=%.1fATR",g); row(l, agg(run(ticks,false,g))); }
    row("BEAR  off", agg(bear));
    for(double g : {0.3,0.5,0.8}){ char l[40]; snprintf(l,sizeof(l),"BEAR  give=%.1fATR",g); row(l, agg(run(ticks,true,g))); }
    return 0;
}
