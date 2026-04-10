// candle_flow_l2_bt.cpp -- CandleFlowEngine backtest against real L2 tick data
// Uses actual depth_bid_levels/depth_ask_levels from Omega L2 logs
// Format: ts_ms,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,depth_bid_levels,depth_ask_levels,
//         depth_events_total,watchdog_dead,vol_ratio,regime,vpin,has_pos
//
// Build: g++ -O2 -std=c++17 -o candle_flow_l2_bt candle_flow_l2_bt.cpp
// Run:   ./candle_flow_l2_bt l2_ticks_2026-04-10.csv [l2_ticks_2026-04-09.csv ...]

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

// ── L2 Tick ──────────────────────────────────────────────────────────────────
struct L2Tick {
    int64_t ts_ms = 0;
    double  bid = 0, ask = 0;
    double  l2_imb = 0.5;
    double  l2_bid_vol = 0, l2_ask_vol = 0;
    int     depth_bid = 0, depth_ask = 0;
    bool    watchdog_dead = false;
    double  vol_ratio = 1.0;
    int     regime = 0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty() || line[0]=='t') return false; // skip header
    std::stringstream ss(line); std::string tok;
    auto next = [&](auto& v) -> bool {
        if (!getline(ss,tok,',')) return false;
        try { v = std::stod(tok); } catch(...) { return false; }
        return true;
    };
    auto nexti = [&](auto& v) -> bool {
        if (!getline(ss,tok,',')) return false;
        try { v = (int)std::stoll(tok); } catch(...) { return false; }
        return true;
    };
    double tmp;
    if (!next(tmp)) return false; t.ts_ms = (int64_t)tmp;
    if (!next(t.bid)) return false;
    if (!next(t.ask)) return false;
    if (!next(t.l2_imb)) return false;
    if (!next(t.l2_bid_vol)) return false;
    if (!next(t.l2_ask_vol)) return false;
    if (!nexti(t.depth_bid)) return false;
    if (!nexti(t.depth_ask)) return false;
    if (!next(tmp)) return false; // depth_events_total
    if (!nexti(t.watchdog_dead)) return false;
    if (!next(t.vol_ratio)) return false;
    if (!nexti(t.regime)) return false;
    return (t.bid > 0 && t.ask > 0 && t.ask >= t.bid);
}

// ── M1 Bar Builder ────────────────────────────────────────────────────────────
struct Bar { double open=0,high=0,low=0,close=0; int64_t ts_open=0; bool valid=false; };
struct M1Builder {
    Bar cur, last, prev;
    int64_t cur_min = 0;
    void update(int64_t ts_ms, double mid) {
        const int64_t bmin = ts_ms / 60000;
        if (bmin != cur_min) {
            if (cur.valid) { prev=last; last=cur; }
            cur={mid,mid,mid,mid,ts_ms,true}; cur_min=bmin;
        } else {
            if (mid>cur.high) cur.high=mid;
            if (mid<cur.low)  cur.low=mid;
            cur.close=mid;
        }
    }
    bool has2() const { return last.valid && prev.valid; }
};

// ── ATR ───────────────────────────────────────────────────────────────────────
struct ATR14 {
    std::deque<double> r;
    double val=0;
    void add(const Bar& b) {
        r.push_back(b.high-b.low);
        if ((int)r.size()>14) r.pop_front();
        double s=0; for(auto x:r) s+=x;
        val=s/r.size();
    }
};

// ── DOM Snapshot (from real L2 data) ─────────────────────────────────────────
struct DOMSnap {
    int bid_count=0, ask_count=0;
    int prev_bid=0, prev_ask=0;
    double bid_vol=0, ask_vol=0;
    double prev_bid_vol=0, prev_ask_vol=0;
    bool vacuum_ask=false, vacuum_bid=false;
    bool wall_above=false, wall_below=false;

    void update(const L2Tick& t, const L2Tick& prev) {
        prev_bid=prev.depth_bid; prev_ask=prev.depth_ask;
        prev_bid_vol=prev.l2_bid_vol; prev_ask_vol=prev.l2_ask_vol;
        bid_count=t.depth_bid; ask_count=t.depth_ask;
        bid_vol=t.l2_bid_vol; ask_vol=t.l2_ask_vol;
        // Vacuum: one side has 0 or 1 levels (offers/bids pulled)
        vacuum_ask = (ask_count <= 1);
        vacuum_bid = (bid_count <= 1);
        // Wall: one side maxed out (5+ levels), other side very thin
        wall_above = (ask_count >= 5 && bid_count <= 2);
        wall_below = (bid_count >= 5 && ask_count <= 2);
    }

