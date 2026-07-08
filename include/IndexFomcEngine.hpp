#pragma once
//  ADVERSE-PROTECTION: (S-2026-07-08c, BACKTESTED VERDICT -- debt cleared) 0.5xATR14(entry) intraday cold stop (p.stop_atr_mult, tick-path STOP_ATR) + hold_bars=1 FOMC-day-close time exit + index_risk_off() entry gate + ExecutionCostGuard. Verdict basis backtest/index_fomc_protection_bt.cpp (REAL class, 180 trades 2019-2026 x US500/USTEC/DJ30 certified dailies, adverse-first stop fills): stop lifts net +1204->+3032bp @3bp rt (PF1.13->1.48), worst trade -633->-294bp, 6/8 years improved, 2024 flipped positive; robust at 6bp (2x). DECAY FLAG (honest): H2 (late-22..2026) negative even stopped at 6bp; 2026 n=12 PF0.10-0.15 -- edge is decayed-recent; engine stays SHADOW lot 0.01, review after 2026 H2 meetings before any size talk.
// =============================================================================
//  IndexFomcEngine.hpp -- pre-FOMC drift on US index CFDs (S44)
//
//  Long into scheduled FOMC announcement days: enter at the D1 close of the
//  trading day immediately BEFORE a scheduled FOMC announcement, exit at the
//  FOMC-day close (hold 1 D1 bar). Lucca-Moench (2015) documented ~all equity
//  premium accrues pre-FOMC; it decayed post-2015 but is STILL alive on our
//  sample (index_validate2.cpp): FOMC-day mean +21.8bp all / +11.8bp 2023-26,
//  pre-day +8.3bp, n=354 across 6 indices. US-only (FOMC is a US event) ->
//  applied to US500.F / USTEC.F / DJ30.F. ~8 events/yr, dates known in advance.
//
//  Mirrors IndexSeasonalEngine: D1 via on_tick aggregation, MSVC-portable date
//  arithmetic, vol-target lot, shadow default, warm-seed, portfolio risk-gate.
//  Long-only. Respects omega::index_risk_off() (no new entry in macro stress).
// =============================================================================
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot (persist_save/restore)
#include "IndexRiskGate.hpp"

namespace omega {

// Scheduled FOMC announcement dates (YYYYMMDD), 2019-2026 (2026 = known/estimated).
static const int kFomcDates[] = {
 20190130,20190320,20190501,20190619,20190731,20190918,20191030,20191211,
 20200129,20200318,20200429,20200610,20200729,20200916,20201105,20201216,
 20210127,20210317,20210428,20210616,20210728,20210922,20211103,20211215,
 20220126,20220316,20220504,20220615,20220727,20220921,20221102,20221214,
 20230201,20230322,20230503,20230614,20230726,20230920,20231101,20231213,
 20240131,20240320,20240501,20240612,20240731,20240918,20241107,20241218,
 20250129,20250319,20250507,20250618,20250730,20250917,20251029,20251210,
 20260128,20260318,20260429,20260617,20260729};

struct IndexFomcParams {
    int    hold_bars      = 1;
    double target_vol_bps = 60.0;
    double max_lot        = 0.50;
    int    atr_period     = 14;
    double usd_per_pt     = 1.0;
    // S-2026-07-08c backtested cold stop (index_fomc_protection_bt.cpp, 180 trades
    // 2019-2026 x 3 US indices, adverse-first fills): 0.5xATR14(entry) intraday stop
    // lifts net +1204->+3032bp @3bp rt (PF 1.13->1.48), halves worst trade
    // (-633->-294bp), improves 6 of 8 years, flips 2024 positive. 0.0 = disabled.
    double stop_atr_mult  = 0.5;
};

class IndexFomcEngine {
public:
    bool             shadow_mode = true;
    bool             enabled     = false;
    double           lot         = 0.01;
    IndexFomcParams  p;
    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit IndexFomcEngine(const char* symbol)
        : symbol_(symbol ? symbol : "UNKNOWN") { engine_name_ = "IndexFomc_" + symbol_; }
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
        // S-2026-07-08c intraday cold stop (backtested, see IndexFomcParams::stop_atr_mult):
        // long-only 1-day event hold -> cut at entry - X*ATR14(entry) on the live tick path.
        // The BT modeled this exact resting-stop semantic (fires whenever the day's low
        // breaches, fill at stop level; live fill = current bid, slippage borne here).
        if (pos_.active && p.stop_atr_mult > 0.0 && pos_.atr_at_entry > 0.0
            && mid <= pos_.entry_px - p.stop_atr_mult * pos_.atr_at_entry)
            close_position(bid, ask, now_ms, "STOP_ATR", cb);
    }

