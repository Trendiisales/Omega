// tick_scalp_gold.cpp — TICK-LEVEL execution test of the EMA9-pullback scalp on gold.
// Signal on m5 bars (EMA9 + bias + VWAP gate, like the harness). EXECUTION on real
// ticks with REAL bid/ask: long entry = LIMIT at the EMA level (fills at the level
// when ask trades down to it -> you EARN the spread on entry); exits fill by crossing
// (stop hit at bid, you pay the spread on exit). No assumed cost -- the real 0.33pt
// spread is paid exactly where microstructure says. Resolves intrabar stop/target order.
// run: ./tick_scalp_gold <xau_tick.csv> [ENTRYMODE=limit|market] [EXITMODE=trail|r]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <deque>
using namespace std;

static int et_off(int64_t ts){ time_t t=ts; struct tm g; gmtime_r(&t,&g); int y=g.tm_year+1900,mo=g.tm_mon+1,d=g.tm_mday;
    auto dow=[](int Y,int M,int D){ struct tm v{}; v.tm_year=Y-1900;v.tm_mon=M-1;v.tm_mday=D;v.tm_hour=12; time_t tt=timegm(&v); struct tm o; gmtime_r(&tt,&o); return o.tm_wday;};
    int mar=14; for(int dd=8;dd<=14;dd++) if(dow(y,3,dd)==0){mar=dd;break;} int nov=7; for(int dd=1;dd<=7;dd++) if(dow(y,11,dd)==0){nov=dd;break;}
    bool dst; if(mo<3||mo>11)dst=false; else if(mo>3&&mo<11)dst=true; else if(mo==3)dst=(d>=mar); else dst=(d<nov); return dst?-4:-5; }
