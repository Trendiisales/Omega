// batch_engine_runner.cpp — generalized engine-zoo BATCH backtest runner.
//
// The "expedite testing" runner over REAL engine CLASSES (not signal stubs): ONE
// streaming tick pass fans out to an ARRAY of (engine × config) CELLS, each a real
// omega:: engine instance with its own config + its own per-cell accumulator, and
// reports per-cell PF / net split by both time-halves. This is what OmegaBacktest
// lacks — it runs many engines once but can't SWEEP configs of one engine or split
// each cell cross-regime.
//
// Pattern: each cell is an ICell (virtual tick + a TradeRecord sink that books to
// THIS cell's stats, keyed by cell name so two configs of the same engine don't
// collide). Add an engine type = one ICell subclass (lift the on_tick wiring from
// backtest/OmegaBacktest.cpp's runners). Add a config = push one more cell.
//
// Builds on Mac/Linux like OmegaBacktest (no FIX/curl/winsock; OMEGA_BACKTEST +
// OmegaTimeShim force-include, links pthread). Build + run:
//   cmake --build build --target BatchEngineRunner
//   ./build/BatchEngineRunner BEAR=/Users/jo/Tick/xau_2022bear_tick.csv \
//                             BULL=/Users/jo/Tick/xau_6mo_corrected.csv [--warmup 20000]
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>

#include "../include/OmegaTradeLedger.hpp"
#include "../include/RSIReversalEngine.hpp"
#include "BtBarEngine.hpp"   // M1 bar indicators to feed the engine (set_bar_rsi/context)

struct TickRow { int64_t ts_ms; double bid, ask; };

// ---- a backtest CELL: one real engine instance + one config + per-cell trades ----
struct ICell {
    std::string name;
    std::vector<std::pair<int64_t,double>> trades;   // (entryTs_sec, pnl)
    int64_t warmup_sec = 0;
    virtual ~ICell() = default;
    virtual void tick(const TickRow& r) = 0;
    void book(const omega::TradeRecord& t){ if(t.entryTs >= warmup_sec) trades.push_back({t.entryTs, t.pnl}); }
};

// ---- RSIReversalEngine cell (config-swept) — wiring lifted from OmegaBacktest RSIRevRunner ----
struct RsiRevCell : ICell {
    omega::RSIReversalEngine eng;
    omega::bt::BtBarEngine<1> m1; // M1 bars -> bar RSI/ATR the engine's entry needs
    double prev_atr_ = 0.0;
    RsiRevCell(const std::string& nm, double oversold, double overbought,
               double sl_atr, double trail_atr, int max_hold_s){
        name = nm;
        eng.enabled        = true;
        eng.shadow_mode    = true;
        eng.RSI_OVERSOLD   = oversold;     // (OmegaBacktest flips these for continuation)
        eng.RSI_OVERBOUGHT = overbought;
        eng.RSI_EXIT_LONG  = 50.0;
        eng.RSI_EXIT_SHORT = 50.0;
        eng.SL_ATR_MULT    = sl_atr;
        eng.TRAIL_ATR_MULT = trail_atr;
        eng.BE_ATR_MULT    = 0.25;
        eng.COOLDOWN_S     = 15;
        eng.COOLDOWN_S_VACUUM = 10;
        eng.MAX_HOLD_S     = max_hold_s;
        eng.MIN_HOLD_S     = 3;
        // gates relaxed to the gold tick scale so the demo actually trades
        // (defaults MIN_ATR_PTS=1.0 / MIN_BAR_ATR=2.0 are tuned for the live feed).
        eng.MIN_ATR_PTS    = 0.3;
        eng.MIN_BAR_ATR    = 0.5;
        eng.MAX_SPREAD_PTS = 5.0;
    }
    void tick(const TickRow& r) override {
        const double mid = (r.bid + r.ask) * 0.5;
        if (m1.on_tick(mid, r.ts_ms) && m1.indicators_ready()) {   // on M1 bar close
            const double atr = m1.atr14();
            eng.set_bar_rsi(m1.rsi14(), mid);
            eng.set_bar_context(atr, atr > prev_atr_, false, m1.adx14() > 20.0);
            prev_atr_ = atr;
        }
        eng.update_indicators(r.bid, r.ask);
        int h = (int)((r.ts_ms/1000/3600)%24);
        int slot = 0;
        if(h>=7&&h<9)slot=1; else if(h>=9&&h<12)slot=2;
        else if(h>=12&&h<14)slot=3; else if(h>=14&&h<17)slot=4;
        else if(h>=17&&h<22)slot=5; else if(h>=22||h<5)slot=6;
        if (eng.has_open_position()) slot = 0;
        eng.on_tick(r.bid, r.ask, slot, r.ts_ms,
                    0.5, false, false, false, false, false,
                    [this](const omega::TradeRecord& t){ this->book(t); });
    }
};

