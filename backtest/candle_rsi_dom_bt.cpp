// candle_rsi_dom_bt.cpp
//
// Architecture (per the RSI+DOM document):
//   ENTRY:  expansion candle (quality gate) + RSI trend direction (slow signal)
//   EXIT:   DOM reversal score >= threshold (fast signal, early warning)
//
// RSI here is tick-RSI: rolling 10-tick RSI delta (slope).
// RSI_trend = EMA(5) of RSI slope. Positive = up trend. Negative = down trend.
// DOM is simulated from price velocity (same as candle_flow_bt.cpp) since
// we only have the 24-month tick CSV, not real L2 logs.
//
// Build: g++ -O2 -std=c++17 -o candle_rsi_dom_bt candle_rsi_dom_bt.cpp
// Run:   ./candle_rsi_dom_bt ~/tick/xauusd_merged_24months.csv

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
#include <unordered_map>

// =============================================================================
// Config -- all tunable params in one place
// =============================================================================

// Candle gate
static constexpr double CFG_BODY_RATIO_MIN   = 0.60;   // body >= 60% of range
static constexpr double CFG_COST_SLIP        = 0.10;   // slippage per side pts
static constexpr double CFG_COST_COMM        = 0.10;   // commission per side pts
static constexpr double CFG_COST_MULT        = 2.0;    // range >= N * total_cost
static constexpr double CFG_MAX_SPREAD       = 0.50;   // skip wide-spread ticks

// RSI trend (entry direction signal)
static constexpr int    CFG_RSI_PERIOD       = 10;     // RSI lookback ticks
static constexpr int    CFG_RSI_SLOPE_EMA    = 5;      // slope smoothing window
static constexpr double CFG_RSI_TREND_THRESH = 0.05;   // min smoothed slope to enter

// DOM exit (early warning)
static constexpr int    CFG_DOM_EXIT_MIN     = 2;      // exit on 2-of-4 signals
static constexpr double CFG_DOM_SMOOTH_MS    = 200.0;  // DOM smoothing window ms
                                                        // (how long a signal must persist)

// Trade management
static constexpr int64_t CFG_STAGNATION_MS   = 60000;  // 60s stagnation window
static constexpr double  CFG_STAGNATION_MULT = 1.5;    // exit if mfe < cost*N
static constexpr double  CFG_RISK_USD        = 30.0;
static constexpr double  CFG_TICK_VALUE      = 100.0;  // XAUUSD $100/pt/lot
static constexpr int64_t CFG_COOLDOWN_MS     = 15000;  // 15s after any exit

// =============================================================================
// Tick parser
// =============================================================================
struct Tick { uint64_t ts=0; double ask=0, bid=0; };
bool parse_tick(const std::string& line, Tick& t) {
    if (line.empty()) return false;
    std::stringstream ss(line); std::string tok;
    if (!std::getline(ss,tok,',')) return false;
    if (tok.empty()||!isdigit((unsigned char)tok[0])) return false;
    try { t.ts=std::stoull(tok); } catch(...) { return false; }
    if (!std::getline(ss,tok,',')) return false;
    try { t.ask=std::stod(tok); } catch(...) { return false; }
    if (!std::getline(ss,tok,',')) return false;
    try { t.bid=std::stod(tok); } catch(...) { return false; }
    return (t.ask>0 && t.bid>0 && t.ask>=t.bid);
}

// =============================================================================
// M1 bar builder
// =============================================================================
struct Bar { double open=0,high=0,low=0,close=0; uint64_t ts_open=0; bool valid=false; };
struct M1Builder {
    Bar cur, last, prev;
    uint64_t cur_min=0;
    void update(uint64_t ts_ms, double mid) {
        const uint64_t bm = ts_ms/60000;
        if (bm != cur_min) {
            if (cur.valid) { prev=last; last=cur; }
            cur={mid,mid,mid,mid,ts_ms,true}; cur_min=bm;
        } else {
            if (mid>cur.high) cur.high=mid;
            if (mid<cur.low)  cur.low=mid;
            cur.close=mid;
        }
    }
    bool has2() const { return last.valid && prev.valid; }
};

