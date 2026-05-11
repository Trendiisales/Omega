// =====================================================================
// backtest/l2_micro_hunt.cpp -- Pass 3 (S33d 2026-05-11)
// ---------------------------------------------------------------------
// L2-specific microstructure signals — can only be tested against the
// 16-column L2 schema (which has l2_imb, l2_bid_vol, l2_ask_vol).
// Dukascopy 3-col format is not usable here.
//
// Available L2 data (Mac):
//   - XAUUSD: 31 days  (data/l2_ticks_XAUUSD_*.csv + legacy 2026-04-09..04-22)
//   - US500:  15 days  (BUT note: indices feed has FIX 264=0 = top-of-book
//                       only — l2_imb is zeroed by design per startup report)
//   - USTEC:  15 days  (same caveat as US500)
//   - NAS100:  3 days  (too short for stats)
//
// So real L2 testing only meaningful on XAU.
//
// Signal families tested (4 L2-specific):
//
//   18. L2_ImbAccel    -- second derivative of L2 imbalance over K ticks.
//                         Catches large incoming flow shifts.
//   19. L2_VolBurst    -- bid_vol+ask_vol spikes above N*median(prev W).
//                         Trade direction of imbalance at the burst.
//   20. L2_SpreadComp  -- microsecond Bollinger-squeeze on spread; trade
//                         direction of mid move post-compression.
//   21. L2_AskBidFlip  -- when l2_imb persists >0.7 for K ticks then
//                         crosses below 0.3 (sudden flip of book pressure)
//                         -> short. Mirror for long.
//
// Bracket: ATR(14) of price action over last 200 ticks (resampled to
// 1-minute mini-bars for ATR), SL = 1.5*ATR, TP = 3.0*ATR.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/l2_micro_hunt.cpp -o backtest/l2_micro_hunt
//
// Run:
//   backtest/l2_micro_hunt --csv 'data/l2_ticks_XAUUSD_*.csv' --sym XAUUSD \
//                          --out backtest/l2_micro_xau.csv
// =====================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if __has_include(<glob.h>)
# include <glob.h>
# define HAVE_GLOB 1
#endif

namespace u {
static std::string trim(std::string s){size_t a=0;while(a<s.size()&&std::isspace((unsigned char)s[a]))++a;size_t b=s.size();while(b>a&&std::isspace((unsigned char)s[b-1]))--b;return s.substr(a,b-a);}
static std::string lower(std::string s){for(auto&c:s)c=(char)std::tolower((unsigned char)c);return s;}
static std::vector<std::string> split(const std::string& l){std::vector<std::string> o;std::string c;for(char ch:l){if(ch==','){o.push_back(c);c.clear();}else c+=ch;}o.push_back(c);for(auto& f:o)f=trim(f);return o;}
static bool pd(const std::string& s,double& v){if(s.empty())return false;try{size_t p=0;v=std::stod(s,&p);return p>0;}catch(...){return false;}}
static bool pl(const std::string& s,long long& v){if(s.empty())return false;try{size_t p=0;v=std::stoll(s,&p);return p>0;}catch(...){return false;}}
static std::vector<std::string> glob_expand(const std::string& pat){std::vector<std::string> o;
#ifdef HAVE_GLOB
    glob_t g{}; if(glob(pat.c_str(),GLOB_TILDE,nullptr,&g)==0)for(size_t i=0;i<g.gl_pathc;++i)o.emplace_back(g.gl_pathv[i]);
    globfree(&g);
#endif
    if(o.empty())o.push_back(pat); return o;}
}

struct L2Tick {
    long long ts = 0;
    double bid = 0, ask = 0, mid = 0;
    double l2_imb = 0;       // [-1, +1] or [0, 1] depending on source
    double bid_vol = 0;
    double ask_vol = 0;
    bool l2_valid = false;
};

