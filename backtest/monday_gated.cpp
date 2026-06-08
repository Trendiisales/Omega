// monday_gated.cpp -- Monday-long with a risk-on regime gate (price > SMA_N at Mon open).
// Skips Mondays in a downtrend (bear protection). Reports gated vs ungated, per-year.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <deque>
using namespace std;
int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage:%s <m5> <label> <costPct> [smaN=20]\n",argv[0]);return 1;}
  string label=argv[2]; double COST=atof(argv[3]); int SMAN=argc>4?atoi(argv[4]):20;
  map<int64_t,double> dc; map<int64_t,int> dy;
  {ifstream f(argv[1]);string ln;bool fst=true;
   while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
     const char*s=ln.c_str();char*e=nullptr;long long ts=strtoll(s,&e,10);if(*e!=',')continue;if(ts>100000000000LL)ts/=1000;
     strtod(e+1,&e);if(*e!=',')continue;strtod(e+1,&e);if(*e!=',')continue;strtod(e+1,&e);if(*e!=',')continue;double c=strtod(e+1,&e);
     if(c>0){int64_t d=ts/86400;dc[d]=c;time_t t=ts;struct tm g;gmtime_r(&t,&g);dy[d]=g.tm_year+1900;}}}
  vector<pair<int64_t,double>> D(dc.begin(),dc.end());
  deque<double> sma; double smaSum=0;
  vector<double> g_rr,u_rr; map<int,vector<double>> g_yr;
  for(size_t i=1;i<D.size();i++){
    // maintain SMA of prior closes
    if(i>=1){ sma.push_back(D[i-1].second); smaSum+=D[i-1].second; if((int)sma.size()>SMAN){smaSum-=sma.front();sma.pop_front();} }
    if(D[i].first-D[i-1].first>3)continue;
    int wd=(int)(((D[i].first+4)%7+7)%7); if(wd!=1)continue;
    double r=(D[i].second-D[i-1].second)/D[i-1].second*100.0 - COST;
    u_rr.push_back(r);
    bool riskon = (int)sma.size()>=SMAN && D[i-1].second > smaSum/sma.size(); // prior close > SMA
    if(riskon){ g_rr.push_back(r); g_yr[dy[D[i].first]].push_back(r); }
  }
  auto rep=[&](vector<double>&rr,const char*tag){int m=rr.size();if(!m){printf("  %-8s n=0\n",tag);return;}
    double s=0,s2=0;int w=0;for(double r:rr){s+=r;s2+=r*r;if(r>0)w++;}double mean=s/m,sd=sqrt((s2-m*mean*mean)/(m-1)),t=mean/(sd/sqrt((double)m));
    double h1=0,h2=0;int mid=m/2;for(int k=0;k<mid;k++)h1+=rr[k];for(int k=mid;k<m;k++)h2+=rr[k];
    printf("  %-8s n=%-3d avg=%+.3f%% win%%=%.0f t=%+.2f tot=%+.1f%% %s\n",tag,m,mean,100.0*w/m,t,s,(h1>0&&h2>0)?"both+":"split");};
  printf("=== %s Monday (SMA%d gate) ===\n",label.c_str(),SMAN);
  rep(u_rr,"ungated"); rep(g_rr,"GATED");
  printf("    gated per-yr: "); for(auto&kv:g_yr){double ys=0;for(double r:kv.second)ys+=r;printf("%d:%+.1f ",kv.first,ys);} printf("\n");
  return 0;
}
