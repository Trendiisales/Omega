// candle_rsi_l2_bt.cpp
//
// Architecture:
//   ENTRY:  expansion candle (quality gate) + RSI slope trend (direction)
//   EXIT:   real L2 DOM reversal (imbalance flip + depth change, smoothed)
//
// Uses real L2 CSV logs from Omega VPS.
// Format: ts_ms,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,depth_bid_levels,
//         depth_ask_levels,depth_events_total,watchdog_dead,vol_ratio,
//         regime,vpin,has_pos
//
// Build: g++ -O2 -std=c++17 -o candle_rsi_l2_bt candle_rsi_l2_bt.cpp
// Run:   ./candle_rsi_l2_bt l2_ticks_2026-04-10.csv

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

// =============================================================================
// Config
// =============================================================================
static constexpr double  CFG_BODY_RATIO_MIN   = 0.60;
static constexpr double  CFG_COST_SLIP        = 0.05;   // tighter -- spread is only 0.12-0.22
static constexpr double  CFG_COST_COMM        = 0.05;
static constexpr double  CFG_COST_MULT        = 1.5;    // lower -- only 1 day, want trades
static constexpr double  CFG_MAX_SPREAD       = 0.30;

// RSI trend (entry direction)
static constexpr int     CFG_RSI_PERIOD       = 10;     // ticks
static constexpr int     CFG_RSI_SLOPE_EMA    = 5;
static constexpr double  CFG_RSI_TREND_THRESH = 0.3;    // tuned up from 0.05

// DOM exit (real L2)
// imbalance: (bid_vol - ask_vol) / (bid_vol + ask_vol), range -1..+1 (centered on 0)
// We use (l2_imb - 0.5) * 2 to convert from 0..1 to -1..+1
static constexpr double  CFG_IMB_EXIT_THRESH  = 0.05;   // imbalance flips this far against us
static constexpr int     CFG_IMB_TICKS        = 4;      // must persist N ticks
static constexpr int     CFG_DEPTH_EXIT_DROP  = 1;      // depth level drop triggers exit signal

// Trade management
static constexpr int64_t CFG_STAGNATION_MS    = 120000; // 2 min (1 day data -- be generous)
static constexpr double  CFG_STAGNATION_MULT  = 1.0;    // mfe >= 1x cost
static constexpr double  CFG_RISK_USD         = 30.0;
static constexpr double  CFG_TICK_VALUE       = 100.0;
static constexpr int64_t CFG_COOLDOWN_MS      = 10000;

// =============================================================================
// L2 Tick
// =============================================================================
struct L2Tick {
    int64_t ts_ms = 0;
    double  bid = 0, ask = 0;
    double  l2_imb = 0.5;       // 0..1, 0.5 = neutral
    double  l2_bid_vol = 0;
    double  l2_ask_vol = 0;
    int     depth_bid = 0;
    int     depth_ask = 0;
    bool    watchdog_dead = false;
    double  vol_ratio = 1.0;
    int     regime = 0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty() || line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd = [&](double& v) -> bool {
        if (!std::getline(ss,tok,',')) return false;
        try { v=std::stod(tok); } catch(...) { return false; }
        return true;
    };
    auto ni = [&](int& v) -> bool {
        if (!std::getline(ss,tok,',')) return false;
        try { v=(int)std::stoll(tok); } catch(...) { return false; }
        return true;
    };
    double tmp;
    if (!nd(tmp))          return false; t.ts_ms=(int64_t)tmp;
    if (!nd(t.bid))        return false;
    if (!nd(t.ask))        return false;
    if (!nd(t.l2_imb))     return false;
    if (!nd(t.l2_bid_vol)) return false;
    if (!nd(t.l2_ask_vol)) return false;
    if (!ni(t.depth_bid))  return false;
    if (!ni(t.depth_ask))  return false;
    if (!nd(tmp))          return false; // depth_events_total
    int wd=0; if (!ni(wd)) return false; t.watchdog_dead=(wd!=0);
    if (!nd(t.vol_ratio))  return false;
    if (!ni(t.regime))     return false;
    return (t.bid>0 && t.ask>0 && t.ask>=t.bid);
}

// =============================================================================
// M1 bar builder
// =============================================================================
struct Bar { double open=0,high=0,low=0,close=0; int64_t ts_open=0; bool valid=false; };
struct M1Builder {
    Bar cur,last,prev; int64_t cur_min=0;
    void update(int64_t ts, double mid) {
        const int64_t bm=ts/60000;
        if (bm!=cur_min) {
            if (cur.valid){prev=last;last=cur;}
            cur={mid,mid,mid,mid,ts,true}; cur_min=bm;
        } else {
            if(mid>cur.high)cur.high=mid;
            if(mid<cur.low) cur.low=mid;
            cur.close=mid;
        }
    }
    bool has2() const { return last.valid&&prev.valid; }
};

