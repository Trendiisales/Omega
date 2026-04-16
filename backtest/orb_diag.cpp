#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Tick { int64_t ms; double bid, ask; };

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path); if (!f) return false;
    std::string line, tok; std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,ci=0;
    { std::istringstream h(line); while(std::getline(h,tok,',')) {
        if(tok=="ts_ms")cm=ci; if(tok=="bid")cb=ci; if(tok=="ask")ca=ci; ++ci; }}
    if(cb<0||ca<0) return false;
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        static char buf[512]; if(line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        const char* flds[32]; int nf=0; flds[nf++]=buf;
        for(char* c=buf;*c&&nf<32;++c) if(*c==','){*c='\0';flds[nf++]=c+1;}
        if(nf<=std::max(cm,std::max(cb,ca))) continue;
        try { Tick t; t.ms=(int64_t)std::stod(flds[cm]);
              t.bid=std::stod(flds[cb]); t.ask=std::stod(flds[ca]);
              out.push_back(t); } catch(...) {}
    }
    return true;
}

int main(int argc, char** argv) {
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for(int i=1;i<argc;++i) { load_csv(argv[i],ticks); }
    fprintf(stderr,"Total: %zu\n", ticks.size());

    // For each day, track 13:00-13:03 range then log every M1 bar close 13:03-14:00
    int64_t cur_day=-1;
    double rhi=0,rlo=1e9;
    bool range_done=false, traded=false;
    double atr=2.0;
    struct M1Bar{double open,high,low,close; int64_t bms;};
    M1Bar cur_bar={}; int64_t cur_bms=0;
    std::deque<M1Bar> bars;

    printf("%-12s %-5s %-8s %-8s %-6s  %s\n",
           "Date","Min","RangeHi","RangeLo","Size","Bars (close body%% breakout)");

    for(auto& t:ticks){
        double mid=(t.bid+t.ask)*0.5;
        int64_t day=(t.ms/1000LL)/86400LL;
        int h=((t.ms/1000LL)%86400LL)/3600LL;
        int m=((t.ms/1000LL)%3600LL)/60LL;

        if(day!=cur_day){
            cur_day=day; rhi=0; rlo=1e9; range_done=false; traded=false;
            cur_bar={}; cur_bms=0;
        }

        // ATR from any time
        int64_t bms2=(t.ms/60000LL)*60000LL;
        if(cur_bms==0){cur_bar={mid,mid,mid,mid,bms2};cur_bms=bms2;}
        else if(bms2!=cur_bms){
            bars.push_back(cur_bar); if((int)bars.size()>20)bars.pop_front();
            if((int)bars.size()>=2){
                double sum=0; int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i];auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,fabs(b.high-pb.close),fabs(b.low-pb.close)});}
                atr=sum/n;}
            cur_bar={mid,mid,mid,mid,bms2};cur_bms=bms2;
        } else {cur_bar.high=std::max(cur_bar.high,mid);cur_bar.low=std::min(cur_bar.low,mid);cur_bar.close=mid;}

        // NY range: 13:00-13:03
        if(h==13 && m<3){
            if(mid>rhi)rhi=mid;
            if(mid<rlo)rlo=mid;
        }
        if(h==13 && m==3 && !range_done){
            range_done=true;
            printf("%lld 13:%02d  %.3f  %.3f  %.3f  |",
                   (long long)day,m,rhi,rlo,rhi-rlo);
        }

        // Log bar closes 13:03-14:00
        if(range_done && !traded && h==13 && m>=3){
            if(bms2!=cur_bms || (t.ms==ticks.back().ms)){
                if(!bars.empty()){
                    auto& lb=bars.back();
                    double body=fabs(lb.close-lb.open);
                    double rng2=lb.high-lb.low;
                    double bpct=rng2>0?body/rng2:0;
                    const char* brk="";
                    if(lb.close>rhi&&lb.close>lb.open){brk="BREAK_UP";traded=true;}
                    else if(lb.close<rlo&&lb.close<lb.open){brk="BREAK_DN";traded=true;}
                    printf(" %d:%.2f(%.0f%%)%s",
                           m,(float)lb.close,(float)(bpct*100),brk);
                }
            }
        }
        if(h==14&&m==0&&range_done) printf("\n");
    }
    printf("\n");
    return 0;
}