static std::vector<L2Tick> load_l2(const std::string& path, bool verbose){
    std::vector<L2Tick> out;
    std::ifstream fs(path); if(!fs) return out;
    std::string line; if(!std::getline(fs,line)) return out;
    auto hdr = u::split(line);
    int c_ts=-1, c_bid=-1, c_ask=-1, c_mid=-1, c_imb=-1, c_bv=-1, c_av=-1;
    bool ts_ms=false;
    for(size_t i=0;i<hdr.size();++i){
        std::string h = u::lower(hdr[i]);
        if(h=="ts_unix"||h=="ts"||h=="timestamp"||h=="time") c_ts=(int)i;
        if(h=="ts_ms"){c_ts=(int)i;ts_ms=true;}
        if(h=="bid") c_bid=(int)i;
        if(h=="ask") c_ask=(int)i;
        if(h=="mid") c_mid=(int)i;
        if(h=="l2_imb"||h=="imb"||h=="imbalance") c_imb=(int)i;
        if(h=="l2_bid_vol"||h=="bid_vol"||h=="bid_volume") c_bv=(int)i;
        if(h=="l2_ask_vol"||h=="ask_vol"||h=="ask_volume") c_av=(int)i;
    }
    if(c_ts<0||c_bid<0||c_ask<0){if(verbose) std::cerr<<"skip "<<path<<" (no bid/ask)\n"; return out;}
    while(std::getline(fs,line)){
        if(u::trim(line).empty()) continue;
        auto row = u::split(line);
        if((int)row.size() <= std::max({c_ts,c_bid,c_ask})) continue;
        L2Tick t; long long raw=0;
        if(!u::pl(row[c_ts], raw)) continue;
        t.ts = ts_ms ? raw/1000 : raw;
        if(!u::pd(row[c_bid],t.bid) || !u::pd(row[c_ask],t.ask)) continue;
        if(t.bid<=0||t.ask<=0) continue;
        if(c_mid >= 0 && (int)row.size() > c_mid) u::pd(row[c_mid], t.mid);
        if(t.mid<=0) t.mid = 0.5 * (t.bid + t.ask);
        if(c_imb >= 0 && (int)row.size() > c_imb){ u::pd(row[c_imb], t.l2_imb); t.l2_valid = true; }
        if(c_bv  >= 0 && (int)row.size() > c_bv) u::pd(row[c_bv], t.bid_vol);
        if(c_av  >= 0 && (int)row.size() > c_av) u::pd(row[c_av], t.ask_vol);
        out.push_back(t);
    }
    if(verbose) std::cerr<<"[l2] "<<path<<" ticks="<<out.size()<<" l2_valid="<<(out.empty()?0:out[0].l2_valid)<<"\n";
    return out;
}

// Mini-bar (1 minute) for ATR / bracket simulation
struct MiniBar {
    long long ts_open = 0;
    double bid_h = 0, bid_l = 0, bid_c = 0;
    double ask_h = 0, ask_l = 0, ask_c = 0;
    double mid_c() const { return 0.5 * (bid_c + ask_c); }
    double mid_h() const { return 0.5 * (bid_h + ask_h); }
    double mid_l() const { return 0.5 * (bid_l + ask_l); }
};

static std::vector<MiniBar> resample_1min(const std::vector<L2Tick>& ts){
    std::vector<MiniBar> out;
    if(ts.empty()) return out;
    int tf_sec = 60;
    long long cur = (ts.front().ts / tf_sec) * tf_sec;
    MiniBar b; b.ts_open=cur; bool st=false;
    for(const auto& t : ts){
        long long buck = (t.ts / tf_sec) * tf_sec;
        if(buck != cur){
            if(st) out.push_back(b);
            cur = buck; b = MiniBar{}; b.ts_open = cur; st = false;
        }
        if(!st){
            b.bid_h=b.bid_l=b.bid_c=t.bid; b.ask_h=b.ask_l=b.ask_c=t.ask; st=true;
        } else {
            if(t.bid>b.bid_h) b.bid_h=t.bid; if(t.bid<b.bid_l) b.bid_l=t.bid; b.bid_c=t.bid;
            if(t.ask>b.ask_h) b.ask_h=t.ask; if(t.ask<b.ask_l) b.ask_l=t.ask; b.ask_c=t.ask;
        }
    }
    if(st) out.push_back(b);
    return out;
}

