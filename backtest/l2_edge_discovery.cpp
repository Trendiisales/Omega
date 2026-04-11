// l2_edge_discovery.cpp
// Comprehensive L2 edge discovery against real cTrader depth data
//
// Tests 6 strategy families across parameter sweeps:
//   1. DRIFT_ONLY      -- EWM drift persistence, no DOM
//   2. DRIFT_DOM       -- EWM drift + DOM imbalance confirmation
//   3. MICROEDGE       -- GoldMicrostructureAnalyzer composite score
//   4. DOM_PERSISTENCE -- Raw imbalance persistence (N consecutive ticks)
//   5. PULL_FADE       -- Liquidity pull + price fade entry
//   6. HYBRID          -- Drift persistence + micro_edge threshold
//
// CSV format (l2_ticks_YYYY-MM-DD.csv):
//   ts_ms, bid, ask, l2_imb, bid_vol, ask_vol, depth_bid, depth_ask,
//   depth_events_total, watchdog_dead, vol_ratio, micro_edge
//
// Build: g++ -O3 -std=c++17 -o l2_edge_discovery l2_edge_discovery.cpp
// Run:   ./l2_edge_discovery l2_ticks_2026-04-09.csv l2_ticks_2026-04-10.csv

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
#include <numeric>

// =============================================================================
// L2 Tick
// =============================================================================
struct L2Tick {
    int64_t ts_ms=0;
    double  bid=0, ask=0, l2_imb=0.5;
    double  bid_vol=0, ask_vol=0;
    int     depth_bid=0, depth_ask=0;
    int64_t depth_events=0;
    bool    watchdog_dead=false;
    double  vol_ratio=1.0;
    double  micro_edge=0.5;  // GoldMicrostructureAnalyzer output (may not be present)
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty() || line[0]=='t' || line[0]=='#') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v)->bool{
        if (!getline(ss,tok,',')) return false;
        try { v=std::stod(tok); } catch(...) { return false; }
        return true;
    };
    auto ni64=[&](int64_t& v)->bool{
        if (!getline(ss,tok,',')) return false;
        try { v=std::stoll(tok); } catch(...) { return false; }
        return true;
    };
    auto ni=[&](int& v)->bool{
        if (!getline(ss,tok,',')) return false;
        try { v=(int)std::stoll(tok); } catch(...) { return false; }
        return true;
    };
    double tmp;
    if (!nd(tmp)) return false; t.ts_ms=(int64_t)tmp;
    if (!nd(t.bid)||!nd(t.ask)) return false;
    if (!nd(t.l2_imb)||!nd(t.bid_vol)||!nd(t.ask_vol)) return false;
    if (!ni(t.depth_bid)||!ni(t.depth_ask)) return false;
    if (!ni64(t.depth_events)) return false;
    int wd=0; if (!ni(wd)) return false; t.watchdog_dead=(wd!=0);
    if (!nd(t.vol_ratio)) return false;
    // micro_edge optional (newer CSV format)
    if (getline(ss,tok,',')) { try { t.micro_edge=std::stod(tok); } catch(...) { t.micro_edge=0.5; } }
    else t.micro_edge=0.5;
    return (t.bid>0 && t.ask>0 && t.ask>=t.bid);
}

// =============================================================================
// Indicators
// =============================================================================
struct EWM {
    double fast=0, slow=0; bool seeded=false;
    void update(double p, double af=0.05, double as_=0.005) {
        if (!seeded) { fast=slow=p; seeded=true; return; }
        fast=af*p+(1-af)*fast; slow=as_*p+(1-as_)*slow;
    }
    double drift() const { return fast-slow; }
};

struct ATR14 {
    std::deque<double> r; double val=0;
    void add(double range) {
        r.push_back(range); if ((int)r.size()>14) r.pop_front();
        double s=0; for(auto x:r) s+=x; val=r.empty()?0:s/r.size();
    }
};

