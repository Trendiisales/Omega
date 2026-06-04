// adaptive_hull.cpp — Ehlers Phase-Accumulation dominant cycle -> adaptive Hull
// MA -> trend-flip strategy ("PA Adaptive Hull Parabolic"). Sweep symbols + levers.
//   Signal: HMA(adaptive period) slope flips green(rising)/red(falling).
//   Entry : on flip, optional same-color candle confirm. Stop: HMA line OR k*ATR.
//   Exit  : flip | rr (1:RR) | trail. Levers via args/env. cost-incl, walk-forward.
// build: g++ -std=c++17 -O2 adaptive_hull.cpp -o adhull
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
using namespace std;
struct Bar{int64_t ts;double o,h,l,c;};
static int64_t iso2unix(const string&s){int Y=atoi(s.substr(0,4).c_str()),M=atoi(s.substr(5,2).c_str()),D=atoi(s.substr(8,2).c_str());
 int h=atoi(s.substr(11,2).c_str()),mi=atoi(s.substr(14,2).c_str()),se=atoi(s.substr(17,2).c_str());
 int y=Y-(M<=2);int era=(y>=0?y:y-399)/400;unsigned yoe=(unsigned)(y-era*400);unsigned doy=(153*(M+(M>2?-3:9))+2)/5+D-1;
 unsigned doe=yoe*365+yoe/4-yoe/100+doy;int64_t days=(int64_t)era*146097+(int)doe-719468;return days*86400+h*3600+mi*60+se;}
