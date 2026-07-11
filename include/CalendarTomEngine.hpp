#pragma once
// =============================================================================
//  CalendarTomEngine.hpp -- TURN-OF-MONTH (TOM) seasonality, long-only index.
//
//  A flows/liquidity calendar staple the book lacked (IndexSeasonal = day-of-week;
//  this = turn-of-month). Long the index across the TOM window: the last LASTN
//  trading days of a month + the first FIRSTN trading days of the next, FLAT
//  otherwise. Enter at the OPEN of the first in-window day, exit at the OPEN of
//  the first out-window day (open-to-open, no same-bar fill).
//
//  VALIDATED (backtest/tom_backtest.py, faithful daily, 2bp RT, 2016-2026,
//  last3+first3): ALL 5 indices PASS both-WF-halves AND both-regimes --
//  SPX 1.61 (bull1.52/BEAR2.29) NDX 1.54 (1.45/2.35) DJ30 1.60 (1.55/1.99)
//  GER40 1.43 (1.40/1.73) UK100 1.47 (1.49/1.35). Book PF1.52, H1 1.53/H2 1.52.
//  KEY: PF is HIGHER in 2022 bear than bull -- a real calendar/flows effect, NOT
//  index beta (in-market ~24% of the time, POSITIVE through the 2022 selloff).
//  Fills the book's bear-positive gap; orthogonal to trend/MR (calendar-timed).
//
//  ADVERSE-PROTECTION: hold-to-window-end by design -- backtested. No cold-cut /
//  BE-ratchet: the edge is the full TOM window's flows drift; a stop mid-window
//  cuts the winners (same finding as IndexSeasonal / trend runners). The
//  protection IS the tight time-box (~6 trading days, ~24% in-market) + long-only
//  in a both-regime-positive window. Verdict: trail/cut lowers net; the window is
//  the control.
//
//  DESIGN -- D1-driven (on_tick aggregates UTC-day bars), portable calendar
//  arithmetic (no gmtime; Hinnant civil-from-days + weekday counting; holidays
//  approximated as weekdays -- minor). One instance per index. Vol-target lot.
//  shadow default. Warm-seed via seed_from_d1_csv. Harness backtest/tom_backtest.py
//  + backtest/tom_engine_validate.cpp.
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
#include "IndexBookBudget.hpp"   // global concurrent-exposure cap for the D1 index book

namespace omega {

struct TomParams {
    int    last_n         = 3;     // last N trading days of the month
    int    first_n        = 3;     // first N trading days of the next month
    double target_vol_bps = 60.0;
    double max_lot        = 0.50;
    int    atr_period     = 14;
    double usd_per_pt     = 1.0;    // per-symbol CFD point value (set in init)
};

class CalendarTomEngine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    TomParams p;
    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit CalendarTomEngine(const char* symbol)
        : symbol_(symbol ? symbol : "UNKNOWN") { engine_name_ = "CalendarTom_" + symbol_; }

