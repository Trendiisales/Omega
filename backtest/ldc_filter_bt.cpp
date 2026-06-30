// ldc_filter_bt.cpp — Lorentzian Distance Classifier (LDC) as an ENTRY-QUALITY
// FILTER, faithful-tick batch backtest. Sibling of batch_bt.cpp.
//
// WHAT THIS IS / WHY:
//   The TradingView "Machine Learning: Lorentzian Classification" indicator
//   (jdehorty) is, stripped of marketing, a kNN vote over normalized oscillators
//   (RSI/WT/CCI/ADX) using a log-warped ("Lorentzian") distance, predicting the
//   sign of the trailing 4-bar move. As a STANDALONE signal it is a smoothed
//   momentum classifier (beta-mirage risk on trending instruments, per the
//   Omega kill-record on Market-Memory-kNN / ML-regime overlays).
//
//   The only port worth doing is to test whether its distance-warped kNN vote
//   adds value as a *gate* on an already-validated breakout entry. So each cell
//   below runs TWICE in the same single tick pass:
//     RAW   — the breakout fires unconditionally (baseline, = batch_bt cell)
//     LDC   — the breakout fires ONLY if the LDC prediction agrees with the
//             breakout direction (long needs pred>0, short needs pred<0)
//   If LDC PF > RAW PF in BOTH regimes AND both halves -> the filter adds edge.
//   If not -> tombstone the indicator for Omega. This is the BACKTEST_TRUTH gate.
//
// Faithful fills identical to batch_bt.cpp: real bid/ask spread, intrabar
// stop-before-tp ordering, stop/maxhold market-slip, per-side commission.
//
// Build + run on the Mac directly (no winsock/cmake):
//   clang++ -std=c++20 -O2 -o ldc_filter_bt ldc_filter_bt.cpp
//   ./ldc_filter_bt BEAR=/Users/jo/Tick/xau_2022bear_tick.csv BULL=/Users/jo/Tick/xau_6mo_corrected.csv
//
// Tick CSV format: `timestamp,bid,ask` (ms epoch). Header auto-skipped.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>

static const double COMM_PER_SIDE = 0.50; // $/oz; ~1.5bps + buffer. Spread paid via bid/ask fills.
static const double STOP_SLIP     = 0.30; // $/oz adverse on stop + maxhold (market) exits
static const int    BAR_SEC       = 1800; // M30
static const int    ATR_N         = 14;
static const int    EMA_N         = 20;
static const int    MAXHOLD_BARS  = 96;   // ~2 days safety exit-at-market

// ---- LDC params (mirror the Pine defaults) ----
static const int    LDC_NEIGHBORS = 8;     // settings.neighborsCount
static const int    LDC_FEATURES  = 5;     // settings.featureCount
static const int    LDC_MAXBACK   = 2000;  // settings.maxBarsBack (sliding window of bars)
static const int    LDC_LABEL_LAG = 4;     // hardcoded 4-bar horizon in the indicator

struct Bar { int64_t ts; double o,h,l,c; };

enum Sig { KELTNER=0, DONCHIAN=1, LDC_NATIVE=2 };
struct CellCfg {
    const char* name;
    Sig sig; double k; int donN;
    double stop_atr, tp_atr;
    bool allow_long, allow_short;
    bool use_ldc;                 // false = RAW baseline, true = LDC-gated
    int  maxhold;                 // per-cell time exit (LDC native = 4 bars)
};

struct Acc {
    int n[2]={0,0}; double net[2]={0,0}, gross_pos[2]={0,0}, gross_neg[2]={0,0}; int win[2]={0,0};
};
struct CellState {
    int pos=0; double entry=0, stop=0, tp=0; int bars_held=0;
    int pending=0; double pend_atr=0;
};
struct Cell { CellCfg cfg; CellState st; Acc acc; };

