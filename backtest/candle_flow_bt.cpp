// candle_flow_bt.cpp -- CandleFlowEngine backtest
// Reads XAUUSD tick CSV, builds M1 bars, runs CandleFlowEngine
// Build: g++ -O2 -std=c++17 -o candle_flow_bt candle_flow_bt.cpp
// Run:   ./candle_flow_bt ~/tick/xauusd_merged_24months.csv

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <algorithm>
#include <iomanip>
#include <functional>
#include <mutex>
#include <atomic>
#include <cassert>

// ── Tick ─────────────────────────────────────────────────────────────────────
struct Tick { uint64_t ts=0; double ask=0, bid=0; };
bool parse_tick(const std::string& line, Tick& t) {
    if (line.empty()) return false;
    std::stringstream ss(line); std::string tok;
    if (!getline(ss,tok,',')) return false;
    if (tok.empty()||!isdigit((unsigned char)tok[0])) return false;
    try { t.ts=std::stoull(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false; try { t.ask=std::stod(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false; try { t.bid=std::stod(tok); } catch(...) { return false; }
    return (t.ask>0&&t.bid>0&&t.ask>=t.bid);
}

// ── M1 bar builder ────────────────────────────────────────────────────────────
struct Bar { double open=0,high=0,low=0,close=0; uint64_t ts_open=0; bool valid=false; };
struct M1Builder {
    Bar cur, last, prev;
    uint64_t cur_min = 0;
    void update(uint64_t ts_ms, double mid) {
        const uint64_t bar_min = ts_ms / 60000;
        if (bar_min != cur_min) {
            if (cur.valid) { prev=last; last=cur; }
            cur.open=mid; cur.high=mid; cur.low=mid; cur.close=mid;
            cur.ts_open=ts_ms; cur.valid=true; cur_min=bar_min;
        } else {
            if (mid > cur.high) cur.high=mid;
            if (mid < cur.low)  cur.low=mid;
            cur.close=mid;
        }
    }
    bool has2() const { return last.valid && prev.valid; }
};

// ── ATR (14-bar) ─────────────────────────────────────────────────────────────
struct ATR14 {
    std::deque<double> ranges;
    double val = 0.0;
    void add(const Bar& b) {
        ranges.push_back(b.high - b.low);
        if (ranges.size() > 14) ranges.pop_front();
        double s=0; for(auto r:ranges) s+=r;
        val = s / ranges.size();
    }
};

// ── Trade ────────────────────────────────────────────────────────────────────
struct Trade {
    bool is_long; double entry,exit_px,sl,size,pnl; int held_s;
    std::string reason;
};

// ── CandleFlowEngine (self-contained, no Omega headers needed) ────────────────
static constexpr double CFE_BODY_RATIO_MIN  = 0.60;
static constexpr double CFE_COST_SLIPPAGE   = 0.10;
static constexpr double CFE_COMMISSION_PTS  = 0.10;
static constexpr double CFE_COST_MULT       = 2.0;
static constexpr int    CFE_DOM_CONFIRM_MIN = 2;
static constexpr int    CFE_DOM_EXIT_MIN    = 2;
static constexpr int64_t CFE_STAGNATION_MS  = 8000;
static constexpr double CFE_STAGNATION_MULT = 1.5;
static constexpr double TICK_VALUE          = 100.0;
static constexpr double MAX_SPREAD          = 0.50;
static constexpr double RISK_USD            = 30.0;

// Simulated DOM from tick data: use price velocity as DOM proxy
// bid_count > ask_count when price rising (buyers consuming offers)
// ask_count > bid_count when price falling (sellers consuming bids)
struct DOMSim {
    int bid_count=3, ask_count=3;
    int prev_bid=3, prev_ask=3;
    bool vacuum_ask=false, vacuum_bid=false;
    bool wall_above=false, wall_below=false;
    void update(double prev_mid, double cur_mid, double spread) {
        prev_bid = bid_count; prev_ask = ask_count;
        // Price rising: bids consuming asks
        if (cur_mid > prev_mid + spread*0.3) {
            bid_count=4; ask_count=1; vacuum_ask=true; vacuum_bid=false;
            wall_above=false; wall_below=false;
        } else if (cur_mid < prev_mid - spread*0.3) {
            bid_count=1; ask_count=4; vacuum_bid=true; vacuum_ask=false;
            wall_above=false; wall_below=false;
        } else {
            bid_count=3; ask_count=3;
            vacuum_ask=false; vacuum_bid=false;
            wall_above=false; wall_below=false;
        }
    }
    int entry_score(bool is_long) const {
        int s=0;
        if (is_long) {
            if (bid_count>ask_count) s++;
            if (ask_count<prev_ask&&prev_ask>0) s++;
            if (vacuum_ask) s++;
            if (bid_count>prev_bid) s++;
        } else {
            if (ask_count>bid_count) s++;
            if (bid_count<prev_bid&&prev_bid>0) s++;
            if (vacuum_bid) s++;
            if (ask_count>prev_ask) s++;
        }
        return s;
    }
    int exit_score(bool is_long) const {
        int s=0;
        if (is_long) {
            if (bid_count<prev_bid*0.7||ask_count>prev_ask*1.3) s++;
            if (wall_above) s++;
            if (bid_count<prev_bid&&ask_count>prev_ask) s++;
            if (vacuum_bid&&!vacuum_ask) s++;
        } else {
            if (ask_count<prev_ask*0.7||bid_count>prev_bid*1.3) s++;
            if (wall_below) s++;
            if (ask_count<prev_ask&&bid_count>prev_bid) s++;
            if (vacuum_ask&&!vacuum_bid) s++;
        }
        return s;
    }
};

int main(int argc, char* argv[]) {
    const char* infile = argc>1 ? argv[1] : "/Users/jo/tick/xauusd_merged_24months.csv";
    std::ifstream f(infile);
    if (!f) { std::cerr << "Cannot open: " << infile << "\n"; return 1; }

    M1Builder bars;
    ATR14 atr;
    DOMSim dom;
    std::vector<Trade> trades;

    // State
    bool in_trade=false, is_long=false;
    double entry=0, sl=0, size=0, cost_pts=0;
    int64_t entry_ts=0;
    double mfe=0;
    double prev_mid=0;

    uint64_t tick_count=0;
    uint64_t last_bar_min=0;

    std::string line;
    getline(f,line); // header

    while(getline(f,line)) {
        Tick t;
        if (!parse_tick(line,t)) continue;
        tick_count++;

        const double mid    = (t.ask+t.bid)*0.5;
        const double spread = t.ask-t.bid;
        if (spread > MAX_SPREAD) { prev_mid=mid; continue; }

        // Update DOM
        dom.update(prev_mid, mid, spread);
        prev_mid=mid;

        // Build M1 bar
        const uint64_t bar_min = t.ts/60000;
        bars.update(t.ts, mid);

        // Update ATR on new bar
        if (bar_min != last_bar_min && bars.last.valid) {
            atr.add(bars.last);
            last_bar_min = bar_min;
        }

        // ── Manage open trade ───────────────────────────────────────────────
        if (in_trade) {
            const double move = is_long ? (mid-entry) : (entry-mid);
            if (move>mfe) mfe=move;

            // Hard SL
            const bool sl_hit = is_long ? (t.bid<=sl) : (t.ask>=sl);
            if (sl_hit) {
                const double px = is_long ? t.bid : t.ask;
                trades.push_back({is_long, entry, px, sl, size,
                    (is_long?(px-entry):(entry-px))*size,
                    (int)((t.ts-entry_ts)/1000), "SL_HIT"});
                in_trade=false; continue;
            }

            // DOM reversal exit
            const int escore = dom.exit_score(is_long);
            if (escore >= CFE_DOM_EXIT_MIN) {
                const double px = is_long ? t.bid : t.ask;
                trades.push_back({is_long, entry, px, sl, size,
                    (is_long?(px-entry):(entry-px))*size,
                    (int)((t.ts-entry_ts)/1000), "DOM_REVERSAL"});
                in_trade=false; continue;
            }

            // Stagnation
            const int64_t held = (int64_t)(t.ts - entry_ts);
            if (held >= CFE_STAGNATION_MS && mfe < cost_pts*CFE_STAGNATION_MULT) {
                const double px = is_long ? t.bid : t.ask;
                trades.push_back({is_long, entry, px, sl, size,
                    (is_long?(px-entry):(entry-px))*size,
                    (int)(held/1000), "STAGNATION"});
                in_trade=false; continue;
            }
            continue;
        }

        // ── Entry check ─────────────────────────────────────────────────────
        if (!bars.has2()) continue;
        if (atr.val <= 0.0) continue;

        const Bar& lb = bars.last;
        const Bar& pb = bars.prev;

        // 1. Candle direction
        const double range = lb.high - lb.low;
        if (range <= 0.0) continue;
        const double body_bull = lb.close - lb.open;
        const double body_bear = lb.open  - lb.close;
        bool bullish = (body_bull>0 && body_bull/range>=CFE_BODY_RATIO_MIN && lb.close>pb.high);
        bool bearish = (body_bear>0 && body_bear/range>=CFE_BODY_RATIO_MIN && lb.close<pb.low);
        if (!bullish && !bearish) continue;

        // 2. Cost coverage
        cost_pts = spread + CFE_COST_SLIPPAGE*2.0 + CFE_COMMISSION_PTS*2.0;
        if (range < CFE_COST_MULT * cost_pts) continue;

        // 3. DOM confirmation
        const int dscore = dom.entry_score(bullish);
        if (dscore < CFE_DOM_CONFIRM_MIN) continue;

        // Enter
        is_long = bullish;
        entry   = is_long ? t.ask : t.bid;
        const double sl_pts = atr.val > 0.0 ? atr.val : 5.0;
        sl      = is_long ? entry - sl_pts : entry + sl_pts;
        size    = std::max(0.01, std::min(0.50, RISK_USD/(sl_pts*TICK_VALUE)));
        size    = std::floor(size/0.001)*0.001;
        entry_ts= t.ts;
        mfe     = 0.0;
        in_trade= true;
    }

    if (trades.empty()) {
        std::cout << "No trades generated\n";
        return 0;
    }

    // ── Stats ────────────────────────────────────────────────────────────────
    double total_pnl=0, win_pnl=0, loss_pnl=0;
    int wins=0, losses=0;
    std::unordered_map<std::string,int> reasons;
    double max_dd=0, peak=0, equity=0;

    for (auto& tr : trades) {
        const double pnl_usd = tr.pnl * TICK_VALUE;
        total_pnl += pnl_usd;
        equity    += pnl_usd;
        if (equity > peak) peak=equity;
        const double dd = peak-equity;
        if (dd > max_dd) max_dd=dd;
        if (pnl_usd>0) { wins++; win_pnl+=pnl_usd; }
        else           { losses++; loss_pnl+=pnl_usd; }
        reasons[tr.reason]++;
    }

    const int n = trades.size();
    const double wr = 100.0*wins/n;
    const double avg_win  = wins   ? win_pnl/wins   : 0;
    const double avg_loss = losses ? loss_pnl/losses : 0;
    const double rr       = avg_loss != 0 ? -avg_win/avg_loss : 0;

    std::cout << "\n=== CandleFlowEngine Backtest ===\n";
    std::cout << "Ticks processed : " << tick_count << "\n";
    std::cout << "Total trades    : " << n << "\n";
    std::cout << "Win rate        : " << std::fixed << std::setprecision(1) << wr << "%\n";
    std::cout << "Total PnL       : $" << std::setprecision(2) << total_pnl << "\n";
    std::cout << "Avg win         : $" << avg_win << "\n";
    std::cout << "Avg loss        : $" << avg_loss << "\n";
    std::cout << "Risk/Reward     : " << std::setprecision(2) << rr << "\n";
    std::cout << "Max drawdown    : $" << max_dd << "\n";
    std::cout << "Exit reasons:\n";
    for (auto& kv : reasons)
        std::cout << "  " << kv.first << ": " << kv.second << "\n";

    return 0;
}
