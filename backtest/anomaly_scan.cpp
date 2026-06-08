// anomaly_scan.cpp -- hunt non-price-action anomalies in m5 data:
//  (1) hour-of-day forward-return bias (enter at UTC hour H, hold K hours)
//  (2) day-of-week daily-return bias
// Reports avg return, win%, and BOTH-HALVES sign consistency (overfit guard).
// run: ./anomaly_scan <m5file> <label> [holdHours=2]
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
struct Bar{int64_t ts;double c;};
int main(int argc,char**argv){
  if(argc<3){fprintf(stderr,"usage:%s <m5> <label> [hold=2]\n",argv[0]);return 1;}
  string label=argv[2]; int HOLD=argc>3?atoi(argv[3]):2;
  vector<Bar> B;{ifstream f(argv[1]);string ln;bool fst=true;
    while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
      const char*s=ln.c_str();char*e=nullptr;long long ts=strtoll(s,&e,10);if(*e!=',')continue;if(ts>100000000000LL)ts/=1000;
      // close = 5th field
      double o=strtod(e+1,&e);if(*e!=',')continue;double h=strtod(e+1,&e);if(*e!=',')continue;double l=strtod(e+1,&e);if(*e!=',')continue;double c=strtod(e+1,&e);
      (void)o;(void)h;(void)l; if(c>0)B.push_back({ts,c});}}
  int n=B.size(); if(n<2000){fprintf(stderr,"few bars\n");return 1;}
  // hourly closes: last m5 close in each UTC hour
  map<int64_t,double> hourClose; for(auto&b:B){ int64_t hr=b.ts/3600; hourClose[hr]=b.c; }
  vector<pair<int64_t,double>> H(hourClose.begin(),hourClose.end());
  int nh=H.size();
  // (1) hour-of-day forward return over HOLD hours
  struct Acc{double sum=0,sum2=0;int n=0,w=0; double h1=0,h2=0;int n1=0;};
  Acc hod[24];
  for(int i=0;i+HOLD<nh;i++){
    // require contiguous (no big gap)
    if(H[i+HOLD].first - H[i].first != HOLD) continue;
    int hr=(int)(((H[i].first%24)+24)%24);
    double r=(H[i+HOLD].second-H[i].second)/H[i].second*100.0;
    Acc&a=hod[hr]; a.sum+=r;a.sum2+=r*r;a.n++; if(r>0)a.w++;
  }
  // both-halves per hour
  { int cnt[24]={0}; vector<vector<double>> rr(24);
    for(int i=0;i+HOLD<nh;i++){ if(H[i+HOLD].first-H[i].first!=HOLD)continue; int hr=(int)(((H[i].first%24)+24)%24);
      rr[hr].push_back((H[i+HOLD].second-H[i].second)/H[i].second*100.0); }
    printf("=== %s HOUR-OF-DAY (enter UTC hour, hold %dh) ===\n",label.c_str(),HOLD);
    printf("hr   n     avgRet%%   win%%   t-stat  H1sum  H2sum  both?\n");
    for(int hr=0;hr<24;hr++){ auto&v=rr[hr]; int m=v.size(); if(m<50)continue;
      double s=0,s2=0;int w=0; for(double r:v){s+=r;s2+=r*r;if(r>0)w++;}
      double mean=s/m,sd=sqrt((s2-m*mean*mean)/(m-1)); double t=mean/(sd/sqrt((double)m));
      double h1=0,h2=0;int mid=m/2; for(int k=0;k<mid;k++)h1+=v[k]; for(int k=mid;k<m;k++)h2+=v[k];
      bool both=(h1>0&&h2>0)||(h1<0&&h2<0);
      if(fabs(t)>2.0) // only significant
        printf("%2d %5d  %+7.4f  %4.1f  %+6.2f  %+6.2f %+6.2f  %s\n",hr,m,mean,100.0*w/m,t,h1,h2,both?"YES":"no");
    }
  }
  // (2) day-of-week: daily return (00->24 UTC close-to-close)
  { map<int64_t,double> dayClose; for(auto&b:B){int64_t d=b.ts/86400;dayClose[d]=b.c;}
    vector<pair<int64_t,double>> D(dayClose.begin(),dayClose.end());
    vector<vector<double>> dow(7);
    for(size_t i=1;i<D.size();i++){ if(D[i].first-D[i-1].first!=1)continue;
      int wd=(int)(((D[i].first+4)%7+7)%7); // 1970-01-01 = Thursday(4)
      dow[wd].push_back((D[i].second-D[i-1].second)/D[i-1].second*100.0); }
    const char* nm[7]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    printf("--- %s DAY-OF-WEEK (daily ret) ---\n",label.c_str());
    for(int d=0;d<7;d++){auto&v=dow[d];int m=v.size();if(m<20)continue;double s=0,s2=0;int w=0;for(double r:v){s+=r;s2+=r*r;if(r>0)w++;}
      double mean=s/m,sd=sqrt((s2-m*mean*mean)/(m-1));double t=mean/(sd/sqrt((double)m));
      if(fabs(t)>1.8) printf("  %s n=%d avgRet=%+.3f%% win%%=%.0f t=%+.2f\n",nm[d],m,mean,100.0*w/m,t);}
  }
  return 0;
}
