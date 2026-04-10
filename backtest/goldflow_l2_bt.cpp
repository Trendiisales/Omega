// goldflow_l2_bt.cpp -- GoldFlow backtest against real L2 tick data
// Same input format as candle_flow_l2_bt.cpp for direct comparison
// Strategy: EWM drift persistence entry, DOM-confirmed, ATR trail exit
//
// Build: g++ -O2 -std=c++17 -o goldflow_l2_bt goldflow_l2_bt.cpp
// Run:   ./goldflow_l2_bt l2_ticks_2026-04-09.csv l2_ticks_2026-04-10.csv

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

// ── L2 Tick ───────────────────────────────────────────────────────────────────
struct L2Tick {
    int64_t ts_ms=0;
    double  bid=0, ask=0, l2_imb=0.5;
    double  l2_bid_vol=0, l2_ask_vol=0;
    int     depth_bid=0, depth_ask=0;
    bool    watchdog_dead=false;
    double  vol_ratio=1.0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty() || line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v){if(!getline(ss,tok,','))return false;try{v=std::stod(tok);}catch(...){return false;}return true;};
    auto ni=[&](int& v){if(!getline(ss,tok,','))return false;try{v=(int)std::stoll(tok);}catch(...){return false;}return true;};
    auto nb=[&](bool& v){if(!getline(ss,tok,','))return false;try{v=(bool)std::stoll(tok);}catch(...){return false;}return true;};
    double tmp;
    if (!nd(tmp)) return false; t.ts_ms=(int64_t)tmp;
    if (!nd(t.bid)||!nd(t.ask)) return false;
    if (!nd(t.l2_imb)||!nd(t.l2_bid_vol)||!nd(t.l2_ask_vol)) return false;
    if (!ni(t.depth_bid)||!ni(t.depth_ask)) return false;
    if (!nd(tmp)) return false; // depth_events_total
    if (!nb(t.watchdog_dead)) return false;
    if (!nd(t.vol_ratio)) return false;
    return (t.bid>0 && t.ask>0 && t.ask>=t.bid);
}

// ── EWM Drift (same as GoldEngineStack) ──────────────────────────────────────
struct EWM {
    double fast=0, slow=0;
    bool seeded=false;
    void update(double price, double alpha_fast=0.05, double alpha_slow=0.005) {
        if (!seeded) { fast=slow=price; seeded=true; return; }
        fast = alpha_fast*price + (1.0-alpha_fast)*fast;
        slow = alpha_slow*price + (1.0-alpha_slow)*slow;
    }
    double drift() const { return fast - slow; }
};

// ── ATR ───────────────────────────────────────────────────────────────────────
struct ATR {
    std::deque<double> r;
    double val=0;
    void update(double range) {
        r.push_back(range);
        if ((int)r.size()>14) r.pop_front();
        double s=0; for(auto x:r) s+=x; val=s/r.size();
    }
};

// ── GoldFlow Parameters ───────────────────────────────────────────────────────
static constexpr double GF_DRIFT_LONG_THRESH  =  0.30;  // drift > +0.30 = LONG signal
static constexpr double GF_DRIFT_SHORT_THRESH = -0.30;  // drift < -0.30 = SHORT signal
static constexpr int    GF_PERSIST_MIN        = 12;     // 12 consecutive ticks same direction
static constexpr double GF_DOM_LONG_THRESH    = 0.55;   // bid-heavy DOM confirms LONG
static constexpr double GF_DOM_SHORT_THRESH   = 0.45;   // ask-heavy DOM confirms SHORT
static constexpr double GF_BE_MULT            = 1.0;    // move to BE at 1x ATR
static constexpr double GF_TRAIL_MULT         = 2.0;    // trail at 2x ATR
static constexpr double MAX_SPREAD            = 0.50;
static constexpr double RISK_USD              = 30.0;
static constexpr double TICK_VALUE            = 100.0;

struct Trade {
    bool is_long; double entry, exit_px, pnl_usd; int held_s; std::string reason;
};