// =============================================================================
// ATR
// =============================================================================
struct ATR14 {
    std::deque<double> r; double val=0;
    void add(const Bar& b) {
        r.push_back(b.high-b.low);
        if((int)r.size()>14)r.pop_front();
        double s=0;for(auto x:r)s+=x;val=s/r.size();
    }
};

// =============================================================================
// Tick RSI + slope EMA
// =============================================================================
struct TickRSI {
    int period, ema_n;
    double thresh;
    std::deque<double> gains, losses;
    double prev_mid=0, rsi_cur=50, rsi_prev=50;
    double rsi_trend=0;
    bool warmed=false;
    double ema_a;

    TickRSI(int p=10, int en=5, double th=0.3)
        : period(p), ema_n(en), thresh(th), ema_a(2.0/(en+1)) {}

    void update(double mid) {
        if (prev_mid==0){prev_mid=mid;return;}
        const double chg=mid-prev_mid; prev_mid=mid;
        gains.push_back(chg>0?chg:0.0);
        losses.push_back(chg<0?-chg:0.0);
        if((int)gains.size()>period){gains.pop_front();losses.pop_front();}
        if((int)gains.size()<period) return;
        double ag=0,al=0;
        for(auto x:gains) ag+=x;
        for(auto x:losses)al+=x;
        ag/=period; al/=period;
        rsi_prev=rsi_cur;
        rsi_cur=(al==0)?100.0:100.0-100.0/(1.0+ag/al);
        const double slope=rsi_cur-rsi_prev;
        if(!warmed){rsi_trend=slope;warmed=true;}
        else rsi_trend=slope*ema_a+rsi_trend*(1.0-ema_a);
    }

    int direction() const {
        if(!warmed) return 0;
        if(rsi_trend >  thresh) return +1;
        if(rsi_trend < -thresh) return -1;
        return 0;
    }
};

// =============================================================================
// Real L2 DOM exit tracker
// Uses l2_imb (actual book imbalance) and depth level changes.
// imb converted: (l2_imb - 0.5) * 2  ->  -1..+1
// Exit long when imbalance goes negative (sellers taking over) for N ticks.
// Exit short when imbalance goes positive (buyers taking over) for N ticks.
// Also exits on depth drop on supporting side.
// =============================================================================
struct L2DOMExit {
    // Smoothing: track consecutive ticks where signal is active
    int imb_against_count = 0;   // ticks imbalance is against our position
    int depth_drop_count  = 0;   // ticks depth dropped on support side
    int prev_depth_bid    = 0;
    int prev_depth_ask    = 0;
    double imb_ema        = 0.0; // EMA of imbalance (smoothed)
    bool first            = true;

    void reset() {
        imb_against_count=0; depth_drop_count=0; first=true; imb_ema=0;
    }

    // Returns true if DOM says exit
    bool should_exit(bool is_long, const L2Tick& t) {
        // Convert imbalance from 0..1 to -1..+1
        const double imb = (t.l2_imb - 0.5) * 2.0;

        // EMA smooth the imbalance
        if (first) { imb_ema=imb; first=false; }
        else imb_ema = imb;

        // Check imbalance direction against position
        // Long: exit if smoothed imb < -threshold (sellers dominant)
        // Short: exit if smoothed imb > +threshold (buyers dominant)
        const bool imb_against = is_long
            ? (imb_ema < -CFG_IMB_EXIT_THRESH)
            : (imb_ema >  CFG_IMB_EXIT_THRESH);

        if (imb_against) imb_against_count++;
        else             imb_against_count = 0;

        // Depth drop on support side
        const bool depth_support_drop = is_long
            ? (t.depth_bid < prev_depth_bid)
            : (t.depth_ask < prev_depth_ask);

        if (depth_support_drop) depth_drop_count++;
        else                    depth_drop_count = 0;

        prev_depth_bid = t.depth_bid;
        prev_depth_ask = t.depth_ask;

        // Exit if imbalance has been against us for N ticks
        // OR imbalance against + any depth drop
        const bool imb_exit   = (imb_against_count >= CFG_IMB_TICKS);
        const bool combo_exit = (imb_against_count >= 2 && depth_drop_count >= 1);

        return imb_exit || combo_exit;
    }
};

