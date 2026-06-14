// amr_grid_bt2.cpp -- regime-aware (bull/bear) validation harness for
// AtrMeanRevGridEngine. Same synthetic-tick driver as amr_grid_bt.cpp, plus:
//   - BULL/BEAR split by EMA200 slope at each trade's entry bar
//   - max drawdown of the cumulative net equity curve
//   - grid depth reached (max legs closed in one cluster) + worst single close
// These are the blow-up signals for a dip-buying martingale grid.
#include "AtrMeanRevGridEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cmath>
using namespace omega;

struct Closed { double pnl; long long entryTs; long long exitTs; double lot; };

template<class Traits>
static int run(const char* barfile, double spread, const char* label) {
    AtrMeanRevGridEngine<Traits> eng;
    eng.enabled = true; eng.shadow_mode = true;
    std::vector<Closed> tr;
    eng.on_close_cb = [&](const TradeRecord& r){ tr.push_back({r.pnl,(long long)r.entryTs,(long long)r.exitTs,0.0}); };

    // Pass 1: read bars, build EMA200 slope regime map keyed by bar-start sec.
    const long long ivs = Traits::BAR_INTERVAL_MS/1000;
    std::unordered_map<long long,int> regime; // bar_ts_sec -> +1 bull / -1 bear
    {
        std::ifstream f(barfile);
        if(!f){ std::fprintf(stderr,"cannot open %s\n",barfile); return 1; }
        std::string ln; double ema=0, prev=0; bool init=false;
        const double a = 2.0/(200.0+1.0);
        while(std::getline(f,ln)){
            if(ln.empty()||ln[0]=='t'||ln[0]=='#') continue;
            char* p=const_cast<char*>(ln.c_str());
            long long ts=std::strtoll(p,&p,10); if(*p==',')++p;
            std::strtod(p,&p); if(*p==',')++p; // o
            std::strtod(p,&p); if(*p==',')++p; // h
            std::strtod(p,&p); if(*p==',')++p; // l
            double c=std::strtod(p,&p);
            if(!init){ ema=c; init=true; } else { prev=ema; ema=a*c+(1-a)*ema; }
            regime[ts] = (ema>=prev)?+1:-1;
        }
    }
    // Pass 2: drive engine via synthetic ticks.
    {
        std::ifstream f(barfile); std::string ln;
        const double hs=spread*0.5;
        while(std::getline(f,ln)){
            if(ln.empty()||ln[0]=='t'||ln[0]=='#') continue;
            char* p=const_cast<char*>(ln.c_str());
            long long ts=std::strtoll(p,&p,10); if(*p==',')++p;
            double o=std::strtod(p,&p); if(*p==',')++p;
            double h=std::strtod(p,&p); if(*p==',')++p;
            double l=std::strtod(p,&p); if(*p==',')++p;
            double c=std::strtod(p,&p);
            const long long base=ts*1000, q=Traits::BAR_INTERVAL_MS/4;
            const double mids[4]={o,l,h,c};
            for(int i=0;i<4;++i){ double m=mids[i]; eng.on_tick(m-hs,m+hs,base+q*i); }
        }
    }
    if(tr.empty()){ std::printf("%-26s n=0 (no trades)\n",label); return 0; }
    std::sort(tr.begin(),tr.end(),[](const Closed&a,const Closed&b){return a.exitTs<b.exitTs;});

    auto stat=[&](const std::vector<Closed>&v,double&net,double&pf,double&wr,int&n){
        n=(int)v.size(); net=0; double gw=0,gl=0; int w=0;
        for(auto&t:v){ net+=t.pnl; if(t.pnl>0){gw+=t.pnl;++w;} else gl+=-t.pnl; }
        pf=(gl>1e-12)?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0;
    };
    // overall + DD + worst + grid depth
    double net,pf,wr; int n; stat(tr,net,pf,wr,n);
    double cum=0,peak=0,maxdd=0,worst=0;
    std::unordered_map<long long,int> cluster; // exitTs -> legs
    for(auto&t:tr){ cum+=t.pnl; if(cum>peak)peak=cum; if(peak-cum>maxdd)maxdd=peak-cum;
                    if(t.pnl<worst)worst=t.pnl; cluster[t.exitTs]++; }
    int maxdepth=0,grid_closes=0; for(auto&kv:cluster){ if(kv.second>maxdepth)maxdepth=kv.second; if(kv.second>1)++grid_closes; }
    // bull/bear split by regime at entry
    std::vector<Closed> bull,bear;
    for(auto&t:tr){ long long bts=(t.entryTs/ivs)*ivs; auto it=regime.find(bts);
        int rg = (it!=regime.end())?it->second:+1; (rg>0?bull:bear).push_back(t); }
    double bn,bp,bw; int bnc; stat(bull,bn,bp,bw,bnc);
    double rn,rp,rw; int rnc; stat(bear,rn,rp,rw,rnc);
    std::printf("%-26s n=%d net=%.5f WR=%.0f%% PF=%.2f maxDD=%.5f worst=%.5f gridMax=%d gridCloses=%d\n",
                label,n,net,wr,pf,maxdd,worst,maxdepth,grid_closes);
    std::printf("    BULL(EMA up):   n=%-4d net=%+.5f WR=%.0f%% PF=%.2f\n", bnc,bn,bw,bp);
    std::printf("    BEAR(EMA down): n=%-4d net=%+.5f WR=%.0f%% PF=%.2f\n", rnc,rn,rw,rp);
    return 0;
}
int main(int argc,char**argv){
    if(argc<4){ std::fprintf(stderr,"usage: %s SYMKEY barfile spread\n",argv[0]); return 2; }
    std::string k=argv[1]; const char* bf=argv[2]; double s=std::atof(argv[3]);
    std::string lab=k+"@"+std::to_string(s);
    if(k=="GBPUSD") return run<AmrTraits_GBPUSD>(bf,s,lab.c_str());
    if(k=="EURUSD") return run<AmrTraits_EURUSD>(bf,s,lab.c_str());
    if(k=="EURGBP") return run<AmrTraits_EURGBP>(bf,s,lab.c_str());
    if(k=="US500")  return run<AmrTraits_US500>(bf,s,lab.c_str());
    if(k=="NAS100") return run<AmrTraits_NAS100>(bf,s,lab.c_str());
    std::fprintf(stderr,"unknown %s\n",k.c_str()); return 2;
}