int main(int argc, char* argv[]) {
    std::vector<std::string> files;
    for (int i=1;i<argc;i++) files.push_back(argv[i]);
    if (files.empty()) { std::cerr << "Usage: goldflow_l2_bt files...\n"; return 1; }

    EWM ewm;
    ATR atr;
    std::vector<Trade> trades;
    std::map<std::string,int> reasons;

    // Persistence counter
    int  persist_long  = 0;
    int  persist_short = 0;

    // Position state
    bool   in_trade = false;
    bool   is_long  = false;
    double entry    = 0, sl = 0, be_price = 0, trail_sl = 0, size = 0;
    int64_t entry_ts = 0;
    double  mfe = 0;
    bool   be_hit = false;
    int64_t tick_count = 0;

    double prev_mid = 0;
    double range_high = 0, range_low = 1e9;
    int64_t last_range_reset = 0;

    for (auto& fname : files) {
        std::ifstream f(fname);
        if (!f) { std::cerr << "Cannot open: " << fname << "\n"; continue; }
        std::string line;
        getline(f, line); // header
        while (getline(f, line)) {
            L2Tick t;
            if (!parse_l2(line, t)) continue;
            if (t.watchdog_dead) continue;
            tick_count++;

            const double mid    = (t.ask + t.bid) * 0.5;
            const double spread = t.ask - t.bid;
            if (spread > MAX_SPREAD || spread <= 0) continue;

            // Update EWM drift
            ewm.update(mid);

            // Rolling range for ATR proxy
            if (t.ts_ms - last_range_reset > 60000) { // reset every minute
                atr.update(range_high - range_low > 0 ? range_high - range_low : spread);
                range_high = mid; range_low = mid;
                last_range_reset = t.ts_ms;
            }
            if (mid > range_high) range_high = mid;
            if (mid < range_low)  range_low  = mid;

            const double drift = ewm.drift();

            // ── Manage open position ──────────────────────────────────────────
            if (in_trade) {
                const double move = is_long ? (mid - entry) : (entry - mid);
                if (move > mfe) mfe = move;
                const double atr_val = atr.val > 0 ? atr.val : 2.0;

                // Hard SL
                const bool sl_hit = is_long ? (t.bid <= sl) : (t.ask >= sl);
                if (sl_hit) {
                    const double px = is_long ? t.bid : t.ask;
                    const double pnl = (is_long?(px-entry):(entry-px))*size*TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"SL_HIT"});
                    reasons["SL_HIT"]++; in_trade=false; persist_long=0; persist_short=0; continue;
                }

                // Move to BE at 1x ATR
                if (!be_hit && move >= atr_val * GF_BE_MULT) {
                    be_price = entry;
                    sl = be_price;
                    be_hit = true;
                }

                // Trail at 2x ATR
                if (be_hit) {
                    const double new_trail = is_long
                        ? mid - atr_val * GF_TRAIL_MULT
                        : mid + atr_val * GF_TRAIL_MULT;
                    if (is_long  && new_trail > sl) sl = new_trail;
                    if (!is_long && new_trail < sl) sl = new_trail;
                }

                // DOM reversal exit: imbalance flips strongly against position
                const bool dom_reversal = is_long
                    ? (t.l2_imb < GF_DOM_SHORT_THRESH && t.depth_ask > t.depth_bid * 2)
                    : (t.l2_imb > GF_DOM_LONG_THRESH  && t.depth_bid > t.depth_ask * 2);
                if (dom_reversal && be_hit) {
                    const double px = is_long ? t.bid : t.ask;
                    const double pnl = (is_long?(px-entry):(entry-px))*size*TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"DOM_REVERSAL"});
                    reasons["DOM_REVERSAL"]++; in_trade=false; persist_long=0; persist_short=0; continue;
                }

                // Counter-trend exit: drift strongly reversed
                const bool ct_exit = is_long ? (drift < -GF_DRIFT_LONG_THRESH*2)
                                             : (drift >  GF_DRIFT_SHORT_THRESH*2*-1);
                if (ct_exit && be_hit) {
                    const double px = is_long ? t.bid : t.ask;
                    const double pnl = (is_long?(px-entry):(entry-px))*size*TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"DRIFT_REVERSE"});
                    reasons["DRIFT_REVERSE"]++; in_trade=false; persist_long=0; persist_short=0; continue;
                }

                prev_mid = mid; continue;
            }

            // ── Entry: drift persistence ──────────────────────────────────────
            if (drift > GF_DRIFT_LONG_THRESH)       { persist_long++;  persist_short=0; }
            else if (drift < GF_DRIFT_SHORT_THRESH) { persist_short++; persist_long=0;  }
            else                                    { persist_long=0;  persist_short=0; }

            const bool long_signal  = (persist_long  >= GF_PERSIST_MIN);
            const bool short_signal = (persist_short >= GF_PERSIST_MIN);
            if (!long_signal && !short_signal) { prev_mid=mid; continue; }

            // DOM confirmation using real depth levels
            const bool dom_ok_long  = (t.l2_imb >= GF_DOM_LONG_THRESH  || t.depth_bid >= t.depth_ask);
            const bool dom_ok_short = (t.l2_imb <= GF_DOM_SHORT_THRESH || t.depth_ask >= t.depth_bid);

            if (long_signal  && !dom_ok_long)  { prev_mid=mid; continue; }
            if (short_signal && !dom_ok_short) { prev_mid=mid; continue; }

            // Enter
            is_long  = long_signal;
            entry    = is_long ? t.ask : t.bid;
            const double atr_val = atr.val > 0 ? atr.val : 2.0;
            sl       = is_long ? entry - atr_val : entry + atr_val;
            size     = std::max(0.01, std::min(0.50, RISK_USD/(atr_val*TICK_VALUE)));
            size     = std::floor(size/0.001)*0.001;
            entry_ts = t.ts_ms; mfe=0; be_hit=false;
            in_trade = true;
            persist_long=0; persist_short=0;
            reasons["ENTRY"]++;
            prev_mid = mid;
        }
    }

    if (trades.empty()) { std::cout << "No trades.\n"; return 0; }

    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for (auto& tr : trades) {
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

    std::cout << "\n=== GoldFlow Backtest (Real L2 DOM) ===\n";
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
        if (kv.first != "ENTRY")
            std::cout << "  " << kv.first << ": " << kv.second << "\n";
    return 0;
}
