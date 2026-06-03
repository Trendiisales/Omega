// mgc_vp_backtest.cpp — does prior-day volume-profile (POC / overhead supply)
// improve a Donchian-long trend trade on gold? Self-contained on MGC 30m bars
// (real COMEX volume). cost-incl, walk-forward.
//
// Variants vs baseline Donchian-long:
//   A baseline          — Donchian breakout long, Donchian-channel exit
//   B overhead-skip     — skip entry if a prior-day HVN sits within K*ATR ABOVE
//                         entry (overhead supply = little room)
//   C poc-target        — take profit at nearest prior-day HVN/POC above entry
//   D both
// build: g++ -std=c++17 -O2 backtest/mgc_vp_backtest.cpp -o backtest/mgc_vp_backtest
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
struct Bar{long long ts;double o,h,l,c,v;int day;};
int main(int argc,char**argv){
    std::string path=argc>1?argv[1]:"data/mgc_30m_hist.csv";
    std::ifstream f(path); if(!f){std::printf("no %s\n",path.c_str());return 1;}
    std::vector<Bar> b; std::string ln; bool fst=true;
    while(std::getline(f,ln)){ if(fst){fst=false;continue;}
        std::stringstream s(ln);std::string t;std::vector<std::string>k;while(std::getline(s,t,','))k.push_back(t);
        if(k.size()<6)continue; Bar x; x.ts=std::atoll(k[0].c_str());
        x.o=std::atof(k[1].c_str());x.h=std::atof(k[2].c_str());x.l=std::atof(k[3].c_str());x.c=std::atof(k[4].c_str());x.v=std::atof(k[5].c_str());
        x.day=(int)(x.ts/86400); if(x.h>0)b.push_back(x); }
    int N=(int)b.size(); std::printf("[tape] MGC 30m bars=%d\n",N); if(N<500)return 1;
    // ATR14 (30m)
    std::vector<double> atr(N,0); double a=0; for(int i=1;i<N;++i){double tr=std::max(b[i].h-b[i].l,std::max(std::fabs(b[i].h-b[i-1].c),std::fabs(b[i].l-b[i-1].c))); a=(i<=14)?(a+tr):(a+(tr-a)/14.0); atr[i]=a;}
    // per-day volume profile -> POC + HVN price list (bins >= 0.6*max)
    struct Prof{double poc=0; std::vector<double> hvn; bool ok=false;};
    std::map<int,Prof> prof; std::map<int,std::vector<int>> byday;
    for(int i=0;i<N;++i) byday[b[i].day].push_back(i);
    const int BINS=30;
    for(auto&kv:byday){ auto&idx=kv.second; double hi=-1e18,lo=1e18,tv=0;
        for(int i:idx){hi=std::max(hi,b[i].h);lo=std::min(lo,b[i].l);tv+=b[i].v;}
        if(hi<=lo||tv<=0)continue; double bs=(hi-lo)/BINS; std::vector<double> vb(BINS,0);
        for(int i:idx){int bi=(int)((b[i].c-lo)/bs); bi=std::max(0,std::min(bi,BINS-1)); vb[bi]+=b[i].v;}
        double mx=0;int pi=0; for(int j=0;j<BINS;++j)if(vb[j]>mx){mx=vb[j];pi=j;}
        Prof p; p.poc=lo+bs*(pi+0.5); p.ok=true;
        for(int j=0;j<BINS;++j)if(vb[j]>=0.6*mx)p.hvn.push_back(lo+bs*(j+0.5));
        prof[kv.first]=p;
    }
    struct Res{int n=0,w=0;double net=0,gw=0,gl=0,dd=0;};
    auto run=[&](int Nin,int Nout,bool ovfilter,bool poctgt,double Kov,double cost,int lo,int hi,double&n1,double&n2,int mid){
        Res r;int pos=0;double entry=0,tp=0,cum=0,peak=0;
        auto ct=[&](double ex,int i){double p=(ex-entry)-cost;r.n++;if(p>=0){r.w++;r.gw+=p;}else r.gl+=-p;cum+=p;if(cum>peak)peak=cum;if(peak-cum>r.dd)r.dd=peak-cum; if(i<mid)n1+=p;else n2+=p;};
        for(int i=std::max(lo,std::max(Nin,Nout)+1);i<hi;++i){
            if(pos==0){double hh=-1e18;for(int j=i-Nin;j<i;++j)hh=std::max(hh,b[j].h);
                if(b[i].c>hh){
                    const Prof* pd = prof.count(b[i].day-1)?&prof.at(b[i].day-1):nullptr;
                    bool ok=true; tp=0;
                    if(pd&&pd->ok){
                        if(ovfilter){ for(double h:pd->hvn) if(h>b[i].c && h<=b[i].c+Kov*atr[i]){ok=false;break;} }
                        if(poctgt){ double best=1e18; for(double h:pd->hvn) if(h>b[i].c&&h<best)best=h; if(pd->poc>b[i].c&&pd->poc<best)best=pd->poc; if(best<1e17)tp=best; }
                    }
                    if(ok){pos=1;entry=b[i].c;}
                }
            } else {
                double ll=1e18;for(int j=i-Nout;j<i;++j)ll=std::min(ll,b[j].l);
                if(tp>0 && b[i].h>=tp){ct(tp,i);pos=0;}
                else if(b[i].c<ll){ct(b[i].c,i);pos=0;}
            }
        }
        if(pos)ct(b[hi-1].c,hi-1); r.net=cum;
        return r;
    };
    auto pf=[](const Res&r){return r.gl>0?r.gw/r.gl:0;};
    int mid=N/2; double cost=0.4;
    auto rep=[&](const char*tag,int Nin,int Nout,bool ov,bool pt,double K){
        double n1=0,n2=0,d1,d2; Res R=run(Nin,Nout,ov,pt,K,cost,0,N,n1,n2,mid);
        double a1=0,a2=0; run(Nin,Nout,ov,pt,K,cost,0,mid,a1,a2,mid); // n1 of first half captured in a1
        std::printf("  %-22s | n=%4d WR=%4.1f%% PF=%.2f net=%8.1f DD=%7.1f rDD=%5.2f | H1=%7.1f H2=%7.1f %s\n",
            tag,R.n,R.n?100.0*R.w/R.n:0,pf(R),R.net,R.dd,R.dd>0?R.net/R.dd:0,n1,n2,(n1>0&&n2>0)?"BOTH+":"");
    };
    std::printf("\n=== MGC Donchian-long + prior-day volume-profile confluence (cost %.1fpt, WF) ===\n",cost);
    for(int Nin : {20,40}){ int Nout=Nin/2; std::printf("Donchian Nin=%d Nout=%d:\n",Nin,Nout);
        rep("A baseline",        Nin,Nout,false,false,0);
        rep("B overhead-skip 1.5",Nin,Nout,true, false,1.5);
        rep("B overhead-skip 3.0",Nin,Nout,true, false,3.0);
        rep("C poc-target",      Nin,Nout,false,true, 0);
        rep("D both (skip1.5+tgt)",Nin,Nout,true, true, 1.5);
    }
    return 0;
}
