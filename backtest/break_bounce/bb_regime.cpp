// bb_regime.cpp -- IS/OOS sweep of the ADX regime guard on the validated config.
//
// Tests whether gating breakouts on ADX(14) >= REGIME_ADX_MIN (BREAK_TF) helps
// or is subtractive (the ER chop-gate dead-end signature: IS up / OOS down,
// trades cut evenly, net lower). Same in-RAM-once / continuous-warm IS/OOS
// pattern as bb_sweep. Reports trade counts so we can see HOW MANY entries the
// gate removes -- a good chop filter should remove low-quality (losing) ones,
// not winners.
//
// Build: g++ -O3 -std=c++17 -I../../include bb_regime.cpp -o bbregime
// Run:   ./bbregime <ticks.csv>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
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

struct Stat{ int n=0,w=0; double net=0,gw=0,gl=0,mdd=0,peak=0,eq=0; void add(double p){
    n++; net+=p; eq+=p; if(p>0){w++;gw+=p;}else gl+=-p; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{return gl>0?gw/gl:(gw>0?999:0);} double wr()const{return n?100.0*w/n:0;} };

int main(int argc,char** argv){
    if(argc<2){ std::printf("usage: bbregime <ticks.csv>\n"); return 1; }
    std::printf("loading ticks...\n");
    std::vector<Tick> ticks; ticks.reserve(160000000);
    { std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
      std::string line; while(std::getline(in,line)){ if(line.empty())continue; Tick t;
        if(parse_line(line.c_str(),t.ts,t.bid,t.ask)) ticks.push_back(t); } }
    if(ticks.empty()){std::printf("no ticks\n");return 1;}
    const int64_t t0=ticks.front().ts,t1=ticks.back().ts;
    const int64_t split=t0+(int64_t)((t1-t0)*0.60);
    std::printf("ticks=%zu span=%.0f days  IS/OOS 60/40\n\n", ticks.size(),(t1-t0)/86400000.0);

    const double adx_grid[] = {0,12,15,18,20,22,25,28};
    std::printf("%-8s | %5s %4s %5s %6s %6s | %5s %4s %5s %6s %6s\n",
        "ADXmin","ISpf","ISwr","ISn","ISnet","ISdd","OOpf","OOwr","OOn","OOnet","OOdd");
    std::printf("%s\n", std::string(82,'-').c_str());
    for(double a : adx_grid){
        omega::BreakBounceEngine e; e.shadow_mode=true;
        e.BIAS_TF_SEC=86400; e.BREAK_TF_SEC=3600; e.RETEST_TF_SEC=1200;  // validated D1/H1/M20
        e.REGIME_ADX_MIN=a; e.init();
        Stat is,oos;
        e.on_trade_record=[&](const omega::TradeRecord& tr){
            if((int64_t)tr.entryTs*1000<split) is.add(tr.pnl); else oos.add(tr.pnl); };
        for(const auto& t:ticks) e.on_tick(t.bid,t.ask,t.ts);
        std::printf("%-8.0f | %5.2f %4.0f %5d %6.0f %6.1f | %5.2f %4.0f %5d %6.0f %6.1f\n",
            a, is.pf(),is.wr(),is.n,is.net,is.mdd, oos.pf(),oos.wr(),oos.n,oos.net,oos.mdd);
    }
    return 0;
}
