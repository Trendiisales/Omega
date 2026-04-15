// regime_check.cpp
// Print monthly ATR statistics to understand what Jul-Aug 2025 looked like
// vs the profitable Jan-Jun 2025 period for A6 SHORT
//
// Build: g++ -O3 -std=c++17 regime_check.cpp -o regime_check
// Run:   ./regime_check ~/Tick/2yr_XAUUSD_tick.csv

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <map>

struct Tick { int64_t ts_ms; double bid, ask; };

static int64_t parse_ts(const char* d, const char* t) {
    auto g=[](const char* s,int n){int v=0;for(int i=0;i<n;i++)v=v*10+(s[i]-'0');return v;};
    int y=g(d,4),mo=g(d+4,2),dy=g(d+6,2),h=g(t,2),mi=g(t+3,2),se=g(t+6,2);
    if(mo<=2){y--;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153*mo+8)/5+dy-719469LL;
    return(days*86400LL+h*3600LL+mi*60LL+se)*1000LL;
}

int main(int argc, char** argv) {
    if(argc<2){printf("need file\n");return 1;}
    FILE* f=fopen(argv[1],"r");if(!f){printf("cannot open\n");return 1;}
    char line[256],d[32],t[32]; double bid,ask;
    fgets(line,256,f);

    // Track daily ATR (high-low of day)
    struct DayStats { double hi=0,lo=1e9,open=0; bool init=false; };
    std::map<int,DayStats> days;

    while(fgets(line,256,f)){
        if(sscanf(line,"%31[^,],%31[^,],%lf,%lf",d,t,&bid,&ask)<4)continue;
        if(bid<=0||ask<=bid)continue;
        int64_t ts=parse_ts(d,t);
        int day=(int)(ts/86400000LL);
        double mid=(bid+ask)*0.5;
        auto& ds=days[day];
        if(!ds.init){ds.open=mid;ds.hi=mid;ds.lo=mid;ds.init=true;}
        if(mid>ds.hi)ds.hi=mid;
        if(mid<ds.lo)ds.lo=mid;
    }
    fclose(f);

    // Print monthly average daily range
    printf("\nMonthly Daily Range (ATR proxy) -- High-Low of each day\n");
    printf("%-10s %8s %8s %8s %8s  %s\n","Month","Avg","Max","Min","Days","Note");
    printf("%s\n",std::string(70,'-').c_str());

    std::map<int,std::vector<double>> monthly;
    for(auto& [day,ds]:days){
        if(!ds.init)continue;
        double rng=ds.hi-ds.lo;
        int64_t sec=(int64_t)day*86400;
        int yr=(int)(sec/31557600+1970),mo=(int)((sec%31557600)/2592000+1);
        int key=yr*100+mo;
        monthly[key].push_back(rng);
    }

    for(auto& [key,rngs]:monthly){
        double sum=0,mx=0,mn=1e9;
        for(double r:rngs){sum+=r;if(r>mx)mx=r;if(r<mn)mn=r;}
        double avg=sum/rngs.size();
        int yr=key/100,mo=key%100;
        const char* note="";
        if(avg>40) note=" <-- EXTREME";
        else if(avg>25) note=" <-- HIGH";
        else if(avg>15) note=" <-- ELEVATED";
        printf("%04d-%02d   %8.1f %8.1f %8.1f %8zu%s\n",
               yr,mo,avg,mx,mn,(size_t)rngs.size(),note);
    }

    return 0;
}
