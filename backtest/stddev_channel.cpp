// stddev_channel.cpp — regression-channel core + RIGOROUS test of the
// Pearson-R-as-trend-quality / position-sizing idea on the gold trend book.
//
// Decisive question: does the entry-bar regression Pearson-R (trend cleanliness)
// PREDICT Donchian-breakout trade quality? If high-R entries have materially
// better expectancy, R has sizing value — independent of any sizing formula.
//
// Method (cost-incl, walk-forward, MULTI-SYMBOL to dodge gold-bull overfit):
//   1. collect every Donchian-breakout long trade + its entry R & slope
//   2. bucket trades by entry-R -> per-quartile expectancy (the real test)
//   3. sizing schemes vs flat: net, net/avg-size (return per unit exposure),
//      rDD, per-trade Sharpe; both walk-forward halves
//
// build: g++ -std=c++17 -O2 backtest/stddev_channel.cpp -o backtest/stddev_channel

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Bar{long long ts;double o,h,l,c;};
static std::vector<Bar> load(const std::string&p,bool ms){std::vector<Bar>v;std::ifstream f(p);if(!f)return v;
    std::string ln;bool fst=true;while(std::getline(f,ln)){if(fst){fst=false;if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9')&&ln[0]!='-')continue;}
        std::stringstream s(ln);std::string t;std::vector<std::string>k;while(std::getline(s,t,','))k.push_back(t);if(k.size()<5)continue;
        Bar b;long long ts=std::atoll(k[0].c_str());b.ts=ms?ts/1000:ts;b.o=std::atof(k[1].c_str());b.h=std::atof(k[2].c_str());b.l=std::atof(k[3].c_str());b.c=std::atof(k[4].c_str());if(b.h>0)v.push_back(b);}return v;}

struct Reg{double slope,center,sd,R;};
static Reg regress(const std::vector<Bar>&b,int n,int W){
    double Sx=0,Sy=0,Sxx=0,Sxy=0,Syy=0; for(int k=0;k<W;++k){double x=k,y=b[n-W+1+k].c;Sx+=x;Sy+=y;Sxx+=x*x;Sxy+=x*y;Syy+=y*y;}
    double den=W*Sxx-Sx*Sx,slope=den!=0?(W*Sxy-Sx*Sy)/den:0,icpt=(Sy-slope*Sx)/W,center=icpt+slope*(W-1);
    double ss=0; for(int k=0;k<W;++k){double y=b[n-W+1+k].c,fit=icpt+slope*k;ss+=(y-fit)*(y-fit);} double sd=std::sqrt(ss/(W>1?W-1:1));
    double cxx=Sxx-Sx*Sx/W,cyy=Syy-Sy*Sy/W,cxy=Sxy-Sx*Sy/W,R=(cxx>0&&cyy>0)?cxy/std::sqrt(cxx*cyy):0;
    return {slope,center,sd,R};
}
struct Trade{double pnl,R,slope;int exitIdx;};

// All Donchian-breakout long trades (no gate) + entry R/slope, in time order.
static std::vector<Trade> collect(const std::vector<Bar>&b,int W,int Nin,int Nout,double cost){
    std::vector<Trade> out; int N=(int)b.size(); int pos=0; double entry=0,eR=0,eS=0;
    int st=std::max(W,std::max(Nin,Nout))+1;
    for(int n=st;n<N;++n){
        if(pos==0){double hh=-1e18;for(int j=n-Nin;j<n;++j)hh=std::max(hh,b[j].h);
            if(b[n].c>hh){Reg g=regress(b,n,W);pos=1;entry=b[n].c;eR=g.R;eS=g.slope;}}
        else{double ll=1e18;for(int j=n-Nout;j<n;++j)ll=std::min(ll,b[j].l);
            if(b[n].c<ll){out.push_back({(b[n].c-entry)-cost,eR,eS,n});pos=0;}}}
    return out;
}

static void quartiles(const std::vector<Trade>&T){
    if(T.size()<8){std::printf("  (too few trades for buckets)\n");return;}
    std::vector<Trade> s=T; std::sort(s.begin(),s.end(),[](const Trade&a,const Trade&b){return a.R<b.R;});
    int q=(int)s.size()/4;
    std::printf("  entry-R quartile expectancy (low R -> high R):\n");
    for(int k=0;k<4;++k){int lo=k*q,hi=(k==3)?(int)s.size():(k+1)*q; double sum=0,gw=0,gl=0;int w=0;double Rlo=s[lo].R,Rhi=s[hi-1].R;
        for(int i=lo;i<hi;++i){sum+=s[i].pnl; if(s[i].pnl>=0){w++;gw+=s[i].pnl;}else gl+=-s[i].pnl;}
        int nn=hi-lo; std::printf("    Q%d R[%.2f..%.2f] n=%3d avgPnL=%7.2f WR=%4.1f%% PF=%.2f net=%8.1f\n",
            k+1,Rlo,Rhi,nn,sum/nn,100.0*w/nn,gl>0?gw/gl:0,sum);}
}

// sizing schemes: size as a function of entry R/slope
static double sz(int scheme,double R,double slope){
    switch(scheme){
        case 0: return 1.0;                                   // flat
        case 1: return std::max(0.0,std::min(1.5,(R-0.4)/0.4));// linear in R (0 at .4, 1.5 at 1.0)
        case 2: return R>0.85?1.5:R>0.7?1.0:R>0.5?0.5:0.25;   // step
        case 3: return std::max(0.0,R)*std::max(0.0,R);       // R^2
        case 4: return slope>0?1.0:0.4;                       // slope-only confidence
    }
    return 1.0;
}
static const char* sname(int s){const char*n[]={"flat","linear-R","step-R","R^2","slope-only"};return n[s];}

static void sizing(const std::vector<Trade>&T,int scheme){
    int N=(int)T.size(); double cum=0,peak=0,dd=0,sumsz=0,snet=0; double m=0; std::vector<double> pl;
    int mid=N/2; double net1=0,net2=0;
    for(int i=0;i<N;++i){double s=sz(scheme,T[i].R,T[i].slope); double v=s*T[i].pnl; sumsz+=s; snet+=v; pl.push_back(v);
        cum+=v; if(cum>peak)peak=cum; if(peak-cum>dd)dd=peak-cum; if(i<mid)net1+=v; else net2+=v; m+=v;}
    double avgsz=sumsz/N, mean=m/N, var=0; for(double v:pl)var+=(v-mean)*(v-mean); double sd=std::sqrt(var/N);
    std::printf("  %-11s | avgSize=%.2f net=%8.1f net/avgSize=%8.1f rDD=%5.2f Sharpe/t=%5.3f | H1=%7.1f H2=%7.1f %s\n",
        sname(scheme),avgsz,snet,snet/avgsz,dd>0?snet/dd:0,sd>0?mean/sd:0,net1,net2,(net1>0&&net2>0)?"BOTH+":"");
}

int main(int argc,char**argv){
    struct S{const char*path;int W;int Nin,Nout;double cost;const char*tag;};
    std::vector<S> syms = {
        {"phase1/signal_discovery/warmup_XAUUSD_H1.csv",128,20,10,0.45,"XAU H1"},
        {"phase1/signal_discovery/warmup_XAUUSD_H4.csv",128,20,10,0.45,"XAU H4"},
        {"phase1/signal_discovery/warmup_GER40_H1.csv", 128,20,10,1.50,"GER40 H1"},
        {"phase1/signal_discovery/warmup_NAS100_H1.csv",128,20,10,1.50,"NAS100 H1"},
        {"phase1/signal_discovery/warmup_US500_H1.csv", 128,20,10,0.50,"SP500 H1"},
    };
    for(auto&S:syms){
        bool ms = std::string(S.path).find("_H1")==std::string::npos && std::string(S.path).find("_D1")==std::string::npos;
        auto b=load(S.path,ms); int N=(int)b.size();
        std::printf("\n===== %s : %d bars (%.1f..%.1f) =====\n",S.tag,N,N?b.front().c:0,N?b.back().c:0);
        if(N<S.W+200){std::printf("  too few bars / missing\n");continue;}
        auto T=collect(b,S.W,S.Nin,S.Nout,S.cost);
        std::printf("  Donchian trades=%zu\n",T.size());
        quartiles(T);
        std::printf("  sizing schemes (net per unit exposure is the fair metric):\n");
        for(int s=0;s<5;++s) sizing(T,s);
    }
    std::printf("\nVERDICT GUIDE: R-sizing is real only if (a) Q4 expectancy >> Q1 monotonically\n");
    std::printf("AND (b) an R-scheme beats flat on net/avgSize + Sharpe/t consistently across symbols + BOTH+.\n");
    return 0;
}
