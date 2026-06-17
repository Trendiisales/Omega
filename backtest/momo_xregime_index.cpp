// momo_xregime_index.cpp — run BigCapMomo's momentum mechanic on multi-year INDEX
// intraday (NAS100/US500) for a bear+bull cross-regime read. Data: duka tick
// (ts_ms,ask,bid). Builds 5m mid-bars, applies day-expansion gate + ignition +
// wide trail (NO vol gate — index tick lacks real volume). Splits by year.
// CAVEAT: index, not individual-stock movers — tests the MECHANIC, not the scanner.
// build: clang++ -O2 -std=c++17 -o /tmp/momox backtest/momo_xregime_index.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cmath>
#include <ctime>
#include <fstream>
struct Bar{ double o,h,l,c; long ts; };
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <tickfile> [gate%%=0.6] [ig%%=0.8] [trail%%=2.0] [lb=6] [maxhold=48]\n",argv[0]);return 1;}
    double GATE=argc>2?atof(argv[2]):0.6, IG=argc>3?atof(argv[3]):0.8, TRAIL=(argc>4?atof(argv[4]):2.0)/100.0;
    int LB=argc>5?atoi(argv[5]):6, MAXHOLD=argc>6?atoi(argv[6]):48; double SLIP=0.0003;
    std::ifstream f(argv[1]); if(!f){fprintf(stderr,"open fail\n");return 1;}
    std::string ln; std::getline(f,ln); // header
    // build 5m bars of mid
    std::vector<Bar> bars; long curb=-1; Bar cur{};
    long n=0;
    while(std::getline(f,ln)){
        if(ln.empty())continue;
        long ts; double mid;
        if(ln.size()>9 && ln[8]==' '){
            // histdata: YYYYMMDD HHMMSSmmm,bid,ask[,vol]  (BID first)
            const char*s=ln.c_str();
            int Y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
            int Mo=(s[4]-'0')*10+(s[5]-'0'),D=(s[6]-'0')*10+(s[7]-'0');
            int hh=(s[9]-'0')*10+(s[10]-'0'),mm=(s[11]-'0')*10+(s[12]-'0'),ss=(s[13]-'0')*10+(s[14]-'0');
            int y=Y-(Mo<=2);int era=(y>=0?y:y-399)/400;unsigned yoe=(unsigned)(y-era*400);
            unsigned doy=(153*(Mo+(Mo>2?-3:9))+2)/5+D-1;unsigned doe=yoe*365+yoe/4-yoe/100+doy;
            long days=(long)era*146097+(long)doe-719468;
            ts=(days*86400L+hh*3600L+mm*60L+ss)*1000L;
            size_t c1=ln.find(',',15); if(c1==std::string::npos)continue;
            size_t c2=ln.find(',',c1+1);
            double bid=strtod(ln.c_str()+c1+1,nullptr); double ask=(c2==std::string::npos)?bid:strtod(ln.c_str()+c2+1,nullptr);
            if(bid<=0||ask<=0)continue; mid=(ask+bid)*0.5;
        } else {
            const char*s=ln.c_str(); char*e;
            ts=strtol(s,&e,10); if(*e!=',')continue; double ask=strtod(e+1,&e); if(*e!=',')continue; double bid=strtod(e+1,&e);
            if(ask<=0||bid<=0)continue; mid=(ask+bid)*0.5;
        }
        long b5=(ts/1000)/300;
        if(curb<0){curb=b5;cur={mid,mid,mid,mid,ts};}
        else if(b5==curb){ if(mid>cur.h)cur.h=mid; if(mid<cur.l)cur.l=mid; cur.c=mid; }
        else { bars.push_back(cur); curb=b5; cur={mid,mid,mid,mid,ts}; }
        n++;
    }
    if(curb>=0)bars.push_back(cur);
    fprintf(stderr,"# %ld ticks -> %zu 5m bars\n",n,bars.size());
    // sim: day-expansion gate + ignition + wide trail (no vol)
    std::map<int,std::vector<double>> byyear; // year -> returns
    bool inpos=false; double entry=0,peak=0; int hold=0,ety=0;
    long cur_day=-1; double day_open=0;
    for(size_t i=21;i<bars.size();++i){
        Bar&b=bars[i];
        long day=(b.ts/1000)/86400; if(day!=cur_day){cur_day=day;day_open=b.o;}
        time_t tt=b.ts/1000; struct tm*g=gmtime(&tt); int yr=g?g->tm_year+1900:0;
        if(inpos){
            hold++; if(b.h>peak)peak=b.h; double ts_=peak*(1-TRAIL);
            if(b.l<=ts_){byyear[ety].push_back(ts_*(1-SLIP)/entry-1);inpos=false;continue;}
            if(hold>=MAXHOLD){byyear[ety].push_back(b.c*(1-SLIP)/entry-1);inpos=false;continue;}
            continue;
        }
        if(day_open<=0)continue; double day_up=(b.c/day_open-1)*100; if(day_up<GATE)continue;
        double base=bars[i-LB].c; if(base<=0||(b.c/base-1)*100<IG)continue;
        entry=b.c*(1+SLIP);peak=b.h;hold=0;inpos=true;ety=yr;
    }
    auto stat=[](std::vector<double>&r,const char*tag){
        if(r.empty()){printf("%-8s n=0\n",tag);return;}
        double gw=0,gl=0,tot=0;int w=0; for(double x:r){tot+=x;if(x>0){gw+=x;w++;}else gl-=x;}
        printf("%-8s n=%4zu WR=%4.0f%% PF=%5.2f tot=%+8.1f%%\n",tag,r.size(),100.0*w/r.size(),gl>0?gw/gl:99.0,100*tot);
    };
    printf("\n=== NAS/index momentum x-regime (gate>=%.1f%% ig>=%.1f%% trail%.1f%% lb=%d) ===\n",GATE,IG,TRAIL*100,LB);
    std::vector<double> all; int bn=0;double bw=0,bl=0,bt=0;int bwc=0; std::vector<double> bear,bull;
    for(auto&kv:byyear){ char t[16];snprintf(t,16,"%d",kv.first); stat(kv.second,t);
        for(double x:kv.second){all.push_back(x); if(kv.first==2022||kv.first==2020)bear.push_back(x);else bull.push_back(x);} }
    printf("--\n"); stat(bear,"BEAR'22"); stat(bull,"BULL"); stat(all,"ALL");
    return 0;
}