struct ImbPersist {
    int long_n=0, short_n=0;
    void update(double imb, double thresh_lo, double thresh_hi) {
        if (imb >= thresh_hi) { long_n++;  short_n=0; }
        else if (imb <= thresh_lo) { short_n++; long_n=0; }
        else { long_n=0; short_n=0; }
    }
    void reset() { long_n=short_n=0; }
};

// =============================================================================
// Trade + Result
// =============================================================================
struct Trade {
    bool is_long; double entry, exit_px, pnl; int held_s; std::string reason;
};

struct Result {
    std::string name;
    int n_trades=0, wins=0, losses=0;
    double total_pnl=0, max_dd=0, avg_win=0, avg_loss=0;
    double wr=0, rr=0, expectancy=0, sharpe=0;
    std::vector<double> equity;
    void compute() {
        if (n_trades==0) return;
        wr=100.0*wins/n_trades;
        double wp=0,lp=0;
        for (auto x:equity) { if(x>0)wp+=x; else lp+=x; }
        avg_win  = wins   ? wp/wins   : 0;
        avg_loss = losses ? lp/losses : 0;
        rr = (avg_loss!=0) ? -avg_win/avg_loss : 0;
        expectancy = total_pnl/n_trades;
        // Sharpe: equity curve volatility
        if (equity.size()>1) {
            double mu=total_pnl/equity.size();
            double var=0; for(auto x:equity) var+=(x-mu)*(x-mu);
            double sd=std::sqrt(var/equity.size());
            sharpe = sd>0 ? (mu/sd)*std::sqrt(252.0) : 0;
        }
    }
    void print() const {
        printf("%-22s | T=%3d WR=%4.0f%% PnL=%7.2f MaxDD=%6.2f AvgW=%5.2f AvgL=%5.2f RR=%.2f Exp=%5.2f\n",
               name.c_str(), n_trades, wr, total_pnl, max_dd,
               avg_win, avg_loss, rr, expectancy);
    }
};

// =============================================================================
// Backtest engine - shared position management
// =============================================================================
struct Backtester {
    // Config
    double risk_usd    = 30.0;
    double tick_value  = 100.0;
    double max_spread  = 0.50;
    double sl_atr_mult = 1.0;
    double be_atr_mult = 1.0;
    double trail_mult  = 2.0;
    int    cooldown_s  = 30;

    // State
    bool   in_trade=false, is_long=false, be_hit=false;
    double entry=0, sl=0, size=0, mfe=0;
    int64_t entry_ts=0, last_exit_ts=0;
    ATR14  atr;
    double range_hi=0, range_lo=1e9;
    int64_t last_atr_update=0;

    std::vector<Trade> trades;

    void reset() {
        in_trade=be_hit=false; entry=sl=size=mfe=0;
        entry_ts=last_exit_ts=0;
        range_hi=0; range_lo=1e9; last_atr_update=0;
        trades.clear(); atr=ATR14{};
    }

    void update_atr(int64_t ts, double mid) {
        if (mid>range_hi) range_hi=mid;
        if (mid<range_lo) range_lo=mid;
        if (ts-last_atr_update>60000) {
            if (range_hi>range_lo) atr.add(range_hi-range_lo);
            range_hi=mid; range_lo=mid; last_atr_update=ts;
        }
    }

