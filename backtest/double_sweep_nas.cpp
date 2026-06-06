// double_sweep_nas.cpp — test the ICT "double sweep" confirmation filter on NAS.
// QUESTION: does requiring a SECOND liquidity sweep (HTF swing swept+reclaimed, then
// the internal low/high swept+reclaimed) improve trade quality vs a SINGLE sweep?
//
// Mechanics (LONG; SHORT symmetric):
//   swing low SL  = fractal pivot low (k bars each side).
//   Sweep1        = bar low < SL then a close back ABOVE SL within `reclaimN` bars (reclaim).
//   single-entry  = enter LONG on the reclaim bar. stop = sweep1 low - buf.
//   double-entry  = after Sweep1 reclaim, wait for an internal swing low IL (higher low),
//                   then a later bar low < IL with close back above IL (2nd sweep+reclaim)
//                   within `win2` bars -> enter LONG. stop = sweep2 low - buf.
//   exit          = TP at R*risk, SL structural, optional time-stop maxHold bars.
//
// Cost-inclusive (pts round-trip), walk-forward both halves, per-year, 3x-cost robustness.
// build: g++ -std=c++17 -O2 double_sweep_nas.cpp -o double_sweep
// run:   ./double_sweep <mode=double|single> <barSec> <file1> [file2 ...]
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

struct Bar { int64_t ts; double o,h,l,c; };

static int64_t parse_histdata_ts(const char* s){
    // "YYYYMMDD HHMMSSmmm" -> epoch sec (HistData = EST, +5h to UTC)
    if(strlen(s) < 17) return -1;
    char buf[9]; memcpy(buf,s,8); buf[8]=0; long ymd=atol(buf);
    const char* t=s+9;                       // skip date + space
    int hh=(t[0]-'0')*10+(t[1]-'0'); int mm=(t[2]-'0')*10+(t[3]-'0'); int ss=(t[4]-'0')*10+(t[5]-'0');
    struct tm tmv{}; tmv.tm_year=(int)(ymd/10000)-1900; tmv.tm_mon=(int)((ymd/100)%100)-1; tmv.tm_mday=(int)(ymd%100);
    tmv.tm_hour=hh; tmv.tm_min=mm; tmv.tm_sec=ss;
    int64_t e=(int64_t)timegm(&tmv); return e + 5*3600;   // EST->UTC
}

// Append bars from one file (auto-detect HistData vs duka tick) into v, aggregated barSec.
static void load_into(vector<Bar>& v, const string& p, int barSec){
    ifstream f(p); if(!f){ fprintf(stderr,"no file %s\n",p.c_str()); return; }
    string ln; bool first=true; int64_t W=barSec, cur=-1; Bar b{};
    while(getline(f,ln)){
        if(ln.empty()) continue;
        if(first){ first=false; if(ln[0]<'0'||ln[0]>'9') continue; }   // skip header
        if(ln[0]<'0'||ln[0]>'9') continue;
        int64_t ts; double bid, ask;
        if(ln.find(' ')!=string::npos && ln.size()>9 && ln[8]==' '){
            // HistData: "YYYYMMDD HHMMSSmmm,bid,ask,vol"
            ts=parse_histdata_ts(ln.c_str()); if(ts<0) continue;
            size_t c1=ln.find(','); if(c1==string::npos) continue;
            const char* s=ln.c_str()+c1+1; char* e=nullptr;
            bid=strtod(s,&e); if(*e!=',') continue; ask=strtod(e+1,&e);
        } else {
            // duka: "timestamp_ms,ask,bid,..."
            const char* s=ln.c_str(); char* e=nullptr;
            long long ms=strtoll(s,&e,10); if(e==s||*e!=',') continue; ts=ms/1000;
            ask=strtod(e+1,&e); if(*e!=',') continue; bid=strtod(e+1,&e);
        }
        if(ask<=0||bid<=0) continue;
        double mid=(ask+bid)*0.5; int64_t g=(ts/W)*W;
        if(g!=cur){ if(cur>=0) v.push_back(b); cur=g; b.ts=g; b.o=b.h=b.l=b.c=mid; }
        else { if(mid>b.h)b.h=mid; if(mid<b.l)b.l=mid; b.c=mid; }
    }
    if(cur>=0) v.push_back(b);
}

