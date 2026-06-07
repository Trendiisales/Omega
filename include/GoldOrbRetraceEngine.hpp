#pragma once
// =============================================================================
// GoldOrbRetraceEngine.hpp -- Peachy GOLD ORB 50%-retrace CONTINUATION with a
// structural RUNNER exit. m5-native, self-contained (aggregates its own m5 bars
// from ticks; no external bar feed). SHADOW cell -- never touches the live order
// path; reports closed trades via on_trade_record -> handle_closed_trade.
//
// Origin: backtest/orb_gold_retrace.cpp (2026-06-06). Distilled from the Peachy
// "gold ORB" YouTube walkthroughs. Mechanizing her STRUCTURE (ORB level) works;
// her indicator/EMA-band content does NOT (raw PF ~1.0, her own "training wheels").
// The load-bearing pieces -- all matching her stated discipline:
//   * 08:20 ET 30-min ORB (COMEX gold open), DST-aware.
//   * breakout = m5 CLOSE beyond ORB -> day bias (two-sided: long above / short below).
//   * trend filter ("larger POV"): only trade WITH the EMA50-of-close direction.
//   * entry = retrace to RETR(=0.382) of the ORB range from the breakout extreme.
//   * tight stop = retrace reaction-bar extreme -/+ buf*ATR (loose ORB stop = dead).
//   * ONE trade/day (first retrace; re-entries dilute).
//   * RUNNER exit = structural trailing stop (prior TRWIN-bar low/high), NO fixed TP
//     -> captures the fat tail ("winners outpace losers by a ton"). This DOUBLED
//     the edge AND made it 3x-cost-robust vs a capped ORB-edge target.
//
// VALIDATED (2yr XAU m5, 2024-03..2026-04, @IBKR cost 0.37pt):
//   PF 2.38, avgR +1.20, WR 30%, both halves 2.56/2.18, all years 2.31/2.69/1.53,
//   3x-cost 1.12, plateau RETR0.382 x TRWIN{2,3,5}=2.45/2.38/2.34. CAVEAT: gold
//   2024-26 is bull-dominated (no sustained gold-bear tape) -> SHADOW, not live-size.
// =============================================================================
#include <string>
#include <deque>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "OmegaTradeLedger.hpp"

namespace omega {

// portable UTC time helpers (MSVC lacks gmtime_r / timegm)
static inline void gorb_gmtime(time_t t, struct tm* out) noexcept {
#ifdef _WIN32
    gmtime_s(out, &t);
#else
    gmtime_r(&t, out);
#endif
}
static inline time_t gorb_timegm(struct tm* v) noexcept {
#ifdef _WIN32
    return _mkgmtime(v);
#else
    return timegm(v);
#endif
}

struct GoldOrbRetraceEngine {
    bool   shadow_mode = true;
    bool   enabled     = false;

    // ---- params (validated config) ----
    int    or_start_et = 500;     // 08:20 ET (minute-of-day in ET) -- COMEX gold open
    int    or_end_et   = 530;     // 08:50 ET -- first 30 min (exclusive)
    int    flat_et     = 960;     // 16:00 ET -- force flat, no overnight
    int    atr_period  = 14;
    int    ema_len     = 50;      // 5m-close EMA trend filter ("larger POV")
    double retr        = 0.382;   // retrace depth from breakout extreme (0.382 best; 0.5/0.618 also +)
    int    trail_win   = 3;       // RUNNER: trail stop to prior trail_win-bar low/high
    double buf_atr     = 0.10;    // tight-stop + trail buffer (fraction of ATR)
    double max_spread  = 1.5;     // pts -- skip fill if spread too wide (gold)
    double lot         = 1.0;

    std::string symbol      = "XAUUSD";
    std::string engine_name = "GoldOrbRetrace";
    std::string tag         = "GOLDORB";   // log prefix (set per-instance, e.g. NASORB)
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;
    bool verbose = false;

    // ---- m5 aggregation state ----
    int64_t bar_start_ms_ = -1;
    double  bar_o_ = 0.0, bar_h_ = 0.0, bar_l_ = 0.0, bar_c_ = 0.0;

    // ---- ATR (SMA TR) + EMA(ema_len) of close + recent lows/highs for the trail ----
    std::deque<double> trq_;
    double atr_ = 0.0, prev_close_ = 0.0;
    double ema_ = 0.0; bool ema_init_ = false;
    std::deque<double> lows_, highs_;             // last bars' L/H for the structural trail

    // ---- per-day ORB / state ----
    int64_t cur_day_ = -1;
    double  or_high_ = 0.0, or_low_ = 1e18;
    bool    or_done_ = false, traded_ = false;
    int     bias_ = 0;                            // +1 broke high, -1 broke low, 0 none
    double  entry_lvl_ = 0.0;                     // armed retrace level

    // ---- open position ----
    struct OpenPos {
        bool   active = false; int side = 0;
        double entry = 0.0, sl = 0.0, lot = 0.0, sl_dist = 0.0, mfe = 0.0;
        int64_t entry_ts_ms = 0;
    } pos_;
    int trade_id_ = 0;
    bool has_open_position() const noexcept { return pos_.active; }