// ATR over 1-min mini-bars (period 14)
static std::vector<double> compute_atr(const std::vector<MiniBar>& bars, int n=14){
    std::vector<double> atr(bars.size(),0);
    if((int)bars.size() <= n) return atr;
    double s=0;
    for(int i=1;i<=n;++i){
        double cp = bars[i-1].mid_c();
        double tr = std::max(bars[i].mid_h()-bars[i].mid_l(),
                             std::max(std::abs(bars[i].mid_h()-cp), std::abs(bars[i].mid_l()-cp)));
        s += tr;
    }
    atr[n] = s/n;
    for(int i=n+1;i<(int)bars.size();++i){
        double cp = bars[i-1].mid_c();
        double tr = std::max(bars[i].mid_h()-bars[i].mid_l(),
                             std::max(std::abs(bars[i].mid_h()-cp), std::abs(bars[i].mid_l()-cp)));
        atr[i] = (atr[i-1]*(n-1) + tr) / n;
    }
    return atr;
}

// Bracket simulator on mini-bars
struct Trade { long long entry_ts=0; int side=0; double pnl_gross=0; int bars=0; };
static const int MAX_HOLD_BARS = 60;     // ~1 hour at 1min bars
static const double LOT = 0.01;
static const double PT_SIZE_XAU = 0.01;
static const double COST = 0.06;

static Trade run_bracket(const std::vector<MiniBar>& bars, size_t i, int side,
                         double atr, double sl_mult, double tp_mult){
    Trade r{};
    if(i >= bars.size() || atr <= 0) return r;
    double ep = (side > 0) ? bars[i].ask_c : bars[i].bid_c;
    if(ep <= 0) return r;
    double sl_d = sl_mult * atr, tp_d = tp_mult * atr;
    double tp = side>0 ? ep+tp_d : ep-tp_d;
    double sl = side>0 ? ep-sl_d : ep+sl_d;
    r.entry_ts = bars[i].ts_open; r.side = side;
    size_t end = std::min(bars.size(), i + 1 + (size_t)MAX_HOLD_BARS);
    double xp = ep;
    for(size_t k=i+1; k<end; ++k){
        const auto& b = bars[k];
        if(side>0){
            if(b.bid_l <= sl){ xp=sl; r.bars=(int)(k-i); break; }
            if(b.bid_h >= tp){ xp=tp; r.bars=(int)(k-i); break; }
            xp = b.bid_c;
        } else {
            if(b.ask_h >= sl){ xp=sl; r.bars=(int)(k-i); break; }
            if(b.ask_l <= tp){ xp=tp; r.bars=(int)(k-i); break; }
            xp = b.ask_c;
        }
    }
    if(r.bars==0) r.bars = (int)(end-i-1);
    double pnl_pts = side>0 ? (xp-ep) : (ep-xp);
    r.pnl_gross = (pnl_pts / PT_SIZE_XAU) * 1.0 * LOT;
    return r;
}

// Find the mini-bar containing/just after a given tick timestamp
static size_t bar_idx_for_ts(const std::vector<MiniBar>& bars, long long ts){
    long long b = (ts / 60) * 60;
    // linear scan from a hint would be faster but corpus is small
    auto it = std::lower_bound(bars.begin(), bars.end(), b,
        [](const MiniBar& bb, long long v){ return bb.ts_open < v; });
    if(it == bars.end()) return bars.size();
    return (size_t)(it - bars.begin());
}

// =====================================================================
// L2-specific signal evaluators (operate on the tick stream, fire entries
// that are resolved against the 1-min mini-bar bracket sim)
// =====================================================================

struct CellTrades {
    std::string name;
    std::vector<Trade> trades;
};

