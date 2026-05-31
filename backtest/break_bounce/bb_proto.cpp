// bb_proto.cpp -- intraday (M15) archetype probe for gold, BOTH directions.
//
// Question: is there a DIFFERENT engine (not break-and-retest) that works on
// 4-20min bars in bull AND bear and survives gold cost? Probes two symmetric
// archetypes on the same tick stream, with real bid/ask fills + ATR stops:
//
//   A. DONCHIAN breakout  -- close breaks N-bar high -> long / low -> short.
//      Optional HTF(H4 EMA50) trend filter (trade only WITH the higher-TF
//      trend). Momentum: works in bull-trend and bear-trend.
//   B. KELTNER reversion  -- close beyond EMA20 +/- K*ATR -> fade (short the
//      top / long the bottom), exit at the mid. Mean-reversion: works in chop.
//
// Both: ATR(14) stop = STOP*ATR, TP = RR*risk, single position, intrabar SL/TP
// on the M15 bar (SL-first if both hit). Entry long@ask / short@bid; exit
// long@bid / short@ask -> full spread cost modelled.
//
// Build: g++ -O3 -std=c++17 bb_proto.cpp -o bbproto
// Run:   ./bbproto <ticks.csv>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Tick { int64_t ts; double bid, ask; };
static int64_t dfc(int y, unsigned m, unsigned d){ y-=m<=2; const int era=(y>=0?y:y-399)/400;
    const unsigned yoe=(unsigned)(y-era*400); const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    const unsigned doe=yoe*365+yoe/4-yoe/100+doy; return era*146097+(int)doe-719468; }
static bool parse(const char* s,int64_t& ts,double& bid,double& ask){
    if(s[0]>='0'&&s[0]<='9'){ char* e=nullptr; double f0=std::strtod(s,&e);
        if(e&&*e==','){ if(f0>=1e11){ char* e2=nullptr; bid=std::strtod(e+1,&e2);
            if(!e2||*e2!=',')return false; ask=std::strtod(e2+1,nullptr); ts=(int64_t)f0;
            if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; } } else return false; }
    if(std::strlen(s)<19)return false;
    int y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0'); int mo=(s[4]-'0')*10+(s[5]-'0');
    int da=(s[6]-'0')*10+(s[7]-'0'); if(s[8]!=',')return false;
    int hh=(s[9]-'0')*10+(s[10]-'0'); int mi=(s[12]-'0')*10+(s[13]-'0'); int se=(s[15]-'0')*10+(s[16]-'0');
    if(y<1971||mo<1||mo>12||da<1||da>31)return false; char* e=nullptr; bid=std::strtod(s+18,&e);
    if(!e||*e!=',')return false; ask=std::strtod(e+1,nullptr);
    ts=(dfc(y,(unsigned)mo,(unsigned)da)*86400+hh*3600+mi*60+se)*1000LL;
    if(ask<bid)std::swap(bid,ask); return bid>0&&ask>0; }

