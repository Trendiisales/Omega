// adaptive_rsi.cpp — C++ port of "Adaptive Modern RSI [GBB]" (Pine v6) + backtest.
//
// Pipeline (faithful):
//   * SuperSmoother (Ehlers 2-pole) on gains/losses -> ss_rsi(smooth)
//   * 60-candidate grid: 5 periods x 3 smoothers x 4 OS/OB pairs
//   * per-bar rolling (incremental add/sub over `lookback`) forward-return score
//     of each candidate's OS-entry / OB-exit triggers (eval_bars horizon)
//   * select best avg-fwd-return with switch-margin + min-triggers stickiness
//   * regime classify + hysteresis (display only)
//   * triggers: os_entry (RSI crosses below OS), ob_exit (RSI crosses below OB)
//
// *** PORT NOTE — bug in the published Pine: ss_rsi(rsi_period, smooth_period)
//     IGNORES rsi_period (body only uses smooth_period). So the 5 "periods"
//     produce identical RSI series; only smoother(3) x threshold(4) = 12 real
//     behaviours. Ported faithfully (period vestigial). ***
//
// Backtest: cost-incl, walk-forward, gold. The indicator is a MEAN-REVERSION
// tool (author: works in chop, dangerous in trend, NOT standalone). Tested as:
//   (A) standalone mean-revert long-only, (B) regime-gated (CHOP only).
//
// build: g++ -std=c++17 -O2 backtest/adaptive_rsi.cpp -o backtest/adaptive_rsi

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Bar { long long ts; double o,h,l,c; };
static std::vector<Bar> load_csv(const std::string& p, bool ms){
    std::vector<Bar> v; std::ifstream f(p); if(!f){std::printf("[load] cannot open %s\n",p.c_str());return v;}
    std::string ln; bool first=true;
    while(std::getline(f,ln)){ if(first){first=false; if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9')&&ln[0]!='-')continue;}
        std::stringstream ss(ln); std::string t; std::vector<std::string> k; while(std::getline(ss,t,','))k.push_back(t);
        if(k.size()<5)continue; Bar b; long long ts=std::atoll(k[0].c_str()); b.ts=ms?ts/1000:ts;
        b.o=std::atof(k[1].c_str());b.h=std::atof(k[2].c_str());b.l=std::atof(k[3].c_str());b.c=std::atof(k[4].c_str());
        if(b.h>0)v.push_back(b);} return v; }

static const double PI=3.14159265358979323846, SQ2=1.4142135623730951;

// SuperSmoother over a full series.
static std::vector<double> supersmooth(const std::vector<double>& src, int period){
    int N=(int)src.size(); std::vector<double> ss(N,0);
    double a=std::exp(-SQ2*PI/period), b=2*a*std::cos(SQ2*PI/period);
    double c2=b, c3=-a*a, c1=1-c2-c3;
    for(int n=0;n<N;++n){ double s1=n>=1?src[n-1]:src[n]; double ss1=n>=1?ss[n-1]:src[n]; double ss2=n>=2?ss[n-2]:src[n];
        ss[n]= c1*(src[n]+s1)/2 + c2*ss1 + c3*ss2; }
    return ss; }

int main(int argc,char**argv){
    std::string path=argc>1?argv[1]:"phase1/signal_discovery/warmup_XAUUSD_H1.csv";
    int lookback = argc>2?std::atoi(argv[2]):300;
    int eval_bars=5, min_trig=5; double switch_margin=10.0;
    bool ms = path.find("_H1")==std::string::npos && path.find("_D1")==std::string::npos;
    auto bars=load_csv(path,ms); int N=(int)bars.size();
    std::printf("[tape] %s : %d bars lookback=%d eval=%d  %.2f..%.2f\n",
        path.c_str(),N,lookback,eval_bars, N?bars.front().c:0, N?bars.back().c:0);
    if(N<lookback+50){std::printf("too few bars\n");return 1;}

    std::vector<double> close(N); for(int i=0;i<N;++i)close[i]=bars[i].c;
    // gains/losses
    std::vector<double> g(N,0),l(N,0); for(int n=1;n<N;++n){double ch=close[n]-close[n-1]; g[n]=std::max(ch,0.0);l[n]=std::max(-ch,0.0);}
    // 3 distinct ss-RSI by smoother (6,10,16). period axis is vestigial (Pine bug).
    int smooths[3]={6,10,16}; int periods[5]={7,10,14,21,28};
    bool FIX = (argc>3 && std::string(argv[3])=="fix");
    auto rma=[&](const std::vector<double>& x,int n){ std::vector<double> o(N,0); double a=1.0/n,prev=0;
        for(int i=0;i<N;++i){ prev=(i==0)?x[i]:prev+a*(x[i]-prev); o[i]=prev; } return o; };
    // rsi15[pi*3+si]. orig: period ignored (Pine bug), smooth drives SuperSmoother.
    // fix: period drives RMA(gains/losses) -> classic RSI, then smooth = output SuperSmoother.
    std::vector<std::vector<double>> rsi15(15, std::vector<double>(N,NAN));
    for(int pi=0;pi<5;++pi)for(int si=0;si<3;++si){
        std::vector<double> ag,al;
        if(FIX){ ag=rma(g,periods[pi]); al=rma(l,periods[pi]); }
        else   { ag=supersmooth(g,smooths[si]); al=supersmooth(l,smooths[si]); }
        std::vector<double> raw(N,NAN);
        for(int n=0;n<N;++n){ if(al[n]==0)raw[n]=NAN; else {double rs=ag[n]/al[n]; raw[n]=100-100/(1+rs);} }
        if(FIX){ std::vector<double> rf(N,50); for(int n=0;n<N;++n)rf[n]=std::isnan(raw[n])?(n>0?rf[n-1]:50):raw[n];
            auto sm=supersmooth(rf,smooths[si]); for(int n=0;n<N;++n)rsi15[pi*3+si][n]=sm[n]; }
        else for(int n=0;n<N;++n)rsi15[pi*3+si][n]=raw[n];
    }
    auto RS=[&](int pi,int si)->const std::vector<double>&{ return rsi15[pi*3+si]; };
    std::printf("[mode] %s\n", FIX?"FIXED (period drives RSI)":"ORIGINAL (period vestigial)");
    double OSv[4]={15,20,25,30}, OBv[4]={85,80,75,70};
    auto pidx=[&](int pi,int si,int ti){return pi*3*4+si*4+ti;};

    // candidate sum_ret/cnt (incremental rolling), and trigger_at like Pine
    const int NPI=5,NSI=3,NTI=4,NC=NPI*NSI*NTI;
    std::vector<double> sum_ret(NC,0.0); std::vector<int> trig_cnt(NC,0);

    int active_pi=2,active_si=1,active_ti=2; double active_score=NAN;
    // regime hysteresis
    int disp_reg=0,cand_reg=0,cand_streak=0,bars_held=0; int reg_hyst=10;
    std::vector<int> activePi(N,2),activeSi(N,1),activeTi(N,2),dispReg(N,0);

    // trading state (variant A: mean-revert long-only, exit on ob_exit or timeout/target)
    auto run=[&](bool regime_gate, int max_hold, double cost, int lo,int hi,
                 const std::vector<int>& aPi,const std::vector<int>& aSi,const std::vector<int>& aTi,const std::vector<int>& dReg,
                 int& n_out,double& net_out,double& pf_out,double& wr_out,double& dd_out){
        int pos=0,heldb=0; double entry=0,cum=0,peak=0,gw=0,gl=0; int nt=0,wins=0;double maxdd=0;
        auto closeT=[&](double ex){double pnl=(ex-entry)-cost; nt++; if(pnl>=0){wins++;gw+=pnl;}else gl+=-pnl;
            cum+=pnl; if(cum>peak)peak=cum; if(peak-cum>maxdd)maxdd=peak-cum;};
        for(int n=std::max(lo,1);n<hi;++n){ int pi=aPi[n],si=aSi[n],ti=aTi[n]; const auto& R=RS(pi,si);
            double rn=R[n],rp=R[n-1]; if(std::isnan(rn)||std::isnan(rp))continue;
            double os=OSv[ti],ob=OBv[ti];
            bool osE = rp>=os && rn<os;          // entered oversold -> long
            bool obX = rp>=ob && rn<ob && rp>ob; // rolled out of OB -> exit
            if(pos==0){ bool gate = !regime_gate || dReg[n]==3 /*CHOP*/; if(osE && gate){pos=1;entry=close[n];heldb=0;} }
            else { heldb++; if(obX || rn>=ob || heldb>=max_hold){ closeT(close[n]); pos=0; } }
        }
        if(pos) closeT(close[hi-1]);
        n_out=nt; net_out=cum; pf_out=gl>0?gw/gl:0; wr_out=nt>0?100.0*wins/nt:0; dd_out=maxdd;
    };

    // main pass: incremental scoring + selection + regime, record active params
    for(int n=0;n<N;++n){
        activePi[n]=active_pi; activeSi[n]=active_si; activeTi[n]=active_ti; dispReg[n]=disp_reg;
        if(n<=lookback+eval_bars){ continue; }
        // --- recompute candidate scores over the lookback window (correct, O(lookback*12)) ---
        std::fill(sum_ret.begin(),sum_ret.end(),0.0); std::fill(trig_cnt.begin(),trig_cnt.end(),0);
        for(int b=eval_bars; b<=lookback; ++b){ int bb=n-b; if(bb-eval_bars<0||bb+1>=N)continue;
            for(int pi=0;pi<NPI;++pi)for(int si=0;si<NSI;++si){
                const auto& R=RS(pi,si); double rn=R[bb],rp=R[bb+1]; if(std::isnan(rn)||std::isnan(rp))continue;
                for(int ti=0;ti<NTI;++ti){ double os=OSv[ti],ob=OBv[ti]; double fret=0; int kind=0;
                    if(rp>=os&&rn<os){double pt=close[bb],pf=close[bb-eval_bars]; if(pt>0){kind=1;fret=pf/pt-1;}}
                    else if(rp>=ob&&rn<ob&&rp>ob){double pt=close[bb],pf=close[bb-eval_bars]; if(pt>0){kind=2;fret=-(pf/pt-1);}}
                    if(kind){int id=pidx(pi,si,ti); sum_ret[id]+=fret; trig_cnt[id]++;} }
            }
        }
        // selection (faithful: strict > tie-break -> lowest pi/si/ti wins)
        double best=NAN; int bpi=active_pi,bsi=active_si,bti=active_ti;
        for(int pi=0;pi<NPI;++pi)for(int si=0;si<NSI;++si)for(int ti=0;ti<NTI;++ti){int id=pidx(pi,si,ti);
            if(trig_cnt[id]>=min_trig){double sc=sum_ret[id]/trig_cnt[id]; if(std::isnan(best)||sc>best){best=sc;bpi=pi;bsi=si;bti=ti;}}}
        if(!std::isnan(best)){
            if(std::isnan(active_score)){active_pi=bpi;active_si=bsi;active_ti=bti;active_score=best;}
            else{ int cid=pidx(active_pi,active_si,active_ti); if(trig_cnt[cid]>=min_trig)active_score=sum_ret[cid]/trig_cnt[cid];
                double margin=std::fabs(active_score)*switch_margin/100.0;
                if(best>active_score+margin){active_pi=bpi;active_si=bsi;active_ti=bti;active_score=best;} }
        }
        // regime classify
        int raw;
        if(std::isnan(active_score))raw=0;
        else if(std::fabs(active_score)<0.0005&&active_si==2)raw=1;
        else if(active_pi>=3&&active_ti>=2)raw=2;
        else if(active_pi<=1&&active_ti<=1)raw=3;
        else raw=4;
        if(raw==cand_reg)cand_streak++; else {cand_reg=raw;cand_streak=1;}
        if(cand_streak>=reg_hyst&&cand_reg!=disp_reg){disp_reg=cand_reg;bars_held=1;} else bars_held++;
        activePi[n]=active_pi; activeSi[n]=active_si; activeTi[n]=active_ti; dispReg[n]=disp_reg;
    }

    const double COST=0.45;
    auto rep=[&](const char* tag,bool gate,int hold){
        int mid=N/2; int n; double net,p,w,dd;
        run(gate,hold,COST,0,N,activePi,activeSi,activeTi,dispReg,n,net,p,w,dd);
        int n1;double net1,p1,w1,dd1; run(gate,hold,COST,0,mid,activePi,activeSi,activeTi,dispReg,n1,net1,p1,w1,dd1);
        int n2;double net2,p2,w2,dd2; run(gate,hold,COST,mid,N,activePi,activeSi,activeTi,dispReg,n2,net2,p2,w2,dd2);
        std::printf("%-30s | n=%4d PF=%.2f WR=%4.1f%% net=%8.1f DD=%7.1f rDD=%5.2f | H1=%7.1f H2=%7.1f %s\n",
            tag,n,p,w,net,dd,dd>0?net/dd:0,net1,net2,(net1>0&&net2>0)?"BOTH+":"");
    };
    std::printf("\n=== Adaptive RSI mean-revert, LONG-ONLY (cost-incl $%.2f, WF) ===\n",COST);
    std::printf("buy&hold net = %.1f pts\n", bars.back().c-bars.front().c);
    rep("standalone hold-to-OB",     false, 100000);
    rep("standalone hold<=24b",      false, 24);
    rep("standalone hold<=eval(5)",  false, eval_bars);
    rep("regime-gated CHOP hold-OB", true,  100000);
    rep("regime-gated CHOP hold<=24",true,  24);
    // regime distribution
    int rc[5]={0,0,0,0,0}; for(int n=lookback+eval_bars+1;n<N;++n)rc[dispReg[n]]++;
    std::printf("regime bars: Adapt=%d Noisy=%d Trend=%d CHOP=%d Quiet=%d\n",rc[0],rc[1],rc[2],rc[3],rc[4]);
    return 0;
}
