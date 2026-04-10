// momentum_bt.cpp -- MomentumBreakoutEngine backtest
// Pure EMA/ATR breakout, no DOM, no L2
// Build: g++ -O3 -std=c++17 -o momentum_bt momentum_bt.cpp
// Run:   ./momentum_bt ~/tick/xauusd_merged_24months.csv

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

// Config -- matches MomentumBreakoutEngine.hpp
static constexpr int    EMA_FAST       = 20;
static constexpr int    EMA_SLOW       = 50;
static constexpr int    ATR_PERIOD     = 14;
static constexpr double ATR_BREAK_MULT = 1.5;
static constexpr double ATR_SL_MULT    = 1.2;
static constexpr double ATR_TP_MULT    = 2.5;
static constexpr double MAX_SPREAD     = 0.40;
static constexpr double EMA_MIN_SEP    = 0.10;
static constexpr int    TIMEOUT_BARS   = 20;
static constexpr double RISK_USD       = 30.0;
static constexpr double TICK_VALUE     = 100.0;
static constexpr int    SESSION_START  = 7;   // UTC hour
static constexpr int    SESSION_END    = 22;  // UTC hour

struct Tick { uint64_t ts=0; double ask=0, bid=0; };
bool parse_tick(const std::string& line, Tick& t) {
    if (line.empty()) return false;
    std::stringstream ss(line); std::string tok;
    if (!std::getline(ss,tok,',')) return false;
    if (tok.empty()||!isdigit((unsigned char)tok[0])) return false;
    try { t.ts=std::stoull(tok); } catch(...) { return false; }
    if (!std::getline(ss,tok,',')) return false; try { t.ask=std::stod(tok); } catch(...) { return false; }
    if (!std::getline(ss,tok,',')) return false; try { t.bid=std::stod(tok); } catch(...) { return false; }
    return (t.ask>0&&t.bid>0&&t.ask>=t.bid);
}

// M1 bar
struct Bar { double open=0,high=0,low=0,close=0; uint64_t ts=0; bool valid=false; };
struct M1Builder {
    Bar cur,last,prev; uint64_t cur_min=0;
    bool new_bar = false;
    void update(uint64_t ts_ms, double mid) {
        new_bar = false;
        const uint64_t bm = ts_ms/60000;
        if (bm != cur_min) {
            if (cur.valid) { prev=last; last=cur; new_bar=true; }
            cur={mid,mid,mid,mid,ts_ms,true}; cur_min=bm;
        } else {
            if (mid>cur.high) cur.high=mid;
            if (mid<cur.low)  cur.low=mid;
            cur.close=mid;
        }
    }
    bool has2() const { return last.valid && prev.valid; }
};

// EMA
struct EMA {
    int period; double val=0; bool warm=false; int count=0;
    double alpha;
    explicit EMA(int p) : period(p), alpha(2.0/(p+1)) {}
    void update(double v) {
        if (!warm) { val=v; count++; if(count>=period) warm=true; }
        else val = v*alpha + val*(1.0-alpha);
    }
};

// ATR14
struct ATR14 {
    std::deque<double> r; double val=0;
    void add(const Bar& b) {
        r.push_back(b.high-b.low);
        if((int)r.size()>14) r.pop_front();
        double s=0; for(auto x:r) s+=x; val=s/r.size();
    }
};

struct Trade {
    bool is_long; double entry,exit_px,pnl_usd; int bars_held; std::string reason;
};