    // Returns true if position closed this tick
    bool manage(const L2Tick& t) {
        if (!in_trade) return false;
        const double mid = (t.bid+t.ask)*0.5;
        const double move = is_long ? (mid-entry) : (entry-mid);
        if (move>mfe) mfe=move;
        const double av = atr.val>0 ? atr.val : 2.0;

        // SL
        bool sl_hit = is_long ? (t.bid<=sl) : (t.ask>=sl);
        if (sl_hit) {
            double px = is_long ? t.bid : t.ask;
            double pnl = (is_long?(px-entry):(entry-px))*size*tick_value;
            trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"SL"});
            in_trade=false; last_exit_ts=t.ts_ms; return true;
        }
        // BE
        if (!be_hit && move>=av*be_atr_mult) { sl=entry; be_hit=true; }
        // Trail
        if (be_hit) {
            double nt = is_long ? mid-av*trail_mult : mid+av*trail_mult;
            if (is_long  && nt>sl) sl=nt;
            if (!is_long && nt<sl) sl=nt;
        }
        return false;
    }

    bool enter(bool long_dir, double bid, double ask, int64_t ts) {
        if (in_trade) return false;
        if ((ts-last_exit_ts)/1000 < cooldown_s) return false;
        const double av = atr.val>0 ? atr.val : 2.0;
        is_long  = long_dir;
        entry    = long_dir ? ask : bid;
        sl       = long_dir ? entry-av*sl_atr_mult : entry+av*sl_atr_mult;
        size     = std::max(0.01, std::min(0.50, risk_usd/(av*tick_value)));
        size     = std::floor(size/0.001)*0.001;
        entry_ts = ts; mfe=0; be_hit=false; in_trade=true;
        return true;
    }

    Result summarise(const std::string& name) {
        Result r; r.name=name;
        double eq=0,peak=0;
        for (auto& t:trades) {
            r.n_trades++; eq+=t.pnl; r.total_pnl+=t.pnl;
            r.equity.push_back(t.pnl);
            if(eq>peak)peak=eq; r.max_dd=std::max(r.max_dd,peak-eq);
            if(t.pnl>0)r.wins++; else r.losses++;
        }
        r.compute(); return r;
    }
};

// =============================================================================
// Strategy 1: DRIFT_ONLY
// =============================================================================
Result run_drift_only(const std::vector<L2Tick>& ticks,
                      double drift_thresh, int persist_min,
                      double trail_mult) {
    Backtester bt; bt.trail_mult=trail_mult;
    EWM ewm; ImbPersist ip;
    int pl=0, ps=0;

    for (auto& t : ticks) {
        if (t.watchdog_dead) continue;
        double mid=(t.bid+t.ask)*0.5;
        double sp=t.ask-t.bid;
        if (sp>bt.max_spread||sp<=0) continue;
        bt.update_atr(t.ts_ms,mid);
        ewm.update(mid);
        bt.manage(t);
        if (bt.in_trade) continue;

        double d=ewm.drift();
        if (d> drift_thresh) { pl++; ps=0; }
        else if (d<-drift_thresh) { ps++; pl=0; }
        else { pl=0; ps=0; }

        if      (pl>=persist_min) bt.enter(true,  t.bid,t.ask,t.ts_ms);
        else if (ps>=persist_min) bt.enter(false, t.bid,t.ask,t.ts_ms);
    }
    char nm[64]; snprintf(nm,sizeof(nm),"DRIFT d=%.2f p=%d tr=%.1f",drift_thresh,persist_min,trail_mult);
    return bt.summarise(nm);
}

// =============================================================================
// Strategy 2: DRIFT + DOM
// =============================================================================
Result run_drift_dom(const std::vector<L2Tick>& ticks,
                     double drift_thresh, int persist_min,
                     double dom_thresh) {
    Backtester bt;
    EWM ewm;
    int pl=0, ps=0;

    for (auto& t : ticks) {
        if (t.watchdog_dead) continue;
        double mid=(t.bid+t.ask)*0.5, sp=t.ask-t.bid;
        if (sp>bt.max_spread||sp<=0) continue;
        bt.update_atr(t.ts_ms,mid); ewm.update(mid);
        bt.manage(t);
        if (bt.in_trade) continue;

        double d=ewm.drift();
        if (d> drift_thresh) { pl++; ps=0; }
        else if (d<-drift_thresh) { ps++; pl=0; }
        else { pl=0; ps=0; }

        bool dom_long  = (t.l2_imb >= 0.5+dom_thresh || t.depth_bid > t.depth_ask);
        bool dom_short = (t.l2_imb <= 0.5-dom_thresh || t.depth_ask > t.depth_bid);

        if (pl>=persist_min && dom_long)  bt.enter(true,  t.bid,t.ask,t.ts_ms);
        if (ps>=persist_min && dom_short) bt.enter(false, t.bid,t.ask,t.ts_ms);
    }
    char nm[64]; snprintf(nm,sizeof(nm),"DRIFT+DOM d=%.2f p=%d dom=%.2f",drift_thresh,persist_min,dom_thresh);
    return bt.summarise(nm);
}

