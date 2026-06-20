// batch_bt.cpp — standalone faithful-tick BATCH backtest harness (array of cells).
//
// WHY: the existing per-cpp backtests each hand-roll one engine + one tick pass.
// This is the generalized "expedite testing" runner the operator asked for: ONE
// streaming pass over a tick file fans out to an ARRAY of engine×config cells,
// each tracked independently, with FAITHFUL tick-level fills (real bid/ask spread
// + intrabar stop-before-tp ordering) — the gate-grade upgrade over bar-replay,
// which fills at the bar open and can't resolve which of stop/tp hit first.
//
// Self-contained: no winsock / globals / cmake. Build + run on the Mac directly:
//   clang++ -std=c++20 -O2 -o batch_bt batch_bt.cpp
//   ./batch_bt BEAR=/Users/jo/Tick/xau_2022bear_tick.csv BULL=/Users/jo/Tick/xau_6mo_corrected.csv
//
// Tick CSV format: `timestamp,bid,ask` (ms epoch). Header auto-skipped.
//
// The cell array below is a small Keltner/Donchian × fixed-R grid mirroring the
// 2026-06-20 bar-replay candidate (M30 Keltner k1.5, stop 2*ATR, TP 6*ATR). To add
// a cell, push one more CellCfg — the harness loops it in the same single pass.
// (Compile-time CRTP via Traits<I>+tuple+fold, as in OmegaSweepHarnessCRTP.cpp, is
//  the zero-overhead variant; a runtime CellCfg array is used here for clarity and
//  so the grid changes without a recompile.)
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>

static const double COMM_PER_SIDE = 0.50; // $/oz; ~1.5bps + buffer. Spread paid via bid/ask fills.
static const int    BAR_SEC       = 1800; // M30
static const int    ATR_N         = 14;
static const int    EMA_N         = 20;
static const int    MAXHOLD_BARS  = 96;   // ~2 days safety exit-at-market

struct Bar { int64_t ts; double o,h,l,c; };

enum Sig { KELTNER=0, DONCHIAN=1 };
struct CellCfg {
    const char* name;
    Sig sig; double k; int donN;     // signal params
    double stop_atr, tp_atr;         // fixed-R: stop & tp as ATR multiples
    bool allow_long, allow_short;
};

struct Acc {                          // per regime, split into two time-halves
    int n[2]={0,0}; double net[2]={0,0}, gross_pos[2]={0,0}, gross_neg[2]={0,0}; int win[2]={0,0};
    double sum_all=0; std::vector<double> wins_for_fattail;
};
struct CellState {
    int pos=0; double entry=0, stop=0, tp=0; int bars_held=0;
    int pending=0; double pend_atr=0;          // signal at bar close -> fill next tick
};

struct Cell {
    CellCfg cfg; CellState st; Acc acc;
    void reset_pos(){ st.pos=0; st.bars_held=0; }
};

static inline void book(Acc& a, int half, double pnl){
    a.n[half]++; a.net[half]+=pnl; a.sum_all+=pnl;
    if(pnl>=0){ a.gross_pos[half]+=pnl; a.win[half]++; }
    else a.gross_neg[half]+= -pnl;
    if(pnl>0) a.wins_for_fattail.push_back(pnl);
}

// ---- indicators over the rolling bar series ----
struct Indi {
    std::deque<Bar> bars; double ema=0; bool ema_seeded=false; double atr=0; bool atr_seeded=false;
    void push(const Bar& b){
        // ATR (Wilder)
        if(!bars.empty()){
            double pc = bars.back().c;
            double tr = std::max(b.h-b.l, std::max(std::fabs(b.h-pc), std::fabs(b.l-pc)));
            if(!atr_seeded){ atr=tr; atr_seeded=true; }
            else atr = (atr*(ATR_N-1)+tr)/ATR_N;
        }
        // EMA20 of close
        double a = 2.0/(EMA_N+1);
        if(!ema_seeded){ ema=b.c; ema_seeded=true; } else ema = a*b.c + (1-a)*ema;
        bars.push_back(b);
        if((int)bars.size()>120) bars.pop_front();
    }
    double donch_hi(int N) const { double m=-1e18; int c=0; for(auto it=bars.rbegin(); it!=bars.rend()&&c<N; ++it,++c) m=std::max(m,it->h); return m; }
    double donch_lo(int N) const { double m=1e18;  int c=0; for(auto it=bars.rbegin(); it!=bars.rend()&&c<N; ++it,++c) m=std::min(m,it->l); return m; }
    bool ready() const { return atr_seeded && ema_seeded && (int)bars.size()>=std::max(ATR_N,EMA_N)+2; }
};

