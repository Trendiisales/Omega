// =============================================================================
// fx_session_breakout.cpp -- FX session breakout on M1 (S43, the deferred edge).
// THE classic FX-native intraday edge: London liquidity injection expands the
// overnight Asian range directionally. Tests Asian-range -> London-open breakout
// with REAL bid/ask spread (Dukascopy M1 bid+ask), long AND short, no-lookahead,
// WF split + 6-block + cost honesty. Also tests a compression filter (only trade
// when the Asian range is narrow -> coil-then-expand).
//
// HONEST: enter by crossing the spread (long@ask, short@bid), exit crossing back.
// One trade/day. SL=opposite Asian edge, TP=R*range, time-exit at cutoff. All
// session windows in UTC. Prints winners AND the compression-filter sweep.
//
// BUILD: c++ -std=c++17 -O2 backtest/fx_session_breakout.cpp -o backtest/fx_session_breakout
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>

struct M1 { int64_t ts; double bid, ask; };   // minute close bid/ask

// Load Dukascopy M1 CSV (timestamp ms, OHLC). Returns close per minute.
static std::unordered_map<int64_t,double> load_close(const char* path){
    std::unordered_map<int64_t,double> m; m.reserve(5000000);
    FILE* f=fopen(path,"r"); if(!f){perror(path);return m;}
    char ln[256]; bool first=true;
    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9') continue;}
        double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
        if(c<=0) continue; m[(int64_t)(ts/1000.0)]=c;
    }
    fclose(f); return m;
}

static int utc_hour(int64_t ts){ int h=(int)((ts/3600)%24); return h<0?h+24:h; }

struct Stats {
    int n=0,win=0; double g=0,gw=0,gl=0,h1=0,h2=0; int h1n=0,h2n=0;
    double blk[6]={0}; int64_t tmin=0,tmax=1;
    void add(double pnl,int64_t ts,int64_t split){
        n++; g+=pnl; if(pnl>0){win++;gw+=pnl;}else gl+=-pnl;
        if(ts<split){h1+=pnl;h1n++;}else{h2+=pnl;h2n++;}
        int bi=(int)(6.0*(ts-tmin)/(double)(tmax-tmin+1)); if(bi<0)bi=0;if(bi>5)bi=5; blk[bi]+=pnl;
    }
    double pf()const{return gl>0?gw/gl:(gw>0?99:0);}
    double wr()const{return n?100.0*win/n:0;}
    int blkpos()const{int p=0;for(int i=0;i<6;i++)if(blk[i]>0)p++;return p;}
    bool robust()const{return h1>0&&h2>0&&h1n>=15&&h2n>=15&&blkpos()>=5&&n>=40;}
};