// =============================================================================
// Strategy 3: MICRO_EDGE score
// =============================================================================
Result run_micro_edge(const std::vector<L2Tick>& ticks,
                      double edge_long, double edge_short,
                      int persist_min) {
    Backtester bt;
    EWM ewm;
    int pl=0, ps=0;
    std::deque<double> edge_buf;

    for (auto& t : ticks) {
        if (t.watchdog_dead) continue;
        double mid=(t.bid+t.ask)*0.5, sp=t.ask-t.bid;
        if (sp>bt.max_spread||sp<=0) continue;
        bt.update_atr(t.ts_ms,mid); ewm.update(mid);
        bt.manage(t);
        if (bt.in_trade) { edge_buf.clear(); pl=ps=0; continue; }

        edge_buf.push_back(t.micro_edge);
        if ((int)edge_buf.size()>persist_min) edge_buf.pop_front();

        if ((int)edge_buf.size()>=persist_min) {
            bool all_long  = true, all_short = true;
            for (auto e:edge_buf) { if(e<edge_long)all_long=false; if(e>edge_short)all_short=false; }
            if (all_long  && ewm.drift()>0) bt.enter(true,  t.bid,t.ask,t.ts_ms);
            if (all_short && ewm.drift()<0) bt.enter(false, t.bid,t.ask,t.ts_ms);
        }
    }
    char nm[64]; snprintf(nm,sizeof(nm),"MICRO el=%.2f es=%.2f p=%d",edge_long,1.0-edge_short,persist_min);
    return bt.summarise(nm);
}

// =============================================================================
// Strategy 4: DOM PERSISTENCE (level count)
// =============================================================================
Result run_dom_persist(const std::vector<L2Tick>& ticks,
                       double imb_thresh, int persist_min,
                       double drift_confirm) {
    Backtester bt;
    EWM ewm;
    ImbPersist ip;

    for (auto& t : ticks) {
        if (t.watchdog_dead) continue;
        double mid=(t.bid+t.ask)*0.5, sp=t.ask-t.bid;
        if (sp>bt.max_spread||sp<=0) continue;
        bt.update_atr(t.ts_ms,mid); ewm.update(mid);
        bt.manage(t);
        if (bt.in_trade) { ip.reset(); continue; }

        ip.update(t.l2_imb, 0.5-imb_thresh, 0.5+imb_thresh);
        double d=ewm.drift();

        if (ip.long_n >=persist_min && d> drift_confirm) bt.enter(true,  t.bid,t.ask,t.ts_ms);
        if (ip.short_n>=persist_min && d<-drift_confirm) bt.enter(false, t.bid,t.ask,t.ts_ms);
    }
    char nm[64]; snprintf(nm,sizeof(nm),"DOM_PERSIST i=%.2f p=%d dc=%.2f",imb_thresh,persist_min,drift_confirm);
    return bt.summarise(nm);
}