// =============================================================================
// Trade
// =============================================================================
struct Trade {
    bool is_long; double entry,exit_px,pnl_usd; int held_s; std::string reason;
};

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    std::vector<std::string> files;
    for (int i=1;i<argc;i++) files.push_back(argv[i]);
    if (files.empty()) {
        std::cerr << "Usage: candle_rsi_l2_bt l2_ticks_YYYY-MM-DD.csv ...\n";
        return 1;
    }

    M1Builder bars;
    ATR14     atr;
    TickRSI   rsi(CFG_RSI_PERIOD, CFG_RSI_SLOPE_EMA, CFG_RSI_TREND_THRESH);
    L2DOMExit dom_exit;

    std::vector<Trade> trades;
    std::map<std::string,int> reasons;

    bool    in_trade  = false;
    bool    is_long   = false;
    double  entry     = 0, sl = 0, size = 0, cost_pts = 0, mfe = 0;
    int64_t entry_ts  = 0, cooldown_until = 0;
    int64_t last_bmin = 0;
    int64_t tick_count= 0;

    // Diagnostics
    int blocked_spread=0, blocked_rsi=0, blocked_candle=0,
        blocked_cost=0, entries=0;

    for (auto& fname : files) {
        std::ifstream f(fname);
        if (!f) { std::cerr << "Cannot open: " << fname << "\n"; continue; }
        std::string line;
        std::getline(f, line); // header

        while (std::getline(f, line)) {
            // Strip Windows \r
            if (!line.empty() && line.back()=='\r') line.pop_back();
            L2Tick t;
            if (!parse_l2(line, t)) continue;
            if (t.watchdog_dead)    continue;

            const double mid    = (t.ask + t.bid) * 0.5;
            const double spread = t.ask - t.bid;
            if (spread > CFG_MAX_SPREAD || spread <= 0) { blocked_spread++; continue; }

            tick_count++;

            // RSI update (unconditional)
            rsi.update(mid);

            // Bar update
            bars.update(t.ts_ms, mid);
            const int64_t bm = t.ts_ms / 60000;
            if (bm != last_bmin && bars.last.valid) {
                atr.add(bars.last);
                last_bmin = bm;
            }

            // Cooldown
            if (t.ts_ms < cooldown_until) continue;

            // ── Manage open trade ──────────────────────────────────────────
            if (in_trade) {
                const double move = is_long ? (mid-entry) : (entry-mid);
                if (move > mfe) mfe = move;

                // Hard SL
                if (is_long ? (t.bid<=sl) : (t.ask>=sl)) {
                    const double px  = is_long ? t.bid : t.ask;
                    const double pnl = (is_long?(px-entry):(entry-px))*size*CFG_TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"SL_HIT"});
                    reasons["SL_HIT"]++;
                    in_trade=false; dom_exit.reset();
                    cooldown_until=t.ts_ms+CFG_COOLDOWN_MS; continue;
                }

                // Real L2 DOM exit
                if (dom_exit.should_exit(is_long, t)) {
                    const double px  = is_long ? t.bid : t.ask;
                    const double pnl = (is_long?(px-entry):(entry-px))*size*CFG_TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"DOM_EXIT"});
                    reasons["DOM_EXIT"]++;
                    in_trade=false; dom_exit.reset();
                    cooldown_until=t.ts_ms+5000; continue;
                }

                // Stagnation
                const int64_t held = t.ts_ms - entry_ts;
                if (held >= CFG_STAGNATION_MS && mfe < cost_pts*CFG_STAGNATION_MULT) {
                    const double px  = is_long ? t.bid : t.ask;
                    const double pnl = (is_long?(px-entry):(entry-px))*size*CFG_TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)(held/1000),"STAGNATION"});
                    reasons["STAGNATION"]++;
                    in_trade=false; dom_exit.reset();
                    cooldown_until=t.ts_ms+CFG_COOLDOWN_MS; continue;
                }
                continue;
            }

            // ── Entry ──────────────────────────────────────────────────────
            if (!bars.has2() || atr.val <= 0) continue;

            // Gate 1: RSI trend direction
            const int rsi_dir = rsi.direction();
            if (rsi_dir == 0) { blocked_rsi++; continue; }

            // Gate 2: expansion candle agreeing with RSI direction
            const Bar& lb = bars.last;
            const Bar& pb = bars.prev;
            const double range = lb.high - lb.low;
            if (range <= 0) continue;
            const double bb = lb.close - lb.open;
            const double br = lb.open  - lb.close;
            const bool bull = (bb>0 && bb/range>=CFG_BODY_RATIO_MIN && lb.close>pb.high);
            const bool bear = (br>0 && br/range>=CFG_BODY_RATIO_MIN && lb.close<pb.low);
            if (rsi_dir==+1 && !bull) { blocked_candle++; continue; }
            if (rsi_dir==-1 && !bear) { blocked_candle++; continue; }

            // Gate 3: cost coverage
            cost_pts = spread + CFG_COST_SLIP*2.0 + CFG_COST_COMM*2.0;
            if (range < CFG_COST_MULT * cost_pts) { blocked_cost++; continue; }

            // Enter
            is_long  = (rsi_dir==+1);
            entry    = is_long ? t.ask : t.bid;
            const double sl_pts = atr.val > 0 ? atr.val : 1.0;
            sl       = is_long ? entry-sl_pts : entry+sl_pts;
            size     = std::max(0.01, std::min(0.50, CFG_RISK_USD/(sl_pts*CFG_TICK_VALUE)));
            size     = std::floor(size/0.001)*0.001;
            entry_ts = t.ts_ms;
            mfe      = 0;
            in_trade = true;
            dom_exit.reset();
            entries++;
            reasons["ENTRY"]++;

            std::cout << "[ENTRY] " << (is_long?"LONG":"SHORT")
                      << " @ " << std::fixed << std::setprecision(2) << entry
                      << " sl=" << sl
                      << " rsi_trend=" << std::setprecision(3) << rsi.rsi_trend
                      << " imb=" << std::setprecision(4) << (t.l2_imb-0.5)*2.0
                      << " depth_bid=" << t.depth_bid
                      << " depth_ask=" << t.depth_ask << "\n";
        }
    }

    std::cout << "\n=== CandleFlow + RSI Trend + Real L2 DOM Exit ===\n";
    std::cout << "Architecture: RSI_slope -> direction | candle -> quality | L2_imb+depth -> exit\n";
    std::cout << std::string(55,'-') << "\n";
    std::cout << "Ticks processed : " << tick_count    << "\n";
    std::cout << "Entries         : " << entries        << "\n";

    if (trades.empty()) {
        std::cout << "No trades closed.\n";
        std::cout << "\nEntry gate diagnostics:\n";
        std::cout << "  Blocked spread  : " << blocked_spread  << "\n";
        std::cout << "  Blocked RSI=0   : " << blocked_rsi     << "\n";
        std::cout << "  Blocked candle  : " << blocked_candle  << "\n";
        std::cout << "  Blocked cost    : " << blocked_cost    << "\n";
        std::cout << "  RSI warmed      : " << (rsi.warmed?"yes":"no") << "\n";
        std::cout << "  RSI trend last  : " << rsi.rsi_trend   << "\n";
        return 0;
    }

    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for (auto& tr : trades) {
        total+=tr.pnl_usd; eq+=tr.pnl_usd;
        if(eq>peak)peak=eq;
        maxdd=std::max(maxdd,peak-eq);
        if(tr.pnl_usd>0){w++;wpnl+=tr.pnl_usd;}
        else{l++;lpnl+=tr.pnl_usd;}
    }
    const int n=(int)trades.size();
    const double wr=100.0*w/n;
    const double aw=w?wpnl/w:0, al=l?lpnl/l:0;
    const double rr=al!=0?-aw/al:0;
    const double exp_pt=(wr/100.0)*aw+(1.0-wr/100.0)*al;

    std::cout << "Total trades    : " << n    << "\n";
    std::cout << "Win rate        : " << std::fixed<<std::setprecision(1)<<wr<<"%\n";
    std::cout << "Total PnL       : $"<<std::setprecision(2)<<total<<"\n";
    std::cout << "Avg win         : $"<<aw<<"\n";
    std::cout << "Avg loss        : $"<<al<<"\n";
    std::cout << "Risk/Reward     : "<<std::setprecision(2)<<rr<<"\n";
    std::cout << "Expectancy/trade: $"<<exp_pt<<"\n";
    std::cout << "Max drawdown    : $"<<maxdd<<"\n";
    std::cout << "Exit reasons:\n";
    for (auto& kv:reasons)
        if(kv.first!="ENTRY")
            std::cout<<"  "<<std::left<<std::setw(16)<<kv.first<<kv.second<<"\n";
    std::cout << "\nEntry gate diagnostics:\n";
    std::cout << "  Blocked spread  : " << blocked_spread  << "\n";
    std::cout << "  Blocked RSI=0   : " << blocked_rsi     << "\n";
    std::cout << "  Blocked candle  : " << blocked_candle  << "\n";
    std::cout << "  Blocked cost    : " << blocked_cost    << "\n";
    std::cout << "Config:\n";
    std::cout << "  RSI period="<<CFG_RSI_PERIOD<<" ema="<<CFG_RSI_SLOPE_EMA<<" thresh="<<CFG_RSI_TREND_THRESH<<"\n";
    std::cout << "  IMB_EXIT_THRESH="<<CFG_IMB_EXIT_THRESH<<" IMB_TICKS="<<CFG_IMB_TICKS<<"\n";
    std::cout << "  COST_MULT="<<CFG_COST_MULT<<" STAGNATION_MS="<<CFG_STAGNATION_MS<<"\n";
    return 0;
}
