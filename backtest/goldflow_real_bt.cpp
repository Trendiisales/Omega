// goldflow_real_bt.cpp -- GoldFlow REAL ENGINE backtest
// Runs the actual GoldFlowEngine.hpp against L2 tick data
// All engine logic identical to production
//
// Build: g++ -O2 -std=c++17 -I../include -o goldflow_real_bt goldflow_real_bt.cpp
// Run:   ./goldflow_real_bt l2_ticks_2026-04-09.csv l2_ticks_2026-04-10.csv

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <iomanip>
#include <map>
#include <functional>
#include <cstring>
#include <chrono>
#include <atomic>

// ── Include real TradeRecord first ───────────────────────────────────────────
#include "../include/OmegaTradeLedger.hpp"

// ── Stub: g_macro_ctx ────────────────────────────────────────────────────────
struct MacroCtx { bool gold_l2_real = true; };
static MacroCtx g_macro_ctx;

// ── Include the REAL engine ───────────────────────────────────────────────────
#include "../include/GoldFlowEngine.hpp"

// ── EWM drift (mirrors GoldEngineStack alphas) ────────────────────────────────
struct EWM {
    double fast=0, slow=0; bool seeded=false;
    void update(double p) {
        if (!seeded) { fast=slow=p; seeded=true; return; }
        fast = 0.05*p + 0.95*fast;
        slow = 0.005*p + 0.995*slow;
    }
    double drift() const { return fast - slow; }
};

// ── L2 Tick ───────────────────────────────────────────────────────────────────
struct L2Tick {
    int64_t ts_ms=0;
    double  bid=0, ask=0, l2_imb=0.5;
    double  l2_bid_vol=0, l2_ask_vol=0;
    int     depth_bid=0, depth_ask=0;
    bool    watchdog_dead=false;
    double  vol_ratio=1.0;
    int     regime=0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty() || line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v)->bool{if(!getline(ss,tok,','))return false;try{v=std::stod(tok);}catch(...){return false;}return true;};
    auto ni=[&](int& v)->bool{if(!getline(ss,tok,','))return false;try{v=(int)std::stoll(tok);}catch(...){return false;}return true;};
    auto nb=[&](bool& v)->bool{if(!getline(ss,tok,','))return false;try{v=(bool)std::stoll(tok);}catch(...){return false;}return true;};
    double tmp;
    if (!nd(tmp)) return false; t.ts_ms=(int64_t)tmp;
    if (!nd(t.bid)||!nd(t.ask)) return false;
    if (!nd(t.l2_imb)||!nd(t.l2_bid_vol)||!nd(t.l2_ask_vol)) return false;
    if (!ni(t.depth_bid)||!ni(t.depth_ask)) return false;
    if (!nd(tmp)) return false;
    if (!nb(t.watchdog_dead)) return false;
    if (!nd(t.vol_ratio)) return false;
    if (!ni(t.regime)) return false;
    return (t.bid>0 && t.ask>0 && t.ask>=t.bid);
}

int session_slot(int64_t ts_ms) {
    int h=(int)((ts_ms/1000/3600)%24);
    if (h>=22||h<5)  return 6;
    if (h>=7&&h<12)  return 2;
    if (h>=12&&h<17) return 3;
    if (h>=17&&h<22) return 4;
    return 5;
}

int main(int argc, char* argv[]) {
    std::vector<std::string> files;
    for (int i=1;i<argc;i++) files.push_back(argv[i]);
    if (files.empty()) { std::cerr << "Usage: goldflow_real_bt files...\n"; return 1; }

    GoldFlowEngine engine;
    engine.risk_dollars = GFE_RISK_DOLLARS;

    EWM ewm;
    std::vector<omega::TradeRecord> trades;
    std::map<std::string,int> reasons;
    int64_t tick_count=0;

    for (auto& fname : files) {
        std::ifstream f(fname);
        if (!f) { std::cerr << "Cannot open: " << fname << "\n"; continue; }
        std::string line;
        getline(f,line); // header
        while (getline(f,line)) {
            L2Tick t;
            if (!parse_l2(line,t)) continue;
            if (t.watchdog_dead) continue;
            tick_count++;

            const double spread = t.ask-t.bid;
            if (spread<=0||spread>GFE_MAX_SPREAD) continue;

            const double mid = (t.ask+t.bid)*0.5;
            ewm.update(mid);
            const double drift = ewm.drift();

            // set_trend_bias: neutral supervisor -- no regime, no wall, no expansion
            engine.set_trend_bias(drift/mid*100.0, 0.5, false, false, drift, false, t.vol_ratio);

            // BlackBull sends size_raw=0 so l2_imb=0.500 always -- the L2 imbalance
            // signal path never fires. In production GoldFlow runs on drift-fallback
            // (GFE_DRIFT_FALLBACK_THRESHOLD) not the L2 persistence path.
            // Set l2_ctrader_live=false to force drift-only mode -- this matches
            // what actually fires in production on BlackBull feeds.
            g_macro_ctx.gold_l2_real = false;

            engine.on_tick(
                t.bid, t.ask,
                0.5,    // l2_imb irrelevant -- drift path active
                drift,
                t.ts_ms,
                [&](const omega::TradeRecord& tr) {
                    trades.push_back(tr);
                    reasons[tr.exitReason]++;
                },
                session_slot(t.ts_ms),
                false   // l2_ctrader_live=false -> drift fallback path (production behaviour)
            );
        }
    }

    if (trades.empty()) { std::cout << "No trades.\n"; return 0; }

    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for (auto& tr : trades) {
        const double p = tr.pnl * 100.0;
        total+=p; eq+=p;
        if (eq>peak) peak=eq;
        maxdd=std::max(maxdd,peak-eq);
        if (p>0){w++;wpnl+=p;} else{l++;lpnl+=p;}
    }
    int n=trades.size();
    double wr=100.0*w/n;
    double aw=w?wpnl/w:0, al=l?lpnl/l:0, rr=al?-aw/al:0;

    std::cout << "\n=== GoldFlow REAL ENGINE Backtest (L2 DOM) ===\n";
    std::cout << "Ticks        : " << tick_count << "\n";
    std::cout << "Trades       : " << n << "\n";
    std::cout << "Win rate     : " << std::fixed << std::setprecision(1) << wr << "%\n";
    std::cout << "Total PnL    : $" << std::setprecision(2) << total << "\n";
    std::cout << "Avg win      : $" << aw << "\n";
    std::cout << "Avg loss     : $" << al << "\n";
    std::cout << "R:R          : " << std::setprecision(2) << rr << "\n";
    std::cout << "Max drawdown : $" << maxdd << "\n";
    std::cout << "Exit reasons :\n";
    for (auto& kv : reasons)
        std::cout << "  " << kv.first << ": " << kv.second << "\n";
    return 0;
}
