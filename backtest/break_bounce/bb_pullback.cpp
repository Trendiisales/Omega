// bb_pullback.cpp -- tune the trend-aligned pullback-reversion for tighter TFs.
//
// The only intraday archetype that worked (bb_proto): buy the lower-Keltner dip
// in an uptrend / sell the upper-band rip in a downtrend (mean-revert WITH the
// HTF trend). This sweeps it to find a viable tight-TF config: TF (M5/M10/M15)
// x band width K x exit mode (mid / RR target) x stop, with IS/OOS split and a
// price give-back lock. Real bid/ask fills.
//
// Build: g++ -O3 -std=c++17 bb_pullback.cpp -o bbpull
// Run:   ./bbpull <ticks.csv>

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

struct Bar { int64_t ts; double o,h,l,c,cb,ca; };
static std::vector<Bar> aggregate(const std::vector<Bar>& m5, int k){
    if(k<=1) return m5;
    std::vector<Bar> out; out.reserve(m5.size()/k+1);
    for(size_t i=0;i<m5.size();i+=k){ Bar b=m5[i];
        for(size_t j=i+1;j<i+k && j<m5.size(); ++j){ b.h=std::max(b.h,m5[j].h); b.l=std::min(b.l,m5[j].l);
            b.c=m5[j].c; b.cb=m5[j].cb; b.ca=m5[j].ca; }
        out.push_back(b); }
    return out;
}
struct Met{ int n=0,L=0,S=0,w=0; double net=0,gw=0,gl=0,mdd=0,peak=0,eq=0,sum=0,sum2=0;
    void add(double p,bool lng){ n++; if(lng)L++;else S++; net+=p; eq+=p; sum+=p; sum2+=p*p;
        if(p>0){w++;gw+=p;}else gl+=-p; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{return gl>0?gw/gl:(gw>0?99:0);} double wr()const{return n?100.0*w/n:0;}
    double sh(double tpy)const{ if(n<2)return 0; double m=sum/n,v=(sum2-sum*sum/n)/(n-1); return v>0?(m/std::sqrt(v))*std::sqrt(tpy):0;} };

int main(int argc,char** argv){
    if(argc<2){ std::printf("usage: bbpull <ticks.csv>\n"); return 1; }
    std::printf("building M5 bars...\n");
    std::vector<Bar> m5; m5.reserve(200000);
    int64_t bucket=-1; Bar cur{}; bool have=false; int64_t first_ts=0,last_ts=0;
    { std::ifstream in(argv[1]); if(!in.is_open()){std::printf("open fail\n");return 1;}
      std::string line; while(std::getline(in,line)){ if(line.empty())continue; int64_t ts; double bid,ask;
        if(!parse(line.c_str(),ts,bid,ask))continue; if(!first_ts)first_ts=ts; last_ts=ts;
        const double mid=(bid+ask)*0.5; const int64_t b=(ts/300000)*300000;
        if(!have){bucket=b;cur={b,mid,mid,mid,mid,bid,ask};have=true;}
        else if(b!=bucket){m5.push_back(cur);bucket=b;cur={b,mid,mid,mid,mid,bid,ask};}
        else{cur.h=std::max(cur.h,mid);cur.l=std::min(cur.l,mid);cur.c=mid;cur.cb=bid;cur.ca=ask;} } }
    if(have)m5.push_back(cur);
    const double span_yr=(last_ts-first_ts)/86400000.0/365.25;
    std::printf("M5 bars=%zu span=%.2fyr\n\n", m5.size(), span_yr);

    // strat: trend-aligned pullback reversion. exit 0=mid(EMA), 1=RR target.
    auto run=[&](int tf_mult,int emaN,int slowN,double K,double STOP,double RR,int exitMode,bool plock,
                 Met& is,Met& oos){
        std::vector<Bar> bars=aggregate(m5,tf_mult); int NB=bars.size();
        if(NB<slowN+30) return;
        std::vector<double> atr(NB,0),emaM(NB,0),emaS(NB,0);
        { double a=0,pc=bars[0].c; int c=0; double sd=0; for(int i=1;i<NB;i++){ double tr=std::max(bars[i].h-bars[i].l,std::max(std::fabs(bars[i].h-pc),std::fabs(bars[i].l-pc))); pc=bars[i].c; if(c<14){sd+=tr;if(++c==14)a=sd/14;}else a=(a*13+tr)/14; atr[i]=a; } }
        { double e=bars[0].c,k=2.0/(emaN+1); for(int i=0;i<NB;i++){e+=(i?k:1)*(bars[i].c-e);emaM[i]=e;} }
        { double e=bars[0].c,k=2.0/(slowN+1); for(int i=0;i<NB;i++){e+=(i?k:1)*(bars[i].c-e);emaS[i]=e;} }
        const int64_t split=first_ts+(int64_t)((last_ts-first_ts)*0.60);
        const double span=(last_ts-first_ts)/86400000.0/365.25;
        int dir=0; double entry=0,sl=0,tp=0,peak=0; bool lng=false;
        int warm=std::max(slowN,20)+2;
        for(int i=warm;i<NB;i++){ const Bar& b=bars[i];
            if(dir!=0){
                const double px=lng?b.cb:b.ca;
                peak=lng?std::max(peak,px):std::min(peak,px);
                bool hitSL=lng?(b.l<=sl):(b.h>=sl);
                bool hitTP=(exitMode==1)&&(lng?(b.h>=tp):(b.l<=tp));
                bool hitMid=(exitMode==0)&&(lng?(b.c>=emaM[i]):(b.c<=emaM[i]));
                // price give-back lock: arm 1R, lock 0.5atr after 0.5atr giveback
                double lockpx=0; bool lockhit=false;
                if(plock){ double fav=lng?(px-entry):(entry-px); double risk=STOP*atr[i];
                    if(risk>0 && fav>=risk){ double gb=lng?(peak-px):(px-peak);
                        if(gb>=0.5*atr[i]){ lockpx=lng?peak-0.5*atr[i]:peak+0.5*atr[i];
                            lockhit=lng?(b.l<=lockpx):(b.h>=lockpx); } } }
                double exit=0; bool closed=false;
                if(hitSL){exit=sl;closed=true;} else if(hitTP){exit=tp;closed=true;}
                else if(lockhit){exit=lockpx;closed=true;} else if(hitMid){exit=lng?b.cb:b.ca;closed=true;}
                if(closed){ double pnl=lng?(exit-entry):(entry-exit);
                    (b.ts<split?is:oos).add(pnl,lng); dir=0; }
                if(dir!=0) continue;
            }
            if(atr[i]<=0) continue;
            const bool up=emaS[i]<b.c;
            const double lo=emaM[i]-K*atr[i], hi=emaM[i]+K*atr[i];
            if(up && b.c<lo){ dir=1;lng=true; entry=b.ca; peak=b.cb; sl=entry-STOP*atr[i]; tp=entry+RR*STOP*atr[i]; }
            else if(!up && b.c>hi){ dir=-1;lng=false; entry=b.cb; peak=b.ca; sl=entry+STOP*atr[i]; tp=entry-RR*STOP*atr[i]; }
        }
        (void)span;
    };

    std::printf("%-26s | %5s %5s %5s | %5s %5s %5s %7s %6s\n",
        "config","ISpf","ISsh","ISn","OOpf","OOsh","OOn","OOnet","OOdd");
    std::printf("%s\n",std::string(82,'-').c_str());
    struct C{const char* l;int tf,ema,slow;double K,stop,rr;int exit;bool plock;};
    C grid[]={
        {"M15 K2 mid stop2",3,20,200,2.0,2.0,0,0,true},
        {"M15 K2 RR1.5 stop1.5",3,20,200,2.0,1.5,1.5,1,true},
        {"M15 K2 RR2 stop1.5",3,20,200,2.0,1.5,2.0,1,true},
        {"M15 K2.5 mid stop2",3,20,200,2.5,2.0,0,0,true},
        {"M15 K1.5 RR2 stop1.5",3,20,200,1.5,1.5,2.0,1,true},
        {"M10 K2 RR1.5 stop1.5",2,20,200,2.0,1.5,1.5,1,true},
        {"M10 K2 mid stop2",2,20,200,2.0,2.0,0,0,true},
        {"M5  K2 RR1.5 stop1.5",1,20,200,2.0,1.5,1.5,1,true},
        {"M5  K2.5 RR2 stop1.5",1,20,200,2.5,1.5,2.0,1,true},
        {"M5  K2 mid stop2",1,20,200,2.0,2.0,0,0,true},
        {"M15 K2 RR1.5 noLock",3,20,200,2.0,1.5,1.5,1,false},
        {"M15 K2 RR2 slow100",3,20,100,2.0,1.5,2.0,1,true},
    };
    for(auto& c:grid){ Met is,oos; run(c.tf,c.ema,c.slow,c.K,c.stop,c.rr,c.exit,c.plock,is,oos);
        double tpyI=is.n/(span_yr*0.6), tpyO=oos.n/(span_yr*0.4);
        std::printf("%-26s | %5.2f %5.2f %5d | %5.2f %5.2f %5d %7.1f %6.0f\n",
            c.l, is.pf(),is.sh(tpyI),is.n, oos.pf(),oos.sh(tpyO),oos.n,oos.net,oos.mdd); }
    return 0;
}
