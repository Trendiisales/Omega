#pragma once
// =============================================================================
//  AdaptiveTfGoldEngine.hpp -- DYNAMIC-TIMEFRAME gold engine (XAUUSD). 2026-06-21.
//
//  WHAT IT IS: a single regime classifier that selects an operating MODE +
//  horizon + stop-scale each base bar -- the "dynamic timeframe" the operator
//  asked for. It does NOT switch charts; it switches the active lookback/hold/
//  stop SET, which is what a timeframe change really is.
//
//  CLASSIFIER (from XAUUSD ticks only -- no extra feed):
//    * Kaufman Efficiency Ratio (ER) over ER_N base bars  -> trendiness 0..1
//    * ATR ratio (fast/slow)                              -> vol expand/compress
//    * tick-count per bar (CFD volume PROXY)              -> participation
//  STATES:
//    TREND : ER>=ER_TREND & vol expanding -> acts like 1h-4h: Donchian(DONCH_HI)
//            breakout, ride with a WIDE ATR trail. (where gold's real edge lives:
//            cf. XauTrendFollow1h / vol-Donchian.)
//    RANGE : ER<=ER_RANGE & vol normal    -> acts like 5-15m: fade band extreme
//            (mid +/- BAND_ATR), quick fixed bracket + timeout.
//    CHOP  : neither (transition)         -> FLAT, stand aside (chop-kill lesson).
//
//  HONEST NOTE: the TREND (high-TF) leg is the part with a proven gold edge; the
//  RANGE (5-15m) leg fights the spot-CFD cost wall ([[omega-intraday-spot-cfd-
//  cost-wall]] / [[omega-gold-intraday-breakout-deadend]]). EVERY entry is gated
//  by ExecutionCostGuard, so non-viable low-TF fades are blocked at fire time --
//  the engine attempts the dynamic switch, the cost gate enforces reality. SHADOW
//  by default; the live shadow ledger reveals which states actually pay.
//
//  ADVERSE-PROTECTION: TREND = structural ATR stop + wide ATR trail (no cold cut
//  -- backtested: tightening hurts trend/trail engines, swing-protection sweep).
//  RANGE = fixed bracket (TP=mid, hard SL) + hard bar TIMEOUT. Verdict: gate +
//  per-mode bracket/trail is the adverse control, not a global loss-cut.
//
//  DESIGN: self-aggregates XAUUSD ticks into BASE_TF_MS bars, runs the classifier
//  + manage on each base-bar close. One position at a time. Portable, MSVC-safe.
//  Warm-seed via seed_from_bar_csv (ts,o,h,l,c). Cost gate on every entry.
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

struct AdaptiveTfParams {
    int64_t base_tf_ms = 300000;   // 5-min base bar
    int    er_n        = 20;       // efficiency-ratio lookback (base bars)
    int    atr_fast    = 14;
    int    atr_slow    = 100;
    int    donch_hi    = 48;       // ~4h of 5m -> the "high-TF" trend channel
    int    range_sma   = 20;       // mean-rev band centre
    double er_trend    = 0.35;     // >= -> TREND
    double er_range    = 0.15;     // <= -> RANGE
    double vol_hi      = 1.10;     // ATR ratio >= -> expanding (TREND confirm)
    double vol_norm_hi = 1.30;     // RANGE wants vol <= this and >= vol_norm_lo
    double vol_norm_lo = 0.60;
    double sl_trend_atr= 2.5;      // TREND structural stop (x atr_fast)
    double trail_atr   = 3.0;      // TREND trail distance
    double band_atr    = 1.5;      // RANGE entry band (x atr_fast from mid)
    double range_sl_atr= 1.8;      // RANGE hard stop
    int    range_hold  = 12;       // RANGE bar timeout (~1h of 5m)
    double target_vol_bps = 50.0;  // vol-target sizing
    double max_lot     = 0.30;
};

class AdaptiveTfGoldEngine {
public:
    bool   shadow_mode    = true;
    bool   enabled        = false;
    bool   use_cost_guard = true;
    double lot            = 0.01;
    AdaptiveTfParams p;
    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit AdaptiveTfGoldEngine(const char* sym="XAUUSD")
        : symbol_(sym?sym:"XAUUSD") { engine_name_ = "AdaptiveTfGold"; }

