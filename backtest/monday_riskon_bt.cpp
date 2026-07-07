// monday_riskon_bt.cpp
// Reproduce the Monday risk-on anomaly ([[omega-monday-riskon-anomaly]], validated
// 2024-26 m5 NAS/GBP/AUD, BULL-CAVEATED) on 5-index DAILY 2016-2026 — the key test
// the original lacked: does it survive the 2022 BEAR, and does a risk-on regime gate
// (close>SMA200) flatten the bear bleed while keeping the bull edge?
// Monday session = the Monday daily bar; return = (close-open)/open. Long only.
// Cost in bps r/t. Regime split: 2022(bear) / 2020(crash) / other(bull). Both-halves.
// build: clang++ -O2 -std=c++17 monday_riskon_bt.cpp -o monday_bt
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace std;
struct Bar{ long t; double o,h,l,c; };
static int wd(long e){ int w=(int)(((e/86400)%7+4+7)%7); return w; } // 0=Sun..6=Sat
static vector<Bar> load(const char*p){ FILE*f=fopen(p,"r"); if(!f){fprintf(stderr,"no %s\n",p);exit(1);}
    vector<Bar>v; char ln[256]; while(fgets(ln,sizeof ln,f)){ Bar b;
    if(sscanf(ln,"%ld,%lf,%lf,%lf,%lf",&b.t,&b.o,&b.h,&b.l,&b.c)==5) v.push_back(b);} fclose(f); return v;}
static int yr(long e){ long d=e/86400; int y=1970; // crude civil year
    while(true){ int leap=((y%4==0&&y%100!=0)||y%400==0); int dy=leap?366:365; if(d<dy)break; d-=dy; y++;} return y;}

struct St{int n=0;double s=0;int w=0;double wp=0,lp=0,eq=0,pk=0,dd=0;};
static void add(St&x,double bp){x.n++;x.s+=bp;if(bp>0){x.w++;x.wp+=bp;}else x.lp+=bp;
    x.eq+=bp;if(x.eq>x.pk)x.pk=x.eq;double d=x.pk-x.eq;if(d>x.dd)x.dd=d;}
static double pf(const St&x){return x.lp<0?x.wp/(-x.lp):(x.wp>0?99:0);}

int main(int argc,char**argv){
    double cost=argc>1?atof(argv[1]):3.0;
    int SMA=argc>2?atoi(argv[2]):200;   // gate SMA length; engine-faithful = 50 (MondayRiskOn: prevDayClose>SMA50)
    const char* nm[5]={"SPX","NDX","DJ30","GER40","UK100"};
    const char* fp[5]={"/Users/jo/Tick/SPX_daily_2016_2026.csv","/Users/jo/Tick/NDX_daily_2016_2026.csv",
        "/Users/jo/Tick/DJ30_daily_2016_2026.csv","/Users/jo/Tick/GER40_daily_2016_2026.csv","/Users/jo/Tick/UK100_daily_2016_2026.csv"};
    long midT=1593561600;
    printf("=== Monday risk-on (Mon session open->close, long) | cost=%.1fbp ===\n",cost);
    printf("    UNCOND vs GATED(close>SMA%d). regime: 2022=bear, 2020=crash, else bull\n\n",SMA);
    St eU,eU_bear,eU_bull,eU_h1,eU_h2, eG,eG_bear,eG_bull,eG_h1,eG_h2;
    for(int k=0;k<5;k++){ auto v=load(fp[k]); int n=v.size();
        vector<double> sma(n,0); double run=0; for(int i=0;i<n;i++){run+=v[i].c; if(i>=SMA)run-=v[i-SMA].c; if(i>=SMA-1)sma[i]=run/(double)SMA;}
        St U,Ub,Ubl,Uh1,Uh2, G,Gb,Gbl,Gh1,Gh2;
        for(int i=1;i<n;i++){ if(wd(v[i].t)!=1) continue;   // Monday
            if(v[i].o<=0) continue;
            double bp=(v[i].c/v[i].o-1.0)*10000.0 - cost;
            int y=yr(v[i].t); bool bear=(y==2022); bool crash=(y==2020); bool bull=!bear&&!crash; bool fh=v[i].t<midT;
            add(U,bp); add(eU,bp); if(bear){add(Ub,bp);add(eU_bear,bp);} if(bull){add(Ubl,bp);add(eU_bull,bp);} add(fh?Uh1:Uh2,bp); add(fh?eU_h1:eU_h2,bp);
            bool gate = (sma[i-1]>0 && v[i-1].c>sma[i-1]);   // risk-on proxy: prior(Fri) close>SMA200
            if(gate){ add(G,bp); add(eG,bp); if(bear){add(Gb,bp);add(eG_bear,bp);} if(bull){add(Gbl,bp);add(eG_bull,bp);} add(fh?Gh1:Gh2,bp); add(fh?eG_h1:eG_h2,bp); }
        }
        printf("%-6s U: n=%4d net=%7.0f avg=%5.1f PF=%4.2f WR=%2.0f%% bear=%+6.0f bull=%+7.0f h1=%+6.0f h2=%+6.0f DD=%5.0f\n",
            nm[k],U.n,U.s,U.n?U.s/U.n:0,pf(U),U.n?100.0*U.w/U.n:0,Ub.s,Ubl.s,Uh1.s,Uh2.s,U.dd);
        printf("       G: n=%4d net=%7.0f avg=%5.1f PF=%4.2f WR=%2.0f%% bear=%+6.0f bull=%+7.0f h1=%+6.0f h2=%+6.0f DD=%5.0f\n\n",
            G.n,G.s,G.n?G.s/G.n:0,pf(G),G.n?100.0*G.w/G.n:0,Gb.s,Gbl.s,Gh1.s,Gh2.s,G.dd);
    }
    printf("===== EW BASKET =====\n");
    printf("UNCOND: n=%d net=%.0f avg=%.1f PF=%.2f | bear=%+.0f(PF%.2f) bull=%+.0f(PF%.2f) | h1=%+.0f h2=%+.0f DD=%.0f\n",
        eU.n,eU.s,eU.s/eU.n,pf(eU),eU_bear.s,pf(eU_bear),eU_bull.s,pf(eU_bull),eU_h1.s,eU_h2.s,eU.dd);
    printf("GATED : n=%d net=%.0f avg=%.1f PF=%.2f | bear=%+.0f(PF%.2f) bull=%+.0f(PF%.2f) | h1=%+.0f h2=%+.0f DD=%.0f\n",
        eG.n,eG.s,eG.s/eG.n,pf(eG),eG_bear.s,pf(eG_bear),eG_bull.s,pf(eG_bull),eG_h1.s,eG_h2.s,eG.dd);
    return 0;
}