// =============================================================================
// Strategy 5: LIQUIDITY PULL fade
// Pull = depth_bid or depth_ask drops suddenly -> fade the move
// =============================================================================
Result run_liq_pull(const std::vector<L2Tick>& ticks,
                    double pull_pct, int confirm_ticks,
                    double drift_thresh) {
    Backtester bt; bt.sl_atr_mult=0.5; bt.trail_mult=1.5;
    EWM ewm;
    int prev_bid=0, prev_ask=0;
    int pl=0, ps=0;

    for (auto& t : ticks) {
        if (t.watchdog_dead) continue;
        double mid=(t.bid+t.ask)*0.5, sp=t.ask-t.bid;
        if (sp>bt.max_spread||sp<=0) { prev_bid=t.depth_bid; prev_ask=t.depth_ask; continue; }
        bt.update_atr(t.ts_ms,mid); ewm.update(mid);
        bt.manage(t);

        if (prev_bid>0 && prev_ask>0 && !bt.in_trade) {
            // Ask side pulled (less resistance above) -> bullish
            bool ask_pull = (prev_ask>3 && t.depth_ask < prev_ask*(1.0-pull_pct));
            // Bid side pulled (less support below) -> bearish
            bool bid_pull = (prev_bid>3 && t.depth_bid < prev_bid*(1.0-pull_pct));

            if (ask_pull) pl++; else pl=0;
            if (bid_pull) ps++; else ps=0;

            double d=ewm.drift();
            if (pl>=confirm_ticks && d>drift_thresh)  bt.enter(true,  t.bid,t.ask,t.ts_ms);
            if (ps>=confirm_ticks && d<-drift_thresh) bt.enter(false, t.bid,t.ask,t.ts_ms);
        }

        prev_bid=t.depth_bid; prev_ask=t.depth_ask;
    }
    char nm[64]; snprintf(nm,sizeof(nm),"LIQ_PULL p=%.0f%% c=%d dt=%.2f",pull_pct*100,confirm_ticks,drift_thresh);
    return bt.summarise(nm);
}