// 18. L2 imbalance acceleration: d/dt of d(l2_imb)/dt over K ticks
static CellTrades sig_l2_imb_accel(const std::vector<L2Tick>& ts,
                                   const std::vector<MiniBar>& bars,
                                   const std::vector<double>& atr,
                                   int K, double thr,
                                   double sl_mult, double tp_mult,
                                   const std::string& name){
    CellTrades r; r.name = name;
    if((int)ts.size() < 3*K) return r;
    int cd_bars = 0;
    long long last_entry_ts = 0;
    for(int i = 2*K; i < (int)ts.size(); ++i){
        if(!ts[i].l2_valid) continue;
        // Numerical 2nd derivative across K-tick windows
        double imb_now  = ts[i].l2_imb;
        double imb_prev = ts[i-K].l2_imb;
        double imb_prev2= ts[i-2*K].l2_imb;
        double accel = (imb_now - 2*imb_prev + imb_prev2);
        int side = 0;
        if(accel >  thr) side = +1;
        if(accel < -thr) side = -1;
        if(side == 0) continue;
        size_t bi = bar_idx_for_ts(bars, ts[i].ts);
        if(bi >= bars.size() || (int)bi <= 14) continue;
        if((int64_t)(ts[i].ts - last_entry_ts) < 60) continue;  // 60s cooldown
        if(atr[bi] <= 0) continue;
        auto t = run_bracket(bars, bi, side, atr[bi], sl_mult, tp_mult);
        if(t.entry_ts == 0) continue;
        r.trades.push_back(t);
        last_entry_ts = ts[i].ts;
    }
    return r;
}

// 19. L2 volume burst: bid_vol+ask_vol spikes above N x median of W previous ticks
static CellTrades sig_l2_vol_burst(const std::vector<L2Tick>& ts,
                                   const std::vector<MiniBar>& bars,
                                   const std::vector<double>& atr,
                                   int W, double mult,
                                   double sl_mult, double tp_mult,
                                   const std::string& name){
    CellTrades r; r.name = name;
    if((int)ts.size() < W+1) return r;
    std::vector<double> vol(ts.size(), 0);
    for(int i=0;i<(int)ts.size();++i) vol[i] = ts[i].bid_vol + ts[i].ask_vol;
    long long last_entry_ts = 0;
    for(int i = W; i < (int)ts.size(); ++i){
        if(!ts[i].l2_valid) continue;
        // Rolling median: approximate with running sort of window slice (small W)
        std::vector<double> w(vol.begin()+i-W, vol.begin()+i);
        std::nth_element(w.begin(), w.begin()+w.size()/2, w.end());
        double med = w[w.size()/2];
        if(med < 1e-9) continue;
        if(vol[i] < mult * med) continue;
        // Trade in direction of l2_imb at burst
        int side = (ts[i].l2_imb > 0.5) ? +1 : (ts[i].l2_imb < -0.5 ? -1 : 0);
        if(side == 0) continue;
        size_t bi = bar_idx_for_ts(bars, ts[i].ts);
        if(bi >= bars.size() || (int)bi <= 14 || atr[bi] <= 0) continue;
        if((int64_t)(ts[i].ts - last_entry_ts) < 60) continue;
        auto t = run_bracket(bars, bi, side, atr[bi], sl_mult, tp_mult);
        if(t.entry_ts == 0) continue;
        r.trades.push_back(t);
        last_entry_ts = ts[i].ts;
    }
    return r;
}

// 20. L2 spread compression breakout
static CellTrades sig_l2_spread_comp(const std::vector<L2Tick>& ts,
                                     const std::vector<MiniBar>& bars,
                                     const std::vector<double>& atr,
                                     int W, double comp_pct,
                                     double sl_mult, double tp_mult,
                                     const std::string& name){
    CellTrades r; r.name = name;
    if((int)ts.size() < W+10) return r;
    long long last_entry_ts = 0;
    for(int i = W; i < (int)ts.size()-1; ++i){
        if((int64_t)(ts[i].ts - last_entry_ts) < 60) continue;
        // Compute trailing spread quantile
        std::vector<double> sp;
        sp.reserve(W);
        for(int k=i-W; k<i; ++k) sp.push_back(ts[k].ask - ts[k].bid);
        std::sort(sp.begin(), sp.end());
        double q = sp[(size_t)(comp_pct * sp.size())];
        double cur_sp = ts[i].ask - ts[i].bid;
        if(cur_sp > q) continue;  // not in compression
        // Trade direction of next-tick price move
        double next_mid = ts[i+1].mid;
        int side = (next_mid > ts[i].mid) ? +1 : (next_mid < ts[i].mid ? -1 : 0);
        if(side == 0) continue;
        size_t bi = bar_idx_for_ts(bars, ts[i].ts);
        if(bi >= bars.size() || (int)bi <= 14 || atr[bi] <= 0) continue;
        auto t = run_bracket(bars, bi, side, atr[bi], sl_mult, tp_mult);
        if(t.entry_ts == 0) continue;
        r.trades.push_back(t);
        last_entry_ts = ts[i].ts;
    }
    return r;
}

