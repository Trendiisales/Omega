// modern_macd.cpp — C port of "Modern MACD [GBB]" (Pine v6) + proper backtest.
//
// Faithful port of the indicator:
//   * f_ema(src, len)            — EMA accepting a SERIES (per-bar) length
//   * f_dominantCycle(min,max)   — Ehlers homodyne discriminator (adaptive period)
//   * adaptive fast/slow/signal  — periods tuned to the dominant cycle
//   * signal speedup multiplier  — Layer 2
//   * normalization              — None / Percent-of-price / Z-Score (Layer 3)
//   * crossUp / crossDown / zero crosses
// (Layer-4 divergences are chart/alert annotations — not traded here; the
//  actionable signals for a backtest are the MACD/signal crosses.)
//
// Backtest discipline (feedback-never-deploy-without-backtest):
//   cost-inclusive, walk-forward halves, vs CLASSIC MACD + buy&hold baselines,
//   reported per config. No deploy — research only.
//
// build: g++ -std=c++17 -O2 backtest/modern_macd.cpp -o backtest/modern_macd

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Bar { long long ts; double o,h,l,c; };

static std::vector<Bar> load_csv(const std::string& path, bool ts_is_ms) {
    std::vector<Bar> v; std::ifstream f(path);
    if (!f) { std::printf("[load] cannot open %s\n", path.c_str()); return v; }
    std::string line; bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false;
            if (!line.empty() && (line[0]<'0'||line[0]>'9') && line[0]!='-') continue; }
        std::stringstream ss(line); std::string t; std::vector<std::string> k;
        while (std::getline(ss,t,',')) k.push_back(t);
        if (k.size()<5) continue;
        Bar b; long long ts=std::atoll(k[0].c_str()); b.ts = ts_is_ms?ts/1000:ts;
        b.o=std::atof(k[1].c_str()); b.h=std::atof(k[2].c_str());
        b.l=std::atof(k[3].c_str()); b.c=std::atof(k[4].c_str());
        if (b.h>0) v.push_back(b);
    }
    return v;
}

static const double PI = 3.14159265358979323846;
static double nz(double x){ return std::isnan(x)?0.0:x; }

// Stateful variable-length EMA (Pine f_ema).
struct VEma { double e = NAN;
    double step(double src, double len){ double a=2.0/(len+1.0);
        e = std::isnan(e) ? src : a*src + (1.0-a)*e; return e; } };

// ---- Indicator output per bar ----
struct Out { std::vector<double> macd, signal, hist, slowLen, fastLen, sigLen; };