static vector<Bar> load(const string&p){vector<Bar>v;ifstream f(p);if(!f){fprintf(stderr,"no %s\n",p.c_str());return v;}
 string ln;bool fst=true;while(getline(f,ln)){if(fst){fst=false;if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9'))continue;}
  stringstream s(ln);string t;vector<string>k;while(getline(s,t,','))k.push_back(t);if(k.size()<5)continue;Bar b;
  b.ts=(k[0].find('T')!=string::npos)?iso2unix(k[0]):(int64_t)atoll(k[0].c_str());
  b.o=atof(k[1].c_str());b.h=atof(k[2].c_str());b.l=atof(k[3].c_str());b.c=atof(k[4].c_str());if(b.h>0)v.push_back(b);}return v;}
static vector<Bar> agg(const vector<Bar>&m,int N){vector<Bar>o;int64_t W=(int64_t)N*60,cur=-1;Bar b{};for(auto&x:m){int64_t g=(x.ts/W)*W;
 if(g!=cur){if(cur>=0)o.push_back(b);cur=g;b=x;b.ts=g;}else{b.h=max(b.h,x.h);b.l=min(b.l,x.l);b.c=x.c;}}if(cur>=0)o.push_back(b);return o;}
static double deg2rad=M_PI/180.0;

// Ehlers Phase-Accumulation dominant cycle. Returns per-bar period series.
static vector<double> ehlers_period(const vector<Bar>&b,double alpha_p){
    int n=(int)b.size(); vector<double> price(n),sm(n,0),dt(n,0),Q1(n,0),I1(n,0),phase(n,0),period(n,0);
    for(int i=0;i<n;++i) price[i]=(b[i].h+b[i].l)*0.5;
    double per_prev=15;
    for(int i=0;i<n;++i){
        if(i>=3) sm[i]=(4*price[i]+3*price[i-1]+2*price[i-2]+price[i-3])/10.0; else sm[i]=price[i];
        double mul=0.075*per_prev+0.54;
        if(i>=6) dt[i]=(.0962*sm[i]+.5769*sm[i-2]-.5769*sm[i-4]-.0962*sm[i-6])*mul; else dt[i]=0;
        if(i>=6) Q1[i]=(.0962*dt[i]+.5769*dt[i-2]-.5769*dt[i-4]-.0962*dt[i-6])*mul; else Q1[i]=0;
        I1[i]=(i>=3)?dt[i-3]:0;
        // phase (deg)
        double ph=0;
        if(fabs(I1[i])>1e-9) ph=atan(Q1[i]/I1[i])/deg2rad;
        if(I1[i]<0 && Q1[i]>0) ph=180-ph;
        else if(I1[i]<0 && Q1[i]<0) ph=180+ph; // wait: use 180-... keep simple quadrant
        else if(I1[i]>0 && Q1[i]<0) ph=-ph;
        phase[i]=ph;
        // delta phase + accumulate to 360
        double dp=(i>0)?(phase[i-1]-phase[i]):1;
        if(i>0 && phase[i-1]<90 && phase[i]>270) dp=360+phase[i-1]-phase[i];
        if(dp<1) dp=1; if(dp>60) dp=60;
        // accumulate
        double sum=0; int count=0; double per=per_prev;
        // recompute delta phases backwards
        for(int k=0;k<40 && i-k>=1;++k){
            double pk = (phase[i-k-1]-phase[i-k]);
            if(phase[i-k-1]<90 && phase[i-k]>270) pk=360+phase[i-k-1]-phase[i-k];
            if(pk<1)pk=1; if(pk>60)pk=60;
            sum+=pk; count++;
            if(sum>360){ per=count; break; }
        }
        if(per<6)per=6; if(per>50)per=50;
        per = alpha_p*per + (1-alpha_p)*per_prev;   // smooth
        period[i]=per; per_prev=per;
        (void)dp;
    }
    return period;
}
static double wma(const vector<double>&v,int i,int len){ if(len<1)len=1; if(i-len+1<0)return v[i];
    double num=0,den=0; for(int k=0;k<len;++k){double w=len-k; num+=w*v[i-k]; den+=w;} return num/den; }

int main(int argc,char**argv){
    if(argc<3){printf("usage: %s csv tf [cost=2] [pmul=1.0] [exit=flip|rr|trail] [rr=1.5] [stop=hma|atr] [katr=2] [confirm=1] [side=0] [name]\n",argv[0]);return 1;}
    string path=argv[1]; int TF=atoi(argv[2]);
    double COST=argc>3?atof(argv[3]):2.0; double PMUL=argc>4?atof(argv[4]):1.0;
    string EXIT=argc>5?argv[5]:"flip"; double RR=argc>6?atof(argv[6]):1.5;
    string STOP=argc>7?argv[7]:"hma"; double KATR=argc>8?atof(argv[8]):2.0;
    int CONFIRM=argc>9?atoi(argv[9]):1; int SIDE=argc>10?atoi(argv[10]):0; string NAME=argc>11?argv[11]:path;
    string MODE=getenv("MODE")?getenv("MODE"):"hull";   // hull | donch | super (Supertrend)
    string DUMP=getenv("DUMP")?getenv("DUMP"):"";        // file: write exit_ts,net per trade
    double STMULT=getenv("STMULT")?atof(getenv("STMULT")):3.0;  // Supertrend ATR mult
    int    STLEN =getenv("STLEN")?atoi(getenv("STLEN")):10;     // Supertrend ATR period
    string PAIR=getenv("PAIR")?getenv("PAIR"):"";        // "" | ema (long only if close>EMA_slow) | adx
    int    EMASLOW=getenv("EMASLOW")?atoi(getenv("EMASLOW")):100;
    double ADXMIN=getenv("ADXMIN")?atof(getenv("ADXMIN")):25.0;
    int    SESS0=getenv("SESS0")?atoi(getenv("SESS0")):-1;   // entry UTC-hour window [SESS0,SESS1) (-1=all)
    int    SESS1=getenv("SESS1")?atoi(getenv("SESS1")):-1;
    auto m=load(path); if((int)m.size()<2000){printf("[%s] few\n",NAME.c_str());return 1;}
    auto b=agg(m,TF); int N=(int)b.size(); if(N<300){printf("[%s] few agg\n",NAME.c_str());return 1;}
    auto per=ehlers_period(b,0.2);
    // adaptive HMA per bar: hma=wma(2*wma(c,p/2)-wma(c,p), sqrt(p))
    vector<double> c(N),hma(N,0); for(int i=0;i<N;++i)c[i]=b[i].c;
    vector<double> raw(N,0);
    for(int i=0;i<N;++i){ int p=max(2,(int)round(per[i]*PMUL)); int ph=max(1,p/2); int ps=max(1,(int)round(sqrt((double)p)));
        double w1=wma(c,i,ph), w2=wma(c,i,p); raw[i]=2*w1-w2; }
    for(int i=0;i<N;++i){ int p=max(2,(int)round(per[i]*PMUL)); int ps=max(1,(int)round(sqrt((double)p)));
        hma[i]=wma(raw,i,ps); }
    // ATR
    vector<double> atr(N,0); {double a=0;int c2=0; for(int i=1;i<N;++i){double tr=max(b[i].h-b[i].l,max(fabs(b[i].h-b[i-1].c),fabs(b[i].l-b[i-1].c)));
        if(c2<14){a+=tr;if(++c2==14)a/=14;}else a=(a*13+tr)/14; atr[i]=a;} }
    // Supertrend direction (median +/- STMULT*ATR(STLEN)) — canonical
    vector<int> stdir(N,1);
    { double a=0;int cc=0; double prevFU=0,prevFL=0,prevST=0; bool init=false;
      for(int i=1;i<N;++i){ double trv=max(b[i].h-b[i].l,max(fabs(b[i].h-b[i-1].c),fabs(b[i].l-b[i-1].c)));
        if(cc<STLEN){a+=trv;if(++cc==STLEN)a/=STLEN;}else a=(a*(STLEN-1)+trv)/STLEN;
        double hl2=(b[i].h+b[i].l)*0.5,bU=hl2+STMULT*a,bL=hl2-STMULT*a;
        if(!init){prevFU=bU;prevFL=bL;prevST=bL;stdir[i]=1;init=true;continue;}
        double fU=(bU<prevFU||b[i-1].c>prevFU)?bU:prevFU;
        double fL=(bL>prevFL||b[i-1].c<prevFL)?bL:prevFL;
        double st=(prevST==prevFU)?((b[i].c<=fU)?fU:fL):((b[i].c>=fL)?fL:fU);
        stdir[i]=b[i].c>st?1:-1; prevFU=fU;prevFL=fL;prevST=st; } }
    // EMA(slow) for the PAIR=ema regime filter
    vector<double> emaS(N,0); { double k=2.0/(EMASLOW+1),e=c[0]; for(int i=0;i<N;++i){e=c[i]*k+e*(1-k);emaS[i]=e;} }
    // CCI(5) + Fractal(3) reversal signal (the video). CCI zero-cross + recent fractal.
    int CCIP=getenv("CCIP")?atoi(getenv("CCIP")):5; int FP=getenv("FP")?atoi(getenv("FP")):3; int FK=getenv("FK")?atoi(getenv("FK")):5;
    vector<double> cci(N,0); vector<char> fracHi(N,0),fracLo(N,0);
    for(int i=CCIP-1;i<N;++i){ double tp=0; for(int k=i-CCIP+1;k<=i;++k)tp+=(b[k].h+b[k].l+b[k].c)/3.0; double sma=tp/CCIP;
        double md=0; for(int k=i-CCIP+1;k<=i;++k)md+=fabs((b[k].h+b[k].l+b[k].c)/3.0-sma); md/=CCIP;
        double tpi=(b[i].h+b[i].l+b[i].c)/3.0; cci[i]=md>0?(tpi-sma)/(0.015*md):0; }
    for(int i=FP;i<N-FP;++i){ bool hi=true,lo=true; for(int k=1;k<=FP;++k){ if(b[i].h<=b[i-k].h||b[i].h<=b[i+k].h)hi=false; if(b[i].l>=b[i-k].l||b[i].l>=b[i+k].l)lo=false; }
        fracHi[i]=hi; fracLo[i]=lo; }   // NOTE: fractal confirmed FP bars late (lookahead-safe: usable at i+FP)
    // strategy: HMA slope flip
    struct T{double e,sl,tp;int dir;double net,r;bool win;int64_t exts;}; vector<T> tr; bool in=false; T cur{}; double trailstop=0;
    int warm=60;
    for(int i=warm;i<N;++i){
        int slope = (hma[i]>hma[i-1])?1:((hma[i]<hma[i-1])?-1:0);
        int pslope= (hma[i-1]>hma[i-2])?1:((hma[i-1]<hma[i-2])?-1:0);
        bool flipUp = slope>0 && pslope<=0; bool flipDn = slope<0 && pslope>=0;
        if(MODE=="donch"){ double dh=-1e18,dl=1e18; for(int k=max(0,i-20);k<i;++k){dh=max(dh,b[k].h);dl=min(dl,b[k].l);}
            flipUp = b[i].c>dh; flipDn = b[i].c<dl; }
        if(MODE=="super"){ flipUp = stdir[i]>0 && stdir[i-1]<=0; flipDn = stdir[i]<0 && stdir[i-1]>=0; }
        if(MODE=="cci"){ bool cu=cci[i]>0&&cci[i-1]<=0, cd=cci[i]<0&&cci[i-1]>=0;
            bool rLo=false,rHi=false; for(int j=i-FP;j>=max(0,i-FP-FK);--j){ if(fracLo[j])rLo=true; if(fracHi[j])rHi=true; }
            flipUp = cu&&rLo; flipDn = cd&&rHi; }
        // manage
        if(in){
            bool sl = cur.dir>0?(b[i].l<=cur.sl):(b[i].h>=cur.sl);
            bool tp = (EXIT=="rr") && (cur.dir>0?(b[i].h>=cur.tp):(b[i].l<=cur.tp));
            bool fx = (EXIT=="flip") && (cur.dir>0?flipDn:flipUp);
            // trail at HMA line
            if(EXIT=="trail"){ double ts=hma[i]; if(cur.dir>0){if(ts>trailstop)trailstop=ts;} else {if(trailstop==0||ts<trailstop)trailstop=ts;} }
            bool trx = (EXIT=="trail") && (cur.dir>0?(b[i].c<trailstop):(b[i].c>trailstop));
            if(sl||tp||fx||trx){
                double ex = sl?cur.sl : (tp?cur.tp : b[i].c);
                double p=(cur.dir>0?(ex-cur.e):(cur.e-ex))-COST; cur.net=p; cur.r=cur.sl!=cur.e?fabs(p)/fabs(cur.e-cur.sl):0; cur.win=p>0; cur.exts=b[i].ts;
                tr.push_back(cur); in=false; trailstop=0;
            }
            continue;
        }
        if(atr[i]<=0) continue;
        bool wantL = flipUp && (SIDE!=2), wantS = flipDn && (SIDE!=1);
        if(CONFIRM){ if(wantL && b[i].c<=b[i].o) wantL=false; if(wantS && b[i].c>=b[i].o) wantS=false; }
        if(PAIR=="ema"){ if(wantL && b[i].c<=emaS[i]) wantL=false; if(wantS && b[i].c>=emaS[i]) wantS=false; }  // regime confirm
        if(SESS0>=0){ int hr=(int)((b[i].ts%86400)/3600); bool ins=(SESS0<=SESS1)?(hr>=SESS0&&hr<SESS1):(hr>=SESS0||hr<SESS1); if(!ins){wantL=wantS=false;} }
        if(wantL||wantS){
            int dir=wantL?1:-1; double e=b[i].c;
            double sl = (STOP=="hma")? hma[i] - dir*0.0 : e - dir*KATR*atr[i];
            if(STOP=="hma") sl = dir>0? min(hma[i], e-0.2*atr[i]) : max(hma[i], e+0.2*atr[i]);
            double rdist=fabs(e-sl); if(rdist<0.1*atr[i]) continue;
            double tp = dir>0? e+RR*rdist : e-RR*rdist;
            cur=T{e,sl,tp,dir,0,0,false}; in=true; trailstop=0;
        }
    }
    auto rep=[&](const char*tag,int lo,int hi){int n=0,w=0;double net=0,gw=0,gl=0,sR=0,pk=0,cm=0,dd=0;vector<double>rs;
        for(int i=lo;i<hi;++i){auto&t=tr[i];n++;net+=t.net;double R=t.r*(t.win?1:-1);sR+=R;rs.push_back(R);
            if(t.win){w++;gw+=t.net;}else gl+=-t.net;cm+=t.net;if(cm>pk)pk=cm;if(pk-cm>dd)dd=pk-cm;}
        double pf=gl>0?gw/gl:(gw>0?99:0),mR=n?sR/n:0,var=0;for(double r:rs)var+=(r-mR)*(r-mR);double sd=rs.size()>1?sqrt(var/(rs.size()-1)):0;
        double tpy=n/2.0,sh=sd>0?mR/sd*sqrt(tpy>0?tpy:1):0;
        printf("  %-9s n=%4d WR=%4.1f%% PF=%4.2f net=%9.1f Sharpe=%5.2f maxDD=%8.1f\n",tag,n,n?100.0*w/n:0,pf,net,sh,dd);};
    printf("[%s] tf=%dm exit=%s rr=%.1f stop=%s katr=%.1f confirm=%d side=%d pmul=%.1f cost=%.1f | tr=%d\n",
        NAME.c_str(),TF,EXIT.c_str(),RR,STOP.c_str(),KATR,CONFIRM,SIDE,PMUL,COST,(int)tr.size());
    if(tr.empty()){printf("  (none)\n");return 0;}
    rep("ALL",0,(int)tr.size());int mid=(int)tr.size()/2;rep("H1",0,mid);rep("H2",mid,(int)tr.size());
    if(!DUMP.empty()){ ofstream o(DUMP); o<<"exts,net\n"; for(auto&t:tr) o<<t.exts<<","<<t.net<<"\n"; }
    return 0;}
