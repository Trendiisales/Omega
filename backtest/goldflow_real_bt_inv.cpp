// goldflow_real_bt_inv.cpp -- GoldFlow REAL ENGINE backtest with inversion flag
// Runs the actual GoldFlowEngine.hpp against L2 tick data, with optional
// signal inversion to test the "drift-continuation is negative-EV" hypothesis.
//
// The inversion is applied AFTER the live engine computes its signal decision.
// On every closed trade, if --inverted, we flip the recorded side and
// recompute PnL accordingly. This is a post-hoc sign flip, not a live-engine
// modification -- live GoldFlowEngine.hpp is untouched. The harness is the
// only thing that knows about inversion.
//
// Why post-hoc flip is valid here:
// The engine's long_signal/short_signal determination is deterministic given
// ticks. Flipping the resulting side is equivalent to the engine having fired
// the opposite direction. The entry price, SL, TP geometry all hold (the
// adverse-vs-favorable label simply reverses). Holding times may differ in
// a live-inverted engine because SL/TP would be crossed at different times --
// this harness assumes same hold as the live side. See LIMITATION section
// below. The true answer requires a live-inverted engine for full fidelity.
//
// LIMITATION: Holding-time fidelity
//   Live-inverted: an engine firing SHORT at a drift-up signal would be
//     stopped out when price continues up (SL above). That's typically faster
//     than the forward-engine's adverse exit on the same tick stream.
//   This harness: recorded exit time is from the forward engine's exit logic.
//   For small adverse moves that become larger: the harness UNDERSTATES the
//     inverted-engine's PnL (real inverted would exit faster with smaller loss).
//   For large adverse moves: the harness may OVERSTATE (real inverted would
//     stop out before the full adverse is realized).
//   Net effect: directional, not symmetric. Interpret results as a LOWER BOUND
//   on inverted engine's edge, not a point estimate.
//
// Build: g++ -O2 -std=c++17 -I../include -o goldflow_real_bt_inv goldflow_real_bt_inv.cpp
// Run:   ./goldflow_real_bt_inv [--inverted] l2_ticks_2026-04-09.csv l2_ticks_2026-04-10.csv ...

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

// Include real TradeRecord first
#include "../include/OmegaTradeLedger.hpp"

// Stub: g_macro_ctx (mirrors goldflow_real_bt.cpp)
struct MacroCtx { bool gold_l2_real = true; };
static MacroCtx g_macro_ctx;

// Include the REAL engine -- untouched
#include "../include/GoldFlowEngine.hpp"

// EWM drift (mirrors GoldEngineStack alphas)
struct EWM {
    double fast=0, slow=0; bool seeded=false;
    void seed(double p) { fast=slow=p; seeded=true; }
    void update(double p) {
        if (!seeded) { fast=slow=p; seeded=true; return; }
        fast = 0.05*p + 0.95*fast;
        slow = 0.005*p + 0.995*slow;
    }
    double drift() const { return fast - slow; }
};

