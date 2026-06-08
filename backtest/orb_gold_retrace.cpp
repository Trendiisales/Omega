// orb_gold_retrace.cpp — Peachy GOLD ORB 50%-retrace continuation.
// Spec (LONG; SHORT symmetric):
//   ORB = high/low of the first 30 min from 08:20 ET (COMEX gold open) = 6 x m5 bars.
//   Breakout: a bar CLOSES above ORBhigh -> bullish bias for the day (below -> bearish).
//   Entry: after breakout, wait a retrace to 50% of ORB (mid). Enter LONG on touch of mid.
//   Target: ORBhigh (the "magnet"). [--ext targets prior-swing/high-of-day proxy instead]
//   Stop: loose = ORBlow ; tight = retrace reaction-bar low - buf  (env STOP=loose|tight)
//   Trend filter (her "larger POV"): env TREND=1 -> only long if mid > slowEMA (else skip).
//   One side per day (the breakout side); re-entries allowed up to MAXTRADES; EOD flat.
// Cost-incl (pts), WF both-halves, per-year, env COST/STOP/TREND/EXT/MAXTRADES.
// build: g++ -std=c++17 -O2 orb_gold_retrace.cpp -o orb_gold
// run:   ./orb_gold <m5_barfile>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
using namespace std;
struct Bar{ int64_t ts; double o,h,l,c; };

