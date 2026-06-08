// daily-return correlation across symbols (diversification check)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <map>
using namespace std;
static map<int64_t,double> dret(const char*p){map<int64_t,double> dc;
  ifstream f(p);string ln;bool fst=true;
  while(getline(f,ln)){if(ln.empty())continue;if(fst){fst=false;if(ln[0]<'0'||ln[0]>'9')continue;}
   const char*s=ln.c_str();char*e=0;long long ts=strtoll(s,&e,10);if(*e!=',')continue;if(ts>100000000000LL)ts/=1000;
   strtod(e+1,&e);if(*e!=',')continue;strtod(e+1,&e);if(*e!=',')continue;strtod(e+1,&e);if(*e!=',')continue;double c=strtod(e+1,&e);
   if(c>0)dc[ts/86400]=c;}
  map<int64_t,double> r;int64_t pd=-1;double pc=0;for(auto&kv:dc){if(pd>=0&&kv.first-pd<=3&&pc>0)r[kv.first]=(kv.second-pc)/pc;pd=kv.first;pc=kv.second;}return r;}
int main(int ac,char**av){int N=(ac-1)/2;vector<string> nm;vector<map<int64_t,double>> R;
  for(int i=0;i<N;i++){nm.push_back(av[2+i*2]);R.push_back(dret(av[1+i*2]));}
  printf("%-8s","");for(auto&n:nm)printf("%7s",n.c_str());printf("\n");
  for(int i=0;i<N;i++){printf("%-8s",nm[i].c_str());for(int j=0;j<N;j++){
    double sx=0,sy=0,sxy=0,sx2=0,sy2=0;int n=0;
    for(auto&kv:R[i]){auto it=R[j].find(kv.first);if(it==R[j].end())continue;double x=kv.second,y=it->second;sx+=x;sy+=y;sxy+=x*y;sx2+=x*x;sy2+=y*y;n++;}
    double c=n>2?(n*sxy-sx*sy)/sqrt((n*sx2-sx*sx)*(n*sy2-sy*sy)):0;printf("%+7.2f",c);}printf("\n");}
  return 0;}