// 21. L2 ask-bid imbalance flip
static CellTrades sig_l2_flip(const std::vector<L2Tick>& ts,
                              const std::vector<MiniBar>& bars,
                              const std::vector<double>& atr,
                              int persist_k, double hi_thr, double lo_thr,
                              double sl_mult, double tp_mult,
                              const std::string& name){
    CellTrades r; r.name = name;
    if((int)ts.size() < persist_k+2) return r;
    long long last_entry_ts = 0;
    int run_up = 0, run_dn = 0;
    for(int i = 1; i < (int)ts.size(); ++i){
        if(!ts[i].l2_valid) continue;
        if(ts[i].l2_imb >  hi_thr) { run_up++; run_dn = 0; }
        else if(ts[i].l2_imb < -hi_thr) { run_dn++; run_up = 0; }
        else { run_up = 0; run_dn = 0; }
        int side = 0;
        // Persistent buy pressure (>hi_thr) then crosses below lo_thr -> short the flip
        if(run_up >= persist_k && ts[i].l2_imb < lo_thr) { side = -1; run_up = 0; }
        else if(run_dn >= persist_k && ts[i].l2_imb > -lo_thr) { side = +1; run_dn = 0; }
        if(side == 0) continue;
        if((int64_t)(ts[i].ts - last_entry_ts) < 60) continue;
        size_t bi = bar_idx_for_ts(bars, ts[i].ts);
        if(bi >= bars.size() || (int)bi <= 14 || atr[bi] <= 0) continue;
        auto t = run_bracket(bars, bi, side, atr[bi], sl_mult, tp_mult);
        if(t.entry_ts == 0) continue;
        r.trades.push_back(t);
        last_entry_ts = ts[i].ts;
    }
    return r;
}

static std::string ym_of(long long ts){std::time_t tt=(std::time_t)ts;std::tm tm{};gmtime_r(&tt,&tm);char b[12];std::strftime(b,sizeof(b),"%Y-%m",&tm);return b;}

