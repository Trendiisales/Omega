// =============================================================================
// gold_orb_sweep.cpp -- gold session opening-range-breakout edge sweep.
// Anchored OR at London(07/08 UTC) / NY(1330 UTC) opens; 1 trade/session; flat
// by session end. Proven index archetype (Peachy ORB), tuned for XAUUSD.
// Real M5 (2yr), real round-trip cost (arg, default $0.22). No-lookahead:
// signal on bar close -> fill next bar open. Robust metrics per config.
// BUILD: c++ -std=c++17 -O2 backtest/gold_orb_sweep.cpp -o backtest/gold_orb_sweep
// RUN:   ./backtest/gold_orb_sweep [cost] 2> rows.csv
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
using namespace std;
struct Bar{long ts;double o,h,l,c;int mod;int day;}; // mod=minute-of-day, day=utc day idx

static vector<Bar> load(const char* p){
    vector<Bar> v; FILE* f=fopen(p,"r"); if(!f){perror(p);return v;}
    char ln[256]; bool first=true; double po=0;
    while(fgets(ln,sizeof ln,f)){
        if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
        long ts; double o,h,l,c;
        if(sscanf(ln,"%ld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;
        if(o<=0||h<=0||l<=0||c<=0)continue;
        if(po>0&&(o>po*1.5||o<po*0.5))continue;
        if(h-l>0.2*c)continue;
        v.push_back({ts,o,h,l,c,(int)((ts/60)%1440),(int)(ts/86400)}); po=c;
    }
    fclose(f); return v;
}

struct P{
    int anchor;    // minute-of-day session anchor (London 420/480, NY 810)
    int or_min;    // opening-range length (min)
    int entry_win; // entry allowed until anchor+entry_win (min)
    int flat_min;  // force flat at this minute-of-day
    double buf;    // breakout buffer in ATR
    double stopmode; // 0=OR-opposite, >0 = ATR mult
    double maxstop;  // cap stop dist in ATR (0=off)
    int exitm;     // 0=fixedTP_R, 1=trail, 2=hold-to-flat
    double tpR; double trail;
    int be; double be_arm;
    int dir;       // 0=long,1=both
    double minor;  // require OR size >= minor*ATR (0=off)
    int trend;     // 0=off,1=ema50>200 gate
};
struct Res{int n,win;double gw,gl,net,h1,h2;double top[5];double worst,maxdd,sumR;};
static FILE* g_dump=nullptr;

static Res run(const vector<Bar>&B,const P&p,double cost,long tmid){
    Res R{0,0,0,0,0,0,0,{0,0,0,0,0},0,0,0};
    int N=B.size();
    double ef=0,es=0;bool eok=false; double kf=2.0/51,ks=2.0/201;
    double atr=0,asum=0;int ac=0;bool aw=false;const int AP=14;double pc=0;
    int curday=-1; bool or_done=false,traded=false; double orh=-1e18,orl=1e18; double oratr=0;
    bool open=false,islong=false;double entry=0,stopd=0,stopl=0,tp=0,eatr=0,extreme=0,mfe=0;int held=0;long ets=0;
    bool pend=false,plong=false; double eq=0,peak=0;
    for(int i=0;i<N;i++){
        const Bar&b=B[i];
        if(b.day!=curday){ curday=b.day; or_done=false; traded=false; orh=-1e18; orl=1e18; }
        // pending fill at open
        if(pend&&!open&&eatr>0){
            open=true;islong=plong;entry=b.o;held=0;mfe=0;extreme=b.o;ets=b.ts;
            stopd = p.stopmode>0? p.stopmode*eatr : (islong? entry-orl : orh-entry);
            if(stopd<=0) stopd=1.0*eatr;
            if(p.maxstop>0 && stopd>p.maxstop*eatr) stopd=p.maxstop*eatr;
            stopl=islong?entry-stopd:entry+stopd;
            tp=islong?entry+p.tpR*stopd:entry-p.tpR*stopd;
        }
        pend=false;
        // manage
        if(open){
            held++;
            double fav=islong?(b.h-entry):(entry-b.l); if(fav>mfe)mfe=fav;
            if(islong){if(b.h>extreme)extreme=b.h;}else{if(b.l<extreme)extreme=b.l;}
            if(p.be&&mfe>=p.be_arm*stopd){double be=islong?entry+cost:entry-cost; if(islong)stopl=max(stopl,be);else stopl=min(stopl,be);}
            if(p.exitm==1){double t=islong?extreme-p.trail*eatr:extreme+p.trail*eatr; if(islong)stopl=max(stopl,t);else stopl=min(stopl,t);}
            bool ex=false;double xp=0;
            if(islong){ if(b.l<=stopl){ex=true;xp=stopl;} else if(p.exitm==0&&b.h>=tp){ex=true;xp=tp;} }
            else      { if(b.h>=stopl){ex=true;xp=stopl;} else if(p.exitm==0&&b.l<=tp){ex=true;xp=tp;} }
            if(!ex && b.mod>=p.flat_min){ex=true;xp=b.c;}          // force flat at session end
            if(ex){
                double pnl=(islong?(xp-entry):(entry-xp))-cost; double Rm=stopd>0?pnl/stopd:0;
                R.n++;R.net+=pnl;R.sumR+=Rm; if(pnl>0){R.win++;R.gw+=pnl;}else R.gl+=-pnl;
                if(ets<tmid)R.h1+=pnl;else R.h2+=pnl; if(pnl<R.worst)R.worst=pnl;
                for(int t=0;t<5;t++){if(pnl>R.top[t]){for(int u=4;u>t;u--)R.top[u]=R.top[u-1];R.top[t]=pnl;break;}}
                eq+=pnl;if(eq>peak)peak=eq;double dd=peak-eq;if(dd>R.maxdd)R.maxdd=dd;
                if(g_dump)fprintf(g_dump,"%ld,%s,%.4f\n",ets,islong?"L":"S",pnl);
                open=false;
            }
        }
        // indicators
        if(pc>0){double tr=max(b.h-b.l,max(fabs(b.h-pc),fabs(b.l-pc))); if(!aw){asum+=tr;if(++ac==AP){atr=asum/AP;aw=true;}}else atr=(atr*(AP-1)+tr)/AP;}
        ef=eok?ef+kf*(b.c-ef):b.c; es=eok?es+ks*(b.c-es):b.c; eok=true;
        // session OR build
        if(aw&&atr>0){
            int rel=b.mod-p.anchor;
            if(rel>=0&&rel<p.or_min){ if(b.h>orh)orh=b.h; if(b.l<orl)orl=b.l; }
            else if(rel>=p.or_min && orh>-1e17) or_done=true;
            // entry signal
            if(or_done&&!open&&!traded&&rel>=p.or_min&&rel<=p.entry_win){
                bool orok=(p.minor<=0)||((orh-orl)>=p.minor*atr);
                bool tu=(p.trend==0)||(ef>es), td=(p.trend==0)||(ef<es);
                bool bu=b.c>orh+p.buf*atr, bd=b.c<orl-p.buf*atr;
                if(orok){
                    if(tu&&bu){pend=true;plong=true;eatr=atr;traded=true;}
                    else if(p.dir==1&&td&&bd){pend=true;plong=false;eatr=atr;traded=true;}
                }
            }
        }
        pc=b.c;
    }
    return R;
}

int main(int argc,char**argv){
    double cost=argc>1?atof(argv[1]):0.22;
    auto B=load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m5.csv");
    fprintf(stderr,"loaded %zu M5 bars cost=$%.2f\n",B.size(),cost);
    long tmid=(B.front().ts+B.back().ts)/2;
    if(getenv("DUMP")){
        // best cluster config: NY1330, or15, trend on, dir both, hold, stop1.0ATR, be off
        P bp{810,15,810+120,1190,0.0,1.0,2.5,2,0,0,0,1.0,0,0.0,1};
        g_dump=fopen("/tmp/orb_best_trades.csv","w");
        run(B,bp,cost,tmid);
        fclose(g_dump); g_dump=nullptr;
        fprintf(stderr,"dumped best-config trades to /tmp/orb_best_trades.csv\n");
        return 0;
    }
    int anchors[]={420,480,810};      // London07, London08, NY13:30
    const char* anm[]={"LON07","LON08","NY1330"};
    int ormins[]={15,30,60};
    int ewins[]={120,240};            // entry window after anchor (min)
    double bufs[]={0.0,0.1};
    double stopms[]={0.0,1.0,1.5};    // 0=OR-opposite else ATR
    int exits[]={0,0,1,2}; double tps[]={1.5,2.5,0,0}; double trls[]={0,0,2.5,0};
    int bes[]={0,1}; int dirs[]={0,1}; double minors[]={0.0,0.5}; int trends[]={0,1};
    int flat=1190; // ~19:50 UTC force flat
    fprintf(stderr,"ROW,anchor,ormin,ewin,buf,stopm,exitm,tpR,trail,be,dir,minor,trend,n,wr,pf,net,avgR,h1,h2,net_ex5,maxdd,worst\n");
    for(int a=0;a<3;a++)for(int om:ormins)for(int ew:ewins)for(double bf:bufs)for(double sm:stopms)
    for(int xi=0;xi<4;xi++)for(int be:bes)for(int dr:dirs)for(double mo:minors)for(int tr:trends){
        P p{anchors[a],om,anchors[a]+ew,flat,bf,sm,2.5,exits[xi],tps[xi],trls[xi],be,1.0,dr,mo,tr};
        Res R=run(B,p,cost,tmid);
        if(R.n<20)continue;
        double pf=R.gl>0?R.gw/R.gl:(R.gw>0?99:0),wr=100.0*R.win/R.n,avgR=R.sumR/R.n;
        double tops=R.top[0]+R.top[1]+R.top[2]+R.top[3]+R.top[4];
        fprintf(stderr,"ROW,%s,%d,%d,%.1f,%.1f,%d,%.1f,%.1f,%d,%d,%.1f,%d,%d,%.1f,%.2f,%.1f,%.3f,%.1f,%.1f,%.1f,%.1f,%.2f\n",
            anm[a],om,ew,bf,sm,exits[xi],tps[xi],trls[xi],be,dr,mo,tr,
            R.n,wr,pf,R.net,avgR,R.h1,R.h2,R.net-tops,R.maxdd,R.worst);
    }
    return 0;
}
