// aggregate XAU tick "YYYYMMDD,HH:MM:SS,bid,ask" (UTC) -> ts,o,h,l,c (sec) at barSec
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
using namespace std;
int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s <tickfile> <barSec>\n",argv[0]);return 1;}
    int W=atoi(argv[2]); ifstream f(argv[1]); if(!f){fprintf(stderr,"no file\n");return 1;}
    printf("ts,o,h,l,c\n");
    string ln; long long cur=-1; double o=0,h=0,l=0,c=0; bool first=true;
    while(getline(f,ln)){
        if(ln.empty())continue; if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
        // YYYYMMDD,HH:MM:SS,bid,ask
        char buf[9]; memcpy(buf,ln.c_str(),8); buf[8]=0; long ymd=atol(buf);
        const char* p=ln.c_str()+9; int hh=(p[0]-'0')*10+p[1]-'0',mm=(p[3]-'0')*10+p[4]-'0',ss=(p[6]-'0')*10+p[7]-'0';
        struct tm tmv{}; tmv.tm_year=ymd/10000-1900; tmv.tm_mon=(ymd/100)%100-1; tmv.tm_mday=ymd%100; tmv.tm_hour=hh; tmv.tm_min=mm; tmv.tm_sec=ss;
        long long ts=(long long)timegm(&tmv);
        const char* q=strchr(ln.c_str()+9,','); if(!q)continue; double bid=strtod(q+1,nullptr);
        const char* r=strchr(q+1,','); if(!r)continue; double ask=strtod(r+1,nullptr);
        if(bid<=0||ask<=0)continue; double mid=(bid+ask)*0.5; long long g=(ts/W)*W;
        if(g!=cur){ if(cur>=0) printf("%lld,%.4f,%.4f,%.4f,%.4f\n",cur,o,h,l,c); cur=g; o=h=l=c=mid; }
        else { if(mid>h)h=mid; if(mid<l)l=mid; c=mid; }
    }
    if(cur>=0) printf("%lld,%.4f,%.4f,%.4f,%.4f\n",cur,o,h,l,c);
    return 0;
}