    void on_d1_bar(double h, double l, double c, double bid, double ask,
                   int64_t day_ms, OnCloseFn cb) noexcept {
        last_bid_ = bid; last_ask_ = ask;
        update_atr(h, l, c); prev_close_ = c; ++day_count_;

        if (pos_.active) { ++pos_.bars_held; { double fav=h-pos_.entry_px; if(fav>pos_.mfe)pos_.mfe=fav; double adv=pos_.entry_px-l; if(adv>pos_.mae)pos_.mae=adv; } if (pos_.bars_held >= p.hold_bars) close_position(bid, ask, day_ms, "FOMC_EXIT", cb); }

        // Entry: this bar is the last trading day BEFORE a scheduled FOMC day.
        const bool entry_day = next_trading_day_is_fomc(day_ms);
        if (enabled && !pos_.active && atr_ > 0.0 && day_count_ >= p.atr_period && entry_day && !omega::index_risk_off())
            open_position(c, bid, ask, day_ms);
    }

    void force_close(int64_t day_ms, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        const double bid = (last_bid_>0.0)?last_bid_:prev_close_, ask=(last_ask_>0.0)?last_ask_:prev_close_;
        close_position(bid, ask, day_ms, "FORCE_CLOSE", cb);
    }
    // book-price form used by the PositionPersistence generic closer (acct_try_close)
    void force_close(double bid, double ask, int64_t now_ms, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        close_position(bid, ask, now_ms, "FORCE_CLOSE", cb);
    }
    void cancel() noexcept { pos_ = Pos{}; }

