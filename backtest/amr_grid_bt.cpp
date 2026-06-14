// amr_grid_bt.cpp -- independent cost-inclusive walk-forward harness for
// AtrMeanRevGridEngine. Drives the PRODUCTION Traits via on_tick() with
// synthetic bid/ask ticks reconstructed from aggregated bars, so the engine
// pays the spread natively (entry@ask / exit@bid) and runs its real intrabar
// SL/TP. Reports n / net / WR / PF + walk-forward (both halves) per run.
//
// Build:
//   g++ -std=c++20 -O2 -I../include amr_grid_bt.cpp -o /tmp/amrbt/amr_grid_bt
// Run:
//   amr_grid_bt <SYMKEY> <barfile> <spread_price>
//   SYMKEY in {GBPUSD,EURUSD,EURGBP,US500,NAS100,GER40}
#include "AtrMeanRevGridEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace omega;

struct Closed { double pnl; long long exitTs; };

template<class Traits>
static int run(const char* barfile, double spread, const char* label) {
    AtrMeanRevGridEngine<Traits> eng;
    eng.enabled = true;
    eng.shadow_mode = true;
    std::vector<Closed> trades;
    eng.on_close_cb = [&](const TradeRecord& tr){ trades.push_back({tr.pnl, (long long)tr.exitTs}); };

    std::ifstream f(barfile);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", barfile); return 1; }
    std::string line;
    const double hs = spread * 0.5;
    long long bars = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0]=='t' || line[0]=='#') continue;
        long long ts=0; double o=0,h=0,l=0,c=0;
        char* p = const_cast<char*>(line.c_str());
        ts = std::strtoll(p,&p,10); if(*p==',')++p;
        o  = std::strtod(p,&p);     if(*p==',')++p;
        h  = std::strtod(p,&p);     if(*p==',')++p;
        l  = std::strtod(p,&p);     if(*p==',')++p;
        c  = std::strtod(p,&p);
        // 4 synthetic ticks within the bar: O, L, H, C (low before high =
        // pessimistic SL for the mean-rev LONGs this engine mostly takes).
        const long long base = ts * 1000;
        const long long q = (Traits::BAR_INTERVAL_MS) / 4;
        const double mids[4] = {o,l,h,c};
        for (int i=0;i<4;++i) {
            const double m = mids[i];
            eng.on_tick(m - hs, m + hs, base + q*i);
        }
        ++bars;
    }
    if (trades.empty()) {
        std::printf("%-22s spread=%.5f bars=%lld  n=0  (no closed trades)\n", label, spread, bars);
        return 0;
    }
    // overall
    std::sort(trades.begin(), trades.end(), [](const Closed&a,const Closed&b){return a.exitTs<b.exitTs;});
    auto stats = [](const std::vector<Closed>& v, double& net, double& pf, double& wr, int& n){
        n=(int)v.size(); net=0; double gw=0,gl=0; int w=0;
        for (auto&t:v){ net+=t.pnl; if(t.pnl>0){gw+=t.pnl;++w;} else gl+=-t.pnl; }
        pf = (gl>1e-12)? gw/gl : (gw>0?999.0:0.0);
        wr = n? 100.0*w/n : 0.0;
    };
    double net,pf,wr; int n; stats(trades,net,pf,wr,n);
    // walk-forward halves (by trade order/time)
    std::vector<Closed> h1(trades.begin(), trades.begin()+n/2);
    std::vector<Closed> h2(trades.begin()+n/2, trades.end());
    double n1,p1,w1; int c1; stats(h1,n1,p1,w1,c1);
    double n2,p2,w2; int c2; stats(h2,n2,p2,w2,c2);
    std::printf("%-22s spread=%.5f bars=%lld  n=%d  net=%.5f  WR=%.0f%%  PF=%.2f  | H1 PF=%.2f(n=%d) H2 PF=%.2f(n=%d)\n",
                label, spread, bars, n, net, wr, pf, p1, c1, p2, c2);
    return 0;
}

int main(int argc, char** argv){
    if (argc < 4) { std::fprintf(stderr,"usage: %s SYMKEY barfile spread\n", argv[0]); return 2; }
    std::string k = argv[1];
    const char* bf = argv[2];
    double spr = std::atof(argv[3]);
    std::string label = k + "@" + std::to_string(spr);
    if      (k=="GBPUSD") return run<AmrTraits_GBPUSD>(bf,spr,label.c_str());
    else if (k=="EURUSD") return run<AmrTraits_EURUSD>(bf,spr,label.c_str());
    else if (k=="EURGBP") return run<AmrTraits_EURGBP>(bf,spr,label.c_str());
    else if (k=="US500")  return run<AmrTraits_US500>(bf,spr,label.c_str());
    else if (k=="NAS100") return run<AmrTraits_NAS100>(bf,spr,label.c_str());
    else if (k=="GER40")  return run<AmrTraits_GER40>(bf,spr,label.c_str());
    std::fprintf(stderr,"unknown SYMKEY %s\n", k.c_str()); return 2;
}
