// monday_test.cpp -- validate the cross-asset Monday-long anomaly.
// Monday session return = prior-day close -> Monday close. Cost-incl, per-year, both-halves.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <map>
using namespace std;
int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage:%s <m5> <label> <costPct>\n",argv[0]);return 1;}
  string label=argv[2]; double COST=atof(argv[3]); // round-trip cost in %
  map<int64_t,double> dayClose; map<int64_t,int> dayYr;
  {ifstream f(argv[1]);string ln;bool fst=true;
   while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
     const char*s=ln.c_str();char*e=nullptr;long long ts=strtoll(s,&e,10);if(*e!=',')continue;if(ts>100000000000LL)ts/=1000;
     strtod(e+1,&e);if(*e!=',')continue;strtod(e+1,&e);if(*e!=',')continue;strtod(e+1,&e);if(*e!=',')continue;double c=strtod(e+1,&e);
     if(c>0){int64_t d=ts/86400;dayClose[d]=c;time_t t=ts;struct tm g;gmtime_r(&t,&g);dayYr[d]=g.tm_year+1900;}}}
  vector<pair<int64_t,double>> D(dayClose.begin(),dayClose.end());
  // Monday = (d+4)%7==1
  vector<double> rr; map<int,vector<double>> byYr;
  for(size_t i=1;i<D.size();i++){ if(D[i].first-D[i-1].first>3)continue; // skip big gaps
    int wd=(int)(((D[i].first+4)%7+7)%7); if(wd!=1)continue; // Monday
    double r=(D[i].second-D[i-1].second)/D[i-1].second*100.0 - COST;
    rr.push_back(r); byYr[dayYr[D[i].first]].push_back(r); }
  int m=rr.size(); if(!m){printf("%s: no mondays\n",label.c_str());return 0;}
  double s=0,s2=0;int w=0;for(double r:rr){s+=r;s2+=r*r;if(r>0)w++;}
  double mean=s/m,sd=sqrt((s2-m*mean*mean)/(m-1)),t=mean/(sd/sqrt((double)m));
  double h1=0,h2=0;int mid=m/2;for(int k=0;k<mid;k++)h1+=rr[k];for(int k=mid;k<m;k++)h2+=rr[k];
  printf("%-8s n=%-3d avgRet=%+.3f%% win%%=%4.1f t=%+.2f totRet=%+.1f%% H1=%+.1f H2=%+.1f %s | ",
    label.c_str(),m,mean,100.0*w/m,t,s,h1,h2,(h1>0&&h2>0)?"both+":"SPLIT");
  for(auto&kv:byYr){double ys=0;for(double r:kv.second)ys+=r;printf("%d:%+.1f ",kv.first,ys);}
  printf("\n");
  if(const char* dp=getenv("DUMP")){FILE*fp=fopen(dp,"a");if(fp){for(size_t i=1;i<D.size();i++){int wd=(int)(((D[i].first+4)%7+7)%7);if(wd!=1)continue;if(D[i].first-D[i-1].first>3)continue;double r=(D[i].second-D[i-1].second)/D[i-1].second*100.0-COST;fprintf(fp,"%lld,%.4f\n",(long long)(D[i].first*86400),r);}fclose(fp);}}
  return 0;
}