// =============================================================================
// Strategy 6: HYBRID -- drift persistence + micro_edge filter
// =============================================================================
Result run_hybrid(const std::vector<L2Tick>& ticks,
                  double drift_thresh, int persist_min,
                  double edge_thresh) {
    Backtester bt;
    EWM ewm;
    int pl=0, ps=0;

    for (auto& t : ticks) {
        if (t.watchdog_dead) continue;
        double mid=(t.bid+t.ask)*0.5, sp=t.ask-t.bid;
        if (sp>bt.max_spread||sp<=0) continue;
        bt.update_atr(t.ts_ms,mid); ewm.update(mid);
        bt.manage(t);
        if (bt.in_trade) continue;

        double d=ewm.drift();
        if (d> drift_thresh) { pl++; ps=0; }
        else if (d<-drift_thresh) { ps++; pl=0; }
        else { pl=0; ps=0; }

        // micro_edge confirms direction
        bool me_long  = (t.micro_edge > 0.5+edge_thresh);
        bool me_short = (t.micro_edge < 0.5-edge_thresh);

        if (pl>=persist_min && me_long)  bt.enter(true,  t.bid,t.ask,t.ts_ms);
        if (ps>=persist_min && me_short) bt.enter(false, t.bid,t.ask,t.ts_ms);
    }
    char nm[64]; snprintf(nm,sizeof(nm),"HYBRID d=%.2f p=%d e=%.2f",drift_thresh,persist_min,edge_thresh);
    return bt.summarise(nm);
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: l2_edge_discovery l2_ticks_YYYY-MM-DD.csv ...\n";
        return 1;
    }

    // Load all ticks
    std::vector<L2Tick> ticks;
    int64_t total_lines=0, parsed=0;
    for (int i=1;i<argc;i++) {
        std::ifstream f(argv[i]);
        if (!f) { std::cerr << "Cannot open: " << argv[i] << "\n"; continue; }
        std::string line;
        getline(f,line); // header
        while (getline(f,line)) {
            total_lines++;
            L2Tick t;
            if (parse_l2(line,t)) { ticks.push_back(t); parsed++; }
        }
        std::cerr << "Loaded " << argv[i] << ": " << parsed << "/" << total_lines << " ticks\n";
    }
    if (ticks.empty()) { std::cerr << "No ticks loaded\n"; return 1; }
    std::cerr << "Total ticks: " << ticks.size() << "\n\n";

    std::vector<Result> results;

    // ── Strategy 1: DRIFT_ONLY sweep ─────────────────────────────────────────
    for (double dt : {0.20, 0.30, 0.50, 0.80, 1.20}) {
        for (int pm : {5, 10, 15, 20, 30}) {
            for (double tr : {1.5, 2.0, 3.0}) {
                auto r = run_drift_only(ticks, dt, pm, tr);
                if (r.n_trades >= 5) results.push_back(r);
            }
        }
    }

    // ── Strategy 2: DRIFT + DOM sweep ────────────────────────────────────────
    for (double dt : {0.20, 0.50, 0.80}) {
        for (int pm : {8, 15, 20}) {
            for (double dom : {0.02, 0.05, 0.08}) {
                auto r = run_drift_dom(ticks, dt, pm, dom);
                if (r.n_trades >= 5) results.push_back(r);
            }
        }
    }

    // ── Strategy 3: MICRO_EDGE sweep ─────────────────────────────────────────
    for (double el : {0.55, 0.60, 0.65, 0.70}) {
        for (int pm : {3, 5, 8, 12}) {
            auto r = run_micro_edge(ticks, el, 1.0-el, pm);
            if (r.n_trades >= 5) results.push_back(r);
        }
    }

    // ── Strategy 4: DOM PERSISTENCE sweep ────────────────────────────────────
    for (double it : {0.03, 0.05, 0.08, 0.12}) {
        for (int pm : {5, 10, 20, 30}) {
            for (double dc : {0.0, 0.20, 0.50}) {
                auto r = run_dom_persist(ticks, it, pm, dc);
                if (r.n_trades >= 5) results.push_back(r);
            }
        }
    }

    // ── Strategy 5: LIQ_PULL sweep ───────────────────────────────────────────
    for (double pp : {0.20, 0.30, 0.40, 0.50}) {
        for (int ct : {1, 2, 3}) {
            for (double dt : {0.0, 0.20, 0.50}) {
                auto r = run_liq_pull(ticks, pp, ct, dt);
                if (r.n_trades >= 5) results.push_back(r);
            }
        }
    }

    // ── Strategy 6: HYBRID sweep ─────────────────────────────────────────────
    for (double dt : {0.20, 0.50, 0.80}) {
        for (int pm : {8, 15, 20}) {
            for (double et : {0.05, 0.10, 0.15}) {
                auto r = run_hybrid(ticks, dt, pm, et);
                if (r.n_trades >= 5) results.push_back(r);
            }
        }
    }

    // ── Sort by expectancy descending ─────────────────────────────────────────
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b){
        return a.expectancy > b.expectancy;
    });

    // ── Print top 30 ─────────────────────────────────────────────────────────
    printf("\n=== L2 EDGE DISCOVERY -- TOP RESULTS BY EXPECTANCY ===\n");
    printf("%-22s | %s\n", "Strategy", "T=trades WR=winrate PnL MaxDD AvgW AvgL RR Exp");
    printf("%s\n", std::string(100,'-').c_str());
    int shown=0;
    for (auto& r : results) {
        if (shown++ >= 30) break;
        r.print();
    }

    // ── Best by WR ────────────────────────────────────────────────────────────
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b){
        return a.wr > b.wr;
    });
    printf("\n=== TOP 10 BY WIN RATE (min 10 trades) ===\n");
    shown=0;
    for (auto& r : results) {
        if (r.n_trades < 10) continue;
        if (shown++ >= 10) break;
        r.print();
    }

    // ── Best by total PnL ────────────────────────────────────────────────────
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b){
        return a.total_pnl > b.total_pnl;
    });
    printf("\n=== TOP 10 BY TOTAL PNL ===\n");
    shown=0;
    for (auto& r : results) {
        if (shown++ >= 10) break;
        r.print();
    }

    printf("\nTotal configs tested: %zu\n", results.size());
    return 0;
}