    int entry_score(bool is_long) const {
        int s=0;
        if (is_long) {
            if (bid_count > ask_count) s++;
            if (ask_count < prev_ask && prev_ask > 0) s++;
            if (vacuum_ask) s++;
            if (bid_count > prev_bid) s++;
        } else {
            if (ask_count > bid_count) s++;
            if (bid_count < prev_bid && prev_bid > 0) s++;
            if (vacuum_bid) s++;
            if (ask_count > prev_ask) s++;
        }
        return s;
    }

    int exit_score(bool is_long) const {
        int s=0;
        if (is_long) {
            bool bid_drop  = (bid_vol  < prev_bid_vol  * 0.7);
            bool ask_surge = (ask_vol  > prev_ask_vol  * 1.3);
            if (bid_drop || ask_surge) s++;
            if (wall_above) s++;
            if (bid_count < prev_bid && ask_count > prev_ask) s++;
            if (vacuum_bid && !vacuum_ask) s++;
        } else {
            bool ask_drop  = (ask_vol  < prev_ask_vol  * 0.7);
            bool bid_surge = (bid_vol  > prev_bid_vol  * 1.3);
            if (ask_drop || bid_surge) s++;
            if (wall_below) s++;
            if (ask_count < prev_ask && bid_count > prev_bid) s++;
            if (vacuum_ask && !vacuum_bid) s++;
        }
        return s;
    }
};

// ── Strategy Constants ────────────────────────────────────────────────────────
static constexpr double BODY_RATIO_MIN  = 0.60;
static constexpr double COST_SLIP       = 0.10;
static constexpr double COST_COMM       = 0.10;
static constexpr double COST_MULT       = 2.0;
static constexpr int    DOM_ENTRY_MIN   = 2;
static constexpr int    DOM_EXIT_MIN    = 2;
static constexpr int64_t STAGNATION_MS  = 8000;
static constexpr double STAGNATION_MULT = 1.5;
static constexpr double MAX_SPREAD      = 0.50;
static constexpr double RISK_USD        = 30.0;
static constexpr double TICK_VALUE      = 100.0;

// ── Trade ────────────────────────────────────────────────────────────────────
struct Trade {
    bool is_long; double entry,exit_px,pnl_usd; int held_s; std::string reason;
};

