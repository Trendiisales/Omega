#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
using namespace std;
int main(int ac,char**av){
  vector<pair<long long,double>> T;
  ifstream f(av[1]);string ln;
  while(getline(f,ln)){if(ln.empty())continue;const char*s=ln.c_str();char*e=0;long long ts=strtoll(s,&e,10);if(*e!=',')continue;double r=strtod(e+1,&e);T.push_back({ts,r});}
  sort(T.begin(),T.end());
  int n=T.size();if(!n){printf("no trades\n");return 0;}
  double eq=0,pk=0,mdd=0,sum=0,sum2=0,gw=0,gl=0;int w=0;
  for(auto&t:T){double r=t.second;eq+=r;if(eq>pk)pk=eq;double dd=pk-eq;if(dd>mdd)mdd=dd;sum+=r;sum2+=r*r;if(r>0){w++;gw+=r;}else gl+=-r;}
  double mean=sum/n,sd=sqrt((sum2-n*mean*mean)/(n-1));
  // both-halves
  double h1=0,h2=0;int mid=n/2;for(int i=0;i<mid;i++)h1+=T[i].second;for(int i=mid;i<n;i++)h2+=T[i].second;
  printf("%-10s n=%-4d WR=%.1f%% PF=%.2f totR=%+.1f maxDD=%.1fR ret/DD=%.2f Sharpe/tr=%+.3f H1=%+.1f H2=%+.1f %s\n",
    av[2],n,100.0*w/n,gl>0?gw/gl:99,sum,mdd,mdd>0?sum/mdd:99,mean/sd,h1,h2,(h1>0&&h2>0)?"both+":"split");
  return 0;
}
