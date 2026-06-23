// index_vwap_reclaim_bt.cpp -- REAL-CLASS faithful BT of IndexVwapReclaimEngine.
// Drives the production engine on NAS 1m bars (aggregated to 5m), applies index
// intraday cost, reports PF / trades-per-week / both-halves. Confirms the 2026-06-23
// dig finding on the REAL engine (not the /tmp/idxhunt port).
// 1m bar CSV: emin,y,mo,d,hh,mm,o,h,l,c,spr,nt,vwap
// build: c++ -std=c++17 -O2 -Iinclude backtest/index_vwap_reclaim_bt.cpp -o /tmp/ivr
// run:   /tmp/ivr <nas_slice.1m.csv> [cost_pts] [label]
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include "IndexVwapReclaimEngine.hpp"

struct M1 { int64_t epoch; double o,h,l,c; };   // epoch=unix sec, 1m bar
int main(int argc,char**argv){
    if(argc<2){ std::fprintf(stderr,"usage: %s <1m.csv> [cost] [label]\n",argv[0]); return 1; }
    double COST = argc>2?atof(argv[2]):2.0;
    std::string label = argc>3?argv[3]:argv[1];
    std::vector<M1> m1; std::ifstream f(argv[1]); std::string ln; bool first=true;
    while(std::getline(f,ln)){
        if(first){first=false; if(ln.find("epoch")!=std::string::npos || ln.find("emin")!=std::string::npos) continue;}
        std::stringstream s(ln); std::string t; std::vector<std::string> k;
        while(std::getline(s,t,',')) k.push_back(t);
        if(k.size()<5) continue;   // epoch,o,h,l,c,...
        M1 b; b.epoch=atoll(k[0].c_str()); b.o=atof(k[1].c_str()); b.h=atof(k[2].c_str());
        b.l=atof(k[3].c_str()); b.c=atof(k[4].c_str()); m1.push_back(b);
    }
    if(m1.size()<300){ std::fprintf(stderr,"few bars %zu\n",m1.size()); return 1; }

    omega::IndexVwapReclaimEngine eng; eng.enabled=true; eng.shadow_mode=true; eng.lot=1.0; eng.init();
    int n=0,w=0; double net=0,gw=0,gl=0,cum=0,peak=0,dd=0,n1=0,n2=0;
    int mid=(int)m1.size()/2; int idx=0;
    auto cb=[&](const omega::TradeRecord& tr){
        double p = tr.pnl - COST;   // index pts, RT cost
        n++; net+=p; if(p>=0){w++;gw+=p;} else gl+=-p;
        cum+=p; if(cum>peak)peak=cum; if(peak-cum>dd)dd=peak-cum;
        (idx<mid?n1:n2)+=p;
    };
    // aggregate 1m -> 5m (group by emin/5), feed closed 5m bars. bull_gate=true
    // (these slices are pre-selected bull windows; the daily macro gate is the
    // deploy-time bear protection, not needed within a clean-bull slice).
    int64_t cur5=-1; double o5=0,h5=0,l5=0,c5=0;
    for(size_t i=0;i<m1.size();++i){ idx=(int)i;
        int64_t b5=(m1[i].epoch/300)*300;   // 5m bucket (sec)
        if(b5!=cur5){
            if(cur5>=0){ eng.on_5m_bar(h5,l5,c5,cur5*1000,true,cb); }
            cur5=b5; o5=m1[i].o; h5=m1[i].h; l5=m1[i].l; c5=m1[i].c;
        } else { if(m1[i].h>h5)h5=m1[i].h; if(m1[i].l<l5)l5=m1[i].l; c5=m1[i].c; }
    }
    if(cur5>=0) eng.on_5m_bar(h5,l5,c5,cur5*1000,true,cb);

    double pf=gl>0?gw/gl:(gw>0?9.9:0);
    double wks = (m1.back().epoch-m1.front().epoch)/(86400.0*7.0);
    std::printf("%-22s n=%-4d t/wk=%4.1f WR=%4.1f%% PF=%5.2f net=%+8.1f rDD=%5.2f | H1=%+7.1f H2=%+7.1f %s\n",
        label.c_str(), n, wks>0?n/wks:0, n?100.0*w/n:0, pf, net, dd>0?net/dd:0, n1, n2,
        (net>0&&n1>0&&n2>0)?"both-halves+ ✓":"FAIL");
    return 0;
}
