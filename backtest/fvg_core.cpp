// fvg_core.cpp — test the CORE continuation thesis, stripped of the fragile
// scalp mechanics that killed v1 (3m IFVG-V trigger, break-even).
//   Thesis: a fresh HTF FVG pointing at an UNTAPPED draw-on-liquidity (DOL)
//   => price reaches the DOL more often than it reverses.
//   Entry  = first retrace (mitigation) into the FVG zone, fill at near edge.
//   Stop   = structural: beyond the gap's far edge (+buf) OR k*ATR (param).
//   Target = the DOL. No BE. Capture MFE/MAE (in R) to reveal the best exit.
// build: g++ -std=c++17 -O2 fvg_core.cpp -o fvgcore
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;
struct Bar { int64_t ts; double o,h,l,c; };
static int64_t iso2unix(const string& s){
    int Y=atoi(s.substr(0,4).c_str()), M=atoi(s.substr(5,2).c_str()), D=atoi(s.substr(8,2).c_str());
    int h=atoi(s.substr(11,2).c_str()), mi=atoi(s.substr(14,2).c_str()), se=atoi(s.substr(17,2).c_str());
    int y=Y-(M<=2); int era=(y>=0?y:y-399)/400; unsigned yoe=(unsigned)(y-era*400);
    unsigned doy=(153*(M+(M>2?-3:9))+2)/5 + D-1; unsigned doe=yoe*365+yoe/4-yoe/100+doy;
    int64_t days=(int64_t)era*146097+(int)doe-719468; return days*86400+h*3600+mi*60+se;
}
static vector<Bar> load(const string& p){
    vector<Bar> v; ifstream f(p); if(!f){fprintf(stderr,"no file %s\n",p.c_str());return v;}
    string ln; bool first=true;
    while(getline(f,ln)){ if(first){first=false; if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9'))continue;}
        stringstream ss(ln); string t; vector<string> k; while(getline(ss,t,','))k.push_back(t);
        if(k.size()<5)continue; Bar b;
        b.ts = (k[0].find('T')!=string::npos) ? iso2unix(k[0]) : (int64_t)atoll(k[0].c_str()); // ISO or unix
        b.o=atof(k[1].c_str()); b.h=atof(k[2].c_str());
        b.l=atof(k[3].c_str()); b.c=atof(k[4].c_str()); if(b.h>0)v.push_back(b); }
    return v;
}
static vector<Bar> agg(const vector<Bar>& m1,int N){ vector<Bar> o; int64_t W=N*60,cur=-1; Bar b{};
    for(auto&x:m1){ int64_t g=(x.ts/W)*W; if(g!=cur){if(cur>=0)o.push_back(b);cur=g;b.ts=g;b.o=x.o;b.h=x.h;b.l=x.l;b.c=x.c;}
        else{b.h=max(b.h,x.h);b.l=min(b.l,x.l);b.c=x.c;} } if(cur>=0)o.push_back(b); return o; }
static int utc_hm(int64_t ts){int s=(int)(ts%86400);return (s/3600)*100+(s%3600)/60;}
static int64_t utc_day(int64_t ts){return ts/86400;}
static double atrAt(const vector<Bar>&b,int i,int n){ if(i<1)return 0; int lo=max(1,i-n+1); double s=0;int c=0;
    for(int k=lo;k<=i;++k){double tr=max(b[k].h-b[k].l,max(fabs(b[k].h-b[k-1].c),fabs(b[k].l-b[k-1].c)));s+=tr;c++;} return c?s/c:0; }
struct FVG{int dir;double lo,hi;int barIdx;int64_t closeTs;bool used;};

int main(int argc,char**argv){
    if(argc<2){printf("usage: %s m1.csv [HTF=15] [sess=ny|all|lon] [cost=0.5] [stopMode=gap|atr] [stopK=1.0] [maxDolAtr=6] [minRR=1.0] [exit=dol|trail|mfe] [trailAtr=2] [name]\n",argv[0]);return 1;}
    string path=argv[1]; int HTF=argc>2?atoi(argv[2]):15; string SESS=argc>3?argv[3]:"ny";
    double COST=argc>4?atof(argv[4]):0.5; string SMODE=argc>5?argv[5]:"gap"; double STOPK=argc>6?atof(argv[6]):1.0;
    double MAXDOL=argc>7?atof(argv[7]):6.0; double MINRR=argc>8?atof(argv[8]):1.0; string EXIT=argc>9?argv[9]:"dol";
    double TRAILA=argc>10?atof(argv[10]):2.0;
    double MINGAP=argc>11?atof(argv[11]):0.05;   // min FVG size in ATR (displacement quality)
    int    MAXAGE=argc>12?atoi(argv[12]):24;     // max HTF bars old at entry (freshness)
    string NAME=argc>13?argv[13]:path;
    // --- fib golden-zone confluence gate (video strat 3) : opt-in, default OFF ---
    // require the FVG retrace-entry to land within [FIBLO,FIBHI] retracement of the
    // recent swing leg (swLo->swHi for longs, swHi->swLo for shorts). 0=off.
    int    FIBGATE=argc>14?atoi(argv[14]):0;     // 0=off 1=zone-gate
    double FIBLO=argc>15?atof(argv[15]):0.618;   // golden-zone near edge
    double FIBHI=argc>16?atof(argv[16]):0.786;   // golden-zone far edge
    int    FIBLB=argc>17?atoi(argv[17]):48;      // swing-leg lookback (HTF bars) for the fib gate
    auto m1=load(path); if((int)m1.size()<500){printf("[%s] few bars\n",NAME.c_str());return 1;}
    auto htf=agg(m1,HTF);
    // prior-day H/L
    vector<int64_t> days; vector<double> dhi,dlo; { int64_t cd=-1;double hi=0,lo=0;
        for(auto&x:m1){int64_t d=utc_day(x.ts); if(d!=cd){if(cd>=0){days.push_back(cd);dhi.push_back(hi);dlo.push_back(lo);}cd=d;hi=x.h;lo=x.l;} else{hi=max(hi,x.h);lo=min(lo,x.l);}}
        if(cd>=0){days.push_back(cd);dhi.push_back(hi);dlo.push_back(lo);} }
    auto pdh=[&](int64_t ts){int64_t d=utc_day(ts);for(int i=(int)days.size()-1;i>=0;--i)if(days[i]<d)return dhi[i];return -1.0;};
    auto pdl=[&](int64_t ts){int64_t d=utc_day(ts);for(int i=(int)days.size()-1;i>=0;--i)if(days[i]<d)return dlo[i];return -1.0;};
    auto inSess=[&](int64_t ts){int hm=utc_hm(ts); if(SESS=="all")return true; if(SESS=="ny")return hm>=1330&&hm<=1600; if(SESS=="lon")return hm>=700&&hm<=1100; return true;};
    auto htfIdxAt=[&](int64_t t){int idx=-1;for(int i=0;i<(int)htf.size();++i){if(htf[i].ts+(int64_t)HTF*60<=t)idx=i;else break;}return idx;};
    // HTF FVGs
    vector<FVG> fvgs;
    for(int i=2;i<(int)htf.size();++i){ double a=atrAt(htf,i,14); if(a<=0)continue;
        if(htf[i-2].h<htf[i].l){double g=htf[i].l-htf[i-2].h; if(g>=MINGAP*a&&g<=6*a) fvgs.push_back({+1,htf[i-2].h,htf[i].l,i,htf[i].ts+(int64_t)HTF*60,false});}
        if(htf[i-2].l>htf[i].h){double g=htf[i-2].l-htf[i].h; if(g>=MINGAP*a&&g<=6*a) fvgs.push_back({-1,htf[i].h,htf[i-2].l,i,htf[i].ts+(int64_t)HTF*60,false});}
    }
    struct T{double entry,sl,tp,r;int dir;double netPts,netR,mfeR,maeR;bool win;};
    vector<T> trades;
    const int TTL=24;
    // drive on m1 for entry/exit precision
    size_t fi=0; vector<FVG> active;
    bool inT=false; T cur{}; double peakFav=0,peakAdv=0; double trailStop=0;
    for(size_t bi=1;bi<m1.size();++bi){
        int64_t now=m1[bi].ts; double H=m1[bi].h,L=m1[bi].l,Cl=m1[bi].c;
        while(fi<fvgs.size()&&fvgs[fi].closeTs<=now){active.push_back(fvgs[fi]);fi++;}
        int hidx=htfIdxAt(now); double atr=hidx>0?atrAt(htf,hidx,14):0;
        for(auto&f:active)if(!f.used&&hidx-f.barIdx>TTL)f.used=true;
        // manage
        if(inT){
            double fav = cur.dir>0? (H-cur.entry):(cur.entry-L);
            double adv = cur.dir>0? (cur.entry-L):(H-cur.entry);
            if(fav>peakFav)peakFav=fav; if(adv>peakAdv)peakAdv=adv;
            // trailing stop (exit=trail): trail by TRAILA*atr from peak favorable
            if(EXIT=="trail"&&atr>0){ double ts2= cur.dir>0? (cur.entry+peakFav-TRAILA*atr):(cur.entry-peakFav+TRAILA*atr);
                if(cur.dir>0)trailStop=max(trailStop,ts2); else trailStop=(trailStop==0?ts2:min(trailStop,ts2)); }
            bool hitSL = cur.dir>0?(L<=cur.sl):(H>=cur.sl);
            bool hitTrail = EXIT=="trail"&&trailStop!=0&&(cur.dir>0?(L<=trailStop):(H>=trailStop))&&peakFav>cur.r; // only after 1R fav
            bool hitTP = (EXIT=="dol"||EXIT=="rr")&&(cur.dir>0?(H>=cur.tp):(L<=cur.tp));
            if(hitSL||hitTP||hitTrail){
                double exit = hitSL?cur.sl : (hitTP?cur.tp:trailStop);
                if(hitSL) exit=cur.sl;
                double pts=(cur.dir>0?(exit-cur.entry):(cur.entry-exit))-COST;
                cur.netPts=pts; cur.netR=cur.r>0?pts/cur.r:0; cur.win=pts>0;
                cur.mfeR=cur.r>0?peakFav/cur.r:0; cur.maeR=cur.r>0?peakAdv/cur.r:0;
                trades.push_back(cur); inT=false;
            }
            continue;
        }
        if(!inSess(now)||atr<=0)continue;
        double px=Cl, ph=pdh(now),pl=pdl(now);
        double swHi=-1,swLo=-1; if(hidx>=4){int lo=max(0,hidx-48);for(int k=lo;k<=hidx-2;++k){swHi=max(swHi,htf[k].h);if(swLo<0||htf[k].l<swLo)swLo=htf[k].l;}}
        double dolUp=-1,dolDn=-1;
        for(double lvl:{ph,swHi})if(lvl>px+0.3*atr){double d=(lvl-px)/atr; if(d<=MAXDOL)dolUp=(dolUp<0?lvl:min(dolUp,lvl));}
        for(double lvl:{pl,swLo})if(lvl>0&&lvl<px-0.3*atr){double d=(px-lvl)/atr; if(d<=MAXDOL)dolDn=(dolDn<0?lvl:max(dolDn,lvl));}
        for(auto&f:active){ if(f.used)continue;
            if(hidx-f.barIdx>MAXAGE)continue;                    // freshness gate
            if(f.dir>0&&dolUp<0)continue; if(f.dir<0&&dolDn<0)continue;
            bool tag=(L<=f.hi&&H>=f.lo); if(!tag)continue;       // retrace into gap
            double entry = f.dir>0? f.hi : f.lo;                 // fill at near edge (mitigation)
            if(FIBGATE){                                          // golden-zone confluence (video strat 3)
                double fHi=-1,fLo=-1;                              // recent-impulse swing leg
                if(hidx>=4){int lo=max(0,hidx-FIBLB);for(int k=lo;k<=hidx-2;++k){fHi=max(fHi,htf[k].h);if(fLo<0||htf[k].l<fLo)fLo=htf[k].l;}}
                if(fHi<=fLo||fLo<=0)continue;                     // need a valid swing leg
                double rng=fHi-fLo;
                double retr = f.dir>0? (fHi-entry)/rng : (entry-fLo)/rng;
                if(retr<FIBLO||retr>FIBHI)continue;               // entry must sit in the golden zone
            }
            double slPx;
            if(SMODE=="gap") slPx = f.dir>0? (f.lo-STOPK*0.1*atr):(f.hi+STOPK*0.1*atr); // just beyond far edge
            else            slPx = f.dir>0? (entry-STOPK*atr):(entry+STOPK*atr);
            double r = f.dir>0?(entry-slPx):(slPx-entry); if(r<=0.05*atr)continue;
            double dol = f.dir>0?dolUp:dolDn; double rr=f.dir>0?(dol-entry)/r:(entry-dol)/r;
            if(rr<MINRR||rr>20)continue;
            // exit=rr: fixed R-multiple target (harvest MFE) using TRAILA as the R mult.
            // Still require the DOL to be at least that far (rr>=TRAILA) so we only
            // take setups with genuine room toward liquidity.
            double tp = dol;
            if(EXIT=="rr"){ if(rr<TRAILA)continue; tp = f.dir>0?(entry+TRAILA*r):(entry-TRAILA*r); }
            cur=T{entry,slPx,tp,r,f.dir,0,0,0,0,false}; inT=true; f.used=true;
            peakFav=0;peakAdv=0;trailStop=0; break;
        }
    }
    auto rep=[&](const char*tag,int lo,int hi){ int n=0,w=0;double net=0,gw=0,gl=0,sR=0,pk=0,cm=0,dd=0;
        double sumWp=0,sumLp=0; int nw=0,nl=0,consec=0,maxConsec=0; std::vector<double> rs;
        for(int i=lo;i<hi;++i){auto&t=trades[i];n++;net+=t.netPts;sR+=t.netR;rs.push_back(t.netR);
            if(t.win){w++;gw+=t.netPts;sumWp+=t.netPts;nw++;consec=0;}else{gl+=-t.netPts;sumLp+=-t.netPts;nl++;consec++;if(consec>maxConsec)maxConsec=consec;}
            cm+=t.netPts;if(cm>pk)pk=cm;if(pk-cm>dd)dd=pk-cm;}
        double pf=gl>0?gw/gl:(gw>0?99:0), mR=n?sR/n:0, var=0;
        for(double r:rs)var+=(r-mR)*(r-mR); double sd=rs.size()>1?std::sqrt(var/(rs.size()-1)):0;
        double sharpeTrade=sd>0?mR/sd:0;                 // per-trade Sharpe
        double tpy = n*(12.0/16.0)/ (n? 1:1);            // (NY 16mo sample) approx trades/yr below
        double trades_per_yr = n>0 ? n/(16.0/12.0) : 0;
        double sharpeAnn = sharpeTrade*std::sqrt(trades_per_yr>0?trades_per_yr:1);
        double avgW=nw?sumWp/nw:0, avgL=nl?sumLp/nl:0, plr=avgL>0?avgW/avgL:0;
        printf("  %-9s n=%3d WR=%4.1f%% PF=%4.2f net=%8.1fpt avgR=%+5.2f | Sharpe/tr=%4.2f Sharpe/yr=%4.2f | avgW=%6.1f avgL=%6.1f W/L=%4.2f | maxDD=%7.1f ret/DD=%4.2f maxConsecL=%d exp=%+5.2fpt\n",
            tag,n,n?100.0*w/n:0,pf,net,mR, sharpeTrade,sharpeAnn, avgW,-avgL,plr, dd, dd>0?net/dd:0, maxConsec, n?net/n:0); (void)tpy; };
    printf("[%s] HTF=%dm sess=%s cost=%.2f stop=%s k=%.1f maxDol=%.0fA minRR=%.1f exit=%s trail=%.1f | tr=%d\n",
        NAME.c_str(),HTF,SESS.c_str(),COST,SMODE.c_str(),STOPK,MAXDOL,MINRR,EXIT.c_str(),TRAILA,(int)trades.size());
    if(trades.empty()){printf("  (no trades)\n");return 0;}
    rep("ALL",0,(int)trades.size()); int mid=(int)trades.size()/2; rep("H1",0,mid); rep("H2",mid,(int)trades.size());
    return 0;
}