int main(int argc, char** argv){
    std::vector<std::string> patterns;
    std::string out_path, sym;
    bool verbose = false;
    auto need=[&](int& i, const char* f)->const char*{if(i+1>=argc)std::exit(2);return argv[++i];};
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        if(a=="--csv") patterns.push_back(need(i,"--csv"));
        else if(a=="--sym") sym = need(i,"--sym");
        else if(a=="--out") out_path = need(i,"--out");
        else if(a=="--verbose") verbose = true;
    }
    if(patterns.empty() || sym.empty()){ std::cerr<<"usage: --sym XAUUSD --csv glob ...\n"; return 2; }

    // Load all L2 ticks
    std::vector<L2Tick> merged;
    for(auto& p : patterns) for(auto& f : u::glob_expand(p)){
        auto v = load_l2(f, verbose);
        merged.insert(merged.end(), v.begin(), v.end());
    }
    std::sort(merged.begin(), merged.end(),
              [](const L2Tick& a, const L2Tick& b){ return a.ts < b.ts; });
    if(merged.empty()){ std::cerr<<"no ticks\n"; return 1; }
    std::cerr<<"[l2_micro] sym="<<sym<<" total_ticks="<<merged.size()<<"\n";

    // Check L2 validity
    int l2_valid_count = 0;
    for(const auto& t : merged) if(t.l2_valid) ++l2_valid_count;
    std::cerr<<"[l2_micro] l2_valid_ticks="<<l2_valid_count<<"\n";
    if(l2_valid_count < 1000){
        std::cerr<<"[l2_micro] WARNING: too few L2-valid ticks; results unreliable\n";
    }

    auto bars = resample_1min(merged);
    auto atr = compute_atr(bars, 14);
    std::cerr<<"[l2_micro] 1m bars="<<bars.size()<<"\n";

    // Run all 4 families × 2 bracket geometries
    std::vector<CellTrades> all;
    struct Br { const char* lbl; double sl; double tp; };
    Br brackets[] = { {"sl1.5tp3.0",1.5,3.0}, {"sl2.0tp4.0",2.0,4.0} };

    for(auto& br : brackets){
        // L2 imbalance acceleration
        for(int K : {5, 20, 50}){
            for(double thr : {0.20, 0.40}){
                std::ostringstream nm; nm << "L2_ImbAccel_K=" << K << "_thr=" << thr << "_" << br.lbl;
                all.push_back(sig_l2_imb_accel(merged, bars, atr, K, thr, br.sl, br.tp, nm.str()));
            }
        }
        // L2 vol burst
        for(int W : {50, 200}){
            for(double mult : {2.0, 3.0}){
                std::ostringstream nm; nm << "L2_VolBurst_W=" << W << "_mult=" << mult << "_" << br.lbl;
                all.push_back(sig_l2_vol_burst(merged, bars, atr, W, mult, br.sl, br.tp, nm.str()));
            }
        }
        // L2 spread compression
        for(int W : {50, 200}){
            for(double pct : {0.20, 0.10}){
                std::ostringstream nm; nm << "L2_SpreadComp_W=" << W << "_pct=" << pct << "_" << br.lbl;
                all.push_back(sig_l2_spread_comp(merged, bars, atr, W, pct, br.sl, br.tp, nm.str()));
            }
        }
        // L2 flip
        for(int K : {5, 20}){
            std::ostringstream nm; nm << "L2_Flip_persist=" << K << "_" << br.lbl;
            all.push_back(sig_l2_flip(merged, bars, atr, K, 0.5, 0.2, br.sl, br.tp, nm.str()));
        }
    }

    // Tabulate
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== L2 MICROSTRUCTURE SIGNALS @ $0.06/RT cost ===\n";
    std::cout << std::left << std::setw(48) << "cell"
              << std::right << std::setw(6) << "n"
              << std::setw(6) << "wins"
              << std::setw(7) << "wr%"
              << std::setw(10) << "gross$"
              << std::setw(10) << "net$"
              << std::setw(7) << "BE$" << "\n";

    if(!out_path.empty()){
        std::ofstream out(out_path);
        out << "cell,n,wins,wr,gross,net,breakeven_cost\n";
        out << std::fixed << std::setprecision(4);
        for(const auto& c : all){
            int n = (int)c.trades.size();
            int wins = 0; double gross = 0;
            for(const auto& t : c.trades){
                gross += t.pnl_gross;
                if(t.pnl_gross - COST > 0) ++wins;
            }
            double net = gross - COST * n;
            double be = n > 0 ? gross / n : 0;
            out << c.name << "," << n << "," << wins << "," << (n? double(wins)/n:0)
                << "," << gross << "," << net << "," << be << "\n";
        }
    }

    // Sort by net for stdout summary
    std::vector<std::tuple<double,int,int,double,double,std::string>> rows;
    for(const auto& c : all){
        int n = (int)c.trades.size();
        int wins = 0; double gross = 0;
        for(const auto& t : c.trades){
            gross += t.pnl_gross;
            if(t.pnl_gross - COST > 0) ++wins;
        }
        double net = gross - COST * n;
        double be = n > 0 ? gross / n : 0;
        rows.emplace_back(net, n, wins, gross, be, c.name);
    }
    std::sort(rows.begin(), rows.end(), std::greater<>());
    for(auto& [net,n,wins,gross,be,name] : rows){
        if(n < 10) continue;
        std::cout << std::left << std::setw(48) << name
                  << std::right << std::setw(6) << n
                  << std::setw(6) << wins
                  << std::setw(7) << (n? 100.0*wins/n : 0)
                  << std::setw(10) << gross
                  << std::setw(10) << net
                  << std::setw(7) << be << "\n";
    }
    return 0;
}