int main(int argc, char** argv){
    // --- cell array (the grid) ---
    std::vector<Cell> cells;
    auto add=[&](const char* nm, Sig s, double k, int N, double stp, double tp, bool L, bool S){
        Cell c; c.cfg={nm,s,k,N,stp,tp,L,S}; cells.push_back(c);
    };
    add("Kelt_k1.5_R3 (candidate)", KELTNER, 1.5, 0, 2.0, 6.0, true, true);
    add("Kelt_k1.5_R2",            KELTNER, 1.5, 0, 2.0, 4.0, true, true);
    add("Kelt_k1.5_RR1.5",         KELTNER, 1.5, 0, 2.0, 3.0, true, true);
    add("Kelt_k2.0_R3",            KELTNER, 2.0, 0, 2.0, 6.0, true, true);
    add("Donch20_R3",              DONCHIAN,0.0, 20,2.0, 6.0, true, true);
    add("Kelt_k1.5_R3_LONGonly",   KELTNER, 1.5, 0, 2.0, 6.0, true, false);

    // --- parse args: TAG=path ---
    std::vector<std::pair<std::string,std::string>> regimes; // (tag,path)
    for(int i=1;i<argc;i++){ std::string a=argv[i]; auto eq=a.find('='); if(eq!=std::string::npos) regimes.push_back({a.substr(0,eq),a.substr(eq+1)}); }
    if(regimes.empty()){ fprintf(stderr,"usage: batch_bt BEAR=path BULL=path\n"); return 2; }

    for(size_t ri=0; ri<regimes.size(); ++ri){
        const std::string& tag=regimes[ri].first; const std::string& path=regimes[ri].second;
        FILE* f=fopen(path.c_str(),"rb"); if(!f){ fprintf(stderr,"cannot open %s\n",path.c_str()); return 1; }
        // first pass file-span for half split: we stream once and split by tick index parity of time.
        // Simpler: collect first/last ts via a light prescan of head/tail is overkill; use a running
        // approach — we don't know median upfront, so split by ELAPSED time vs file midpoint estimated
        // from first ts + a fixed window is unreliable. Instead: tag each trade with its entry ts and
        // split halves in a post-pass. Store (cell_idx, entry_ts, pnl).
        struct TR{ int cell; int64_t ts; double pnl; }; std::vector<TR> trades;

        for(auto& c:cells){ c.st=CellState{}; }
        Indi ind;
        // current M30 bucket
        bool have_bar=false; int64_t bucket=0; Bar cur{0,0,0,0,0};
        char line[256]; bool first=true;
        int64_t fts=0, lts=0;

        while(fgets(line,sizeof(line),f)){
            // parse ts,bid,ask
            char* p=line; if(first){ first=false; if(line[0]<'0'||line[0]>'9') continue; }
            int64_t ts=0; while(*p>='0'&&*p<='9'){ ts=ts*10+(*p-'0'); p++; } if(*p!=',') continue; p++;
            double bid=strtod(p,&p); if(*p!=',') continue; p++; double ask=strtod(p,&p);
            if(ts<=0||bid<=0||ask<=0) continue;
            if(fts==0) fts=ts; lts=ts;
            double mid=(bid+ask)/2.0;
            int64_t b = ts/1000/BAR_SEC;             // M30 bucket index
            if(!have_bar){ bucket=b; cur={ts,mid,mid,mid,mid}; have_bar=true; }
            else if(b!=bucket){
                // ---- close bar `cur`, dispatch to cells ----
                // Evaluate breakout of cur.c against indicators through the PRIOR
                // bars only (ind not yet updated with cur) — no same-bar look-ahead.
                if(ind.ready()){
                    double e=ind.ema, atr=ind.atr;
                    for(auto& c:cells){
                        if(c.st.pos!=0) continue; // only arm when flat
                        double up,dn;
                        if(c.cfg.sig==KELTNER){ up=e+c.cfg.k*atr; dn=e-c.cfg.k*atr; }
                        else { up=ind.donch_hi(c.cfg.donN); dn=ind.donch_lo(c.cfg.donN); }
                        if(c.cfg.allow_long  && cur.c>up){ c.st.pending=+1; c.st.pend_atr=atr; }
                        else if(c.cfg.allow_short && cur.c<dn){ c.st.pending=-1; c.st.pend_atr=atr; }
                    }
                }
                ind.push(cur);   // now incorporate the just-closed bar
                // bars_held++ for open positions
                for(auto& c:cells) if(c.st.pos!=0) c.st.bars_held++;
                bucket=b; cur={ts,mid,mid,mid,mid};
            } else { cur.h=std::max(cur.h,mid); cur.l=std::min(cur.l,mid); cur.c=mid; }

            // ---- per-tick fills (faithful intrabar) ----
            for(size_t ci=0; ci<cells.size(); ++ci){ Cell& c=cells[ci];
                // fill pending entry at THIS tick (next tick after the signal bar close)
                if(c.st.pos==0 && c.st.pending!=0){
                    double ea = c.st.pend_atr;
                    if(c.st.pending==+1){ c.st.entry=ask; c.st.stop=ask-c.cfg.stop_atr*ea; c.st.tp=ask+c.cfg.tp_atr*ea; c.st.pos=+1; }
                    else                { c.st.entry=bid; c.st.stop=bid+c.cfg.stop_atr*ea; c.st.tp=bid-c.cfg.tp_atr*ea; c.st.pos=-1; }
                    c.st.bars_held=0; c.st.pending=0; continue;
                }
                if(c.st.pos==0) continue;
                double exit_px=0; bool out=false;
                if(c.st.pos==+1){
                    if(bid<=c.st.stop){ exit_px=bid; out=true; }            // stop first (conservative)
                    else if(bid>=c.st.tp){ exit_px=c.st.tp; out=true; }     // limit fills at tp
                    else if(c.st.bars_held>=MAXHOLD_BARS){ exit_px=bid; out=true; }
                } else {
                    if(ask>=c.st.stop){ exit_px=ask; out=true; }
                    else if(ask<=c.st.tp){ exit_px=c.st.tp; out=true; }
                    else if(c.st.bars_held>=MAXHOLD_BARS){ exit_px=ask; out=true; }
                }
                if(out){
                    double gross = (c.st.pos==+1)?(exit_px-c.st.entry):(c.st.entry-exit_px);
                    double pnl = gross - 2*COMM_PER_SIDE;
                    trades.push_back({(int)ci, ts, pnl});
                    c.st.pos=0; c.st.bars_held=0;
                }
            }
        }
        fclose(f);

        // ---- aggregate: split trades into two halves by entry ts ----
        int64_t mid_ts = (fts+lts)/2;
        for(auto& c:cells) c.acc=Acc{};
        for(auto& t:trades){ int half=(t.ts<mid_ts)?0:1; book(cells[t.cell].acc, half, t.pnl); }

        // ---- print ----
        printf("\n========== REGIME: %s  (%s) ==========\n", tag.c_str(), path.c_str());
        printf("%-26s %6s %8s %5s | %6s %8s %5s | %6s %8s\n",
               "cell","PF_h1","net_h1","n_h1","PF_h2","net_h2","n_h2","PF_all","net_all");
        for(auto& c:cells){ Acc&a=c.acc;
            auto pf=[&](int h){ return a.gross_neg[h]>0? a.gross_pos[h]/a.gross_neg[h] : (a.gross_pos[h]>0?99.9:0.0); };
            double gp=a.gross_pos[0]+a.gross_pos[1], gn=a.gross_neg[0]+a.gross_neg[1];
            double pfall = gn>0? gp/gn : (gp>0?99.9:0.0);
            double netall=a.net[0]+a.net[1]; int nall=a.n[0]+a.n[1];
            printf("%-26s %6.2f %8.0f %5d | %6.2f %8.0f %5d | %6.2f %8.0f (n=%d)\n",
                   c.cfg.name, pf(0),a.net[0],a.n[0], pf(1),a.net[1],a.n[1], pfall,netall,nall);
        }
    }
    printf("\nNOTE: faithful tick fills (bid/ask spread + intrabar stop-before-tp) + $%.2f/side comm.\n", COMM_PER_SIDE);
    printf("PF/net are per-regime; require + in BOTH regimes AND both halves to survive.\n");
    return 0;
}
