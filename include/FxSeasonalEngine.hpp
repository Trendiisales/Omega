#pragma once
// =============================================================================
//  FxSeasonalEngine.hpp -- FX Friday-seasonality (S43, combined-sleeve component)
//
//  Long FX into the weekend: enter at Friday's D1 close, exit Monday's D1 close.
//  Part of the validated combined thin-edge sleeve (fx_combined_sleeve.cpp):
//  Friday(weekly) Sharpe 1.50, +5565bp over 9 pairs, 5/6 blocks, both halves+.
//  Uncorrelated (~0) with carry + cross-RV + COT/session. COST-SENSITIVE (edge
//  dies at 3x retail spread; fine at ~1x majors) -> shadow-validate before live.
//
//  DESIGN -- one instance per pair (mirrors FxCarryEngine). D1-driven (on_tick
//  aggregates UTC-day bars). Portable weekday (no gmtime_r): 1970-01-01=Thu(4),
//  Fri=5. Long-only (the tested seasonal bias). Vol-target lot. shadow default.
//  Warm-seed via seed_from_d1_csv (drops flat Saturday artifact).
// =============================================================================
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

struct FxSeasonalParams {
    int    fri_wday        = 5;        // Friday (Sun=0..Sat=6)
    int    hold_bars       = 1;        // exit after N D1 closes (Fri->Mon = 1)
    double target_vol_bps  = 50.0;
    double max_lot         = 0.10;
    int    atr_period      = 14;
    double usd_per_pt      = 100000.0;
};

class FxSeasonalEngine {
public:
    bool             shadow_mode = true;
    bool             enabled     = false;
    double           lot         = 0.01;
    FxSeasonalParams p;
    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit FxSeasonalEngine(const char* symbol)
        : symbol_(symbol ? symbol : "UNKNOWN") { engine_name_ = "FxSeasonal_" + symbol_; }

