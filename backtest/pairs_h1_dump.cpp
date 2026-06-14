// pairs_h1_dump.cpp -- drive the production EurGbpPairsEngine with pre-built H1
// bars (one interleaved tick stream), validated cfg, dump per-trade exitTs,net.
// BUILD via build_pairs_sweep-style flags. Inputs: eur_h1.csv gbp_h1.csv out_dump.txt
#include "OmegaTimeShim.hpp"
#include "../include/OmegaTradeLedger.hpp"
#include "../include/EurGbpPairsEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <utility>

struct Bar{ long long ts; double o,h,l,c; };
static std::vector<Bar> load(const char* p){
    std::vector<Bar> v; std::ifstream f(p); std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){ Bar b{}; if(sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&b.ts,&b.o,&b.h,&b.l,&b.c)==5) v.push_back(b);} return v;
}
int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s eur_h1 gbp_h1 out_dump\n",argv[0]);return 1;}
    auto eur=load(argv[1]); auto gbp=load(argv[2]);
    int W=getenv("PW")?atoi(getenv("PW")):120;
    double ZI=getenv("PZI")?atof(getenv("PZI")):1.5;
    double ZO=getenv("PZO")?atof(getenv("PZO")):0.5;
    int H=getenv("PH")?atoi(getenv("PH")):48;
    double cost=getenv("PC")?atof(getenv("PC")):0.00010;
    omega::EurGbpPairsEngine eng; eng.shadow_mode=true; eng.enabled=true;
    eng.p.z_window=W; eng.p.z_in=ZI; eng.p.z_out=ZO; eng.p.hold_timeout_h1=H;
    eng.p.max_spread_eur=1.0; eng.p.max_spread_gbp=1.0; eng.p.weekend_close_gate=false;
    std::vector<std::pair<long long,double>> dump; double pnl=0; int n=0,win=0;
    auto on_close=[&](const omega::TradeRecord& tr){ double cd=cost*100000.0*tr.size*2.0; double net=tr.pnl-cd; pnl+=net; ++n; if(net>0)++win; dump.push_back({(long long)tr.exitTs,net}); };
    // interleave by ts, feed close as a single tick per H1 bar with small synthetic spread
    const double es=0.00010, gs=0.00012;
    size_t ie=0,ig=0;
    while(ie<eur.size()||ig<gbp.size()){
        bool te; if(ie>=eur.size())te=false; else if(ig>=gbp.size())te=true; else te=(eur[ie].ts<=gbp[ig].ts);
        if(te){ auto&b=eur[ie++]; long long ms=b.ts*1000LL; eng.on_tick_eur(b.c-es/2,b.c+es/2,ms,on_close);}
        else  { auto&b=gbp[ig++]; long long ms=b.ts*1000LL; eng.on_tick_gbp(b.c-gs/2,b.c+gs/2,ms,on_close);}
    }
    FILE* pf=fopen(argv[3],"w"); if(pf){for(auto&x:dump)fprintf(pf,"%lld,%.6f\n",x.first,x.second);fclose(pf);}
    double gw=0,gl=0; for(auto&x:dump){if(x.second>0)gw+=x.second; else gl+=-x.second;}
    double profit_factor=gl>0?gw/gl:(gw>0?99:0);
    fprintf(stderr,"[H1DUMP] w=%d zi=%.1f zo=%.1f h=%d  n=%d pnl=%.2f WR=%.1f%% PF=%.2f -> %s\n",
            W,ZI,ZO,H,n,pnl,n?100.0*win/n:0,profit_factor,argv[3]);
    return 0;
}