    const std::string& symbol() const noexcept { return symbol_; }
    bool has_open_position() const noexcept { return pos_.active; }

    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn cb) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        const double mid = (bid + ask) * 0.5;
        const int64_t day = (now_ms / 86400000LL) * 86400000LL;
        if (!acc_open_) { acc_open_ = true; acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid; return; }
        if (day != acc_day_) {
            on_d1_bar(acc_h_, acc_l_, acc_c_, bid, ask, acc_day_, day, cb);
            acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid;
        } else { if (mid > acc_h_) acc_h_ = mid; if (mid < acc_l_) acc_l_ = mid; acc_c_ = mid; }
    }

    // day_ms = the UTC day that just CLOSED; new_day_ms = the ACTUAL next bar's day (the open
    // we fill at -- NOT day_ms+1, which would land on a weekend across Fri->Mon gaps and cause
    // a spurious window exit/re-entry). bid/ask = that new day's first tick (~open).
    void on_d1_bar(double h, double l, double c, double bid, double ask,
                   int64_t day_ms, int64_t new_day_ms, OnCloseFn cb) noexcept {
        last_bid_ = bid; last_ask_ = ask;
        update_atr(h, l, c); prev_close_ = c; ++day_count_;

        const int64_t new_day_z = new_day_ms / 86400000LL;   // the day whose open we fill at
        const bool in_win = is_tom_day(new_day_z);

        // exit at the open of the first out-of-window day
        if (pos_.active && !in_win) { close_position(bid, ask, day_ms, "TOM_EXIT", cb); }
        // enter at the open of the first in-window day
        if (enabled && !pos_.active && in_win && atr_ > 0.0 && day_count_ >= p.atr_period)
            open_position(ask, bid, ask, day_ms);
        // update MFE/MAE while held (LONG)
        if (pos_.active) {
            double fav = h - pos_.entry_px; if (fav > pos_.mfe) pos_.mfe = fav;
            double adv = pos_.entry_px - l; if (adv > pos_.mae) pos_.mae = adv;
            ++pos_.bars_held;
        }
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
    void cancel() noexcept { if (pos_.active) IndexBookBudget::g().release(IdxDir::LONG); pos_ = Pos{}; }

    // ---- restart persistence (S-2026-07-08). TOM holds ~6 trading days across
    // near-daily restarts; unpersisted, every restart orphaned the leg and the
    // window exit fired on nothing -> ZERO TOM round-trips ledgered since the
    // 06-21 ship. bars_held is re-derived from entry_ts (cosmetic here: the exit
    // is window-driven, not bar-counted).
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!pos_.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos_.lot; o.entry=pos_.entry_px;
        o.sl=0.0; o.tp=0.0; o.entry_ts=pos_.entry_ts/1000; o.mfe=pos_.mfe; o.mae=pos_.mae;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (pos_.active) return false;                       // adopt won't double an open slot
        // re-take the budget slot so the eventual close's release() stays paired
        // (observe_only in shadow -> never blocks; a genuinely open leg must be
        // restored regardless, so the return value is deliberately not checked).
        IndexBookBudget::g().reserve(IdxDir::LONG, engine_name_.c_str(), symbol_.c_str());
        pos_=Pos{}; pos_.active=true; pos_.entry_px=ps.entry; pos_.lot=ps.size;
        pos_.entry_ts=ps.entry_ts*1000; pos_.mfe=ps.mfe; pos_.mae=ps.mae;
        pos_.bars_held=weekdays_between(pos_.entry_ts, (int64_t)time(nullptr)*1000LL);
        return true;
    }

    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SEED-FATAL] CalendarTom %s: cannot open %s\n",symbol_.c_str(),path.c_str()); std::fflush(stdout); return 0; }
        const bool was = enabled; enabled = false;
        auto nub = [](const omega::TradeRecord&){};
        std::string line; std::getline(f, line); size_t n=0;
        while (std::getline(f, line)) {
            double ts=0,o=0,h=0,l=0,c=0;
            if (std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
            if (c<=0.0) continue;
            int64_t day_ms=(ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
            { int w=weekday_of(day_ms/86400000LL); if(w==0||w==6) continue; }   // drop weekend stubs
            day_ms=(day_ms/86400000LL)*86400000LL;
            const double sp=c*0.00010; on_d1_bar(h,l,c,c-sp,c+sp,day_ms,day_ms+86400000LL,nub); ++n;
        }
        enabled = was;
        std::printf("[SEED][CalendarTom-%s] %zu D1 bars replayed atr=%.4f -- hot\n",symbol_.c_str(),n,atr_);
        std::fflush(stdout); return n;
    }

private:
    // ---- portable calendar (no gmtime) ----
    static void civil_from_days(int64_t z, int& y, int& m, int& d) noexcept {
        z += 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        int64_t doe = z - era * 146097;
        int64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
        int64_t yy  = yoe + era * 400;
        int64_t doy = doe - (365*yoe + yoe/4 - yoe/100);
        int64_t mp  = (5*doy + 2) / 153;
        d = (int)(doy - (153*mp + 2)/5 + 1);
        m = (int)(mp < 10 ? mp + 3 : mp - 9);
        y = (int)(yy + (m <= 2));
    }
    static int weekday_of(int64_t z) noexcept { return (int)(((z % 7) + 4 + 7) % 7); } // 0=Sun..6=Sat
    static bool is_weekday_z(int64_t z) noexcept { int w = weekday_of(z); return w >= 1 && w <= 5; }
    // completed weekday D1 bars strictly between the entry day and `to` day
    static int weekdays_between(int64_t from_ms, int64_t to_ms) noexcept {
        const int64_t a = from_ms/86400000LL, b = to_ms/86400000LL;
        int n = 0; for (int64_t z = a+1; z < b; ++z) if (is_weekday_z(z)) ++n; return n;
    }
    static int days_in_month(int y, int m) noexcept {
        static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 2) { bool leap = (y%4==0 && y%100!=0) || (y%400==0); return leap ? 29 : 28; }
        return dm[m-1];
    }
    bool is_tom_day(int64_t z) const noexcept {
        if (!is_weekday_z(z)) return false;
        int y, m, d; civil_from_days(z, y, m, d);
        const int64_t z_first = z - (d - 1);                 // 1st calendar day of month
        int tdom = 0; for (int64_t k = z_first; k <= z; ++k) if (is_weekday_z(k)) ++tdom;
        if (tdom <= p.first_n) return true;                  // first FIRSTN trading days
        const int dim = days_in_month(y, m);
        const int64_t z_last = z + (dim - d);                // last calendar day of month
        int bde = 0; for (int64_t k = z; k <= z_last; ++k) if (is_weekday_z(k)) ++bde;
        return bde <= p.last_n;                               // last LASTN trading days
    }

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
        if (atr_>0.0 && !ExecutionCostGuard::is_viable(symbol_.c_str(), ask-bid, atr_, L, 1.5)) return;
        // D1 index-book concurrent-exposure cap (LONG-only sleeve). observe_only in shadow.
        if (!IndexBookBudget::g().reserve(IdxDir::LONG, engine_name_.c_str(), symbol_.c_str())) return;
        pos_=Pos{}; pos_.active=true; pos_.entry_px=ask; pos_.lot=L; pos_.entry_ts=day_ms;
        std::printf("[CalendarTom-%s] ENTRY LONG (TOM) px=%.2f lot=%.3f%s\n",symbol_.c_str(),
                    ask,pos_.lot,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
    }
    void close_position(double bid,double ask,int64_t day_ms,const char* why,OnCloseFn cb) noexcept {
        if(!pos_.active)return;
        IndexBookBudget::g().release(IdxDir::LONG);   // pair with reserve() in open_position
        const double exit_px=bid;                                  // long exits at bid
        const double price_bp=(exit_px-pos_.entry_px)/pos_.entry_px*10000.0;
        // S-2026-07-11 GOLD PHASE 1 (GOLD_BOOK_ROADMAP bug 4, CONFIRMED): pnl was
        // emitted as return-fraction x notional (price_bp/1e4 * lot * usd_per_pt),
        // i.e. the correct USD pnl DIVIDED by the entry price (~4000x understated
        // on XAU, ~120x on US500). The ledger contract (trade_lifecycle Step 1) is
        // RAW price-points x lot; handle_closed_trade applies tick_value_multiplier
        // and recomputes net_pnl from spreadAtEntry via apply_realistic_costs.
        const double pnl_raw=(exit_px-pos_.entry_px)*pos_.lot;    // pts x lot (ledger scales to USD)
        const double spread=std::fabs(ask-bid);
        std::printf("[CalendarTom-%s] EXIT %s price_bp=%+.1f pnl_raw=%.4f (~$%.2f) bars=%d%s\n",
                    symbol_.c_str(),why,price_bp,pnl_raw,pnl_raw*p.usd_per_pt,
                    pos_.bars_held,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
        omega::TradeRecord tr{}; tr.symbol=symbol_; tr.side="LONG"; tr.entryPrice=pos_.entry_px; tr.exitPrice=exit_px;
        tr.size=pos_.lot; tr.pnl=pnl_raw; tr.entryTs=pos_.entry_ts/1000; tr.exitTs=day_ms/1000;
        tr.engine=engine_name_; tr.exitReason=why; tr.spreadAtEntry=spread; tr.shadow=shadow_mode;
        tr.mfe=pos_.mfe; tr.mae=pos_.mae;
        if(cb) cb(tr); pos_=Pos{};
    }
    std::string symbol_, engine_name_;
    bool acc_open_=false; int64_t acc_day_=0; double acc_h_=0,acc_l_=0,acc_c_=0,last_bid_=0,last_ask_=0;
    double atr_=0,atr_sum_=0; int atr_warm_=0; double prev_close_=0; int day_count_=0;
};

} // namespace omega