static inline void book(Acc& a, int half, double pnl){
    a.n[half]++; a.net[half]+=pnl;
    if(pnl>=0){ a.gross_pos[half]+=pnl; a.win[half]++; }
    else a.gross_neg[half]+= -pnl;
}

// ===================== indicators (ATR / EMA / Donchian) =====================
struct Indi {
    std::deque<Bar> bars; double ema=0; bool ema_seeded=false; double atr=0; bool atr_seeded=false;
    void push(const Bar& b){
        if(!bars.empty()){
            double pc = bars.back().c;
            double tr = std::max(b.h-b.l, std::max(std::fabs(b.h-pc), std::fabs(b.l-pc)));
            if(!atr_seeded){ atr=tr; atr_seeded=true; }
            else atr = (atr*(ATR_N-1)+tr)/ATR_N;
        }
        double a = 2.0/(EMA_N+1);
        if(!ema_seeded){ ema=b.c; ema_seeded=true; } else ema = a*b.c + (1-a)*ema;
        bars.push_back(b);
        if((int)bars.size()>120) bars.pop_front();
    }
    double donch_hi(int N) const { double m=-1e18; int c=0; for(auto it=bars.rbegin(); it!=bars.rend()&&c<N; ++it,++c) m=std::max(m,it->h); return m; }
    double donch_lo(int N) const { double m=1e18;  int c=0; for(auto it=bars.rbegin(); it!=bars.rend()&&c<N; ++it,++c) m=std::min(m,it->l); return m; }
    bool ready() const { return atr_seeded && ema_seeded && (int)bars.size()>=std::max(ATR_N,EMA_N)+2; }
};

// ===================== LDC feature engine =====================
// Computes 5 normalized features per M30 bar (matching the Pine defaults:
// f1=RSI14, f2=WT(10,11), f3=CCI20, f4=ADX14, f5=RSI9), maintains rolling
// history + the 4-bar trailing-move label, and runs the approximate-NN
// Lorentzian vote. Normalization mirrors Pine: bounded oscillators (RSI/ADX)
// use fixed rescale to [0,1]; unbounded (WT/CCI) use cumulative min/max
// (== Pine's `normalize`).
struct Norm { double mn=1e18, mx=-1e18; double apply(double x){ mn=std::min(mn,x); mx=std::max(mx,x); double r=mx-mn; return r>1e-12? (x-mn)/r : 0.0; } };