// ---- streaming tick loader: `timestamp,bid,ask` (ms), header auto-skipped ----
static bool load_ticks(const char* path, std::vector<TickRow>& out){
    FILE* f = fopen(path, "rb"); if(!f) return false;
    char line[256]; bool first = true;
    while(fgets(line, sizeof(line), f)){
        char* p = line;
        if(first){ first = false; if(line[0] < '0' || line[0] > '9') continue; }
        int64_t ts = 0; while(*p>='0'&&*p<='9'){ ts=ts*10+(*p-'0'); p++; }
        if(*p!=',') continue; p++;
        double bid = strtod(p,&p); if(*p!=',') continue; p++;
        double ask = strtod(p,&p);
        if(ts>0 && bid>0 && ask>0) out.push_back({ts,bid,ask});
    }
    fclose(f); return true;
}

static void make_cells(std::vector<std::unique_ptr<ICell>>& cells){
    // config-sweep grid of ONE real engine — the capability OmegaBacktest lacks.
    // NOTE: RSIReversalEngine is an ILLUSTRATIVE demo here — its flipped-continuation
    // RSI-turn-at-extreme entry is narrow and fires rarely on raw gold (it is tuned
    // for its live feed). The harness (real-engine array + one pass + bar feed +
    // per-cell both-halves) is the deliverable; point it at an engine/data that fit.
    cells.emplace_back(new RsiRevCell("RsiRev sl0.5 tr0.30 hold90", 70,30, 0.5, 0.30, 90));
    cells.emplace_back(new RsiRevCell("RsiRev sl1.0 tr0.30 hold90", 70,30, 1.0, 0.30, 90));
    cells.emplace_back(new RsiRevCell("RsiRev sl0.5 tr0.50 hold180",70,30, 0.5, 0.50, 180));
    cells.emplace_back(new RsiRevCell("RsiRev sl0.8 tr0.40 hold120",70,30, 0.8, 0.40, 120));
    // To add another ENGINE TYPE: define `struct FooCell : ICell` wrapping omega::FooEngine
    // (lift its on_tick wiring from OmegaBacktest.cpp) and emplace_back it here.
}

int main(int argc, char** argv){
    std::vector<std::pair<std::string,std::string>> regimes;
    int warmup = 20000;
    for(int i=1;i<argc;i++){
        std::string a = argv[i];
        if(a=="--warmup" && i+1<argc){ warmup = atoi(argv[++i]); continue; }
        auto eq=a.find('='); if(eq!=std::string::npos) regimes.push_back({a.substr(0,eq), a.substr(eq+1)});
    }
    if(regimes.empty()){ fprintf(stderr,"usage: BatchEngineRunner BEAR=path BULL=path [--warmup N]\n"); return 2; }

    for(auto& [tag,path] : regimes){
        std::vector<TickRow> ticks;
        if(!load_ticks(path.c_str(), ticks) || ticks.empty()){ fprintf(stderr,"[ERR] no ticks: %s\n",path.c_str()); return 1; }
        int64_t warm_sec = ticks[std::min((size_t)warmup, ticks.size()-1)].ts_ms/1000;

        std::vector<std::unique_ptr<ICell>> cells;
        make_cells(cells);
        for(auto& c:cells) c->warmup_sec = warm_sec;

        for(const auto& r : ticks) for(auto& c:cells) c->tick(r);

        // results -> STDERR so the engines' stdout diagnostics can be discarded (1>/dev/null)
        fprintf(stderr, "\n========== REGIME: %s  (%s)  ticks=%zu warmup=%d ==========\n",
               tag.c_str(), path.c_str(), ticks.size(), warmup);
        fprintf(stderr, "%-30s %6s %8s %5s | %6s %8s %5s | %6s %9s\n",
               "cell","PF_h1","net_h1","n_h1","PF_h2","net_h2","n_h2","PF_all","net_all");
        for(auto& c:cells){
            auto& T = c->trades;
            if(T.empty()){ fprintf(stderr, "%-30s %6s %8s %5s | %6s %8s %5s | %6.2f %9.0f (n=0)\n",
                                   c->name.c_str(),"-","-","0","-","-","0",0.0,0.0); continue; }
            int64_t lo=T.front().first, hi=T.front().first;
            for(auto& t:T){ lo=std::min(lo,t.first); hi=std::max(hi,t.first); }
            int64_t mid=(lo+hi)/2;
            double gp[2]={0,0}, gn[2]={0,0}, net[2]={0,0}; int n[2]={0,0};
            for(auto& t:T){ int hf=(t.first<mid)?0:1; n[hf]++; net[hf]+=t.second; if(t.second>=0)gp[hf]+=t.second; else gn[hf]+=-t.second; }
            auto pf=[&](int h){ return gn[h]>0? gp[h]/gn[h] : (gp[h]>0?99.9:0.0); };
            double GP=gp[0]+gp[1], GN=gn[0]+gn[1];
            double pfall = GN>0? GP/GN : (GP>0?99.9:0.0);
            fprintf(stderr, "%-30s %6.2f %8.0f %5d | %6.2f %8.0f %5d | %6.2f %9.0f (n=%d)\n",
                   c->name.c_str(), pf(0),net[0],n[0], pf(1),net[1],n[1], pfall, net[0]+net[1], n[0]+n[1]);
        }
    }
    fprintf(stderr, "\nNOTE: array of REAL engine CLASSES (config-swept) in ONE tick pass; per-cell both-halves.\n");
    fprintf(stderr, "Add an engine type = one ICell subclass; add a config = one make_cells() line.\n");
    return 0;
}