// =============================================================================
// ATR-14
// =============================================================================
struct ATR14 {
    std::deque<double> r; double val=0;
    void add(const Bar& b) {
        r.push_back(b.high-b.low);
        if ((int)r.size()>14) r.pop_front();
        double s=0; for(auto x:r) s+=x; val=s/r.size();
    }
};

// =============================================================================
// RSI (tick-level, rolling N ticks)
// Computes RSI from mid-price changes tick by tick.
// Also computes smoothed RSI slope (trend direction signal).
// =============================================================================
struct TickRSI {
    int period;
    int slope_ema_n;
    double rsi_thresh;

    std::deque<double> gains, losses;   // per-tick gain/loss
    double prev_mid   = 0.0;
    double rsi_prev   = 50.0;
    double rsi_cur    = 50.0;
    double rsi_slope  = 0.0;           // raw slope = rsi_cur - rsi_prev
    double rsi_trend  = 0.0;           // EMA of slope (smoothed direction)
    bool   warmed_up  = false;
    int    tick_count = 0;

    double ema_alpha;

    explicit TickRSI(int p=10, int se=5, double thresh=0.05)
        : period(p), slope_ema_n(se), rsi_thresh(thresh)
    {
        ema_alpha = 2.0 / (slope_ema_n + 1);
    }

    void update(double mid) {
        if (prev_mid == 0.0) { prev_mid=mid; return; }
        const double chg = mid - prev_mid;
        prev_mid = mid;
        tick_count++;

        const double g = chg > 0 ? chg : 0.0;
        const double l = chg < 0 ? -chg : 0.0;
        gains.push_back(g);
        losses.push_back(l);
        if ((int)gains.size() > period) { gains.pop_front(); losses.pop_front(); }
        if ((int)gains.size() < period) return;

        double ag=0, al=0;
        for (auto x: gains)  ag+=x;
        for (auto x: losses) al+=x;
        ag/=period; al/=period;

        rsi_prev = rsi_cur;
        rsi_cur  = (al==0.0) ? 100.0 : 100.0 - 100.0/(1.0 + ag/al);

        // Raw slope
        rsi_slope = rsi_cur - rsi_prev;

        // EMA of slope (smoothed trend direction)
        // First tick: seed EMA with raw slope
        if (!warmed_up) { rsi_trend=rsi_slope; warmed_up=true; }
        else             rsi_trend = rsi_slope*ema_alpha + rsi_trend*(1.0-ema_alpha);
    }

    // Returns +1 (UP trend), -1 (DOWN trend), 0 (neutral)
    int trend_direction() const {
        if (!warmed_up) return 0;
        if  (rsi_trend >  rsi_thresh) return +1;
        if  (rsi_trend < -rsi_thresh) return -1;
        return 0;
    }
};

// =============================================================================
// DOM simulator (from tick price velocity, same as candle_flow_bt)
// bid_count/ask_count = proxy for liquidity level count from tick data.
// Smoothed via a short window to avoid reacting to single-tick flickers.
// =============================================================================
struct DOMSim {
    // Current and previous state
    int bid_count=3, ask_count=3;
    int prev_bid=3,  prev_ask=3;
    double bid_vol=3.0, ask_vol=3.0;
    double prev_bid_vol=3.0, prev_ask_vol=3.0;
    bool vacuum_ask=false, vacuum_bid=false;
    bool wall_above=false, wall_below=false;

    // Smoothing: track how long each signal has been active
    // Signal fires for exit only if it persists >= CFG_DOM_SMOOTH_MS
    struct SigTimer { uint64_t first_ts=0; bool active=false; };
    SigTimer sig_bid_drop, sig_ask_surge,
             sig_wall_above, sig_wall_below,
             sig_bids_shrink_asks_stack,
             sig_vacuum_bid, sig_vacuum_ask;

