// bb_session_engine.cpp -- drive ticks through the LIVE IndexSessionEngine to
// confirm the on_tick state machine reproduces the validated sim. Same code
// path that trades live.
//
// Build: g++ -O3 -std=c++17 -I../../include bb_session_engine.cpp -o bbse
// Run:   ./bbse <ticks.csv> <rth_open_h> <rth_close_h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "IndexSessionEngine.hpp"

static bool parse(const char* s,int64_t& ts,double& bid,double& ask){
    if(s[0]>='0'&&s[0]<='9'){ char* e=nullptr; double f0=std::strtod(s,&e);
        if(e&&*e==','){ if(f0>=1e11){ char* e2=nullptr; bid=std::strtod(e+1,&e2);
            if(!e2||*e2!=',')return false; ask=std::strtod(e2+1,nullptr); ts=(int64_t)f0;
            if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; } } }
    return false;
}
int main(int argc,char** argv){
    if(argc<4){ std::printf("usage: bbse <ticks.csv> <OH> <CH>\n"); return 1; }
    omega::IndexSessionEngine e; e.shadow_mode=true;
    e.RTH_OPEN_H=std::atoi(argv[2]); e.RTH_CLOSE_H=std::atoi(argv[3]); e.init();
    struct M{int n=0,w=0;double net=0,gw=0,gl=0,sum=0,sq=0; void add(double r){n++;net+=r;sum+=r;sq+=r*r;if(r>0){w++;gw+=r;}else gl+=-r;}
        double pf()const{return gl>0?gw/gl:0;} double wr()const{return n?100.0*w/n:0;}
        double sh()const{if(n<2)return 0;double m=sum/n,v=(sq-sum*sum/n)/(n-1);return v>0?(m/std::sqrt(v))*std::sqrt(252.0):0;}};
    M all,is,oos; double eq=0,peak=0,mdd=0;
    std::vector<int64_t> tss; std::vector<double> rets;
    e.on_trade_record=[&](const omega::TradeRecord& tr){
        double r=(tr.exitPrice-tr.entryPrice)/tr.entryPrice*100.0;
        tss.push_back(tr.entryTs); rets.push_back(r);
        eq+=r; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; };
    std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
    std::string line; std::getline(in,line); int64_t ts; double bid,ask;
    while(std::getline(in,line)){ if(parse(line.c_str(),ts,bid,ask)) e.on_tick(bid,ask,ts); }
    if(tss.empty()){std::printf("no trades\n");return 0;}
    int64_t t0=tss.front(),t1=tss.back(),sp=t0+(int64_t)((t1-t0)*0.6);
    for(size_t i=0;i<rets.size();i++){ all.add(rets[i]); (tss[i]<sp?is:oos).add(rets[i]); }
    std::printf("ENGINE all: trades=%d WR=%.1f%% PF=%.2f Sharpe=%+.2f cum=%+.1f%% DD=%.1f%% | IS PF=%.2f Sh=%+.2f | OOS PF=%.2f Sh=%+.2f\n",
        all.n,all.wr(),all.pf(),all.sh(),all.net,mdd, is.pf(),is.sh(), oos.pf(),oos.sh());
    return 0;
}