    static int64_t day_of(int64_t ms) noexcept { return (ms/1000LL)/86400LL; }

    // US Eastern offset (-4 EDT / -5 EST), DST 2nd-Sun-Mar .. 1st-Sun-Nov.
    static int et_off(int64_t ms) noexcept {
        time_t t = ms/1000LL; struct tm g; gorb_gmtime(t,&g);
        int y=g.tm_year+1900, mo=g.tm_mon+1, d=g.tm_mday;
        auto dow=[](int Y,int M,int D){ struct tm v{}; v.tm_year=Y-1900; v.tm_mon=M-1; v.tm_mday=D; v.tm_hour=12; time_t tt=gorb_timegm(&v); struct tm o; gorb_gmtime(tt,&o); return o.tm_wday; };
        int marSun=14; for(int dd=8;dd<=14;dd++) if(dow(y,3,dd)==0){marSun=dd;break;}
        int novSun=7;  for(int dd=1;dd<=7;dd++)  if(dow(y,11,dd)==0){novSun=dd;break;}
        bool dst; if(mo<3||mo>11) dst=false; else if(mo>3&&mo<11) dst=true; else if(mo==3) dst=(d>=marSun); else dst=(d<novSun);
        return dst?-4:-5;
    }
    static int et_minute_of_day(int64_t ms) noexcept {
        int64_t lt = ms/1000LL + (int64_t)et_off(ms)*3600LL;
        int s=(int)(((lt%86400LL)+86400LL)%86400LL); return s/60;
    }
    static int64_t et_day_of(int64_t ms) noexcept {
        int64_t lt = ms/1000LL + (int64_t)et_off(ms)*3600LL;
        return (lt - ((lt%86400LL)+86400LL)%86400LL)/86400LL;
    }