// Ehlers homodyne dominant cycle + full Modern-MACD pipeline over a bar series.
static Out compute(const std::vector<Bar>& bars, bool adaptive,
                   int cycMin, int cycMax, int fastP, int slowP, int sigP,
                   double rFast, double rSig, double sigMult, int normMode, int zLen) {
    const int N = (int)bars.size();
    Out o; o.macd.assign(N,NAN); o.signal.assign(N,NAN); o.hist.assign(N,NAN);
    o.slowLen.assign(N,NAN); o.fastLen.assign(N,NAN); o.sigLen.assign(N,NAN);
    std::vector<double> price(N), smooth(N,0), detr(N,0), i1(N,0), q1(N,0),
        i2(N,0), q2(N,0), re(N,0), im(N,0), period(N,0), smoothP(N,0);
    auto P=[&](const std::vector<double>&a,int idx,int back){int j=idx-back;return j>=0?a[j]:0.0;};

    VEma emaFast, emaSlow, emaSig;
    std::vector<double> macdRawV(N,NAN), macdLineV(N,NAN);

    for (int n=0;n<N;++n){
        price[n] = (bars[n].h + bars[n].l)/2.0;
        double pPrev = (n>0)? period[n-1] : (double)cycMin;
        if (n==0){ period[n]=cycMin; smoothP[n]=cycMin; }
        smooth[n] = (4.0*price[n] + 3.0*P(price,n,1) + 2.0*P(price,n,2) + P(price,n,3))/10.0;
        double adj = 0.075*pPrev + 0.54;
        detr[n] = (0.0962*smooth[n] + 0.5769*P(smooth,n,2) - 0.5769*P(smooth,n,4) - 0.0962*P(smooth,n,6))*adj;
        q1[n] = (0.0962*detr[n] + 0.5769*P(detr,n,2) - 0.5769*P(detr,n,4) - 0.0962*P(detr,n,6))*adj;
        i1[n] = P(detr,n,3);
        double ji = (0.0962*i1[n] + 0.5769*P(i1,n,2) - 0.5769*P(i1,n,4) - 0.0962*P(i1,n,6))*adj;
        double jq = (0.0962*q1[n] + 0.5769*P(q1,n,2) - 0.5769*P(q1,n,4) - 0.0962*P(q1,n,6))*adj;
        double i2p=(n>0)?i2[n-1]:0.0, q2p=(n>0)?q2[n-1]:0.0;
        i2[n] = 0.2*(i1[n]-jq) + 0.8*i2p;
        q2[n] = 0.2*(q1[n]+ji) + 0.8*q2p;
        double rep=(n>0)?re[n-1]:0.0, imp=(n>0)?im[n-1]:0.0;
        re[n] = 0.2*(i2[n]*P(i2,n,1) + q2[n]*P(q2,n,1)) + 0.8*rep;
        im[n] = 0.2*(i2[n]*P(q2,n,1) - q2[n]*P(i2,n,1)) + 0.8*imp;
        double per = pPrev;
        if (im[n]!=0.0 && re[n]!=0.0) per = 2.0*PI/std::atan(im[n]/re[n]);
        per = std::min(std::max(per, 0.67*pPrev), 1.5*pPrev);
        per = std::max(std::min(per, (double)cycMax), (double)cycMin);
        period[n] = 0.2*per + 0.8*pPrev;
        double spPrev = (n>0)?smoothP[n-1]:(double)cycMin;
        smoothP[n] = 0.33*period[n] + 0.67*spPrev;

        double domCyc = smoothP[n];
        double slowL = adaptive ? domCyc : (double)slowP;
        double fastL = adaptive ? std::max(domCyc*rFast,2.0) : (double)fastP;
        double sigB  = adaptive ? std::max(domCyc*rSig ,2.0) : (double)sigP;
        double sigL  = std::max(sigB*sigMult, 2.0);
        o.slowLen[n]=slowL; o.fastLen[n]=fastL; o.sigLen[n]=sigL;

        double fastMA = emaFast.step(bars[n].c, fastL);
        double slowMA = emaSlow.step(bars[n].c, slowL);
        double macdRaw = fastMA - slowMA; macdRawV[n]=macdRaw;

        // normalization on the macd line BEFORE signal smoothing
        double macdLine = macdRaw;
        if (normMode==1) macdLine = bars[n].c!=0.0 ? macdRaw/bars[n].c*100.0 : 0.0;
        else if (normMode==2){
            int lo=n-zLen+1; if(lo>=0){ double s=0; for(int j=lo;j<=n;++j)s+=macdRawV[j];
                double mean=s/zLen, v=0; for(int j=lo;j<=n;++j){double d=macdRawV[j]-mean;v+=d*d;}
                double sd=std::sqrt(v/zLen); macdLine = sd!=0.0?(macdRaw-mean)/sd:0.0; }
            else macdLine = 0.0;
        }
        macdLineV[n]=macdLine;
        double sigLine = emaSig.step(macdLine, sigL);
        o.macd[n]=macdLine; o.signal[n]=sigLine; o.hist[n]=macdLine-sigLine;
    }
    return o;
}

struct Res { int n=0,wins=0; double net=0,gw=0,gl=0,maxdd=0; };
static double pf(const Res&r){return r.gl>0?r.gw/r.gl:0;}
static double wr(const Res&r){return r.n>0?100.0*r.wins/r.n:0;}