    const std::string& symbol() const noexcept { return symbol_; }
    bool has_open_position() const noexcept { return pos_.active; }

    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn cb) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        const double mid = (bid + ask) * 0.5;
        const int64_t day = (now_ms / 86400000LL) * 86400000LL;
        if (!acc_open_) { acc_open_ = true; acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid; return; }
        if (day != acc_day_) {
            on_d1_bar(acc_h_, acc_l_, acc_c_, bid, ask, acc_day_, cb);
            acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid;
        } else { if (mid > acc_h_) acc_h_ = mid; if (mid < acc_l_) acc_l_ = mid; acc_c_ = mid; }
    }

    void on_d1_bar(double h, double l, double c, double bid, double ask,
                   int64_t day_ms, OnCloseFn cb) noexcept {
        last_bid_ = bid; last_ask_ = ask;
        update_atr(h, l, c); prev_close_ = c; ++day_count_;
        const int wd = (int)((((day_ms/86400000LL) % 7) + 4 + 7) % 7);   // portable, Fri=5, Sat=6

        // Exit an open position once held hold_bars.
        if (pos_.active) {
            ++pos_.bars_held;
            { double fav=h-pos_.entry_px; if(fav>pos_.mfe)pos_.mfe=fav; double adv=pos_.entry_px-l; if(adv>pos_.mae)pos_.mae=adv; }  // LONG excursion
            if (pos_.bars_held >= p.hold_bars) close_position(bid, ask, day_ms, "SEASONAL_EXIT", cb);
        }
        // Entry: Friday close, long only.
        if (enabled && !pos_.active && atr_ > 0.0 && day_count_ >= p.atr_period && wd == p.fri_wday)
            open_position(c, bid, ask, day_ms);
    }

    void force_close(int64_t day_ms, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        const double bid = (last_bid_>0.0)?last_bid_:prev_close_, ask=(last_ask_>0.0)?last_ask_:prev_close_;
        close_position(bid, ask, day_ms, "FORCE_CLOSE", cb);
    }
    void cancel() noexcept { pos_ = Pos{}; }

    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SEED-FATAL] FxSeasonal %s: cannot open %s\n",symbol_.c_str(),path.c_str()); std::fflush(stdout); return 0; }
        const bool was = enabled; enabled = false;
        auto nub = [](const omega::TradeRecord&){};
        std::string line; std::getline(f, line); size_t n=0;
        while (std::getline(f, line)) {
            double ts=0,o=0,h=0,l=0,c=0;
            if (std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
            if (c<=0.0) continue;
            int64_t day_ms=(ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
            { int wd=(int)((((day_ms/86400000LL)%7)+4+7)%7); if(wd==6) continue; }   // drop Sat
            day_ms=(day_ms/86400000LL)*86400000LL;
            const double sp=c*0.00005; on_d1_bar(h,l,c,c-sp,c+sp,day_ms,nub); ++n;
        }
        enabled = was;
        std::printf("[SEED][FxSeasonal-%s] %zu D1 bars replayed atr=%.6f -- hot\n",symbol_.c_str(),n,atr_);
        std::fflush(stdout); return n;
    }

private:
    struct Pos { bool active=false; double entry_px=0,lot=0; int64_t entry_ts=0; int bars_held=0; double mfe=0,mae=0; } pos_;
    void update_atr(double h,double l,double c) noexcept {
        if(prev_close_<=0.0){prev_close_=c;return;}
        double tr=std::fmax(h-l,std::fmax(std::fabs(h-prev_close_),std::fabs(l-prev_close_)));
        if(atr_warm_<p.atr_period){atr_sum_+=tr;if(++atr_warm_==p.atr_period)atr_=atr_sum_/p.atr_period;}
        else atr_=(atr_*(p.atr_period-1)+tr)/p.atr_period;
    }
    double sized_lot(double price) const noexcept {
        if(atr_<=0.0||price<=0.0)return lot; double ab=atr_/price*10000.0; if(ab<=0)return lot;
        double L=(p.target_vol_bps/ab)*lot; if(L<0.01)L=0.01; if(L>p.max_lot)L=p.max_lot; return L;
    }
    void open_position(double close_px,double bid,double ask,int64_t day_ms) noexcept {
        const double L=sized_lot(close_px);
        // cost gate: 1-ATR expected move proxy (1-day seasonal hold)
        if (atr_>0.0 && !ExecutionCostGuard::is_viable(symbol_.c_str(), ask-bid, atr_, L, 1.5)) return;
        pos_=Pos{}; pos_.active=true; pos_.entry_px=ask; pos_.lot=L; pos_.entry_ts=day_ms;
        std::printf("[FxSeasonal-%s] ENTRY LONG (Fri) px=%.5f lot=%.3f%s\n",symbol_.c_str(),ask,pos_.lot,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
    }
    void close_position(double bid,double ask,int64_t day_ms,const char* why,OnCloseFn cb) noexcept {
        (void)ask; if(!pos_.active)return;
        const double exit_px=bid;                                  // long exits at bid
        const double price_bp=(exit_px-pos_.entry_px)/pos_.entry_px*10000.0;
        const double notional=pos_.lot*p.usd_per_pt, pnl=price_bp/10000.0*notional;
        const double spread=std::fabs(ask-bid), cost=spread/pos_.entry_px*notional;
        std::printf("[FxSeasonal-%s] EXIT %s price_bp=%+.1f pnl=%.2f bars=%d%s\n",symbol_.c_str(),why,price_bp,pnl,pos_.bars_held,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
        omega::TradeRecord tr{}; tr.symbol=symbol_; tr.side="LONG"; tr.entryPrice=pos_.entry_px; tr.exitPrice=exit_px;
        tr.size=pos_.lot; tr.pnl=pnl; tr.net_pnl=pnl-cost; tr.entryTs=pos_.entry_ts/1000; tr.exitTs=day_ms/1000;
        tr.engine=engine_name_; tr.exitReason=why; tr.spreadAtEntry=spread; tr.shadow=shadow_mode;
        tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        if(cb) cb(tr); pos_=Pos{};
    }
    std::string symbol_, engine_name_;
    bool acc_open_=false; int64_t acc_day_=0; double acc_h_=0,acc_l_=0,acc_c_=0,last_bid_=0,last_ask_=0;
    double atr_=0,atr_sum_=0; int atr_warm_=0; double prev_close_=0; int day_count_=0;
};

} // namespace omega