    void _close(double exit_px, const char* reason, int64_t now_ms) noexcept {
        const double pnl = pos_.side * (exit_px - pos_.entry) * pos_.lot;   // RAW pts*lot (ledger applies multiplier)
        omega::TradeRecord tr{};
        tr.symbol=symbol; tr.side=pos_.side>0?"LONG":"SHORT"; tr.engine=engine_name;
        tr.exitReason=reason; tr.entryPrice=pos_.entry; tr.exitPrice=exit_px;
        tr.sl=pos_.sl; tr.tp=0.0; tr.size=pos_.lot; tr.pnl=pnl;
        tr.entryTs=pos_.entry_ts_ms/1000LL; tr.exitTs=now_ms/1000LL;
        tr.mfe=pos_.mfe; tr.atr_at_entry=atr_; tr.shadow=shadow_mode;
        std::printf("[%s] CLOSE %s @ %.2f entry=%.2f pnl=%.2f %s%s\n", tag.c_str(),
                    tr.side.c_str(), exit_px, pos_.entry, pnl, reason, shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
        if (on_trade_record) on_trade_record(tr);
        pos_ = OpenPos{};
    }

    // ---- finalize one m5 bar: roll ATR/EMA/trail, build ORB, detect breakout + arm retrace ----
    void _on_m5_bar(double o, double h, double l, double c, int64_t bar_start_ms) noexcept {
        const int64_t day = et_day_of(bar_start_ms);
        const int     mod = et_minute_of_day(bar_start_ms);
        if (day != cur_day_) {
            cur_day_ = day; or_high_=0.0; or_low_=1e18; or_done_=false; traded_=false; bias_=0;
        }
        // ATR = SMA of TR
        if (prev_close_ > 0.0) {
            const double tr = std::max({h-l, std::fabs(h-prev_close_), std::fabs(l-prev_close_)});
            trq_.push_back(tr); while ((int)trq_.size()>atr_period) trq_.pop_front();
            double s=0; for(double v:trq_) s+=v; atr_=s/(double)trq_.size();
        }
        prev_close_ = c;
        // EMA(ema_len)
        if (!ema_init_) { ema_=c; ema_init_=true; } else { const double k=2.0/(ema_len+1); ema_+=k*(c-ema_); }
        // recent lows/highs for the structural trail
        lows_.push_back(l); highs_.push_back(h);
        while ((int)lows_.size()  > trail_win) lows_.pop_front();
        while ((int)highs_.size() > trail_win) highs_.pop_front();

        // WARM-SEED GUARD: during seed replay (enabled=false) warm ATR/EMA/trail only --
        // never open positions on historical bars (else phantom multi-month entries).
        if (!enabled) return;

        // accumulate ORB
        if (mod >= or_start_et && mod < or_end_et) { if(h>or_high_)or_high_=h; if(l<or_low_)or_low_=l; return; }
        if (mod >= or_end_et && !or_done_ && or_high_>0.0 && or_low_<1e18) or_done_=true;

        if (!or_done_ || traded_ || pos_.active) return;
        if (mod < or_end_et || mod >= flat_et) return;
        if (atr_ <= 0.0) return;
        const double range = or_high_ - or_low_; if (range <= 0.0) return;

        // detect breakout (first m5 close beyond ORB sets the day bias)
        if (bias_ == 0) {
            if (c > or_high_)      bias_ = +1;
            else if (c < or_low_)  bias_ = -1;
            else return;
            // trend filter ("larger POV"): bias must agree with EMA50 side
            if ((bias_>0 && !(c > ema_)) || (bias_<0 && !(c < ema_))) { bias_ = 0; return; }
            entry_lvl_ = bias_>0 ? (or_high_ - retr*range) : (or_low_ + retr*range);
            if (verbose) std::printf("[%s] BREAKOUT %s or[%.2f,%.2f] lvl=%.2f ema=%.2f\n", tag.c_str(),
                                     bias_>0?"UP":"DN", or_low_, or_high_, entry_lvl_, ema_);
            return;
        }

        // armed: first retrace bar that touches the level = enter (tight stop = this bar extreme)
        const bool touch = bias_>0 ? (l <= entry_lvl_) : (h >= entry_lvl_);
        if (!touch) return;
        const double sl = bias_>0 ? (l - buf_atr*atr_) : (h + buf_atr*atr_);
        const double risk = bias_>0 ? (entry_lvl_ - sl) : (sl - entry_lvl_);
        if (risk <= 0.05*atr_) { traded_=true; return; }
        pos_.active=true; pos_.side=bias_; pos_.entry=entry_lvl_; pos_.sl=sl;
        pos_.lot=lot; pos_.sl_dist=risk; pos_.mfe=0.0; pos_.entry_ts_ms=bar_start_ms;
        traded_=true; ++trade_id_;
        std::printf("[%s] ENTRY %s @ %.2f sl=%.2f risk=%.2f atr=%.2f%s\n", tag.c_str(),
                    bias_>0?"LONG":"SHORT", pos_.entry, pos_.sl, risk, atr_, shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
    }

    // ---- tick: aggregate m5, manage open position (RUNNER trail) ----
    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;
        const int64_t b5 = (now_ms/300000LL)*300000LL;
        if (bar_start_ms_ < 0) { bar_start_ms_=b5; bar_o_=bar_h_=bar_l_=bar_c_=mid; }
        else if (b5 != bar_start_ms_) { _on_m5_bar(bar_o_,bar_h_,bar_l_,bar_c_,bar_start_ms_); bar_start_ms_=b5; bar_o_=bar_h_=bar_l_=bar_c_=mid; }
        else { if(mid>bar_h_)bar_h_=mid; if(mid<bar_l_)bar_l_=mid; bar_c_=mid; }

        const int mod = et_minute_of_day(now_ms);
        if (!pos_.active) return;

        const double move = pos_.side * (mid - pos_.entry);
        if (move > pos_.mfe) pos_.mfe = move;

        // RUNNER: trail stop to the prior trail_win-bar structure (only tightens)
        if (pos_.side > 0) {
            double sl=1e18; for(double v:lows_) sl=std::min(sl,v); sl -= buf_atr*atr_;
            if (sl > pos_.sl) pos_.sl = sl;
            if (bid <= pos_.sl) { _close(pos_.sl, "TRAIL_STOP", now_ms); return; }
        } else {
            double sh=-1e18; for(double v:highs_) sh=std::max(sh,v); sh += buf_atr*atr_;
            if (sh < pos_.sl) pos_.sl = sh;
            if (ask >= pos_.sl) { _close(pos_.sl, "TRAIL_STOP", now_ms); return; }
        }
        if (mod >= flat_et) { _close(mid, "FLAT_EOD", now_ms); return; }
    }

    // ---- warm-seed: replay m5 bars (ts[ms|s],o,h,l,c[,v]) ----
    int seed_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[%s] SEED FAIL '%s'\n", tag.c_str(), path.c_str()); return 0; }
        const bool was = enabled; enabled=false;
        std::string line; std::getline(f,line);
        int fed=0;
        while (std::getline(f,line)) {
            if (line.empty() || line[0]=='t' || line[0]=='b') continue;
            std::stringstream ss(line); std::string a,o,h,l,c;
            std::getline(ss,a,','); std::getline(ss,o,','); std::getline(ss,h,',');
            std::getline(ss,l,','); std::getline(ss,c,',');
            if (o.empty()||h.empty()||l.empty()||c.empty()) continue;
            double op=std::strtod(o.c_str(),nullptr),hi=std::strtod(h.c_str(),nullptr),
                   lo=std::strtod(l.c_str(),nullptr),cl=std::strtod(c.c_str(),nullptr);
            int64_t ms=std::strtoll(a.c_str(),nullptr,10); if(ms<100000000000LL) ms*=1000LL;
            if (hi<=0||lo<=0||cl<=0) continue;
            _on_m5_bar(op,hi,lo,cl,ms); ++fed;
        }
        enabled=was;
        std::printf("[%s] SEED fed=%d atr=%.3f ema=%.2f or_done=%d or[%.2f,%.2f]\n", tag.c_str(),
                    fed, atr_, ema_, (int)or_done_, or_low_, or_high_);
        std::fflush(stdout);
        return fed;
    }
};

} // namespace omega