// US Eastern offset (-4 EDT / -5 EST). DST: 2nd Sun Mar 07:00 UTC .. 1st Sun Nov 06:00 UTC.
static int et_off(int64_t ts){
    time_t t=ts; struct tm g; gmtime_r(&t,&g); int y=g.tm_year+1900,mo=g.tm_mon+1,d=g.tm_mday;
    // compute DST boundaries (approx, sufficient for window logic)
    auto dow=[](int Y,int M,int D){ struct tm tmv{}; tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D; tmv.tm_hour=12; time_t tt=timegm(&tmv); struct tm o; gmtime_r(&tt,&o); return o.tm_wday; };
    int marSun=14; for(int dd=8;dd<=14;dd++) if(dow(y,3,dd)==0){marSun=dd;break;}     // 2nd Sunday
    int novSun=7;  for(int dd=1;dd<=7;dd++)  if(dow(y,11,dd)==0){novSun=dd;break;}    // 1st Sunday
    bool dst;
    if(mo<3||mo>11) dst=false; else if(mo>3&&mo<11) dst=true;
    else if(mo==3) dst=(d>marSun||(d==marSun)); else dst=(d<novSun);
    return dst?-4:-5;
}
static int et_hm(int64_t ts){ int off=et_off(ts); int64_t lt=ts+off*3600; int s=(int)(((lt%86400)+86400)%86400); return (s/3600)*100+(s%3600)/60; }
static int64_t et_day(int64_t ts){ int off=et_off(ts); int64_t lt=ts+off*3600; return (lt - ((lt%86400)+86400)%86400)/86400; }
static int yearOf(int64_t ts){ time_t t=ts; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <m5file>\n",argv[0]); return 1; }
    string STOP = getenv("STOP")? getenv("STOP"):"tight";
    int    TREND= getenv("TREND")? atoi(getenv("TREND")):0;
    int    EXT  = getenv("EXT")?   atoi(getenv("EXT")):0;        // 1 -> target high/low of day instead of ORB edge
    int    MAXTR= getenv("MAXTRADES")? atoi(getenv("MAXTRADES")):2;
    double COST = getenv("COST")? atof(getenv("COST")):0.5;      // round-trip pts (gold)
    double bufA = 0.10;                                          // tight-stop buffer (ATR frac)
    int    emaN = getenv("EMAN")? atoi(getenv("EMAN")):50;
    double RETR = getenv("RETR")? atof(getenv("RETR")):0.5;

    // load m5
    vector<Bar> B; { ifstream f(argv[1]); string ln; bool first=true;
        while(getline(f,ln)){ if(ln.empty())continue; if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
            const char* s=ln.c_str(); char* e=nullptr; long long ts=strtoll(s,&e,10); if(*e!=',')continue;
            if(ts>100000000000LL) ts/=1000;   // accept ms timestamps (dukascopy)
            Bar b; b.ts=ts; b.o=strtod(e+1,&e); if(*e!=',')continue; b.h=strtod(e+1,&e); if(*e!=',')continue;
            b.l=strtod(e+1,&e); if(*e!=',')continue; b.c=strtod(e+1,&e); if(b.h>0) B.push_back(b);} }
    int n=B.size(); if(n<500){ fprintf(stderr,"too few bars %d\n",n); return 1; }
    sort(B.begin(),B.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});

    // slow EMA + ATR
    vector<double> ema(n,0), atr(n,0);
    { double k=2.0/(emaN+1); ema[0]=B[0].c; for(int i=1;i<n;i++) ema[i]=ema[i-1]+k*(B[i].c-ema[i-1]);
      double a=0; int W=14; for(int i=1;i<n;i++){ double tr=max(B[i].h-B[i].l,max(fabs(B[i].h-B[i-1].c),fabs(B[i].l-B[i-1].c)));
        a = (i<=W)? (a*(i-1)+tr)/i : a+(tr-a)/W; atr[i]=a; } }

    struct Tr{ int64_t ts; double R; int yr; };
    vector<Tr> trades; long days=0, hasOrb=0, broke=0, entered=0;

    int i=0;
    while(i<n){
        int64_t day=et_day(B[i].ts);
        // collect this day's bars [ds,de)
        int ds=i; while(i<n && et_day(B[i].ts)==day) i++; int de=i;
        days++;
        // ORB window: OR_START (hhmm ET) .. +OR_LEN min  [gold 0820/30 default]
        static int ORS = getenv("OR_START")? atoi(getenv("OR_START")):820;
        static int ORL = getenv("OR_LEN")? atoi(getenv("OR_LEN")):30;
        int orsMin=(ORS/100)*60+(ORS%100); int oreMin=orsMin+ORL; int ORE=(oreMin/60)*100+(oreMin%60);
        double orbH=-1e18, orbL=1e18; int orbEnd=-1;
        for(int k=ds;k<de;k++){ int hm=et_hm(B[k].ts); if(hm>=ORS&&hm<ORE){ orbH=max(orbH,B[k].h); orbL=min(orbL,B[k].l); orbEnd=k; } }
        if(orbEnd<0||orbH<=orbL) continue; hasOrb++;
        double range=orbH-orbL; double mid=(orbH+orbL)*0.5; /* retrace level set per-dir below */
        // find first breakout close beyond ORB after orbEnd, within the cash day (<=1600 ET)
        int dir=0, bk=-1;
        for(int k=orbEnd+1;k<de;k++){ int hm=et_hm(B[k].ts); if(hm>=1600) break;
            if(B[k].c>orbH){ dir=+1; bk=k; break; } if(B[k].c<orbL){ dir=-1; bk=k; break; } }
        if(dir==0) continue; broke++;
        // trend filter
        if(TREND){ if(dir>0 && !(mid>ema[bk])) continue; if(dir<0 && !(mid<ema[bk])) continue; }
        // retrace entry level (RETR depth from breakout extreme; 0.5=mid, 0.618=deeper)
        double lvl = dir>0 ? (orbH - RETR*range) : (orbL + RETR*range);
        // after breakout: look for retrace to lvl, enter, manage to target/stop, repeat up to MAXTR
        int tr_today=0; int k=bk+1;
        while(k<de && tr_today<MAXTR){
            int hm=et_hm(B[k].ts); if(hm>=1600) break;
            bool touch = dir>0 ? (B[k].l<=lvl) : (B[k].h>=lvl);
            if(!touch){ k++; continue; }
            // entry at lvl (limit). stop + target
            double ep=lvl;
            double stop = (STOP=="loose") ? (dir>0?orbL:orbH)
                                          : (dir>0? B[k].l-bufA*atr[k] : B[k].h+bufA*atr[k]);
            double tgt;
            if(!EXT) tgt = dir>0?orbH:orbL;
            else { // high/low of day so far as extended target
                double hi=-1e18,lo=1e18; for(int q=ds;q<=k;q++){hi=max(hi,B[q].h);lo=min(lo,B[q].l);} tgt=dir>0?hi:lo; if(dir>0&&tgt<=orbH)tgt=orbH; if(dir<0&&tgt>=orbL)tgt=orbL; }
            double risk = dir>0?(ep-stop):(stop-ep); if(risk<=1e-9){ k++; continue; }
            // EXITMODE: "edge"=fixed ORB-edge target (default) ; "trail"=RUNNER: ride with a
            // structural trailing stop (prior TRWIN-bar low/high), no fixed target -> capture
            // the fat tail (her "winners outpace losers by a ton, don't panic-sell").
            static const char* EM = getenv("EXITMODE")? getenv("EXITMODE"):"edge";
            static int TRWIN = getenv("TRWIN")? atoi(getenv("TRWIN")):3;
            double R=0; bool done=false; int q=k+1; double tstop=stop;
            for(; q<de; q++){ int hh=et_hm(B[q].ts);
                if(!strcmp(EM,"trail")){
                    // trail the stop to the structure as price advances
                    if(dir>0){ double sl=1e18; for(int z=max(ds,q-TRWIN); z<q; z++) sl=min(sl,B[z].l); tstop=max(tstop,sl-bufA*atr[q]);
                               if(B[q].l<=tstop){ R=((tstop-ep)/risk); done=true; break; } }
                    else     { double sh=-1e18; for(int z=max(ds,q-TRWIN); z<q; z++) sh=max(sh,B[z].h); tstop=min(tstop,sh+bufA*atr[q]);
                               if(B[q].h>=tstop){ R=((ep-tstop)/risk); done=true; break; } }
                } else {
                    if(dir>0){ if(B[q].l<=stop){R=-1;done=true;break;} if(B[q].h>=tgt){R=(tgt-ep)/risk;done=true;break;} }
                    else     { if(B[q].h>=stop){R=-1;done=true;break;} if(B[q].l<=tgt){R=(ep-tgt)/risk;done=true;break;} }
                }
                if(hh>=1600){ double xp=B[q].c; R=(dir>0?(xp-ep):(ep-xp))/risk; done=true; break; }
            }
            if(!done){ double xp=B[de-1].c; R=(dir>0?(xp-ep):(ep-xp))/risk; }
            R -= COST/risk;
            trades.push_back({B[k].ts,R,yearOf(B[k].ts)}); entered++; tr_today++;
            k = (q>k)?q+1:k+1;
        }
    }

    int N=trades.size();
    printf("=== GOLD ORB 50%%-RETRACE  STOP=%s TREND=%d EXT=%d MAXTR=%d COST=%.2f ===\n",STOP.c_str(),TREND,EXT,MAXTR,COST);
    printf("days=%ld withORB=%ld breakout=%ld entries=%d\n",days,hasOrb,broke,N);
    if(!N){ printf("NO TRADES\n"); return 0; }
    auto stat=[&](vector<Tr>&t,const char*tag){ int m=t.size(); if(!m){printf("  %-6s n=0\n",tag);return;}
        int w=0; double gw=0,gl=0,sum=0,sum2=0; for(auto&x:t){ if(x.R>0){w++;gw+=x.R;}else gl+=-x.R; sum+=x.R; sum2+=x.R*x.R; }
        double pf=gl>0?gw/gl:99, mean=sum/m, sd=sqrt(max(1e-9,(sum2-m*mean*mean)/max(1,m-1)));
        printf("  %-6s n=%d WR=%.1f%% PF=%.2f avgR=%+.3f totR=%+.1f Sharpe/tr=%+.2f\n",tag,m,100.0*w/m,pf,mean,sum,mean/sd); };
  {double eq=0,pk=0,mdd=0; for(auto&t:trades){eq+=t.R; if(eq>pk)pk=eq; double dd=pk-eq; if(dd>mdd)mdd=dd;} double tot=0; for(auto&t:trades)tot+=t.R; printf("  MAXDD=%.1fR  ret/DD=%.2f  (totR=%.1f peak=%.1f)\n",mdd, mdd>0?tot/mdd:99, tot, pk);}
    stat(trades,"ALL");
    vector<Tr> h1(trades.begin(),trades.begin()+N/2), h2(trades.begin()+N/2,trades.end()); stat(h1,"H1"); stat(h2,"H2");
    vector<int> ys; for(auto&t:trades) if(find(ys.begin(),ys.end(),t.yr)==ys.end()) ys.push_back(t.yr); sort(ys.begin(),ys.end());
    for(int y:ys){ vector<Tr> yt; for(auto&t:trades) if(t.yr==y) yt.push_back(t); char tg[8]; snprintf(tg,8,"%d",y); stat(yt,tg); }
    if(const char* dp=getenv("DUMP")){ FILE* fp=fopen(dp,"a"); if(fp){ for(auto&t:trades) fprintf(fp,"%lld,%.4f\n",(long long)t.ts,t.R); fclose(fp); } }
    return 0;
}
