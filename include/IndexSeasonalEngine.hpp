#pragma once
// =============================================================================
//  IndexSeasonalEngine.hpp -- equity-index day-of-week seasonality (S44)
//
//  Long the two strong weekday SESSIONS on index CFDs:
//    * enter at TUESDAY D1 close, exit WEDNESDAY close  (Wed session +11.2bp)
//    * enter at FRIDAY  D1 close, exit MONDAY    close  (Mon session +11.8bp,
//      Fri-close->Mon-close, spans weekend)
//  Validated (index_seasonal_sharpe.cpp / index_d1_edges.cpp, 6 indices, 7.4yr
//  Dukascopy D1 incl. 2020 crash + 2022 bear):
//    best-2 sleeve Sharpe 0.69 vs 0.36 buy&hold, net +40775bp > +32227 hold,
//    maxDD 3608 < 4715, only 40% time-in-market, both halves +, blk 5/6.
//    REGIME-ROBUST: survives below-200d-SMA (bear, +19.5bp/trade) and high vol.
//    DRIFT-CONTROLLED (not beta) and CROSS-SECTIONALLY broad (all 6 indices).
//  Direct analog of FxSeasonalEngine (Friday-long FX). Long-only (no viable
//  short: Thursday is the only weak session, -2.5bp, below round-trip cost).
//
//  OPTIONAL VIX TERM-STRUCTURE GATE (default OFF): index_vix_gate.cpp showed a
//  contango gate (VIX/VIX3M < 1.00 at entry) lifts Sharpe 0.69->0.72 and cuts
//  maxDD -36% (3608->2297) -- but the live engine only has VIX.F LEVEL, not
//  VIX3M (which lives in the Mac-side vix_term.py). A VIX-LEVEL gate was tested
//  and HURTS (Sharpe 0.50). So the ratio gate stays OFF until VIX3M is wired to
//  the VPS; call set_vix_ratio() + set gate_by_vix=true once available.
//
//  DESIGN -- one instance per index. D1-driven (on_tick aggregates UTC-day bars).
//  Portable weekday (no gmtime_r, MSVC-safe): 1970-01-01=Thu(4), Tue=2, Fri=5.
//  Vol-target lot. shadow default. Warm-seed via seed_from_d1_csv.
// =============================================================================
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "IndexRiskGate.hpp"     // S44 shared macro risk-off (VIX+credit+dollar)

namespace omega {

struct IndexSeasonalParams {
    int    tue_wday        = 2;        // Tuesday entry  (Sun=0..Sat=6)
    int    fri_wday        = 5;        // Friday  entry
    int    hold_bars       = 1;        // exit after N D1 closes (Tue->Wed, Fri->Mon = 1)
    double target_vol_bps  = 60.0;     // index daily ranges wider than FX
    double max_lot         = 0.50;
    int    atr_period      = 14;
    double usd_per_pt      = 1.0;      // CFD contract value per point (per-symbol, set in init)
};

class IndexSeasonalEngine {
public:
    bool              shadow_mode  = true;
    bool              enabled      = false;
    double            lot          = 0.01;
    // optional VIX term-structure gate. Skip entry when VIX/VIX3M >= vix_gate_ratio
    // (deep backwardation). Validated: gate at 1.05 lifts best-2 sleeve Sharpe
    // 0.69->0.80 and HALVES maxDD (3608->1755, index_vix_gate.cpp). The ratio is
    // fed from a file (broker has no VIX3M); if the file is missing/stale the gate
    // degrades GRACEFULLY to ungated (the proven 0.69 edge) -- never blocks on bad data.
    bool              gate_by_vix    = false;
    double            vix_gate_ratio = 1.05;   // skip entry when ratio >= this (backwardation)
    std::string       vix_ratio_path;          // file "epoch_sec,ratio" refreshed daily by external fetcher
    int               vix_max_age_days = 4;     // ignore (trade ungated) if ratio older than this
    IndexSeasonalParams p;
    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit IndexSeasonalEngine(const char* symbol)
        : symbol_(symbol ? symbol : "UNKNOWN") { engine_name_ = "IndexSeasonal_" + symbol_; }

    const std::string& symbol() const noexcept { return symbol_; }
    bool has_open_position() const noexcept { return pos_.active; }
    void set_vix_ratio(double r) noexcept { cur_vix_ratio_ = r; }   // feed VIX/VIX3M at/before entry

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
        const int wd = (int)((((day_ms/86400000LL) % 7) + 4 + 7) % 7);   // portable, Tue=2, Fri=5, Sat=6

        // Exit an open position once held hold_bars (runs before entry -> no overlap).
        if (pos_.active) {
            ++pos_.bars_held;
            if (pos_.bars_held >= p.hold_bars) close_position(bid, ask, day_ms, "SEASONAL_EXIT", cb);
        }
        // Refresh VIX term-structure ratio (once/day, cheap) if gate enabled.
        if (gate_by_vix && !vix_ratio_path.empty()) refresh_vix_ratio(day_ms);

        // Entry: Tuesday or Friday close, long only. Optional VIX-ratio gate.
        // Gate blocks ONLY when a FRESH ratio says deep backwardation; missing/stale
        // ratio (cur_vix_ratio_ < 0) => no block => trade the proven ungated edge.
        const bool entry_day = (wd == p.tue_wday) || (wd == p.fri_wday);
        // own per-instance VIX gate (back-compat) OR the shared portfolio macro
        // risk-off (VIX+credit+dollar). Combo validated: seasonal Sharpe 1.13->1.27.
        const bool vix_block = (gate_by_vix && cur_vix_ratio_ >= 0.0 && cur_vix_ratio_ >= vix_gate_ratio)
                            || omega::index_risk_off();
        if (enabled && !pos_.active && atr_ > 0.0 && day_count_ >= p.atr_period && entry_day && !vix_block)
            open_position(c, bid, ask, day_ms, wd);
    }