int main(int argc, char* argv[]) {
    const char* infile = argc>1?argv[1]:"/Users/jo/tick/xauusd_merged_24months.csv";
    std::ifstream f(infile);
    if (!f) { std::cerr<<"Cannot open: "<<infile<<"\n"; return 1; }

    M1Builder bars;
    EMA ema_fast(EMA_FAST), ema_slow(EMA_SLOW);
    ATR14 atr;
    std::vector<Trade> trades;
    std::map<std::string,int> reasons;

    bool in_trade=false, is_long=false;
    double entry=0, sl=0, tp=0, size=0, mfe=0;
    int bars_held=0;
    int64_t entry_ts=0, cooldown_until=0;
    uint64_t tick_count=0, last_bar_min=0;

    std::string line;
    std::getline(f,line); // header

    while (std::getline(f,line)) {
        Tick t;
        if (!parse_tick(line,t)) continue;
        tick_count++;

        const double mid    = (t.ask+t.bid)*0.5;
        const double spread = t.ask-t.bid;
        if (spread>MAX_SPREAD||spread<=0) continue;

        // Session filter
        const int utc_hour = (int)((t.ts/1000) % 86400 / 3600);
        const bool in_session = (utc_hour >= SESSION_START && utc_hour < SESSION_END);

        // Update M1 bars
        bars.update(t.ts, mid);
        const uint64_t bm = t.ts/60000;
        if (bm != last_bar_min && bars.last.valid) {
            ema_fast.update(bars.last.close);
            ema_slow.update(bars.last.close);
            atr.add(bars.last);
            last_bar_min = bm;
        }

        if ((int64_t)t.ts < cooldown_until) continue;

        // ── Manage open trade (tick level SL/TP) ─────────────────────────────
        if (in_trade) {
            const double move = is_long?(mid-entry):(entry-mid);
            if (move>mfe) mfe=move;

            if (is_long?(t.bid<=sl):(t.ask>=sl)) {
                const double px=is_long?t.bid:t.ask;
                const double pnl=(is_long?(px-entry):(entry-px))*size*TICK_VALUE;
                trades.push_back({is_long,entry,px,pnl,bars_held,"SL_HIT"});
                reasons["SL_HIT"]++;
                in_trade=false; cooldown_until=(int64_t)t.ts+10000; continue;
            }
            if (is_long?(t.ask>=tp):(t.bid<=tp)) {
                const double px=is_long?t.ask:t.bid;
                const double pnl=(is_long?(px-entry):(entry-px))*size*TICK_VALUE;
                trades.push_back({is_long,entry,px,pnl,bars_held,"TP_HIT"});
                reasons["TP_HIT"]++;
                in_trade=false; cooldown_until=(int64_t)t.ts+10000; continue;
            }

            // On new bar: check EMA cross exit and timeout
            if (bars.new_bar && bars.has2()) {
                bars_held++;
                const bool ema_cross_against =
                    (is_long  && ema_fast.val < ema_slow.val - EMA_MIN_SEP) ||
                    (!is_long && ema_fast.val > ema_slow.val + EMA_MIN_SEP);
                if (ema_cross_against) {
                    const double px=is_long?t.bid:t.ask;
                    const double pnl=(is_long?(px-entry):(entry-px))*size*TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,bars_held,"EMA_CROSS"});
                    reasons["EMA_CROSS"]++;
                    in_trade=false; cooldown_until=(int64_t)t.ts+10000; continue;
                }
                if (bars_held >= TIMEOUT_BARS) {
                    const double px=is_long?t.bid:t.ask;
                    const double pnl=(is_long?(px-entry):(entry-px))*size*TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,bars_held,"TIMEOUT"});
                    reasons["TIMEOUT"]++;
                    in_trade=false; cooldown_until=(int64_t)t.ts+10000; continue;
                }
            }
            continue;
        }

        // ── Entry check (on new bar close only) ───────────────────────────────
        if (!bars.new_bar) continue;
        if (!bars.has2()) continue;
        if (!ema_fast.warm || !ema_slow.warm) continue;
        if (atr.val <= 0.0) continue;
        if (!in_session) continue;

        const double ef = ema_fast.val;
        const double es = ema_slow.val;
        const double ema_sep = ef - es;
        const bool uptrend   = (ema_sep >  EMA_MIN_SEP);
        const bool downtrend = (ema_sep < -EMA_MIN_SEP);
        if (!uptrend && !downtrend) continue;

        const double breakout  = atr.val * ATR_BREAK_MULT;
        const bool long_break  = uptrend   && (bars.last.close > bars.prev.high + breakout);
        const bool short_break = downtrend && (bars.last.close < bars.prev.low  - breakout);
        if (!long_break && !short_break) continue;

        is_long  = long_break;
        entry    = is_long ? t.ask : t.bid;
        const double sl_pts = atr.val * ATR_SL_MULT;
        const double tp_pts = atr.val * ATR_TP_MULT;
        sl       = is_long ? entry-sl_pts : entry+sl_pts;
        tp       = is_long ? entry+tp_pts : entry-tp_pts;
        size     = std::max(0.01, std::min(0.50, RISK_USD/(sl_pts*TICK_VALUE)));
        size     = std::floor(size/0.001)*0.001;
        entry_ts = (int64_t)t.ts;
        mfe=0; bars_held=0; in_trade=true;
        reasons["ENTRY"]++;
    }

    if (trades.empty()) {
        std::cout<<"No trades. Ticks="<<tick_count<<"\n"; return 0;
    }

    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for (auto& tr:trades) {
        total+=tr.pnl_usd; eq+=tr.pnl_usd;
        if(eq>peak)peak=eq; maxdd=std::max(maxdd,peak-eq);
        if(tr.pnl_usd>0){w++;wpnl+=tr.pnl_usd;}else{l++;lpnl+=tr.pnl_usd;}
    }
    int n=(int)trades.size();
    double wr=100.0*w/n;
    double aw=w?wpnl/w:0, al=l?lpnl/l:0;
    double rr=al!=0?-aw/al:0;
    double exp_pt=(wr/100.0)*aw+(1.0-wr/100.0)*al;

    std::cout<<"\n=== MomentumBreakoutEngine Backtest (24-month tick) ===\n";
    std::cout<<"EMA_FAST="<<EMA_FAST<<" EMA_SLOW="<<EMA_SLOW
             <<" ATR_BREAK="<<ATR_BREAK_MULT<<"x ATR_SL="<<ATR_SL_MULT
             <<"x ATR_TP="<<ATR_TP_MULT<<"x\n";
    std::cout<<std::string(52,'-')<<"\n";
    std::cout<<"Ticks      : "<<tick_count<<"\n";
    std::cout<<"Trades     : "<<n<<"\n";
    std::cout<<"Win rate   : "<<std::fixed<<std::setprecision(1)<<wr<<"%\n";
    std::cout<<"Total PnL  : $"<<std::setprecision(2)<<total<<"\n";
    std::cout<<"Avg win    : $"<<aw<<"\n";
    std::cout<<"Avg loss   : $"<<al<<"\n";
    std::cout<<"R:R        : "<<std::setprecision(2)<<rr<<"\n";
    std::cout<<"Exp/trade  : $"<<exp_pt<<"\n";
    std::cout<<"Max DD     : $"<<maxdd<<"\n";
    std::cout<<"Exits:\n";
    for (auto& kv:reasons)
        if(kv.first!="ENTRY")
            std::cout<<"  "<<std::left<<std::setw(12)<<kv.first<<kv.second<<"\n";
    return 0;
}