    bool has_open() const noexcept { return pos_.active; }
    const char* cur_state() const noexcept { return state_; }

    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn cb) noexcept {
        if (bid<=0.0||ask<=0.0) return;
        last_bid_=bid; last_ask_=ask;
        const double mid=(bid+ask)*0.5;
        const int64_t bk=(now_ms/p.base_tf_ms)*p.base_tf_ms;
        if(!bar_open_){ bar_open_=true; bk_=bk; o_=h_=l_=c_=mid; tc_=1; return; }
        if(bk!=bk_){ on_base_bar(o_,h_,l_,c_,tc_,bk_,cb); bk_=bk; o_=h_=l_=c_=mid; tc_=1; }
        else { if(mid>h_)h_=mid; if(mid<l_)l_=mid; c_=mid; ++tc_; }
    }

    void force_close(int64_t ts, OnCloseFn cb) noexcept {
        if(pos_.active) close_pos(last_bid_>0?last_bid_:c_, last_ask_>0?last_ask_:c_, ts, "FORCE_CLOSE", cb);
    }
    void cancel() noexcept { pos_=Pos{}; }

    size_t seed_from_bar_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if(!f.is_open()){ std::printf("[SEED-FATAL] %s: cannot open %s\n",engine_name_.c_str(),path.c_str()); std::fflush(stdout); return 0; }
        std::string line; std::getline(f,line); size_t n=0;
        while(std::getline(f,line)){ double ts=0,o=0,hh=0,ll=0,c=0;
            if(std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&hh,&ll,&c)!=5) continue;
            if(c<=0.0) continue;
            push_bar(o,hh,ll,c,avg_tc_>0?(int)avg_tc_:50); ++n;   // neutral tick-count during seed
        }
        std::printf("[SEED][%s] %zu bars -- closes=%zu atr_f=%.3f\n",engine_name_.c_str(),n,closes_.size(),atr_f_);
        std::fflush(stdout); return n;
    }

