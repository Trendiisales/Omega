// gapshort_backtest.cpp — C++ port of Evan gap-short backtest (NO python).
// Reproduces the validated config. Reads gappers.csv + intraday.csv + float.csv.
// build: g++ -std=c++17 -O2 gapshort_backtest.cpp -o gapshort_bt
// run:   ./gapshort_bt gappers.csv intraday.csv float.csv [gapMin=75] [stop=0.50] [coverHr=13]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
using namespace std;

struct HBar{ int hr; double o,h,l,c; };

int main(int argc,char**argv){
    string gp=argv[1], ip=argv[2], fp=argv[3];
    bool useFloat = (fp!="none");   // pass "none" to ship the validated no-float config (PF 1.45)
    double GAP=argc>4?atof(argv[4]):75, STOP=argc>5?atof(argv[5]):1.00;   // improved: 100% stop
    int COVER=argc>6?atoi(argv[6]):99;                                    // 99 = cover at close
    double COST=argc>7?atof(argv[7]):0.01;
    double PXLO=3,PXHI=20,FLO=3e6,FHI=20e6,KELLY=0.12;

    // float (optional: pass "none" to ship the validated no-float config -> PF 1.45)
    unordered_map<string,double> flt;
    if(useFloat){ ifstream f(fp); string ln; getline(f,ln);
      while(getline(f,ln)){ auto p=ln.find(','); auto q=ln.find(',',p+1);
        if(p==string::npos)continue; string t=ln.substr(0,p); double v=atof(ln.substr(p+1,q-p-1).c_str()); if(v>0)flt[t]=v; } }
    // gappers -> meta keyed "ticker|date"
    struct M{ double g,o; };
    unordered_map<string,M> meta;
    { ifstream f(gp); string ln; getline(f,ln);
      while(getline(f,ln)){ stringstream ss(ln); string d,t,pc,o,h,l,c,vol,g; getline(ss,d,',');getline(ss,t,',');getline(ss,pc,',');getline(ss,o,',');getline(ss,h,',');getline(ss,l,',');getline(ss,c,',');getline(ss,vol,',');getline(ss,g,',');
        double gg=atof(g.c_str()); if(gg>0) meta[t+"|"+d]={gg,atof(o.c_str())}; } }
    // intraday -> key -> bars
    map<string,vector<HBar>> days;
    { ifstream f(ip); string ln; getline(f,ln);
      while(getline(f,ln)){ stringstream ss(ln); string t,d,hh,o,h,l,c,v; getline(ss,t,',');getline(ss,d,',');getline(ss,hh,',');getline(ss,o,',');getline(ss,h,',');getline(ss,l,',');getline(ss,c,',');
        int hr=atoi(hh.c_str()); days[t+"|"+d].push_back({hr,atof(o.c_str()),atof(h.c_str()),atof(l.c_str()),atof(c.c_str())}); } }

    vector<pair<string,double>> trades; unordered_map<string,double> tkNet; long c_meta=0,c_gp=0,c_flt=0;
    for(auto&kv:days){
        const string&key=kv.first; auto mi=meta.find(key); if(mi==meta.end())continue;
        string t=key.substr(0,key.find('|')), d=key.substr(key.find('|')+1);
        if(mi->second.g<GAP) continue; c_meta++;
        if(!(PXLO<=mi->second.o && mi->second.o<=PXHI)) continue; c_gp++;
        if(useFloat){ auto fi=flt.find(t); if(fi==flt.end()) continue; if(!(FLO<=fi->second && fi->second<FHI)) continue; } c_flt++;
        auto bars=kv.second; bars.erase(remove_if(bars.begin(),bars.end(),[](const HBar&b){return b.hr<9||b.hr>16;}),bars.end());
        if(bars.size()<3) continue; sort(bars.begin(),bars.end(),[](const HBar&a,const HBar&b){return a.hr<b.hr;});
        double o=bars[0].o, stop=o*(1+STOP), ex=-1;
        for(auto&b:bars){ if(b.h>=stop){ex=stop;break;} if(b.hr>=COVER){ex=b.c;break;} }
        if(ex<0) ex=bars.back().c;
        double ret=(o-ex)/o-COST; trades.push_back({d,ret}); tkNet[t]+=ret;
    }
    sort(trades.begin(),trades.end());
    int n=trades.size(),w=0; double gw=0,gl=0,sum=0,sum2=0,eq=50000,peak=50000,mdd=0;
    map<string,vector<double>> byyr;
    for(auto&[d,r]:trades){ if(r>0){w++;gw+=r;}else gl+=-r; sum+=r;sum2+=r*r;
        eq+= eq*KELLY*r/STOP; peak=max(peak,eq); mdd=max(mdd,(peak-eq)/peak*100); byyr[d.substr(0,4)].push_back(r); }
    double pf=gl>0?gw/gl:99, m=sum/n, sd=sqrt((sum2-n*m*m)/(n-1));
    int mid=n/2; double h1=0,h2=0; for(int i=0;i<mid;i++)h1+=trades[i].second; for(int i=mid;i<n;i++)h2+=trades[i].second;
    printf("=== EVAN GAP-SHORT C++ BACKTEST ===\n");
    printf("config: gap>=%.0f%% $%.0f-%.0f float %.0f-%.0fM short@open stop%.0f%% cover%d:00\n",GAP,PXLO,PXHI,FLO/1e6,FHI/1e6,STOP*100,COVER);
    printf("trades=%d win%%=%.1f PF=%.2f avgRet=%+.2f%% Sharpe/tr=%+.2f\n",n,100.0*w/n,pf,m*100,m/sd);
    printf("Kelly%.0f%%: $50k->$%.0f (%+.0f%%) maxDD=%.0f%%\n",KELLY*100,eq,(eq/50000-1)*100,mdd);
    printf("WF: H1=%+.2f%% H2=%+.2f%%\n",h1/mid*100,h2/(n-mid)*100);
    for(auto&[y,v]:byyr){ int ww=0;double s=0; for(double r:v){if(r>0)ww++;s+=r;} printf("  %s: n=%d win%%=%.0f totRet=%+.0f%%\n",y.c_str(),(int)v.size(),100.0*ww/v.size(),s*100); }
    // concentration: top-5 tickers' share of total positive net (single-name dominance check)
    double totNet=0; for(auto&[d,r]:trades) totNet+=r;
    vector<pair<double,string>> tk; for(auto&[t,v]:tkNet) tk.push_back({v,t}); sort(tk.rbegin(),tk.rend());
    double top5=0; for(int i=0;i<5&&i<(int)tk.size();i++) top5+=tk[i].first;
    printf("concentration: totNet=%+.1f | top5 tickers=%+.1f (%.0f%% of net) | uniq tickers=%zu\n",
           totNet,top5, totNet!=0?100*top5/totNet:0, tkNet.size());
    return 0;
}