// M15 bar with bid/ask close for fills.
struct Bar { double o,h,l,c, cb,ca; };
struct Met{ int n=0,L=0,S=0,w=0; double net=0,gw=0,gl=0,mdd=0,peak=0,eq=0,sum=0,sum2=0;
    void add(double p,bool lng){ n++; if(lng)L++;else S++; net+=p; eq+=p; sum+=p; sum2+=p*p;
        if(p>0){w++;gw+=p;}else gl+=-p; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{return gl>0?gw/gl:(gw>0?99:0);} double wr()const{return n?100.0*w/n:0;}
    double sh(double tpy)const{ if(n<2)return 0; double m=sum/n,v=(sum2-sum*sum/n)/(n-1); return v>0?(m/std::sqrt(v))*std::sqrt(tpy):0;} };

int main(int argc,char** argv){
    if(argc<2){ std::printf("usage: bbproto <ticks.csv>\n"); return 1; }
    std::printf("loading + building M15 bars...\n");
    // Build M15 bars streaming (no full tick retention).
    std::vector<Bar> bars; bars.reserve(80000);
    int64_t bucket=-1; Bar cur{}; bool have=false; int64_t first_ts=0,last_ts=0;
    { std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
      std::string line;
      while(std::getline(in,line)){ if(line.empty())continue; int64_t ts; double bid,ask;
        if(!parse(line.c_str(),ts,bid,ask)) continue;
        if(!first_ts)first_ts=ts; last_ts=ts;
        const double mid=(bid+ask)*0.5; const int64_t b=(ts/900000)*900000;
        if(!have){ bucket=b; cur={mid,mid,mid,mid,bid,ask}; have=true; }
        else if(b!=bucket){ bars.push_back(cur); bucket=b; cur={mid,mid,mid,mid,bid,ask}; }
        else { cur.h=std::max(cur.h,mid); cur.l=std::min(cur.l,mid); cur.c=mid; cur.cb=bid; cur.ca=ask; } } }
    if(have) bars.push_back(cur);
    const int NB=(int)bars.size();
    const double span_yr=(last_ts-first_ts)/86400000.0/365.25;
    std::printf("M15 bars=%d  span=%.2f yr\n", NB, span_yr);

    // Indicators: ATR14, EMA20, H4 EMA50 (H4 = 16 M15 bars; approximate via EMA on M15 close with n=50*16? -> use slow EMA on M15).
    std::vector<double> atr(NB,0), ema20(NB,0), emaSlow(NB,0); // emaSlow ~ H4 trend proxy (EMA200 on M15 ~ 50h)
    { double a=0,pc=bars[0].c; int cnt=0; double seed=0;
      for(int i=1;i<NB;i++){ double tr=std::max(bars[i].h-bars[i].l,std::max(std::fabs(bars[i].h-pc),std::fabs(bars[i].l-pc))); pc=bars[i].c;
        if(cnt<14){seed+=tr; if(++cnt==14)a=seed/14;} else a=(a*13+tr)/14; atr[i]=a; } }
    { double e=bars[0].c; double k=2.0/21; for(int i=0;i<NB;i++){ e+= (i?k:1)*(bars[i].c-e); ema20[i]=e; } }
    { double e=bars[0].c; double k=2.0/201; for(int i=0;i<NB;i++){ e+= (i?k:1)*(bars[i].c-e); emaSlow[i]=e; } } // EMA200 M15 = ~50h HTF proxy

    auto run=[&](int strat,int N,double STOP,double RR,double K,int htf)->Met{
        // strat 0=Donchian breakout, 1=Keltner reversion. htf: 0 off,1 require-aligned.
        Met m; int dir=0; double entry=0,sl=0,tp=0; bool lng=false; int warm=std::max(N,200)+2;
        for(int i=warm;i<NB;i++){
            const Bar& b=bars[i];
            // manage open
            if(dir!=0){
                bool hitSL = lng ? (b.l<=sl) : (b.h>=sl);
                bool hitTP = lng ? (b.h>=tp) : (b.l<=tp);
                double exit=0; bool closed=false;
                if(hitSL){ exit=sl; closed=true; } else if(hitTP){ exit=tp; closed=true; }
                else if(strat==1){ // reversion: exit at mid (ema20) cross
                    if(lng? (b.c>=ema20[i]) : (b.c<=ema20[i])){ exit= lng? b.cb : b.ca; closed=true; } }
                if(closed){ double pnl= lng? (exit-entry):(entry-exit); m.add(pnl,lng); dir=0; }
                if(dir!=0) continue;
            }
            if(atr[i]<=0) continue;
            const bool up = emaSlow[i] < b.c;   // HTF proxy: price above slow EMA = uptrend
            if(strat==0){ // Donchian breakout (both dir)
                double hh=-1e18,ll=1e18; for(int j=i-N;j<i;j++){hh=std::max(hh,bars[j].h);ll=std::min(ll,bars[j].l);}
                if(b.c>hh){ if(htf&&!up)continue; dir=1;lng=true; entry=b.ca; sl=entry-STOP*atr[i]; tp=entry+RR*STOP*atr[i]; }
                else if(b.c<ll){ if(htf&&up)continue; dir=-1;lng=false; entry=b.cb; sl=entry+STOP*atr[i]; tp=entry-RR*STOP*atr[i]; }
            } else { // Keltner reversion (fade)
                double up_b=ema20[i]+K*atr[i], lo_b=ema20[i]-K*atr[i];
                if(b.c>up_b){ if(htf&&up)continue; dir=-1;lng=false; entry=b.cb; sl=entry+STOP*atr[i]; tp=ema20[i]; }
                else if(b.c<lo_b){ if(htf&&!up)continue; dir=1;lng=true; entry=b.ca; sl=entry-STOP*atr[i]; tp=ema20[i]; }
            }
        }
        return m;
    };

    auto row=[&](const char* lbl,Met m){ double tpy=span_yr>0?m.n/span_yr:0;
        std::printf("%-32s | %5d | %3d/%-4d | %4.1f | %5.2f | %6.2f | %8.1f | %6.0f\n",
            lbl,m.n,m.L,m.S,m.wr(),m.pf(),m.sh(tpy),m.net,m.mdd); };
    std::printf("\n%-32s | %5s | long/short| %4s | %5s | %6s | %8s | %6s\n","config","trd","WR%","PF","Sharpe","net","maxDD");
    std::printf("%s\n",std::string(96,'-').c_str());

    std::printf("-- A) DONCHIAN M15 breakout --\n");
    row("Donchian N20 stop2 RR1.5 htfOFF", run(0,20,2.0,1.5,0,0));
    row("Donchian N20 stop2 RR1.5 htfON ", run(0,20,2.0,1.5,0,1));
    row("Donchian N20 stop1.5 RR2 htfON ", run(0,20,1.5,2.0,0,1));
    row("Donchian N40 stop2 RR1.5 htfON ", run(0,40,2.0,1.5,0,1));
    row("Donchian N10 stop2 RR1.5 htfON ", run(0,10,2.0,1.5,0,1));
    std::printf("-- B) KELTNER M15 reversion --\n");
    row("Keltner K2 stop2 htfOFF        ", run(1,0,2.0,0,2.0,0));
    row("Keltner K2.5 stop2 htfOFF      ", run(1,0,2.0,0,2.5,0));
    row("Keltner K2 stop2 counter-trend ", run(1,0,2.0,0,2.0,1));
    return 0;
}
