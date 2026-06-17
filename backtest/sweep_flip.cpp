// sweep_flip.cpp — mechanize the "Harry" SMC strategy as a LIQUIDITY-SWEEP
// CONTINUATION model (not the reversal-at-key-level shape that died in
// reversal_mss.cpp). Thesis:
//   In an HTF trend, price retraces and SWEEPS a counter-trend swing extreme
//   (grabs liquidity: wick takes the level, body closes back through it),
//   then RESUMES the trend toward the next opposing liquidity pool.
//   Entry  = close of the sweep bar (displacement confirmation).
//   Stop   = beyond the sweep extreme (the invalidation wick).
//   Target = nearest opposing swing-liquidity / prior-day level (technical).
//   Filter = minRR (Harry's >=3R rule, the selectivity lever) + session.
//   Mgmt   = break-even at beAt R (Harry: BE@2R). Optional trail.
// Reports ALL/H1/H2 (walk-forward both halves) + MFE/MAE in R.
// build: g++ -std=c++17 -O2 sweep_flip.cpp -o sweepflip
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
static vector<Bar> load(const string& p){
    vector<Bar> v; ifstream f(p); if(!f){fprintf(stderr,"no file %s\n",p.c_str());return v;}
    string ln; bool first=true;
    while(getline(f,ln)){ if(first){first=false; if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9'))continue;}
        stringstream ss(ln); string t; vector<string> k; while(getline(ss,t,','))k.push_back(t);
        if(k.size()<5)continue; Bar b; b.ts=(int64_t)atoll(k[0].c_str());
        b.o=atof(k[1].c_str()); b.h=atof(k[2].c_str()); b.l=atof(k[3].c_str()); b.c=atof(k[4].c_str());
        if(b.h>0)v.push_back(b); }
    return v;
}
static vector<Bar> agg(const vector<Bar>& base,int N){ vector<Bar> o; int64_t W=(int64_t)N*60,cur=-1; Bar b{};
    for(auto&x:base){ int64_t g=(x.ts/W)*W; if(g!=cur){if(cur>=0)o.push_back(b);cur=g;b.ts=g;b.o=x.o;b.h=x.h;b.l=x.l;b.c=x.c;}
        else{b.h=max(b.h,x.h);b.l=min(b.l,x.l);b.c=x.c;} } if(cur>=0)o.push_back(b); return o; }
static int utc_hm(int64_t ts){int s=(int)(ts%86400);return (s/3600)*100+(s%3600)/60;}
static int64_t utc_day(int64_t ts){return ts/86400;}
static double atrAt(const vector<Bar>&b,int i,int n){ if(i<1)return 0; int lo=max(1,i-n+1); double s=0;int c=0;
    for(int k=lo;k<=i;++k){double tr=max(b[k].h-b[k].l,max(fabs(b[k].h-b[k-1].c),fabs(b[k].l-b[k-1].c)));s+=tr;c++;} return c?s/c:0; }

int main(int argc,char**argv){
    if(argc<2){printf("usage: %s base.csv [HTF=15] [sess=lonny|ny|lon|all] [cost=0.5] [piv=2] [swingWin=60] [maxDol=8] [minRR=3] [beAt=2] [trendMode=ema|off] [emaF=20] [emaS=50] [bufAtr=0.10] [dispAtr=0] [exit=dol|trail] [trailAtr=2.5] [name]\n",argv[0]);return 1;}
    string path=argv[1]; int HTF=argc>2?atoi(argv[2]):15; string SESS=argc>3?argv[3]:"lonny";
    double COST=argc>4?atof(argv[4]):0.5; int PIV=argc>5?atoi(argv[5]):2; int SWIN=argc>6?atoi(argv[6]):60;
    double MAXDOL=argc>7?atof(argv[7]):8.0; double MINRR=argc>8?atof(argv[8]):3.0; double BEAT=argc>9?atof(argv[9]):2.0;
    string TM=argc>10?argv[10]:"ema"; int EMAF=argc>11?atoi(argv[11]):20; int EMAS=argc>12?atoi(argv[12]):50;
    double BUF=argc>13?atof(argv[13]):0.10; double DISP=argc>14?atof(argv[14]):0.0;
    string EXIT=argc>15?argv[15]:"dol"; double TRAILA=argc>16?atof(argv[16]):2.5;
    double MAXSTOP=argc>17?atof(argv[17]):0.0; // 0=off; else skip setups where r > MAXSTOP*atr (tight-invalidation selectivity)
    string NAME=argc>18?argv[18]:path;
    auto base=load(path); if((int)base.size()<500){printf("[%s] few bars\n",NAME.c_str());return 1;}
    auto htf=agg(base,HTF); int NH=(int)htf.size(); if(NH<100){printf("[%s] few htf\n",NAME.c_str());return 1;}
    // HTF EMAs (trend / order-flow proxy)
    vector<double> emF(NH),emS(NH); double kF=2.0/(EMAF+1),kS=2.0/(EMAS+1);
    for(int i=0;i<NH;++i){ if(i==0){emF[i]=htf[i].c;emS[i]=htf[i].c;} else{emF[i]=htf[i].c*kF+emF[i-1]*(1-kF); emS[i]=htf[i].c*kS+emS[i-1]*(1-kS);} }
    auto trendDir=[&](int i){ if(TM=="off")return 0; if(emF[i]>emS[i]&&htf[i].c>emS[i])return 1; if(emF[i]<emS[i]&&htf[i].c<emS[i])return -1; return 0; };
    // confirmed fractal pivots on HTF
    vector<char> pHi(NH,0),pLo(NH,0);
    for(int i=PIV;i<NH-PIV;++i){ bool hi=true,lo=true;
        for(int k=i-PIV;k<=i+PIV;++k){ if(k==i)continue; if(htf[k].h>=htf[i].h)hi=false; if(htf[k].l<=htf[i].l)lo=false; }
        pHi[i]=hi; pLo[i]=lo; }
    // prior-day H/L from base bars
    vector<int64_t> days; vector<double> dhi,dlo; { int64_t cd=-1;double hi=0,lo=0;
        for(auto&x:base){int64_t d=utc_day(x.ts); if(d!=cd){if(cd>=0){days.push_back(cd);dhi.push_back(hi);dlo.push_back(lo);}cd=d;hi=x.h;lo=x.l;} else{hi=max(hi,x.h);lo=min(lo,x.l);}}
        if(cd>=0){days.push_back(cd);dhi.push_back(hi);dlo.push_back(lo);} }
    auto pdh=[&](int64_t ts){int64_t d=utc_day(ts);for(int i=(int)days.size()-1;i>=0;--i)if(days[i]<d)return dhi[i];return -1.0;};
    auto pdl=[&](int64_t ts){int64_t d=utc_day(ts);for(int i=(int)days.size()-1;i>=0;--i)if(days[i]<d)return dlo[i];return -1.0;};
    auto inSess=[&](int64_t ts){int hm=utc_hm(ts); if(SESS=="all")return true; if(SESS=="ny")return hm>=1330&&hm<=1600;
        if(SESS=="lon")return hm>=700&&hm<=1100; if(SESS=="lonny")return hm>=700&&hm<=2100; return true; };
    auto htfDoneIdx=[&](int64_t t){ // index of latest HTF bar fully closed by time t
        int idx=-1; for(int i=0;i<NH;++i){ if(htf[i].ts+(int64_t)HTF*60<=t)idx=i; else break;} return idx; };

    struct T{double entry,sl,tp,r;int dir;double netPts,netR,mfeR,maeR;bool win;int64_t ts;};
    vector<T> trades;
    bool inT=false; T cur{}; double peakFav=0,peakAdv=0,trailStop=0; bool beMoved=false;
    int lastEval=-1;
    for(size_t bi=1;bi<base.size();++bi){
        int64_t now=base[bi].ts; double H=base[bi].h,L=base[bi].l;
        // ---- manage open trade ----
        if(inT){
            int hidx=htfDoneIdx(now); double atr=hidx>0?atrAt(htf,hidx,14):0;
            double fav=cur.dir>0?(H-cur.entry):(cur.entry-L);
            double adv=cur.dir>0?(cur.entry-L):(H-cur.entry);
            if(fav>peakFav)peakFav=fav; if(adv>peakAdv)peakAdv=adv;
            if(!beMoved&&BEAT>0&&fav>=BEAT*cur.r){ cur.sl=cur.entry; beMoved=true; } // break-even
            if(EXIT=="trail"&&atr>0&&peakFav>cur.r){ double ts2=cur.dir>0?(cur.entry+peakFav-TRAILA*atr):(cur.entry-peakFav+TRAILA*atr);
                if(cur.dir>0)trailStop=max(trailStop,ts2); else trailStop=(trailStop==0?ts2:min(trailStop,ts2)); }
            bool hitSL=cur.dir>0?(L<=cur.sl):(H>=cur.sl);
            bool hitTrail=EXIT=="trail"&&trailStop!=0&&(cur.dir>0?(L<=trailStop):(H>=trailStop));
            bool hitTP=EXIT=="dol"&&(cur.dir>0?(H>=cur.tp):(L<=cur.tp));
            if(hitSL||hitTP||hitTrail){ double exit=hitSL?cur.sl:(hitTP?cur.tp:trailStop);
                double pts=(cur.dir>0?(exit-cur.entry):(cur.entry-exit))-COST;
                cur.netPts=pts; cur.netR=cur.r>0?pts/cur.r:0; cur.win=pts>0;
                cur.mfeR=cur.r>0?peakFav/cur.r:0; cur.maeR=cur.r>0?peakAdv/cur.r:0; cur.ts=now;
                trades.push_back(cur); inT=false; }
            continue;
        }
        // ---- look for a signal on the latest just-closed HTF bar ----
        int hidx=htfDoneIdx(now); if(hidx<PIV+2)continue; if(hidx==lastEval)continue; lastEval=hidx;
        if(!inSess(htf[hidx].ts+(int64_t)HTF*60))continue;
        double atr=atrAt(htf,hidx,14); if(atr<=0)continue;
        int tr=trendDir(hidx);
        double barRange=htf[hidx].h-htf[hidx].l; if(DISP>0&&barRange<DISP*atr)continue; // displacement quality
        // SHORT continuation (downtrend): sweep a counter-trend swing HIGH
        if(TM=="off"||tr<0){
            int j=-1; for(int k=hidx-PIV;k>=max(PIV,hidx-SWIN);--k){ if(k+PIV>hidx)continue; if(!pHi[k])continue;
                double L0=htf[k].h; if(htf[hidx].h>L0 && htf[hidx].c<L0){ j=k; break; } } // nearest swept pivot-high
            if(j>=0){ double entry=htf[hidx].c; double sl=htf[hidx].h+BUF*atr; double r=sl-entry;
                if(MAXSTOP>0&&r>MAXSTOP*atr){} else if(r>0.05*atr){ // target = nearest sell-side liquidity below entry (swing low / PDL)
                    double tgt=-1; for(int k=hidx-PIV;k>=max(PIV,hidx-SWIN);--k){ if(k+PIV>hidx)continue; if(!pLo[k])continue;
                        double lv=htf[k].l; if(lv<entry-0.1*atr && (entry-lv)<=MAXDOL*atr){ tgt=(tgt<0?lv:max(tgt,lv)); } }
                    double pl=pdl(now); if(pl>0&&pl<entry-0.1*atr&&(entry-pl)<=MAXDOL*atr)tgt=(tgt<0?pl:max(tgt,pl));
                    if(tgt>0){ double rr=(entry-tgt)/r; if(rr>=MINRR&&rr<=25){
                        cur=T{entry,sl,tgt,r,-1,0,0,0,0,false}; inT=true; peakFav=0;peakAdv=0;trailStop=0;beMoved=false; continue; } } } }
        }
        // LONG continuation (uptrend): sweep a counter-trend swing LOW
        if(TM=="off"||tr>0){
            int j=-1; for(int k=hidx-PIV;k>=max(PIV,hidx-SWIN);--k){ if(k+PIV>hidx)continue; if(!pLo[k])continue;
                double L0=htf[k].l; if(htf[hidx].l<L0 && htf[hidx].c>L0){ j=k; break; } }
            if(j>=0){ double entry=htf[hidx].c; double sl=htf[hidx].l-BUF*atr; double r=entry-sl;
                if(MAXSTOP>0&&r>MAXSTOP*atr){} else if(r>0.05*atr){ double tgt=-1; for(int k=hidx-PIV;k>=max(PIV,hidx-SWIN);--k){ if(k+PIV>hidx)continue; if(!pHi[k])continue;
                        double lv=htf[k].h; if(lv>entry+0.1*atr && (lv-entry)<=MAXDOL*atr){ tgt=(tgt<0?lv:min(tgt,lv)); } }
                    double ph=pdh(now); if(ph>0&&ph>entry+0.1*atr&&(ph-entry)<=MAXDOL*atr)tgt=(tgt<0?ph:min(tgt,ph));
                    if(tgt>0){ double rr=(tgt-entry)/r; if(rr>=MINRR&&rr<=25){
                        cur=T{entry,sl,tgt,r,+1,0,0,0,0,false}; inT=true; peakFav=0;peakAdv=0;trailStop=0;beMoved=false; continue; } } } }
        }
    }
    if(getenv("PORT_DUMP")){FILE*pd=fopen(getenv("PORT_DUMP"),"w");for(auto&td:trades)fprintf(pd,"%lld,%.6f\n",(long long)td.ts,td.netPts);fclose(pd);}
    auto rep=[&](const char*tag,int lo,int hi){ int n=0,w=0;double net=0,gw=0,gl=0,sR=0,pk=0,cm=0,dd=0,sumWp=0,sumLp=0,sMfe=0,sMae=0;
        int nw=0,nl=0,consec=0,maxConsec=0; std::vector<double> rs;
        for(int i=lo;i<hi;++i){auto&t=trades[i];n++;net+=t.netPts;sR+=t.netR;rs.push_back(t.netR);sMfe+=t.mfeR;sMae+=t.maeR;
            if(t.win){w++;gw+=t.netPts;sumWp+=t.netPts;nw++;consec=0;}else{gl+=-t.netPts;sumLp+=-t.netPts;nl++;consec++;if(consec>maxConsec)maxConsec=consec;}
            cm+=t.netPts;if(cm>pk)pk=cm;if(pk-cm>dd)dd=pk-cm;}
        double pf=gl>0?gw/gl:(gw>0?99:0),mR=n?sR/n:0,var=0; for(double r:rs)var+=(r-mR)*(r-mR);
        double sd=rs.size()>1?std::sqrt(var/(rs.size()-1)):0,shT=sd>0?mR/sd:0;
        double avgW=nw?sumWp/nw:0,avgL=nl?sumLp/nl:0,plr=avgL>0?avgW/avgL:0;
        printf("  %s n=%d WR=%.1f%% PF=%.2f net=%.2f avgR=%+.2f | Sh/tr=%.2f | MFE=%.2fR MAE=%.2fR | maxDD=%.2f ret/DD=%.2f maxConL=%d\n",
            tag,n,n?100.0*w/n:0,pf,net,mR,shT,n?sMfe/n:0,n?sMae/n:0,dd,dd>0?net/dd:0,maxConsec); };
    printf("[%s] HTF=%dm sess=%s cost=%.4f piv=%d swin=%d maxDol=%.0fA minRR=%.1f beAt=%.1f trend=%s(%d/%d) buf=%.2f disp=%.1f exit=%s | tr=%d\n",
        NAME.c_str(),HTF,SESS.c_str(),COST,PIV,SWIN,MAXDOL,MINRR,BEAT,TM.c_str(),EMAF,EMAS,BUF,DISP,EXIT.c_str(),(int)trades.size());
    if(trades.empty()){printf("  (no trades)\n");return 0;}
    rep("ALL",0,(int)trades.size()); int mid=(int)trades.size()/2; rep("H1",0,mid); rep("H2",mid,(int)trades.size());
    return 0;
}