    void update(double prev_mid, double cur_mid, double spread, uint64_t ts_ms) {
        prev_bid = bid_count; prev_ask = ask_count;
        prev_bid_vol = bid_vol; prev_ask_vol = ask_vol;

        // Price direction -> DOM state proxy
        if (cur_mid > prev_mid + spread*0.3) {
            // Rising: buyers consuming offers
            bid_count=4; ask_count=1;
            bid_vol=4.0; ask_vol=1.0;
            vacuum_ask=true;  vacuum_bid=false;
            wall_above=false; wall_below=false;
        } else if (cur_mid < prev_mid - spread*0.3) {
            // Falling: sellers consuming bids
            bid_count=1; ask_count=4;
            bid_vol=1.0; ask_vol=4.0;
            vacuum_bid=true;  vacuum_ask=false;
            wall_above=false; wall_below=false;
        } else {
            // Flat: balanced
            bid_count=3; ask_count=3;
            bid_vol=3.0; ask_vol=3.0;
            vacuum_ask=false; vacuum_bid=false;
            wall_above=false; wall_below=false;
        }

        // Update smoothing timers for each signal
        // A signal activates its timer on first fire, resets when it stops
        auto tick_sig = [&](SigTimer& s, bool active_now) {
            if (active_now) {
                if (!s.active) { s.first_ts=ts_ms; s.active=true; }
            } else {
                s.active=false; s.first_ts=0;
            }
        };

        tick_sig(sig_bid_drop,               bid_vol  < prev_bid_vol  * 0.7);
        tick_sig(sig_ask_surge,              ask_vol  > prev_ask_vol  * 1.3);
        tick_sig(sig_wall_above,             wall_above);
        tick_sig(sig_wall_below,             wall_below);
        tick_sig(sig_bids_shrink_asks_stack, bid_count < prev_bid && ask_count > prev_ask);
        tick_sig(sig_vacuum_bid,             vacuum_bid && !vacuum_ask);
        tick_sig(sig_vacuum_ask,             vacuum_ask && !vacuum_bid);
    }

    // A signal counts for exit only if it has been continuously active for >= smooth_ms
    bool sig_sustained(const SigTimer& s, uint64_t now_ms) const {
        return s.active && (now_ms - s.first_ts) >= (uint64_t)CFG_DOM_SMOOTH_MS;
    }

    // Exit score: count how many of 4 reversal signals have sustained >= smooth_ms
    int exit_score(bool is_long, uint64_t now_ms) const {
        int score=0;
        if (is_long) {
            // Rule 1: bid vol dropped OR ask vol surged
            if (sig_sustained(sig_bid_drop,  now_ms) ||
                sig_sustained(sig_ask_surge, now_ms)) score++;
            // Rule 2: sell wall appeared above price
            if (sig_sustained(sig_wall_above, now_ms)) score++;
            // Rule 3: bids shrinking + asks stacking
            if (sig_sustained(sig_bids_shrink_asks_stack, now_ms)) score++;
            // Rule 4: bid vacuum (support bids disappeared)
            if (sig_sustained(sig_vacuum_bid, now_ms)) score++;
        } else {
            // Rule 1: ask vol dropped OR bid vol surged
            if (sig_sustained(sig_ask_surge, now_ms) ||
                sig_sustained(sig_bid_drop,  now_ms)) score++;
            // Rule 2: buy wall appeared below price
            if (sig_sustained(sig_wall_below, now_ms)) score++;
            // Rule 3: asks shrinking + bids stacking (reverse of above)
            // (bids_shrink_asks_stack signal fires when bids<prev AND asks>prev)
            // For short: asks shrink + bids stack = opposite. Use vacuum_ask instead.
            if (sig_sustained(sig_vacuum_ask, now_ms)) score++;
            // Rule 4: ask vacuum (resistance offers disappeared = shorts at risk)
            if (sig_sustained(sig_vacuum_ask, now_ms)) score++;
        }
        return score;
    }
};

