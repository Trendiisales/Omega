#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
using namespace std;
int main(int argc,char**argv){int W=atoi(argv[2]);ifstream f(argv[1]);if(!f)return 1;printf("ts,o,h,l,c,v\n");
 string ln;long long cur=-1;double o=0,h=0,l=0,c=0;long vol=0;bool first=true;
 while(getline(f,ln)){if(ln.empty())continue;if(first){first=false;if(ln[0]<'0'||ln[0]>'9')continue;}
  if(ln.size()<17||ln[8]!=' ')continue;
  char b[9];memcpy(b,ln.c_str(),8);b[8]=0;long ymd=atol(b);const char*p=ln.c_str()+9;
  int hh=(p[0]-'0')*10+p[1]-'0',mm=(p[2]-'0')*10+p[3]-'0',ss=(p[4]-'0')*10+p[5]-'0';
  struct tm t{};t.tm_year=ymd/10000-1900;t.tm_mon=(ymd/100)%100-1;t.tm_mday=ymd%100;t.tm_hour=hh;t.tm_min=mm;t.tm_sec=ss;
  long long ts=(long long)timegm(&t)+5*3600; // EST->UTC
  const char*q=strchr(ln.c_str()+9,',');if(!q)continue;double bid=strtod(q+1,nullptr);
  const char*r=strchr(q+1,',');if(!r)continue;double ask=strtod(r+1,nullptr);
  if(bid<=0||ask<=0)continue;double mid=(bid+ask)*0.5;long long g=(ts/W)*W;
  if(g!=cur){if(cur>=0)printf("%lld,%.2f,%.2f,%.2f,%.2f,%ld\n",cur,o,h,l,c,vol);cur=g;o=h=l=c=mid;vol=1;}
  else{if(mid>h)h=mid;if(mid<l)l=mid;c=mid;vol++;}}
 if(cur>=0)printf("%lld,%.2f,%.2f,%.2f,%.2f,%ld\n",cur,o,h,l,c,vol);return 0;}