static int et_hm(int64_t ts){ int64_t lt=ts+et_off(ts)*3600; int s=((lt%86400)+86400)%86400; return (s/3600)*100+(s%3600)/60; }
static int64_t et_day(int64_t ts){ int64_t lt=ts+et_off(ts)*3600; return (lt-((lt%86400)+86400)%86400)/86400; }

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <tick.csv>\n",argv[0]);return 1;}
    string ENTRY = getenv("ENTRYMODE")?getenv("ENTRYMODE"):"limit";
    string EXIT  = getenv("EXITMODE")?getenv("EXITMODE"):"trail";
    double Rmult = getenv("R")?atof(getenv("R")):2.0;
    int    barSec= getenv("BARSEC")?atoi(getenv("BARSEC")):300;
    int    emaN  = 9;
    double bufA  = 0.10; int trailWin=3;

    ifstream f(argv[1]); if(!f){fprintf(stderr,"no file\n");return 1;}
    // rolling bar + indicators
    int64_t curbar=-1; double bo=0,bh=0,bl=0,bc=0;
    double ema9=0,ema50=0,atr=0,prevc=0; bool einit=false; int64_t cday=-1; double pv=0,vv=0,vwap=0;
    deque<double> lows,highs; double k9=2.0/(emaN+1),k50=2.0/51;
    // bar-close state used to arm signals
    bool armed=false; int bias=0; double entry_lvl=0, react_stop=0; int64_t lastTradeDay=-1; bool tradedToday=false;
    // open position
    bool inpos=false; int side=0; double ep=0,sl=0; double trailStop=0; int64_t entryDay=-1;
    // stats
    long n=0,w=0; double gw=0,gl=0,sumR=0; long entries=0;
    vector<double> Rs; vector<int> Ys;

    auto onBarClose=[&](int64_t bs,double o,double h,double l,double c){
        if(prevc>0){ double tr=fmax(h-l,fmax(fabs(h-prevc),fabs(l-prevc))); static double a=0; static int cnt=0;
            cnt++; a = cnt<=14? (a*(cnt-1)+tr)/cnt : a+(tr-a)/14; atr=a; }
        prevc=c;
        if(!einit){ema9=c;ema50=c;einit=true;} else {ema9+=k9*(c-ema9); ema50+=k50*(c-ema50);}
        int64_t d=et_day(bs); if(d!=cday){cday=d;pv=0;vv=0;} pv+=(h+l+c)/3.0; vv+=1; vwap=pv/vv;
        lows.push_back(l); highs.push_back(h); while((int)lows.size()>trailWin)lows.pop_front(); while((int)highs.size()>trailWin)highs.pop_front();
        // signal (only 10:00-16:00 ET, one/day, no open pos)
        int hm=et_hm(bs); int64_t day=et_day(bs);
        if(day!=lastTradeDay){lastTradeDay=day;tradedToday=false;}
        if(inpos||tradedToday||armed) return;
        if(hm<1000||hm>=1600) return; if(atr<=0) return;
        int b = c>ema9?1:(c<ema9?-1:0); if(!b) return;
        if(b>0 && !(ema9>vwap)) return; if(b<0 && !(ema9<vwap)) return;
        // pullback: this bar touched ema9 from the bias side
        bool touch = b>0 ? (l<=ema9) : (h>=ema9);
        if(!touch) return;
        bias=b; entry_lvl=ema9; react_stop = b>0 ? (l-bufA*atr) : (h+bufA*atr); armed=true;
    };

    string ln; bool first=true;
    while(getline(f,ln)){
        if(ln.empty())continue; if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
        if(ln[0]<'0'||ln[0]>'9')continue;
        // YYYYMMDD,HH:MM:SS,bid,ask
        char db[9]; memcpy(db,ln.c_str(),8); db[8]=0; long ymd=atol(db);
        const char* p=ln.c_str()+9; int hh=(p[0]-'0')*10+p[1]-'0',mm=(p[3]-'0')*10+p[4]-'0',ss=(p[6]-'0')*10+p[7]-'0';
        struct tm tmv{}; tmv.tm_year=ymd/10000-1900;tmv.tm_mon=(ymd/100)%100-1;tmv.tm_mday=ymd%100;tmv.tm_hour=hh;tmv.tm_min=mm;tmv.tm_sec=ss;
        int64_t ts=(int64_t)timegm(&tmv);
        const char* q=strchr(ln.c_str()+9,','); if(!q)continue; double bid=strtod(q+1,nullptr);
        const char* r=strchr(q+1,','); if(!r)continue; double ask=strtod(r+1,nullptr);
        if(bid<=0||ask<=0)continue; double mid=(bid+ask)*0.5;
        // ---- tick execution FIRST (uses live bid/ask) ----
        if(inpos){
            // trail
            if(EXIT=="trail"){
                if(side>0){ double s=1e18; for(double v:lows)s=fmin(s,v); s-=bufA*atr; if(s>trailStop)trailStop=s;
                    if(bid<=trailStop){ double R=(trailStop-ep)/(ep-sl); // exit at bid (cross)
                        sumR+=R; if(R>0){w++;gw+=R;}else gl+=-R; Rs.push_back(R); Ys.push_back((int)(ymd/10000)); inpos=false; tradedToday=true; } }
                else { double s=-1e18; for(double v:highs)s=fmax(s,v); s+=bufA*atr; if(s<trailStop)trailStop=s;
                    if(ask>=trailStop){ double R=(ep-trailStop)/(sl-ep); sumR+=R; if(R>0){w++;gw+=R;}else gl+=-R; Rs.push_back(R); Ys.push_back((int)(ymd/10000)); inpos=false; tradedToday=true; } }
            } else {
                double tgt = side>0? ep+Rmult*(ep-sl) : ep-Rmult*(sl-ep);
                if(side>0){ if(bid<=sl){double R=-1; sumR+=R;gl+=1;Rs.push_back(R);Ys.push_back((int)(ymd/10000));inpos=false;tradedToday=true;}
                            else if(bid>=tgt){double R=Rmult;sumR+=R;w++;gw+=R;Rs.push_back(R);Ys.push_back((int)(ymd/10000));inpos=false;tradedToday=true;} }
                else { if(ask>=sl){double R=-1;sumR+=R;gl+=1;Rs.push_back(R);Ys.push_back((int)(ymd/10000));inpos=false;tradedToday=true;}
                       else if(ask<=tgt){double R=Rmult;sumR+=R;w++;gw+=R;Rs.push_back(R);Ys.push_back((int)(ymd/10000));inpos=false;tradedToday=true;} }
            }
            int hm=et_hm(ts); if(inpos && hm>=1600){ double xp= side>0?bid:ask; double R= side>0?(xp-ep)/(ep-sl):(ep-xp)/(sl-ep);
                sumR+=R; if(R>0){w++;gw+=R;}else gl+=-R; Rs.push_back(R); Ys.push_back((int)(ymd/10000)); inpos=false; tradedToday=true; }
        }
        // ---- fill an armed entry at tick level ----
        if(!inpos && armed){
            bool fill=false; double fillpx=0;
            if(ENTRY=="limit"){
                // limit at level: long fills when ask<=level (you buy the offer down at level)
                if(bias>0 && ask<=entry_lvl){ fill=true; fillpx=entry_lvl; }
                if(bias<0 && bid>=entry_lvl){ fill=true; fillpx=entry_lvl; }
            } else { // market: cross spread immediately on touch
                if(bias>0 && mid<=entry_lvl){ fill=true; fillpx=ask; }
                if(bias<0 && mid>=entry_lvl){ fill=true; fillpx=bid; }
            }
            if(fill){ double risk = bias>0?(fillpx-react_stop):(react_stop-fillpx);
                if(risk>0.01){ inpos=true; side=bias; ep=fillpx; sl=react_stop; trailStop=react_stop; entries++; }
                armed=false; }
            // cancel arm if day ends
            if(et_hm(ts)>=1600){ armed=false; }
        }
        // ---- bar aggregation ----
        int64_t bs=(ts/barSec)*barSec;
        if(curbar<0){curbar=bs;bo=bh=bl=bc=mid;}
        else if(bs!=curbar){ onBarClose(curbar,bo,bh,bl,bc); curbar=bs; bo=bh=bl=bc=mid; }
        else { if(mid>bh)bh=mid; if(mid<bl)bl=mid; bc=mid; }
        n++;
    }
    int N=Rs.size(); double pf= gl>0?gw/gl:99, mean= N?sumR/N:0;
    int wins=0; for(double R:Rs) if(R>0)wins++;
    printf("=== TICK-LEVEL GOLD SCALP  ENTRY=%s EXIT=%s barSec=%d (REAL bid/ask, no assumed cost) ===\n",ENTRY.c_str(),EXIT.c_str(),barSec);
    printf("ticks=%ld entries=%ld closed=%d WR=%.1f%% PF=%.2f avgR=%+.3f totR=%+.1f\n",n,entries,N,N?100.0*wins/N:0,pf,mean,sumR);
    // halves
    if(N>1){ double h1=0,h2=0; int m=N/2; for(int i=0;i<m;i++)h1+=Rs[i]; for(int i=m;i<N;i++)h2+=Rs[i];
        printf("  WF: H1 totR=%+.1f (n=%d)  H2 totR=%+.1f (n=%d)\n",h1,m,h2,N-m); }
    return 0;
}