    void force_close(int64_t day_ms, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        const double bid = (last_bid_>0.0)?last_bid_:prev_close_, ask=(last_ask_>0.0)?last_ask_:prev_close_;
        close_position(bid, ask, day_ms, "FORCE_CLOSE", cb);
    }
    void cancel() noexcept { pos_ = Pos{}; }

    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SEED-FATAL] IndexSeasonal %s: cannot open %s\n",symbol_.c_str(),path.c_str()); std::fflush(stdout); return 0; }
        const bool was = enabled; enabled = false;
        auto nub = [](const omega::TradeRecord&){};
        std::string line; std::getline(f, line); size_t n=0;
        while (std::getline(f, line)) {
            double ts=0,o=0,h=0,l=0,c=0;
            if (std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
            if (c<=0.0) continue;
            int64_t day_ms=(ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
            { int wd=(int)((((day_ms/86400000LL)%7)+4+7)%7); if(wd==6||wd==0) continue; }   // drop weekend stubs
            day_ms=(day_ms/86400000LL)*86400000LL;
            const double sp=c*0.00010; on_d1_bar(h,l,c,c-sp,c+sp,day_ms,nub); ++n;
        }
        enabled = was;
        std::printf("[SEED][IndexSeasonal-%s] %zu D1 bars replayed atr=%.4f -- hot\n",symbol_.c_str(),n,atr_);
        std::fflush(stdout); return n;
    }

private:
    // Read the daily VIX/VIX3M ratio file ("epoch_sec,ratio", last line wins).
    // Sets cur_vix_ratio_ if the file is fresh (within vix_max_age_days of this
    // bar), else -1 so the gate stays open (graceful degrade to ungated).
    void refresh_vix_ratio(int64_t day_ms) noexcept {
        cur_vix_ratio_ = -1.0;
        std::ifstream f(vix_ratio_path);
        if (!f.is_open()) return;
        std::string line, last;
        while (std::getline(f, line)) if (!line.empty() && (line[0]=='-'||(line[0]>='0'&&line[0]<='9'))) last = line;
        if (last.empty()) return;
        double ts=0, ratio=0;
        if (std::sscanf(last.c_str(), "%lf,%lf", &ts, &ratio) != 2) return;
        if (ratio <= 0.0 || ts <= 0.0) return;
        const int64_t age_s = day_ms/1000 - (int64_t)ts;
        if (age_s > (int64_t)vix_max_age_days*86400) return;   // stale -> ungated
        cur_vix_ratio_ = ratio;
    }
    struct Pos { bool active=false; double entry_px=0,lot=0; int64_t entry_ts=0; int bars_held=0; int wday=0; } pos_;
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
    void open_position(double close_px,double bid,double ask,int64_t day_ms,int wd) noexcept {
        const double L=sized_lot(close_px);
        // cost gate: 1-ATR expected move proxy (1-day seasonal hold)
        if (atr_>0.0 && !ExecutionCostGuard::is_viable(symbol_.c_str(), ask-bid, atr_, L, 1.5)) return;
        pos_=Pos{}; pos_.active=true; pos_.entry_px=ask; pos_.lot=L; pos_.entry_ts=day_ms; pos_.wday=wd;
        std::printf("[IndexSeasonal-%s] ENTRY LONG (%s) px=%.2f lot=%.3f%s\n",symbol_.c_str(),
                    wd==p.tue_wday?"Tue":"Fri",ask,pos_.lot,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
    }
    void close_position(double bid,double ask,int64_t day_ms,const char* why,OnCloseFn cb) noexcept {
        (void)ask; if(!pos_.active)return;
        const double exit_px=bid;                                  // long exits at bid
        const double price_bp=(exit_px-pos_.entry_px)/pos_.entry_px*10000.0;
        const double notional=pos_.lot*p.usd_per_pt, pnl=price_bp/10000.0*notional;
        const double spread=std::fabs(ask-bid), cost=spread/pos_.entry_px*notional;
        std::printf("[IndexSeasonal-%s] EXIT %s price_bp=%+.1f pnl=%.2f bars=%d%s\n",symbol_.c_str(),why,price_bp,pnl,pos_.bars_held,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
        omega::TradeRecord tr{}; tr.symbol=symbol_; tr.side="LONG"; tr.entryPrice=pos_.entry_px; tr.exitPrice=exit_px;
        tr.size=pos_.lot; tr.pnl=pnl; tr.net_pnl=pnl-cost; tr.entryTs=pos_.entry_ts/1000; tr.exitTs=day_ms/1000;
        tr.engine=engine_name_; tr.exitReason=why; tr.spreadAtEntry=spread; tr.shadow=shadow_mode;
        if(cb) cb(tr); pos_=Pos{};
    }
    std::string symbol_, engine_name_;
    bool acc_open_=false; int64_t acc_day_=0; double acc_h_=0,acc_l_=0,acc_c_=0,last_bid_=0,last_ask_=0;
    double atr_=0,atr_sum_=0; int atr_warm_=0; double prev_close_=0; int day_count_=0;
    double cur_vix_ratio_=-1.0;
};

} // namespace omega