struct LDC {
    // --- RSI state (Wilder) ---
    struct RSI { int n; double ag=0,al=0; bool seed=false; double pc=0; bool hasc=false;
        double step(double c){ if(!hasc){pc=c;hasc=true;return 50.0;} double ch=c-pc; pc=c;
            double g=ch>0?ch:0, l=ch<0?-ch:0;
            if(!seed){ag=g;al=l;seed=true;} else {ag=(ag*(n-1)+g)/n; al=(al*(n-1)+l)/n;}
            double rs= al>1e-12? ag/al : (ag>0?100.0:0.0); return 100.0 - 100.0/(1.0+rs); } };
    // --- CCI(20) over hlc3 ---
    std::deque<double> tp_win; int cci_n=20;
    double cci(double tp){ tp_win.push_back(tp); if((int)tp_win.size()>cci_n) tp_win.pop_front();
        int m=tp_win.size(); double sma=0; for(double v:tp_win) sma+=v; sma/=m;
        double md=0; for(double v:tp_win) md+=std::fabs(v-sma); md/=m;
        return md>1e-12? (tp-sma)/(0.015*md) : 0.0; }
    // --- WaveTrend(10,11) over hlc3 ---
    double wt_esa=0, wt_d=0; bool wt_seed=false; std::deque<double> tci_win; int wt_n2=11;
    double wt_ema(double& s,double x,int n,bool& sd){ double a=2.0/(n+1); if(!sd){s=x;sd=true;}else s=a*x+(1-a)*s; return s; }
    bool wt_esa_sd=false, wt_d_sd=false, wt_tci_sd=false; double wt_tci=0;
    double wavetrend(double hlc3){
        double esa=wt_ema(wt_esa,hlc3,10,wt_esa_sd);
        double d  =wt_ema(wt_d,std::fabs(hlc3-esa),10,wt_d_sd);
        double ci = d>1e-12? (hlc3-esa)/(0.015*d) : 0.0;
        double tci= wt_ema(wt_tci,ci,wt_n2,wt_tci_sd);
        return tci;
    }
    // --- ADX(14) ---
    double adx_atr=0; bool adx_seed=false; double pdm=0,ndm=0; double adx=0; bool adx_dx_seed=false;
    double ph=0,pl=0,pc2=0; bool adx_hasc=false; double sdi=0; // smoothed
    double sm_tr=0, sm_pdm=0, sm_ndm=0; bool adx_sm_seed=false;
    double adx_step(double h,double l,double c){
        if(!adx_hasc){ph=h;pl=l;pc2=c;adx_hasc=true;return 0.0;}
        double up=h-ph, dn=pl-l;
        double pDM= (up>dn && up>0)? up:0; double nDM=(dn>up && dn>0)? dn:0;
        double tr=std::max(h-l,std::max(std::fabs(h-pc2),std::fabs(l-pc2)));
        ph=h;pl=l;pc2=c;
        int n=14;
        if(!adx_sm_seed){ sm_tr=tr; sm_pdm=pDM; sm_ndm=nDM; adx_sm_seed=true; }
        else { sm_tr=sm_tr-sm_tr/n+tr; sm_pdm=sm_pdm-sm_pdm/n+pDM; sm_ndm=sm_ndm-sm_ndm/n+nDM; }
        double pDI= sm_tr>1e-12? 100.0*sm_pdm/sm_tr:0;
        double nDI= sm_tr>1e-12? 100.0*sm_ndm/sm_tr:0;
        double dx = (pDI+nDI)>1e-12? 100.0*std::fabs(pDI-nDI)/(pDI+nDI):0;
        if(!adx_dx_seed){ adx=dx; adx_dx_seed=true; } else adx=(adx*(n-1)+dx)/n;
        return adx;
    }

    RSI rsi14{14}, rsi9{9};
    Norm nWT, nCCI; // cumulative normalize for unbounded features

    // history (oldest-first), aligned
    std::vector<double> f1,f2,f3,f4,f5; std::vector<int> label;
    std::deque<double> close_hist; // for 4-bar label

    // compute features for a freshly closed bar; returns the current feature vec
    // (does NOT yet push into history — predict-then-learn)
    void features(const Bar& b, double& o1,double& o2,double& o3,double& o4,double& o5){
        double hlc3=(b.h+b.l+b.c)/3.0;
        double r14=rsi14.step(b.c);
        double wt =wavetrend(hlc3);
        double cc =cci(hlc3);
        double ax =adx_step(b.h,b.l,b.c);
        double r9 =rsi9.step(b.c);
        o1 = r14/100.0;            // RSI bounded
        o2 = nWT.apply(wt);        // WT cumulative-normalized
        o3 = nCCI.apply(cc);       // CCI cumulative-normalized
        o4 = std::min(ax/100.0,1.0); // ADX bounded
        o5 = r9/100.0;             // RSI bounded
    }

    static inline double lor(double a,double b){ return std::log(1.0+std::fabs(a-b)); }