int main(int argc,char**argv){
    const char* sym = argc>1? argv[1] : "eurusd";
    printf("FX SESSION BREAKOUT (M1, real bid/ask) -- %s -- honest WF+6block+cost\n", sym);
    char bp[256],ap[256];
    snprintf(bp,sizeof bp,"/Users/jo/Omega/download/%s-m1-bid-2019-01-01-2026-05-31.csv",sym);
    snprintf(ap,sizeof ap,"/Users/jo/Omega/download/%s-m1-ask-2019-01-01-2026-05-31.csv",sym);
    auto bidm=load_close(bp), askm=load_close(ap);
    if(bidm.empty()||askm.empty()){printf("missing M1 bid/ask for %s\n",sym);return 1;}
    // merge to sorted minute vector where BOTH sides exist
    std::vector<M1> v; v.reserve(bidm.size());
    for(auto&kv:bidm){ auto it=askm.find(kv.first); if(it!=askm.end()&&it->second>kv.second) v.push_back({kv.first,kv.second,it->second}); }
    if(v.size()<100000){printf("too few merged minutes (%zu)\n",v.size());return 1;}
    std::sort(v.begin(),v.end(),[](const M1&a,const M1&b){return a.ts<b.ts;});
    int64_t TMIN=v.front().ts, TMAX=v.back().ts, SPLIT=v[v.size()/2].ts;
    double avg_spread_bp=0; for(auto&m:v) avg_spread_bp+=(m.ask-m.bid)/m.bid*1e4; avg_spread_bp/=v.size();
    printf("merged minutes=%zu span=%ld..%ld  avg spread=%.2fbp\n",v.size(),(long)TMIN,(long)TMAX,avg_spread_bp);

    // group minute indices by UTC day
    std::vector<std::pair<int64_t,std::pair<int,int>>> days; // day -> [start,end) idx
    { int64_t cur=-1,s=0; for(size_t i=0;i<v.size();i++){ int64_t d=(v[i].ts/86400)*86400;
        if(d!=cur){ if(cur>=0)days.push_back({cur,{(int)s,(int)i}}); cur=d; s=i; } }
      if(cur>=0)days.push_back({cur,{(int)s,(int)v.size()}}); }

    // Asian-range -> London breakout. asianEnd/lonEnd/cutoff in UTC hours.
    auto run=[&](int asianStart,int asianEnd,int lonEnd,int cutoff,double bufR,double tpR,
                 double maxRangeBp /*compression filter; 1e9=off*/)->Stats{
        Stats s; s.tmin=TMIN; s.tmax=TMAX;
        for(auto&day:days){
            int a=day.second.first,b=day.second.second;
            // 1) Asian range (mid) in [asianStart,asianEnd)
            double hi=-1e18,lo=1e18; bool any=false;
            for(int i=a;i<b;i++){ int h=utc_hour(v[i].ts); if(h>=asianStart&&h<asianEnd){ double mid=(v[i].bid+v[i].ask)*0.5; if(mid>hi)hi=mid; if(mid<lo)lo=mid; any=true; } }
            if(!any||hi<=lo) continue;
            double range=hi-lo; double rngbp=range/lo*1e4;
            if(rngbp>maxRangeBp) continue;            // compression filter
            double buf=range*bufR;
            // 2) London breakout window [asianEnd,lonEnd): first close beyond range+-buf
            bool in=false,is_long=false; double entry=0,sl=0,tp=0; int ei=-1;
            for(int i=a;i<b;i++){ int h=utc_hour(v[i].ts);
                if(!in){
                    if(h<asianEnd||h>=lonEnd) continue;
                    double mid=(v[i].bid+v[i].ask)*0.5;
                    if(mid>hi+buf){ in=true;is_long=true; entry=v[i].ask; sl=lo; tp=entry+tpR*range; ei=i; }
                    else if(mid<lo-buf){ in=true;is_long=false; entry=v[i].bid; sl=hi; tp=entry-tpR*range; ei=i; }
                } else {
                    // manage: SL/TP/time-cutoff. exit crosses spread.
                    double xb=v[i].bid, xa=v[i].ask;
                    bool ex=false; double xpx=0;
                    if(is_long){ if(xb<=sl){ex=true;xpx=xb;} else if(xb>=tp){ex=true;xpx=xb;} }
                    else       { if(xa>=sl){ex=true;xpx=xa;} else if(xa<=tp){ex=true;xpx=xa;} }
                    if(!ex && h>=cutoff){ ex=true; xpx=is_long?xb:xa; }
                    if(ex){ double pnl=(is_long?(xpx-entry):(entry-xpx))/entry*1e4; s.add(pnl,v[ei].ts,SPLIT); in=false; break; }
                }
            }
            if(in){ /* force close at last bar of day */ double xpx=is_long?v[b-1].bid:v[b-1].ask;
                double pnl=(is_long?(xpx-entry):(entry-xpx))/entry*1e4; s.add(pnl,v[ei].ts,SPLIT); }
        }
        return s;
    };
    auto rep=[&](const char* tag,const Stats& s){
        const char* fl=s.robust()?" *** ROBUST":((s.h1>0&&s.h2>0&&s.n>=40)?" ** WF+":(s.g>0?" (1-sided)":""));
        printf("  %-40s n=%-4d WR=%4.1f PF=%.2f net=%+7.0fbp H1=%+5.0f H2=%+5.0f blk=%d/6%s\n",
               tag,s.n,s.wr(),s.pf(),s.g,s.h1,s.h2,s.blkpos(),fl);
    };

    printf("\n--- Asian(00-07)->London(07-12) breakout, exit cutoff, buf/tp/compression sweep ---\n");
    for(double buf:{0.05,0.10,0.20}) for(double tp:{1.0,2.0}) for(double mr:{1e9,60.0,40.0}){
        char t[80]; snprintf(t,sizeof t,"buf%.2f tp%.1fR cutoff21 maxR%.0f",buf,tp,mr>1e8?0:mr);
        rep(t,run(0,7,12,21,buf,tp,mr));
    }
    printf("\n--- NY session: Asian+London range(00-12)->NY(12-15) breakout ---\n");
    for(double buf:{0.05,0.10}) for(double tp:{1.0,2.0}){
        char t[80]; snprintf(t,sizeof t,"NY buf%.2f tp%.1fR",buf,tp);
        rep(t,run(0,12,15,21,buf,tp,1e9));
    }
    printf("\n*** ROBUST = both halves + 5/6 blocks + n>=40. Honest: real bid/ask spread, no-lookahead.\n");
    return 0;
}