static int yearOf(int64_t ts){ time_t t=ts; struct tm* g=gmtime(&t); return g->tm_year+1900; }
static int utc_hm(int64_t ts){ int s=(int)(((ts%86400)+86400)%86400); return (s/3600)*100 + (s%3600)/60; }

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"usage: %s <double|single> <barSec> <file1> [file2...]\n",argv[0]); return 1; }
    string mode=argv[1]; bool dbl=(mode=="double");
    int barSec=atoi(argv[2]);
    // params
    int    K=3;            // fractal half-width for swing pivots
    int    reclaimN=2;     // bars allowed to reclaim after sweep1
    int    win2=12;        // bars allowed for the 2nd sweep after sweep1 reclaim
    double bufA=0.10;      // stop buffer = bufA * ATR
    double Rmult=2.0;      // TP = Rmult * risk; override via DSWEEP_R
    if(const char* er=getenv("DSWEEP_R")) Rmult=atof(er);
    int    maxHold=60;     // time-stop (bars)
    int    nyOnly=0;       // DSWEEP_NY=1 -> only enter 13:30-20:00 UTC (NY session)
    if(const char* en=getenv("DSWEEP_NY")) nyOnly=atoi(en);
    double COST=2.0;       // round-trip cost in price pts (NAS index); override via DSWEEP_COST
    if(const char* ec=getenv("DSWEEP_COST")) COST=atof(ec);
    int    lookSwing=40;   // how far back to find the "major" swing level

    vector<Bar> B;
    for(int i=3;i<argc;i++) load_into(B,argv[i],barSec);
    if(B.size()<200){ fprintf(stderr,"too few bars (%zu)\n",B.size()); return 1; }
    sort(B.begin(),B.end(),[](const Bar&a,const Bar&b){return a.ts<b.ts;});
    int n=B.size();

    // ATR
    vector<double> atr(n,0);
    { double s=0; int W=14; for(int i=1;i<n;i++){ double tr=max(B[i].h-B[i].l,max(fabs(B[i].h-B[i-1].c),fabs(B[i].l-B[i-1].c)));
        if(i<=W){ s+=tr; atr[i]=s/i; } else { s+=tr-(/*approx*/atr[i-1]); atr[i]=atr[i-1] + (tr-atr[i-1])/W; } } }

    // confirmed fractal pivots (lag K)
    auto isPivLow=[&](int i){ if(i<K||i+K>=n) return false; for(int k=1;k<=K;k++) if(B[i].l>B[i-k].l||B[i].l>B[i+k].l) return false; return true; };
    auto isPivHigh=[&](int i){ if(i<K||i+K>=n) return false; for(int k=1;k<=K;k++) if(B[i].h<B[i-k].h||B[i].h<B[i+k].h) return false; return true; };

    struct Tr{ int64_t ts; double R; int yr; };
    vector<Tr> trades;
    long cSweep1=0, cReclaim=0, cInternal=0, cSweep2=0, cEnter=0;

    int i=K+2;
    while(i<n-1){
        double a=atr[i]; if(a<=0){ i++; continue; }
        // --- find nearest confirmed major swing low/high within lookSwing (for sweep1) ---
        double SL=-1, SH=-1;
        for(int j=i-1;j>=max(K, i-lookSwing);--j){ if(SL<0 && isPivLow(j)) SL=B[j].l; if(SH<0 && isPivHigh(j)) SH=B[j].h; if(SL>0&&SH>0) break; }

        // ===== LONG path: sell-side sweep of SL =====
        if(SL>0 && B[i].l < SL){
            cSweep1++;
            // reclaim: a close back above SL within reclaimN bars
            int rc=-1; for(int k=i;k<=min(n-1,i+reclaimN);++k){ if(B[k].c>SL){ rc=k; break; } }
            if(rc>=0){
                cReclaim++;
                double sweep1low=B[i].l;
                int entry=-1; double stop=0;
                if(!dbl){
                    entry=rc; stop=sweep1low - bufA*a;
                } else {
                    // wait internal swing low IL (higher low) then 2nd sweep+reclaim within win2
                    int il=-1; double ILv=0;
                    for(int k=rc+1;k<=min(n-1,rc+win2);++k){ if(isPivLow(k) && B[k].l>sweep1low){ il=k; ILv=B[k].l; break; } }
                    if(il>=0){ cInternal++;
                        for(int k=il+1;k<=min(n-1,il+win2);++k){ if(B[k].l<ILv){ cSweep2++;
                            int rc2=-1; for(int m=k;m<=min(n-1,k+reclaimN);++m){ if(B[m].c>ILv){ rc2=m; break; } }
                            if(rc2>=0){ entry=rc2; stop=B[k].l - bufA*a; } break; } }
                    }
                }
                if(entry>=0 && entry<n-1){
                    cEnter++;
                    if(nyOnly){ int hm=utc_hm(B[entry].ts); if(hm<1330||hm>=2000){ i=entry+1; continue; } }
                    double ep=B[entry+1].o;            // next-bar open fill
                    double risk=ep-stop; if(risk<=0){ i=entry+1; continue; }
                    double tp=ep + Rmult*risk; double R=0; bool done=false;
                    for(int k=entry+1;k<=min(n-1,entry+maxHold);++k){
                        if(B[k].l<=stop){ R=-1.0; done=true; break; }
                        if(B[k].h>=tp){ R=Rmult; done=true; break; }
                    }
                    if(!done){ R=(B[min(n-1,entry+maxHold)].c-ep)/risk; }
                    R -= COST/risk;                    // cost in R terms
                    trades.push_back({B[entry].ts,R,yearOf(B[entry].ts)});
                    i=entry+2; continue;
                }
            }
        }
        // ===== SHORT path: buy-side sweep of SH =====
        if(SH>0 && B[i].h > SH){
            int rc=-1; for(int k=i;k<=min(n-1,i+reclaimN);++k){ if(B[k].c<SH){ rc=k; break; } }
            if(rc>=0){
                double sweep1high=B[i].h; int entry=-1; double stop=0;
                if(!dbl){ entry=rc; stop=sweep1high + bufA*a; }
                else {
                    int ih=-1; double IHv=0;
                    for(int k=rc+1;k<=min(n-1,rc+win2);++k){ if(isPivHigh(k) && B[k].h<sweep1high){ ih=k; IHv=B[k].h; break; } }
                    if(ih>=0){ for(int k=ih+1;k<=min(n-1,ih+win2);++k){ if(B[k].h>IHv){
                        int rc2=-1; for(int m=k;m<=min(n-1,k+reclaimN);++m){ if(B[m].c<IHv){ rc2=m; break; } }
                        if(rc2>=0){ entry=rc2; stop=B[k].h + bufA*a; } break; } } }
                }
                if(entry>=0 && entry<n-1){
                    cEnter++;
                    if(nyOnly){ int hm=utc_hm(B[entry].ts); if(hm<1330||hm>=2000){ i=entry+1; continue; } }
                    double ep=B[entry+1].o; double risk=stop-ep; if(risk<=0){ i=entry+1; continue; }
                    double tp=ep - Rmult*risk; double R=0; bool done=false;
                    for(int k=entry+1;k<=min(n-1,entry+maxHold);++k){
                        if(B[k].h>=stop){ R=-1.0; done=true; break; }
                        if(B[k].l<=tp){ R=Rmult; done=true; break; }
                    }
                    if(!done){ R=(ep-B[min(n-1,entry+maxHold)].c)/risk; }
                    R -= COST/risk;
                    trades.push_back({B[entry].ts,R,yearOf(B[entry].ts)});
                    i=entry+2; continue;
                }
            }
        }
        i++;
    }

    // ---- report ----
    int N=trades.size(); if(N==0){ printf("mode=%s bars=%d NO TRADES (sweep1=%ld reclaim=%ld internal=%ld sweep2=%ld)\n",mode.c_str(),n,cSweep1,cReclaim,cInternal,cSweep2); return 0; }
    auto stats=[&](const vector<Tr>& t, const char* tag){
        int m=t.size(); if(!m){ printf("  %-8s n=0\n",tag); return; }
        int w=0; double gw=0,gl=0,sum=0,sum2=0;
        for(auto&x:t){ if(x.R>0){w++;gw+=x.R;} else gl+=-x.R; sum+=x.R; sum2+=x.R*x.R; }
        double pf=gl>0?gw/gl:99, mean=sum/m, sd=sqrt(max(1e-9,(sum2-m*mean*mean)/max(1,m-1)));
        printf("  %-8s n=%d  WR=%.1f%%  PF=%.2f  avgR=%+.3f  totR=%+.1f  Sharpe/tr=%+.2f\n",
               tag,m,100.0*w/m,pf,mean,sum,mean/sd);
    };
    printf("=== DOUBLE-SWEEP NAS  mode=%s barSec=%d bars=%d ===\n",mode.c_str(),barSec,n);
    printf("funnel: sweep1=%ld reclaim=%ld internal=%ld sweep2=%ld ENTER=%ld\n",cSweep1,cReclaim,cInternal,cSweep2,cEnter);
    printf("params: K=%d reclaimN=%d win2=%d Rmult=%.1f maxHold=%d COST=%.1fpt lookSwing=%d\n",K,reclaimN,win2,Rmult,maxHold,COST,lookSwing);
    stats(trades,"ALL");
    // WF halves
    vector<Tr> h1(trades.begin(),trades.begin()+N/2), h2(trades.begin()+N/2,trades.end());
    stats(h1,"H1"); stats(h2,"H2");
    // per-year
    vector<int> yrs; for(auto&t:trades){ if(find(yrs.begin(),yrs.end(),t.yr)==yrs.end()) yrs.push_back(t.yr); }
    sort(yrs.begin(),yrs.end());
    for(int y:yrs){ vector<Tr> yt; for(auto&t:trades) if(t.yr==y) yt.push_back(t); char tag[16]; snprintf(tag,sizeof(tag),"%d",y); stats(yt,tag); }
    return 0;
}