    // approximate-NN Lorentzian vote over the rolling window; returns sum-of-labels
    double predict(double q1,double q2,double q3,double q4,double q5){
        int len = (int)label.size();
        if(len < LDC_NEIGHBORS+8) return 0.0;
        int start = std::max(0, len - LDC_MAXBACK);
        std::vector<double> dist; std::vector<double> pred;
        dist.reserve(LDC_NEIGHBORS+2); pred.reserve(LDC_NEIGHBORS+2);
        double lastDistance = -1.0;
        for(int i=start;i<len;i++){
            if(i % 4 == 0) continue;                 // chronological spacing (Pine i%4!=0)
            double d = lor(q1,f1[i])+lor(q2,f2[i])+lor(q3,f3[i])+lor(q4,f4[i])+lor(q5,f5[i]);
            if(d >= lastDistance){
                lastDistance = d;
                dist.push_back(d); pred.push_back((double)label[i]);
                if((int)pred.size() > LDC_NEIGHBORS){
                    lastDistance = dist[(int)std::lround(LDC_NEIGHBORS*3.0/4.0)];
                    dist.erase(dist.begin()); pred.erase(pred.begin());
                }
            }
        }
        double s=0; for(double v:pred) s+=v; return s;
    }

    // push the just-closed bar into history + assign its trailing-4-bar label
    void learn(double q1,double q2,double q3,double q4,double q5,double close){
        close_hist.push_back(close);
        // label for the bar that closed LDC_LABEL_LAG bars ago is now knowable;
        // but Pine labels the CURRENT bar by close[4]<close[0]. Replicate: label of
        // THIS bar = sign(close_now - close_4ago). Needs >=5 closes.
        int lbl=0;
        if((int)close_hist.size() > LDC_LABEL_LAG){
            double c0=close_hist.back();
            double c4=close_hist[close_hist.size()-1-LDC_LABEL_LAG];
            lbl = c0>c4? 1 : (c0<c4? -1 : 0);
        }
        f1.push_back(q1); f2.push_back(q2); f3.push_back(q3); f4.push_back(q4); f5.push_back(q5);
        label.push_back(lbl);
        if((int)close_hist.size()>LDC_MAXBACK+16) close_hist.pop_front();
        // keep feature vectors bounded to window+slack (indices stay aligned because
        // predict() uses start=len-MAXBACK; we trim from front and never index below it)
        // NOTE: trimming front would shift indices; simpler to retain all — memory is
        // fine for these file sizes (~tens of thousands of M30 bars).
    }
};