int main(int argc, char* argv[]) {
    std::vector<std::string> files;
    for (int i=1;i<argc;i++) files.push_back(argv[i]);
    if (files.empty()) {
        std::cerr << "Usage: candle_flow_l2_bt l2_ticks_YYYY-MM-DD.csv ...\n";
        return 1;
    }

    M1Builder bars;
    ATR14 atr;
    DOMSnap dom;
    std::vector<Trade> trades;

    bool in_trade=false, is_long=false;
    double entry=0, sl=0, size=0, cost_pts=0;
    int64_t entry_ts=0; double mfe=0;
    int64_t last_bar_min=0;
    int64_t tick_count=0;
    L2Tick prev_tick;

    std::map<std::string,int> reasons;

    for (auto& fname : files) {
        std::ifstream f(fname);
        if (!f) { std::cerr << "Cannot open: " << fname << "\n"; continue; }
        std::string line;
        getline(f,line); // header
        while (getline(f,line)) {
            L2Tick t;
            if (!parse_l2(line,t)) continue;
            if (t.watchdog_dead) { prev_tick=t; continue; }
            tick_count++;

            const double mid    = (t.ask+t.bid)*0.5;
            const double spread = t.ask-t.bid;
            if (spread > MAX_SPREAD || spread <= 0) { prev_tick=t; continue; }

            // Update DOM from real L2 data
            dom.update(t, prev_tick);
            prev_tick = t;

            // Build M1 bar
            bars.update(t.ts_ms, mid);
            const int64_t bmin = t.ts_ms/60000;
            if (bmin != last_bar_min && bars.last.valid) {
                atr.add(bars.last);
                last_bar_min = bmin;
            }

            // ── Manage ───────────────────────────────────────────────────────
            if (in_trade) {
                const double move = is_long ? (mid-entry) : (entry-mid);
                if (move>mfe) mfe=move;

                // Hard SL
                if (is_long ? (t.bid<=sl) : (t.ask>=sl)) {
                    const double px=is_long?t.bid:t.ask;
                    trades.push_back({is_long,entry,px,
                        (is_long?(px-entry):(entry-px))*size*TICK_VALUE,
                        (int)((t.ts_ms-entry_ts)/1000),"SL_HIT"});
                    reasons["SL_HIT"]++; in_trade=false; continue;
                }
                // DOM reversal
                if (dom.exit_score(is_long) >= DOM_EXIT_MIN) {
                    const double px=is_long?t.bid:t.ask;
                    trades.push_back({is_long,entry,px,
                        (is_long?(px-entry):(entry-px))*size*TICK_VALUE,
                        (int)((t.ts_ms-entry_ts)/1000),"DOM_REVERSAL"});
                    reasons["DOM_REVERSAL"]++; in_trade=false; continue;
                }
                // Stagnation
                const int64_t held=t.ts_ms-entry_ts;
                if (held>=STAGNATION_MS && mfe<cost_pts*STAGNATION_MULT) {
                    const double px=is_long?t.bid:t.ask;
                    trades.push_back({is_long,entry,px,
                        (is_long?(px-entry):(entry-px))*size*TICK_VALUE,
                        (int)(held/1000),"STAGNATION"});
                    reasons["STAGNATION"]++; in_trade=false; continue;
                }
                continue;
            }

            // ── Entry ─────────────────────────────────────────────────────────
            if (!bars.has2() || atr.val<=0) continue;

            const Bar& lb=bars.last; const Bar& pb=bars.prev;
            const double range=lb.high-lb.low;
            if (range<=0) continue;

            const double body_bull=lb.close-lb.open;
            const double body_bear=lb.open-lb.close;
            bool bullish=(body_bull>0 && body_bull/range>=BODY_RATIO_MIN && lb.close>pb.high);
            bool bearish=(body_bear>0 && body_bear/range>=BODY_RATIO_MIN && lb.close<pb.low);
            if (!bullish && !bearish) continue;

            // Cost coverage
            cost_pts=spread+COST_SLIP*2.0+COST_COMM*2.0;
            if (range < COST_MULT*cost_pts) continue;

            // DOM confirmation from real L2
            if (dom.entry_score(bullish) < DOM_ENTRY_MIN) continue;

            // Enter
            is_long=bullish;
            entry=is_long?t.ask:t.bid;
            const double sl_pts=atr.val>0?atr.val:5.0;
            sl=is_long?entry-sl_pts:entry+sl_pts;
            size=std::max(0.01,std::min(0.50,RISK_USD/(sl_pts*TICK_VALUE)));
            size=std::floor(size/0.001)*0.001;
            entry_ts=t.ts_ms; mfe=0; in_trade=true;
            reasons["ENTRY"]++;
        }
    }

    if (trades.empty()) { std::cout << "No trades.\n"; return 0; }

    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for (auto& tr:trades) {
        total+=tr.pnl_usd; eq+=tr.pnl_usd;
        if (eq>peak) peak=eq;
        maxdd=std::max(maxdd,peak-eq);
        if (tr.pnl_usd>0){w++;wpnl+=tr.pnl_usd;}
        else{l++;lpnl+=tr.pnl_usd;}
    }
    int n=trades.size();
    double wr=100.0*w/n;
    double aw=w?wpnl/w:0, al=l?lpnl/l:0;
    double rr=al!=0?-aw/al:0;

    std::cout << "\n=== CandleFlowEngine Backtest (Real L2 DOM) ===\n";
    std::cout << "Ticks        : " << tick_count << "\n";
    std::cout << "Trades       : " << n << "\n";
    std::cout << "Win rate     : " << std::fixed << std::setprecision(1) << wr << "%\n";
    std::cout << "Total PnL    : $" << std::setprecision(2) << total << "\n";
    std::cout << "Avg win      : $" << aw << "\n";
    std::cout << "Avg loss     : $" << al << "\n";
    std::cout << "R:R          : " << std::setprecision(2) << rr << "\n";
    std::cout << "Max drawdown : $" << maxdd << "\n";
    std::cout << "Exit reasons :\n";
    for (auto& kv:reasons)
        if (kv.first!="ENTRY")
            std::cout<<"  "<<kv.first<<": "<<kv.second<<"\n";
    return 0;
}