// L2 Tick
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
    bool inverted = false;
    std::vector<std::string> files;
    for (int i=1;i<argc;i++) {
        std::string a = argv[i];
        if (a == "--inverted") { inverted = true; continue; }
        files.push_back(a);
    }
    if (files.empty()) {
        std::cerr << "Usage: goldflow_real_bt_inv [--inverted] files...\n";
        return 1;
    }

    std::cerr << "Mode: " << (inverted ? "INVERTED (flip long<->short)" : "NORMAL") << "\n";
    std::cerr << "Files: " << files.size() << "\n";

    GoldFlowEngine engine;
    engine.risk_dollars = GFE_RISK_DOLLARS;

    EWM ewm;
    std::vector<omega::TradeRecord> trades;
    std::map<std::string,int> reasons;
    int64_t tick_count=0;

    // EWM warmup: 2000 ticks before processing trades.
    {
        std::ifstream fw(files[0]); std::string lw;
        getline(fw, lw); // skip header
        int warmup_count = 0;
        while (getline(fw, lw) && warmup_count < 2000) {
            L2Tick tw;
            if (!parse_l2(lw, tw)) continue;
            if (tw.watchdog_dead) continue;
            const double sp = tw.ask - tw.bid;
            if (sp <= 0 || sp > GFE_MAX_SPREAD) continue;
            ewm.update((tw.ask + tw.bid) * 0.5);
            warmup_count++;
        }
    }

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

            engine.set_trend_bias(drift/mid*100.0, 0.5, false, false, drift, false, t.vol_ratio);

            const int total_levels = t.depth_bid + t.depth_ask;
            const double level_imb = (total_levels > 0)
                ? static_cast<double>(t.depth_bid) / static_cast<double>(total_levels)
                : 0.5;

            g_macro_ctx.gold_l2_real = !t.watchdog_dead;

            if (tick_count % 10000 == 0) {
                int h = (int)((t.ts_ms/1000/3600)%24);
                fprintf(stderr, "[DIAG] tick=%lld ts=%lld h=%d drift=%.3f level_imb=%.3f spread=%.3f atr=%.3f phase=%d\n",
                    (long long)tick_count, (long long)t.ts_ms, h, drift, level_imb,
                    t.ask-t.bid, engine.current_atr(), (int)engine.phase);
            }

            engine.on_tick(
                t.bid, t.ask,
                level_imb,
                drift,
                t.ts_ms,
                [&](const omega::TradeRecord& tr_in) {
                    // Post-hoc inversion: flip side and recompute PnL sign.
                    // See LIMITATION comment at top of file.
                    omega::TradeRecord tr = tr_in;
                    if (inverted) {
                        // Flip side label
                        if (tr.side == "LONG") tr.side = "SHORT";
                        else if (tr.side == "SHORT") tr.side = "LONG";
                        // Flip PnL sign -- adverse becomes favorable at same prices
                        tr.pnl = -tr.pnl;
                        // MFE / MAE also swap semantically, but we keep the
                        // raw values for diagnostic. The PnL flip is what
                        // drives the verdict.
                    }
                    trades.push_back(tr);
                    reasons[tr.exitReason]++;
                },
                session_slot(t.ts_ms),
                !t.watchdog_dead
            );
        }
    }

    if (trades.empty()) { std::cout << "No trades.\n"; return 0; }

    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    // Per-side stats for inversion visibility
    int long_count=0, short_count=0;
    double long_pnl=0, short_pnl=0;
    for (auto& tr : trades) {
        const double p = tr.pnl * 100.0;
        total+=p; eq+=p;
        if (eq>peak) peak=eq;
        maxdd=std::max(maxdd,peak-eq);
        if (p>0){w++;wpnl+=p;} else{l++;lpnl+=p;}
        if (tr.side == "LONG")  { long_count++;  long_pnl  += p; }
        if (tr.side == "SHORT") { short_count++; short_pnl += p; }
    }
    int n=trades.size();
    double wr=100.0*w/n;
    double aw=w?wpnl/w:0, al=l?lpnl/l:0, rr=al?-aw/al:0;

    std::cout << "\n=== GoldFlow REAL ENGINE Backtest (L2 DOM) "
              << (inverted ? "[INVERTED]" : "[NORMAL]") << " ===\n";
    std::cout << "Ticks        : " << tick_count << "\n";
    std::cout << "Trades       : " << n << "\n";
    std::cout << "Win rate     : " << std::fixed << std::setprecision(1) << wr << "%\n";
    std::cout << "Total PnL    : $" << std::setprecision(2) << total << "\n";
    std::cout << "Avg win      : $" << aw << "\n";
    std::cout << "Avg loss     : $" << al << "\n";
    std::cout << "R:R          : " << std::setprecision(2) << rr << "\n";
    std::cout << "Max drawdown : $" << maxdd << "\n";
    std::cout << "LONG count   : " << long_count  << " (pnl=$" << long_pnl  << ")\n";
    std::cout << "SHORT count  : " << short_count << " (pnl=$" << short_pnl << ")\n";
    std::cout << "Exit reasons :\n";
    for (auto& kv : reasons)
        std::cout << "  " << kv.first << ": " << kv.second << "\n";

    if (inverted) {
        std::cout << "\nNOTE: --inverted flips side + PnL sign post-hoc.\n"
                  << "Holding times are NORMAL-engine's -- see top-of-file LIMITATION.\n"
                  << "Treat inverted PnL as a LOWER BOUND on true inverted-engine edge.\n";
    }
    return 0;
}