int main(int argc, char** argv){
    std::vector<Cell> cells;
    auto add=[&](const char* nm, Sig s, double k, int N, double stp, double tp, bool L, bool S, bool ldc, int mh){
        Cell c; c.cfg={nm,s,k,N,stp,tp,L,S,ldc,mh}; cells.push_back(c);
    };
    // A) breakout cells: RAW baseline then LDC-gated (does LDC filter help momentum?)
    add("Kelt_k1.5_R3  RAW", KELTNER, 1.5, 0, 2.0, 6.0, true, true, false, MAXHOLD_BARS);
    add("Kelt_k1.5_R3  LDC", KELTNER, 1.5, 0, 2.0, 6.0, true, true, true,  MAXHOLD_BARS);
    add("Donch20_R3    RAW", DONCHIAN,0.0,20, 2.0, 6.0, true, true, false, MAXHOLD_BARS);
    add("Donch20_R3    LDC", DONCHIAN,0.0,20, 2.0, 6.0, true, true, true,  MAXHOLD_BARS);
    // B) LDC standalone, the indicator's NATIVE use: enter on pred sign-flip,
    //    hold 4 bars (hardcoded horizon), market exit. wide stop/tp so only the
    //    4-bar time-exit fires == exactly the indicator's trade-stats claim.
    add("LDC_native_4bar  ", LDC_NATIVE,0.0,0, 999.0,999.0, true, true, true, LDC_LABEL_LAG);
    // C) LDC INVERTED (fade): since the vote is anti-momentum, test the contrarian read
    add("LDC_native_4bar INV", LDC_NATIVE,0.0,0, 999.0,999.0, true, true, false, LDC_LABEL_LAG);

    std::vector<std::pair<std::string,std::string>> regimes;
    for(int i=1;i<argc;i++){ std::string a=argv[i]; auto eq=a.find('='); if(eq!=std::string::npos) regimes.push_back({a.substr(0,eq),a.substr(eq+1)}); }
    if(regimes.empty()){ fprintf(stderr,"usage: ldc_filter_bt BEAR=path BULL=path\n"); return 2; }

    for(size_t ri=0; ri<regimes.size(); ++ri){
        const std::string& tag=regimes[ri].first; const std::string& path=regimes[ri].second;
        FILE* f=fopen(path.c_str(),"rb"); if(!f){ fprintf(stderr,"cannot open %s\n",path.c_str()); return 1; }
        struct TR{ int cell; int64_t ts; double pnl; }; std::vector<TR> trades;

        for(auto& c:cells){ c.st=CellState{}; }
        Indi ind; LDC ldc;
        double cur_pred=0.0, prev_pred=0.0;   // LDC prediction at last closed bar (+ prior)
        bool have_bar=false; int64_t bucket=0; Bar cur{0,0,0,0,0};
        char line[256]; bool first=true; int64_t fts=0, lts=0;

        while(fgets(line,sizeof(line),f)){
            char* p=line; if(first){ first=false; if(line[0]<'0'||line[0]>'9') continue; }
            int64_t ts=0; while(*p>='0'&&*p<='9'){ ts=ts*10+(*p-'0'); p++; } if(*p!=',') continue; p++;
            double bid=strtod(p,&p); if(*p!=',') continue; p++; double ask=strtod(p,&p);
            if(ts<=0||bid<=0||ask<=0) continue;
            if(fts==0) fts=ts; lts=ts;
            double mid=(bid+ask)/2.0;
            int64_t b = ts/1000/BAR_SEC;
            if(!have_bar){ bucket=b; cur={ts,mid,mid,mid,mid}; have_bar=true; }
            else if(b!=bucket){
                // ---- bar `cur` closed ----
                if(ind.ready()){
                    // 1) LDC: compute features for cur, predict (using PRIOR history), then learn
                    double q1,q2,q3,q4,q5;
                    ldc.features(cur,q1,q2,q3,q4,q5);
                    cur_pred = ldc.predict(q1,q2,q3,q4,q5);
                    ldc.learn(q1,q2,q3,q4,q5,cur.c);
                    // 2) breakout arming against PRIOR indicator state
                    double e=ind.ema, atr=ind.atr;
                    for(auto& c:cells){
                        if(c.st.pos!=0) continue;
                        bool longBreak=false, shortBreak=false;
                        if(c.cfg.sig==LDC_NATIVE){
                            // native: enter on prediction sign-FLIP. use_ldc=false => inverted (fade)
                            int dir = cur_pred>0? +1 : cur_pred<0? -1 : 0;
                            int pdir= prev_pred>0?+1 : prev_pred<0? -1 : 0;
                            if(!c.cfg.use_ldc) { dir=-dir; pdir=-pdir; }   // INV cell
                            longBreak  = (dir>0 && pdir<=0);
                            shortBreak = (dir<0 && pdir>=0);
                        } else {
                            double up,dn;
                            if(c.cfg.sig==KELTNER){ up=e+c.cfg.k*atr; dn=e-c.cfg.k*atr; }
                            else { up=ind.donch_hi(c.cfg.donN); dn=ind.donch_lo(c.cfg.donN); }
                            longBreak  = c.cfg.allow_long  && cur.c>up;
                            shortBreak = c.cfg.allow_short && cur.c<dn;
                            if(c.cfg.use_ldc){                  // gate breakout by LDC agreement
                                longBreak  = longBreak  && (cur_pred>0);
                                shortBreak = shortBreak && (cur_pred<0);
                            }
                        }
                        if(longBreak){ c.st.pending=+1; c.st.pend_atr=atr; }
                        else if(shortBreak){ c.st.pending=-1; c.st.pend_atr=atr; }
                    }
                    prev_pred = cur_pred;
                } else {
                    // still warming indicators: feed LDC so its history builds too
                    double q1,q2,q3,q4,q5; ldc.features(cur,q1,q2,q3,q4,q5); ldc.learn(q1,q2,q3,q4,q5,cur.c);
                }
                ind.push(cur);
                for(auto& c:cells) if(c.st.pos!=0) c.st.bars_held++;
                bucket=b; cur={ts,mid,mid,mid,mid};
            } else { cur.h=std::max(cur.h,mid); cur.l=std::min(cur.l,mid); cur.c=mid; }

            // ---- per-tick faithful fills ----
            for(size_t ci=0; ci<cells.size(); ++ci){ Cell& c=cells[ci];
                if(c.st.pos==0 && c.st.pending!=0){
                    double ea=c.st.pend_atr;
                    if(c.st.pending==+1){ c.st.entry=ask; c.st.stop=ask-c.cfg.stop_atr*ea; c.st.tp=ask+c.cfg.tp_atr*ea; c.st.pos=+1; }
                    else                { c.st.entry=bid; c.st.stop=bid+c.cfg.stop_atr*ea; c.st.tp=bid-c.cfg.tp_atr*ea; c.st.pos=-1; }
                    c.st.bars_held=0; c.st.pending=0; continue;
                }
                if(c.st.pos==0) continue;
                double exit_px=0; bool out=false;
                if(c.st.pos==+1){
                    if(bid<=c.st.stop){ exit_px=bid-STOP_SLIP; out=true; }
                    else if(bid>=c.st.tp){ exit_px=c.st.tp; out=true; }
                    else if(c.st.bars_held>=c.cfg.maxhold){ exit_px=bid-STOP_SLIP; out=true; }
                } else {
                    if(ask>=c.st.stop){ exit_px=ask+STOP_SLIP; out=true; }
                    else if(ask<=c.st.tp){ exit_px=c.st.tp; out=true; }
                    else if(c.st.bars_held>=c.cfg.maxhold){ exit_px=ask+STOP_SLIP; out=true; }
                }
                if(out){
                    double gross=(c.st.pos==+1)?(exit_px-c.st.entry):(c.st.entry-exit_px);
                    double pnl=gross-2*COMM_PER_SIDE;
                    trades.push_back({(int)ci,ts,pnl});
                    c.st.pos=0; c.st.bars_held=0;
                }
            }
        }
        fclose(f);

        int64_t mid_ts=(fts+lts)/2;
        for(auto& c:cells) c.acc=Acc{};
        for(auto& t:trades){ int half=(t.ts<mid_ts)?0:1; book(cells[t.cell].acc, half, t.pnl); }

        printf("\n========== REGIME: %s  (%s) ==========\n", tag.c_str(), path.c_str());
        printf("%-22s %6s %8s %5s | %6s %8s %5s | %6s %8s\n",
               "cell","PF_h1","net_h1","n_h1","PF_h2","net_h2","n_h2","PF_all","net_all");
        for(auto& c:cells){ Acc&a=c.acc;
            auto pf=[&](int h){ return a.gross_neg[h]>0? a.gross_pos[h]/a.gross_neg[h] : (a.gross_pos[h]>0?99.9:0.0); };
            double gp=a.gross_pos[0]+a.gross_pos[1], gn=a.gross_neg[0]+a.gross_neg[1];
            double pfall= gn>0? gp/gn : (gp>0?99.9:0.0);
            double netall=a.net[0]+a.net[1]; int nall=a.n[0]+a.n[1];
            printf("%-22s %6.2f %8.0f %5d | %6.2f %8.0f %5d | %6.2f %8.0f (n=%d)\n",
                   c.cfg.name, pf(0),a.net[0],a.n[0], pf(1),a.net[1],a.n[1], pfall,netall,nall);
        }
    }
    printf("\nNOTE: faithful tick fills (bid/ask + stop-before-tp + $%.2f/oz slip) + $%.2f/side comm.\n", STOP_SLIP, COMM_PER_SIDE);
    printf("LDC adds edge ONLY if the LDC row beats its RAW row in BOTH regimes AND both halves.\n");
    return 0;
}