// =============================================================================
// Trade record
// =============================================================================
struct Trade {
    bool   is_long;
    double entry, exit_px, pnl_usd;
    int    held_s;
    std::string reason;
};

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    const char* infile = argc>1 ? argv[1] : "/Users/jo/tick/xauusd_merged_24months.csv";
    std::ifstream f(infile);
    if (!f) { std::cerr << "Cannot open: " << infile << "\n"; return 1; }

    M1Builder bars;
    ATR14     atr;
    TickRSI   rsi(CFG_RSI_PERIOD, CFG_RSI_SLOPE_EMA, CFG_RSI_TREND_THRESH);
    DOMSim    dom;

    std::vector<Trade> trades;
    std::map<std::string,int> reasons;

    // Trade state
    bool    in_trade   = false;
    bool    is_long    = false;
    double  entry      = 0.0;
    double  sl         = 0.0;
    double  size       = 0.0;
    double  cost_pts   = 0.0;
    int64_t entry_ts   = 0;
    double  mfe        = 0.0;
    int64_t cooldown_until = 0;

    uint64_t tick_count    = 0;
    uint64_t last_bar_min  = 0;
    double   prev_mid      = 0.0;

    std::string line;
    std::getline(f, line); // header

    while (std::getline(f, line)) {
        Tick t;
        if (!parse_tick(line, t)) continue;
        tick_count++;

        const double mid    = (t.ask + t.bid) * 0.5;
        const double spread = t.ask - t.bid;
        if (spread > CFG_MAX_SPREAD || spread <= 0) { prev_mid=mid; continue; }

        // Update RSI (runs on every tick, unconditionally)
        rsi.update(mid);

        // Update DOM (simulated from price velocity)
        dom.update(prev_mid, mid, spread, t.ts);
        prev_mid = mid;

        // Build M1 bar
        bars.update(t.ts, mid);
        const uint64_t bm = t.ts / 60000;
        if (bm != last_bar_min && bars.last.valid) {
            atr.add(bars.last);
            last_bar_min = bm;
        }

        // Cooldown
        if ((int64_t)t.ts < cooldown_until) continue;

        // ── Manage open trade ────────────────────────────────────────────────
        if (in_trade) {
            const double move = is_long ? (mid-entry) : (entry-mid);
            if (move > mfe) mfe = move;

            // Hard SL
            const bool sl_hit = is_long ? (t.bid <= sl) : (t.ask >= sl);
            if (sl_hit) {
                const double px = is_long ? t.bid : t.ask;
                const double pnl = (is_long?(px-entry):(entry-px))*size*CFG_TICK_VALUE;
                trades.push_back({is_long,entry,px,pnl,(int)((t.ts-entry_ts)/1000),"SL_HIT"});
                reasons["SL_HIT"]++;
                in_trade=false; cooldown_until=(int64_t)t.ts+CFG_COOLDOWN_MS; continue;
            }

            // DOM reversal exit (smoothed -- main exit mechanism)
            const int escore = dom.exit_score(is_long, t.ts);
            if (escore >= CFG_DOM_EXIT_MIN) {
                const double px = is_long ? t.bid : t.ask;
                const double pnl = (is_long?(px-entry):(entry-px))*size*CFG_TICK_VALUE;
                trades.push_back({is_long,entry,px,pnl,(int)((t.ts-entry_ts)/1000),"DOM_EXIT"});
                reasons["DOM_EXIT"]++;
                in_trade=false; cooldown_until=(int64_t)t.ts+5000; continue; // short cooldown after DOM exit
            }

            // Stagnation safety exit
            const int64_t held = (int64_t)(t.ts - entry_ts);
            if (held >= CFG_STAGNATION_MS && mfe < cost_pts * CFG_STAGNATION_MULT) {
                const double px = is_long ? t.bid : t.ask;
                const double pnl = (is_long?(px-entry):(entry-px))*size*CFG_TICK_VALUE;
                trades.push_back({is_long,entry,px,pnl,(int)(held/1000),"STAGNATION"});
                reasons["STAGNATION"]++;
                in_trade=false; cooldown_until=(int64_t)t.ts+CFG_COOLDOWN_MS; continue;
            }
            continue;
        }

        // ── Entry check ──────────────────────────────────────────────────────
        if (!bars.has2() || atr.val <= 0.0) continue;
        if (!rsi.warmed_up) continue;

        // Gate 1: RSI trend direction (replaces DOM entry confirmation)
        // This is the primary entry signal. DOM is NOT used for entry.
        const int rsi_dir = rsi.trend_direction();
        if (rsi_dir == 0) continue;  // no clear RSI trend

        // Gate 2: Expansion candle in the direction of RSI trend
        // Candle must agree with RSI: bullish candle for up trend, bearish for down
        const Bar& lb = bars.last;
        const Bar& pb = bars.prev;
        const double range = lb.high - lb.low;
        if (range <= 0.0) continue;

        const double body_bull = lb.close - lb.open;
        const double body_bear = lb.open  - lb.close;
        const bool bullish = (body_bull > 0
                              && body_bull/range >= CFG_BODY_RATIO_MIN
                              && lb.close > pb.high);
        const bool bearish = (body_bear > 0
                              && body_bear/range >= CFG_BODY_RATIO_MIN
                              && lb.close < pb.low);

        // RSI direction must agree with candle direction
        if (rsi_dir == +1 && !bullish) continue;
        if (rsi_dir == -1 && !bearish) continue;

        // Gate 3: Cost coverage
        cost_pts = spread + CFG_COST_SLIP*2.0 + CFG_COST_COMM*2.0;
        if (range < CFG_COST_MULT * cost_pts) continue;

        // All gates passed: enter in RSI trend direction
        is_long = (rsi_dir == +1);
        entry   = is_long ? t.ask : t.bid;
        const double sl_pts = atr.val > 0.0 ? atr.val : 5.0;
        sl      = is_long ? entry - sl_pts : entry + sl_pts;
        size    = std::max(0.01, std::min(0.50, CFG_RISK_USD / (sl_pts * CFG_TICK_VALUE)));
        size    = std::floor(size / 0.001) * 0.001;
        entry_ts = (int64_t)t.ts;
        mfe      = 0.0;
        in_trade = true;
        reasons["ENTRY"]++;
    }

    if (trades.empty()) {
        std::cout << "No trades generated.\n";
        std::cout << "Ticks processed: " << tick_count << "\n";
        std::cout << "RSI warmed: " << (rsi.warmed_up?"yes":"no")
                  << " rsi_trend=" << rsi.rsi_trend
                  << " entries_blocked=" << "check threshold\n";
        return 0;
    }

    // ==========================================================================
    // Stats
    // ==========================================================================
    double total=0, wpnl=0, lpnl=0, peak=0, eq=0, maxdd=0;
    int w=0, l=0;
    std::map<std::string,int> exit_reasons;

    for (auto& tr : trades) {
        total += tr.pnl_usd; eq += tr.pnl_usd;
        if (eq > peak) peak = eq;
        maxdd = std::max(maxdd, peak - eq);
        if (tr.pnl_usd > 0) { w++; wpnl += tr.pnl_usd; }
        else                  { l++; lpnl += tr.pnl_usd; }
        exit_reasons[tr.reason]++;
    }
    const int n = (int)trades.size();
    const double wr   = 100.0 * w / n;
    const double aw   = w ? wpnl/w : 0;
    const double al   = l ? lpnl/l : 0;
    const double rr   = al != 0 ? -aw/al : 0;
    const double exp_ = (wr/100.0)*aw + (1.0-wr/100.0)*al; // expectancy per trade

    std::cout << "\n=== CandleFlow + RSI Trend + DOM Exit Backtest ===\n";
    std::cout << "Architecture: RSI_slope_EMA -> direction | candle -> quality | DOM -> exit\n";
    std::cout << std::string(52, '-') << "\n";
    std::cout << "Ticks processed : " << tick_count    << "\n";
    std::cout << "Total trades    : " << n             << "\n";
    std::cout << "Win rate        : " << std::fixed << std::setprecision(1) << wr << "%\n";
    std::cout << "Total PnL       : $" << std::setprecision(2) << total << "\n";
    std::cout << "Avg win         : $" << aw << "\n";
    std::cout << "Avg loss        : $" << al << "\n";
    std::cout << "Risk/Reward     : " << std::setprecision(2) << rr << "\n";
    std::cout << "Expectancy/trade: $" << exp_ << "\n";
    std::cout << "Max drawdown    : $" << maxdd << "\n";
    std::cout << "Exit reasons:\n";
    for (auto& kv : exit_reasons)
        std::cout << "  " << std::left << std::setw(18) << kv.first << kv.second << "\n";
    std::cout << "\nConfig:\n";
    std::cout << "  RSI period=" << CFG_RSI_PERIOD
              << " slope_ema=" << CFG_RSI_SLOPE_EMA
              << " thresh=" << CFG_RSI_TREND_THRESH << "\n";
    std::cout << "  DOM_EXIT_MIN=" << CFG_DOM_EXIT_MIN
              << " smooth_ms=" << CFG_DOM_SMOOTH_MS << "\n";
    std::cout << "  BODY_RATIO=" << CFG_BODY_RATIO_MIN
              << " COST_MULT=" << CFG_COST_MULT << "\n";
    std::cout << "  STAGNATION_MS=" << CFG_STAGNATION_MS << "\n";

    return 0;
}