private:
    struct Pos { bool active=false; int dir=0; int mode=0; // mode 0=TREND 1=RANGE
        double entry=0,stop=0,target=0,lot=0,trail=0; int64_t ts=0; int held=0; double mfe=0,mae=0; bool has_tp=false; } pos_;

    std::deque<double> closes_, highs_, lows_;
    std::deque<int>    tcs_;
    double atr_f_=0, atr_s_=0, atr_f_sum_=0, atr_s_sum_=0, prev_c_=0; int af_w_=0, as_w_=0;
    double avg_tc_=0;
    std::string symbol_, engine_name_, state_pers_;
    const char* state_="WARMUP";
    double last_bid_=0,last_ask_=0;
    bool bar_open_=false; int64_t bk_=0; double o_=0,h_=0,l_=0,c_=0; int tc_=0;

    void push_bar(double o,double h,double l,double c,int tc) noexcept {
        if(prev_c_>0.0){ double tr=std::fmax(h-l,std::fmax(std::fabs(h-prev_c_),std::fabs(l-prev_c_)));
            if(af_w_<p.atr_fast){atr_f_sum_+=tr;if(++af_w_==p.atr_fast)atr_f_=atr_f_sum_/p.atr_fast;} else atr_f_=(atr_f_*(p.atr_fast-1)+tr)/p.atr_fast;
            if(as_w_<p.atr_slow){atr_s_sum_+=tr;if(++as_w_==p.atr_slow)atr_s_=atr_s_sum_/p.atr_slow;} else atr_s_=(atr_s_*(p.atr_slow-1)+tr)/p.atr_slow;
        }
        prev_c_=c;
        closes_.push_back(c); highs_.push_back(h); lows_.push_back(l); tcs_.push_back(tc);
        const size_t cap=(size_t)(std::max({p.atr_slow,p.donch_hi,p.er_n,p.range_sma})+50);
        while(closes_.size()>cap){closes_.pop_front();highs_.pop_front();lows_.pop_front();tcs_.pop_front();}
        avg_tc_ = avg_tc_<=0? tc : (avg_tc_*0.98 + tc*0.02);
    }

    double er() const noexcept {
        const int sz=(int)closes_.size(); if(sz<p.er_n+1) return -1.0;
        double dir=std::fabs(closes_.back()-closes_[(size_t)(sz-1-p.er_n)]);
        double path=0; for(int i=sz-p.er_n;i<sz;++i) path+=std::fabs(closes_[(size_t)i]-closes_[(size_t)(i-1)]);
        if(path<=0.0) return 0.0; return dir/path;
    }
    double sma(int n) const noexcept { const int sz=(int)closes_.size(); if(sz<n) return -1.0;
        double s=0; for(int i=sz-n;i<sz;++i) s+=closes_[(size_t)i]; return s/n; }
    double donch_hi() const noexcept { const int sz=(int)highs_.size(); if(sz<p.donch_hi+1) return -1.0;
        double m=highs_[(size_t)(sz-1-p.donch_hi)]; for(int i=sz-p.donch_hi;i<sz-1;++i) if(highs_[(size_t)i]>m)m=highs_[(size_t)i]; return m; }
    double donch_lo() const noexcept { const int sz=(int)lows_.size(); if(sz<p.donch_hi+1) return -1.0;
        double m=lows_[(size_t)(sz-1-p.donch_hi)]; for(int i=sz-p.donch_hi;i<sz-1;++i) if(lows_[(size_t)i]<m)m=lows_[(size_t)i]; return m; }

    bool warmed() const noexcept {
        return (int)closes_.size() >= std::max({p.atr_slow,p.donch_hi,p.er_n,p.range_sma})+1 && atr_f_>0.0 && atr_s_>0.0;
    }

    double sized_lot(double price) const noexcept {
        if(atr_f_<=0.0||price<=0.0) return lot; double ab=atr_f_/price*10000.0; if(ab<=0) return lot;
        double L=(p.target_vol_bps/ab)*lot; if(L<0.01)L=0.01; if(L>p.max_lot)L=p.max_lot; return L;
    }

    void on_base_bar(double o,double h,double l,double c,int tc,int64_t ts,OnCloseFn cb) noexcept {
        push_bar(o,h,l,c,tc);
        if(pos_.active){ manage(h,l,c,ts,cb); if(pos_.active) return; }   // one position at a time
        if(!enabled || !warmed()){ state_="WARMUP"; return; }

        const double e=er(); const double vr=(atr_s_>0?atr_f_/atr_s_:1.0);
        const bool vol_expand = tcs_.back() > avg_tc_;   // participation proxy
        if(e>=p.er_trend && vr>=p.vol_hi){ state_="TREND"; try_trend(c,ts); }
        else if(e<=p.er_range && vr<=p.vol_norm_hi && vr>=p.vol_norm_lo){ state_="RANGE"; (void)vol_expand; try_range(c,ts); }
        else state_="CHOP";
    }

    void try_trend(double c,int64_t ts) noexcept {
        const double dh=donch_hi(), dl=donch_lo(); if(dh<0||dl<0) return;
        int dir=0; if(c>dh) dir=1; else if(c<dl) dir=-1; if(!dir) return;
        const double sl = dir>0? c - p.sl_trend_atr*atr_f_ : c + p.sl_trend_atr*atr_f_;
        open_pos(dir,0,sl,0.0,false,ts);   // mode TREND, no fixed TP, ride with trail
    }
    void try_range(double c,int64_t ts) noexcept {
        const double mid=sma(p.range_sma); if(mid<0) return;
        const double up=mid+p.band_atr*atr_f_, dn=mid-p.band_atr*atr_f_;
        int dir=0; if(c>up) dir=-1; else if(c<dn) dir=1; if(!dir) return;   // fade the extreme
        const double sl = dir>0? c - p.range_sl_atr*atr_f_ : c + p.range_sl_atr*atr_f_;
        open_pos(dir,1,sl,mid,true,ts);    // mode RANGE, TP=mid
    }

    void open_pos(int dir,int mode,double stop,double tp,bool has_tp,int64_t ts) noexcept {
        const double bid=last_bid_,ask=last_ask_; if(bid<=0||ask<=0) return;
        const double L=sized_lot(c_);
        if(use_cost_guard && atr_f_>0.0 && !ExecutionCostGuard::is_viable(symbol_.c_str(),ask-bid,atr_f_,L,1.5)) return;
        pos_=Pos{}; pos_.active=true; pos_.dir=dir; pos_.mode=mode; pos_.lot=L; pos_.ts=ts;
        pos_.entry = dir>0?ask:bid; pos_.stop=stop; pos_.target=tp; pos_.has_tp=has_tp;
        pos_.trail = dir>0? pos_.entry - p.trail_atr*atr_f_ : pos_.entry + p.trail_atr*atr_f_;
        std::printf("[%s] ENTRY %s %s px=%.2f stop=%.2f%s lot=%.3f er=%.2f vr=%.2f%s\n",engine_name_.c_str(),
            mode?"RANGE":"TREND", dir>0?"LONG":"SHORT", pos_.entry, stop, has_tp?" (tp)":"", L, er(), (atr_s_>0?atr_f_/atr_s_:0.0),
            shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
    }

    void manage(double h,double l,double c,int64_t ts,OnCloseFn cb) noexcept {
        ++pos_.held;
        const double fav = pos_.dir>0?(h-pos_.entry):(pos_.entry-l); if(fav>pos_.mfe)pos_.mfe=fav;
        const double adv = pos_.dir>0?(pos_.entry-l):(h-pos_.entry); if(adv>pos_.mae)pos_.mae=adv;
        if(pos_.mode==0){   // TREND: trail + structural stop
            if(pos_.dir>0){ double nt=c - p.trail_atr*atr_f_; if(nt>pos_.trail)pos_.trail=nt;
                if(l<=pos_.stop){ close_pos(pos_.stop,pos_.stop,ts,"SL",cb); return; }
                if(l<=pos_.trail){ close_pos(pos_.trail,pos_.trail,ts,"TRAIL",cb); return; } }
            else { double nt=c + p.trail_atr*atr_f_; if(nt<pos_.trail)pos_.trail=nt;
                if(h>=pos_.stop){ close_pos(pos_.stop,pos_.stop,ts,"SL",cb); return; }
                if(h>=pos_.trail){ close_pos(pos_.trail,pos_.trail,ts,"TRAIL",cb); return; } }
        } else {            // RANGE: TP / SL / timeout
            if(pos_.dir>0){ if(l<=pos_.stop){ close_pos(pos_.stop,pos_.stop,ts,"SL",cb); return; }
                if(pos_.has_tp && h>=pos_.target){ close_pos(pos_.target,pos_.target,ts,"TP",cb); return; } }
            else { if(h>=pos_.stop){ close_pos(pos_.stop,pos_.stop,ts,"SL",cb); return; }
                if(pos_.has_tp && l<=pos_.target){ close_pos(pos_.target,pos_.target,ts,"TP",cb); return; } }
            if(pos_.held>=p.range_hold){ close_pos(last_bid_,last_ask_,ts,"TIMEOUT",cb); return; }
        }
    }

    void close_pos(double bid_or_px,double ask_or_px,int64_t ts,const char* why,OnCloseFn cb) noexcept {
        if(!pos_.active) return;
        // for SL/TRAIL/TP we pass the level as both args; for TIMEOUT/FORCE we pass live bid/ask
        const bool level = (bid_or_px==ask_or_px);
        const double exit_px = level? bid_or_px : (pos_.dir>0?bid_or_px:ask_or_px);
        const double price_bp = pos_.dir*(exit_px-pos_.entry)/pos_.entry*10000.0;
        const double usd_per_pt = 100.0;   // XAUUSD: $100/pt per lot (ledger convention)
        const double notional = pos_.lot*usd_per_pt, pnl=price_bp/10000.0*notional;
        const double spread = level? 0.0 : std::fabs(ask_or_px-bid_or_px);
        const double cost = spread/pos_.entry*notional;
        std::printf("[%s] EXIT %s %s %s bp=%+.1f pnl=%.2f held=%d%s\n",engine_name_.c_str(),
            pos_.mode?"RANGE":"TREND", pos_.dir>0?"LONG":"SHORT", why, price_bp, pnl, pos_.held, shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
        omega::TradeRecord tr{}; tr.symbol=symbol_; tr.side=pos_.dir>0?"LONG":"SHORT";
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px; tr.size=pos_.lot; tr.pnl=pnl; tr.net_pnl=pnl-cost;
        tr.entryTs=pos_.ts/1000; tr.exitTs=ts/1000; tr.engine=engine_name_;
        tr.exitReason=std::string(pos_.mode?"RANGE_":"TREND_")+why; tr.spreadAtEntry=spread; tr.shadow=shadow_mode;
        tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        if(cb) cb(tr); pos_=Pos{};
    }
};

} // namespace omega
