// MgcFastDonchianBacktest.cpp — engine-driven backtest: feeds the REAL
// MgcFastDonchian30mEngine the MGC 30m history and tallies. Confirms the engine
// reproduces the proxy result (PF~1.54 with HVN skip), cost-incl, walk-forward.
// build: g++ -std=c++17 -O2 -Iinclude backtest/MgcFastDonchianBacktest.cpp -o backtest/mgc_fastdon_bt
#include "MgcFastDonchian30mEngine.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Row{long long ts;double o,h,l,c,v;};
int main(int argc,char**argv){
    std::string path=argc>1?argv[1]:"data/mgc_30m_hist.csv";
    std::ifstream f(path); if(!f){std::printf("no %s\n",path.c_str());return 1;}
    std::vector<Row> rows; std::string ln; bool fst=true;
    while(std::getline(f,ln)){ if(fst){fst=false;continue;}
        std::stringstream s(ln);std::string t;std::vector<std::string>k;while(std::getline(s,t,','))k.push_back(t);
        if(k.size()<6)continue; Row r{std::atoll(k[0].c_str()),std::atof(k[1].c_str()),std::atof(k[2].c_str()),std::atof(k[3].c_str()),std::atof(k[4].c_str()),std::atof(k[5].c_str())};
        if(r.h>0)rows.push_back(r);}
    int N=(int)rows.size(); std::printf("[tape] MGC 30m bars=%d (engine-driven)\n",N); if(N<500)return 1;
    long long tmid=rows[N/2].ts;
    const double COST=0.4;

    auto run=[&](bool skip,int Nin){
        omega::MgcFastDonchian30mEngine e; e.enabled=true; e.shadow_mode=true;
        e.use_hvn_skip=skip; e.Nin=Nin; e.Nout=Nin/2; e.lot=1.0; // lot=1 -> pnl in points
        struct T{double pnl;long long ts;}; std::vector<T> tr;
        auto cb=[&](const omega::TradeRecord& t){ tr.push_back({t.pnl - COST, t.exitTs}); };
        for(auto& r:rows) e.on_30m_bar(r.o,r.h,r.l,r.c,r.v,r.ts,cb);
        int n=0,w=0;double net=0,gw=0,gl=0,cum=0,peak=0,dd=0,n1=0,n2=0;
        for(auto& t:tr){n++;net+=t.pnl;if(t.pnl>=0){w++;gw+=t.pnl;}else gl+=-t.pnl;cum+=t.pnl;if(cum>peak)peak=cum;if(peak-cum>dd)dd=peak-cum; if(t.ts<tmid)n1+=t.pnl;else n2+=t.pnl;}
        std::printf("  Nin=%2d skip=%d | n=%4d WR=%4.1f%% PF=%.2f net=%8.1f DD=%7.1f rDD=%5.2f | H1=%7.1f H2=%7.1f %s\n",
            Nin,skip,n,n?100.0*w/n:0,gl>0?gw/gl:0,net,dd,dd>0?net/dd:0,n1,n2,(n1>0&&n2>0)?"BOTH+":"");
    };
    std::printf("\n=== engine-driven (real MgcFastDonchian30mEngine), cost %.1fpt, WF ===\n",COST);
    run(false,20); run(true,20);
    run(false,40); run(true,40);
    return 0;
}