    // ---- restart persistence (S-2026-07-08). Same orphan class as CalendarTom:
    // the 1-bar hold spans an overnight restart. bars_held is LOAD-BEARING
    // (hold_bars exit) -> re-derived from entry_ts so a restored leg exits on the
    // next D1 close instead of restarting its hold clock.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!pos_.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos_.lot; o.entry=pos_.entry_px;
        // S-2026-07-08c: persist the stop LEVEL so the intraday stop survives a restart
        o.sl=(p.stop_atr_mult>0.0 && pos_.atr_at_entry>0.0)
                ? pos_.entry_px - p.stop_atr_mult*pos_.atr_at_entry : 0.0;
        o.tp=0.0; o.entry_ts=pos_.entry_ts/1000; o.mfe=pos_.mfe; o.mae=pos_.mae;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (pos_.active) return false;                       // adopt won't double an open slot
        pos_=Pos{}; pos_.active=true; pos_.entry_px=ps.entry; pos_.lot=ps.size;
        pos_.entry_ts=ps.entry_ts*1000; pos_.mfe=ps.mfe; pos_.mae=ps.mae;
        pos_.bars_held=weekdays_between(pos_.entry_ts, (int64_t)time(nullptr)*1000LL);
        // S-2026-07-08c: rehydrate the stop from the persisted level (sl=0 legacy
        // snapshot -> atr_at_entry stays 0 -> stop disarms, falls back to time-exit)
        if (ps.sl > 0.0 && p.stop_atr_mult > 0.0 && ps.entry > ps.sl)
            pos_.atr_at_entry = (ps.entry - ps.sl) / p.stop_atr_mult;
        return true;
    }

    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SEED-FATAL] IndexFomc %s: cannot open %s\n",symbol_.c_str(),path.c_str()); std::fflush(stdout); return 0; }
        const bool was = enabled; enabled = false; auto nub = [](const omega::TradeRecord&){};
        std::string line; std::getline(f, line); size_t n=0;
        while (std::getline(f, line)) {
            double ts=0,o=0,h=0,l=0,c=0;
            if (std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
            if (c<=0.0) continue;
            int64_t day_ms=(ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
            { int wd=(int)((((day_ms/86400000LL)%7)+4+7)%7); if(wd==6||wd==0) continue; }
            day_ms=(day_ms/86400000LL)*86400000LL;
            const double sp=c*0.00010; on_d1_bar(h,l,c,c-sp,c+sp,day_ms,nub); ++n;
        }
        enabled = was;
        std::printf("[SEED][IndexFomc-%s] %zu D1 bars replayed atr=%.4f -- hot\n",symbol_.c_str(),n,atr_);
        std::fflush(stdout); return n;
    }

private:
    // completed weekday D1 bars strictly between the entry day and `to` day
    static int weekdays_between(int64_t from_ms, int64_t to_ms) noexcept {
        const int64_t a = from_ms/86400000LL, b = to_ms/86400000LL;
        int n = 0;
        for (int64_t z = a+1; z < b; ++z) { int w=(int)(((z%7)+4+7)%7); if (w>=1 && w<=5) ++n; }
        return n;
    }
    struct Pos { bool active=false; double entry_px=0,lot=0; int64_t entry_ts=0; int bars_held=0; double mfe=0,mae=0; double atr_at_entry=0; } pos_;

    // YYYYMMDD for a UTC day_ms, via portable civil-from-days (Howard Hinnant).
    static int ymd_from_days(int64_t z) {
        z += 719468; int64_t era=(z>=0?z:z-146096)/146097; int64_t doe=z-era*146097;
        int64_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365; int64_t y=yoe+era*400;
        int64_t doy=doe-(365*yoe+yoe/4-yoe/100); int64_t mp=(5*doy+2)/153;
        int64_t d=doy-(153*mp+2)/5+1; int64_t m=mp+(mp<10?3:-9); y+=(m<=2);
        return (int)(y*10000 + m*100 + d);
    }
    static bool is_fomc_ymd(int ymd){ for(int x:kFomcDates) if(x==ymd) return true; return false; }
    static bool next_trading_day_is_fomc(int64_t day_ms){
        int64_t days = day_ms/86400000LL;
        // step forward to the next weekday (Sat=6,Sun=0 via portable wd)
        for (int step=1; step<=4; ++step){
            int64_t nd = days+step; int wd=(int)(((nd%7)+4+7)%7);
            if (wd==0 || wd==6) continue;                 // skip weekend
            return is_fomc_ymd(ymd_from_days(nd));         // first trading day after `day`
        }
        return false;
    }
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
        // cost gate: 1-ATR expected move proxy (1-day FOMC hold)
        if (atr_>0.0 && !ExecutionCostGuard::is_viable(symbol_.c_str(), ask-bid, atr_, L, 1.5)) return;
        pos_=Pos{}; pos_.active=true; pos_.entry_px=ask; pos_.lot=L; pos_.entry_ts=day_ms;
        pos_.atr_at_entry=atr_;   // S-2026-07-08c: freeze ATR for the intraday stop level
        std::printf("[IndexFomc-%s] ENTRY LONG (pre-FOMC) px=%.2f lot=%.3f%s\n",symbol_.c_str(),ask,pos_.lot,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
    }
    void close_position(double bid,double ask,int64_t day_ms,const char* why,OnCloseFn cb) noexcept {
        (void)ask; if(!pos_.active)return;
        const double exit_px=bid; const double price_bp=(exit_px-pos_.entry_px)/pos_.entry_px*10000.0;
        const double notional=pos_.lot*p.usd_per_pt, pnl=price_bp/10000.0*notional;
        const double spread=std::fabs(ask-bid), cost=spread/pos_.entry_px*notional;
        std::printf("[IndexFomc-%s] EXIT %s price_bp=%+.1f pnl=%.2f bars=%d%s\n",symbol_.c_str(),why,price_bp,pnl,pos_.bars_held,shadow_mode?" [SHADOW]":"");
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