// Trade the macd/signal cross. long_short: flip; else long-only (flat on crossDown).
// Returns per-trade net (price points), cost subtracted per round trip.
static Res backtest(const std::vector<Bar>& bars, const Out& o, bool long_short,
                    double cost_pts, int lo, int hi, std::vector<double>* eq=nullptr){
    Res r; int pos=0; double entry=0; double cum=0,peak=0;
    auto close_trade=[&](double exit){ double pnl=(pos>0?(exit-entry):(entry-exit))-cost_pts;
        r.n++; r.net+=pnl; if(pnl>=0){r.wins++;r.gw+=pnl;}else r.gl+=-pnl;
        cum+=pnl; if(cum>peak)peak=cum; if(peak-cum>r.maxdd)r.maxdd=peak-cum;
        if(eq)eq->push_back(cum); };
    for(int n=std::max(lo,1); n<hi; ++n){
        if(std::isnan(o.macd[n])||std::isnan(o.signal[n])||std::isnan(o.macd[n-1])) continue;
        bool cu = o.macd[n-1]<=o.signal[n-1] && o.macd[n]>o.signal[n];
        bool cd = o.macd[n-1]>=o.signal[n-1] && o.macd[n]<o.signal[n];
        double px = bars[n].c;
        if(cu){ if(pos<0){close_trade(px);pos=0;} if(pos==0){pos=1;entry=px;} }
        else if(cd){ if(pos>0){close_trade(px);pos=0;} if(long_short&&pos==0){pos=-1;entry=px;} }
    }
    if(pos!=0) close_trade(bars[hi-1].c);
    return r;
}

static void report(const char* tag, const std::vector<Bar>& b, const Out& o, bool ls, double cost){
    int N=(int)b.size(), mid=N/2;
    Res full=backtest(b,o,ls,cost,1,N);
    Res h1=backtest(b,o,ls,cost,1,mid);
    Res h2=backtest(b,o,ls,cost,mid,N);
    std::printf("%-34s | n=%4d PF=%.2f WR=%4.1f%% net=%8.1f DD=%7.1f rDD=%5.2f | H1net=%7.1f H2net=%7.1f %s\n",
        tag, full.n, pf(full), wr(full), full.net, full.maxdd,
        full.maxdd>0?full.net/full.maxdd:0, h1.net, h2.net,
        (h1.net>0&&h2.net>0)?"BOTH+":"");
}

// ---- Donchian trend proxy (our gold book's family) + MACD confluence filter ----
// Long-only (gold-bull + book is long-biased). Entry: close breaks above prior
// Nin-bar high. Exit: close breaks below prior Nout-bar low. Filter gates the
// ENTRY on adaptive-MACD momentum agreement.
enum Filt { F_NONE, F_MACD_GT_SIG, F_HIST_RISE, F_MACD_GT_0 };
static const char* filt_name(Filt f){ return f==F_NONE?"no-filter":f==F_MACD_GT_SIG?"macd>signal":
    f==F_HIST_RISE?"hist rising":"macd>0"; }

static Res donchian(const std::vector<Bar>& b, const Out& o, int Nin, int Nout,
                    Filt f, double cost, int lo, int hi){
    Res r; int pos=0; double entry=0,cum=0,peak=0;
    auto close_t=[&](double ex){ double pnl=(ex-entry)-cost; r.n++; r.net+=pnl;
        if(pnl>=0){r.wins++;r.gw+=pnl;}else r.gl+=-pnl;
        cum+=pnl; if(cum>peak)peak=cum; if(peak-cum>r.maxdd)r.maxdd=peak-cum; };
    int start=std::max(lo,std::max(Nin,Nout)+1);
    for(int n=start;n<hi;++n){
        if(pos==0){
            double hh=-1e18; for(int j=n-Nin;j<n;++j) hh=std::max(hh,b[j].h);
            if(b[n].c>hh){
                bool ok=true;
                if(!std::isnan(o.macd[n])&&!std::isnan(o.signal[n])){
                    if(f==F_MACD_GT_SIG) ok=o.macd[n]>o.signal[n];
                    else if(f==F_HIST_RISE) ok=(n>0)&&!std::isnan(o.hist[n-1])&&o.hist[n]>=o.hist[n-1];
                    else if(f==F_MACD_GT_0) ok=o.macd[n]>0.0;
                } else if(f!=F_NONE) ok=false;
                if(ok){ pos=1; entry=b[n].c; }
            }
        } else {
            double ll=1e18; for(int j=n-Nout;j<n;++j) ll=std::min(ll,b[j].l);
            if(b[n].c<ll){ close_t(b[n].c); pos=0; }
        }
    }
    if(pos) close_t(b[hi-1].c);
    return r;
}

