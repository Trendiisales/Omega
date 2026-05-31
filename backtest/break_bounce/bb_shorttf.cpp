// bb_shorttf.cpp -- can BreakBounce work on SHORTER timeframes?
//
// The native D1/M15/M5 scalp loses (PF 0.86) because fixed spread (~$0.48 on
// XAU) does NOT shrink with TF -- short-TF moves are too small to clear cost.
// Levers tested here:
//   * faster structure/bias TFs (so the whole stack is quicker),
//   * MIN_ATR_SPREAD_MULT -- skip entries whose retest-TF ATR is < K * spread
//     (the cost guard: only take short-TF setups big enough to pay for the
//     round-trip),
//   * profit-lock ON (default) -- short TF has more reversals to catch.
//
// Build: g++ -O3 -std=c++17 -I../../include bb_shorttf.cpp -o bbshort
// Run:   ./bbshort <ticks.csv>

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

struct Met{ int n=0,w=0; double net=0,gw=0,gl=0,mdd=0,peak=0,eq=0,sum=0,sum2=0;
    void add(double p){ n++; net+=p; eq+=p; sum+=p; sum2+=p*p; if(p>0){w++;gw+=p;}else gl+=-p;
        if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{return gl>0?gw/gl:(gw>0?99:0);} double wr()const{return n?100.0*w/n:0;}
    double sh(double tpy)const{ if(n<2)return 0; double m=sum/n,v=(sum2-sum*sum/n)/(n-1); return v>0?(m/std::sqrt(v))*std::sqrt(tpy):0;} };

struct Cfg{ const char* lbl; int64_t bias,brk,ret; double minedge; };

int main(int argc,char** argv){
    if(argc<2){ std::printf("usage: bbshort <ticks.csv>\n"); return 1; }
    std::printf("loading ticks...\n");
    std::vector<Tick> ticks; ticks.reserve(160000000);
    { std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
      std::string line; while(std::getline(in,line)){ if(line.empty())continue; Tick t;
        if(parse_line(line.c_str(),t.ts,t.bid,t.ask)) ticks.push_back(t); } }
    if(ticks.empty()){std::printf("no ticks\n");return 1;}
    const double span_yr = (ticks.back().ts-ticks.front().ts)/86400000.0/365.25;

    std::vector<Cfg> grid;
    struct TF{const char* l; int64_t b,k,r;} tfs[] = {
        {"D1/H1/M5", 86400,3600,300}, {"D1/M30/M5",86400,1800,300}, {"D1/M15/M5",86400,900,300},
        {"H4/H1/M5", 14400,3600,300}, {"H4/M30/M5",14400,1800,300}, {"H4/M15/M5",14400,900,300},
        {"D1/M15/M1",86400,900,60},   {"H4/M15/M1",14400,900,60},
        {"D1/H1/M15(ref)",86400,3600,900}, {"D1/H1/M20(prod)",86400,3600,1200},
    };
    for(auto& t : tfs) for(double me : {0.0, 2.0, 3.0, 4.0})
        grid.push_back(Cfg{t.l,t.b,t.k,t.r,me});

    std::printf("\n%-16s | %5s | %5s | %4s | %5s | %6s | %7s | %6s\n",
        "TF(b/k/r)","edge","trd","WR%","PF","Sharpe","net","maxDD");
    std::printf("-----------------+-------+-------+------+-------+--------+---------+-------\n");
    for(auto& c : grid){
        omega::BreakBounceEngine e; e.shadow_mode=true;       // profit-lock ON by default
        e.BIAS_TF_SEC=c.bias; e.BREAK_TF_SEC=c.brk; e.RETEST_TF_SEC=c.ret;
        e.MIN_ATR_SPREAD_MULT=c.minedge; e.init();
        Met m; e.on_trade_record=[&](const omega::TradeRecord& tr){ m.add(tr.pnl); };
        for(const auto& t:ticks) e.on_tick(t.bid,t.ask,t.ts);
        const double tpy = span_yr>0 ? m.n/span_yr : 0;
        std::printf("%-16s | %5.1f | %5d | %4.1f | %5.2f | %6.2f | %7.1f | %6.1f\n",
            c.lbl, c.minedge, m.n, m.wr(), m.pf(), m.sh(tpy), m.net, m.mdd);
    }
    return 0;
}