static void report_filt(const char* tag, const std::vector<Bar>& b, const Out& o,
                        int Nin,int Nout, Filt f, double cost){
    int N=(int)b.size(),mid=N/2;
    Res full=donchian(b,o,Nin,Nout,f,cost,0,N);
    Res h1=donchian(b,o,Nin,Nout,f,cost,0,mid);
    Res h2=donchian(b,o,Nin,Nout,f,cost,mid,N);
    std::printf("  %-14s | n=%4d PF=%.2f WR=%4.1f%% net=%8.1f DD=%7.1f rDD=%5.2f | H1=%7.1f H2=%7.1f %s\n",
        tag, full.n, pf(full), wr(full), full.net, full.maxdd,
        full.maxdd>0?full.net/full.maxdd:0, h1.net, h2.net,
        (h1.net>0&&h2.net>0)?"BOTH+":"");
}

int main(int argc,char**argv){
    std::string path = argc>1?argv[1]:"phase1/signal_discovery/warmup_XAUUSD_H1.csv";
    bool ms = path.find("_H1")==std::string::npos && path.find("_D1")==std::string::npos; // H1/D1 sec; M15/M30/H4 ms
    auto bars = load_csv(path, ms);
    std::printf("[tape] %s : %zu bars  %.2f..%.2f\n", path.c_str(), bars.size(),
        bars.empty()?0:bars.front().c, bars.empty()?0:bars.back().c);
    if(bars.size()<300){std::printf("too few bars\n");return 1;}
    const double COST=0.45; // XAUUSD round-trip ~$0.45 (spread+commission)

    std::printf("\n=== buy & hold ===\n");
    double bh = bars.back().c - bars.front().c;
    std::printf("buy&hold net points = %.1f over %zu bars\n", bh, bars.size());

    std::printf("\n=== MACD-cross strategies (cost-incl $%.2f rt, walk-forward halves) ===\n", COST);
    std::printf("%-34s | %s\n","config","stats");
    // Classic baseline
    Out cl = compute(bars,false,8,50,12,26,9,0.5,0.346,1.0,0,100);
    report("CLASSIC 12/26/9 long-only", bars, cl, false, COST);
    report("CLASSIC 12/26/9 long-short", bars, cl, true, COST);
    // Modern adaptive
    Out ad = compute(bars,true,8,50,12,26,9,0.5,0.346,1.0,0,100);
    report("MODERN adaptive long-only", bars, ad, false, COST);
    report("MODERN adaptive long-short", bars, ad, true, COST);
    // Modern + signal speedup
    Out ads = compute(bars,true,8,50,12,26,9,0.5,0.346,0.5,0,100);
    report("MODERN adapt + sigMult0.5 LO", bars, ads, false, COST);
    report("MODERN adapt + sigMult0.5 LS", bars, ads, true, COST);
    // Modern + z-score normalization
    Out adz = compute(bars,true,8,50,12,26,9,0.5,0.346,1.0,2,100);
    report("MODERN adapt + zscore LO", bars, adz, false, COST);
    report("MODERN adapt + zscore LS", bars, adz, true, COST);
    // Modern + percent norm
    Out adp = compute(bars,true,8,50,12,26,9,0.5,0.346,1.0,1,100);
    report("MODERN adapt + pct LO", bars, adp, false, COST);

    // === Donchian trend + adaptive-MACD confluence FILTER (the real question) ===
    std::printf("\n=== Donchian trend LONG-ONLY + adaptive-MACD entry filter (cost-incl, WF) ===\n");
    for (int Nin : {20, 55}) {
        int Nout = Nin/2;
        std::printf("Donchian breakout N=%d / exit N=%d:\n", Nin, Nout);
        report_filt("no-filter",   bars, ad, Nin,Nout, F_NONE,        COST);
        report_filt("macd>signal", bars, ad, Nin,Nout, F_MACD_GT_SIG, COST);
        report_filt("hist rising", bars, ad, Nin,Nout, F_HIST_RISE,   COST);
        report_filt("macd>0",      bars, ad, Nin,Nout, F_MACD_GT_0,   COST);
    }
    return 0;
}
