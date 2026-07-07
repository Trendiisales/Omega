#pragma once
// =============================================================================
//  CrossSectionalIndexEngine.hpp -- cross-sectional (relative-value) RANKING
//  across the equity-index basket (SPX / NDX / DJ30 / GER40 / UK100). 2026-06-20.
//
//  THE GAP THIS FILLS: every other Omega index engine trades each symbol
//  DIRECTIONALLY (turtle / mean-rev / seasonal, one instance per symbol). NONE
//  ranks the indices AGAINST EACH OTHER. This engine is the Jegadeesh-Titman /
//  AQR cross-sectional staple -- ORTHOGONAL to the entire existing directional
//  book (correlation to the per-symbol legs ~0). One engine, all 5 symbols.
//
//  Three modes (one instance each, config-driven):
//    MOM_LONG : BULL-gated long-only -- long the STRONGEST index by lb-return.
//               flat in bear/chop. lb120 hold20.   (faithful PF 2.10, h1/h2 +)
//    MOM_LS   : market-NEUTRAL long-short -- long strongest, short weakest.
//               skip CHOP. lb250 hold20.           (faithful PF 2.06, BEAR 2.14)
//    MR_LS    : NON-BULL (bear+chop) long-short -- long the WEAKEST (loser),
//               short the STRONGEST (winner). lb5 hold3 (S-2026-06-23 was hold20=
//               BROKEN, BEAR PF0.61 -8.9% -- gives back the fast reversion). hold3 =
//               BEAR PF1.25 +14.4% AND HELD recent OOS bear 2025-26 PF4.11 +11.3%.
//
//  TIMEFRAME LAW (xs_timeframe_matrix.py): momentum = SLOW lookback (120-250d);
//  mean-reversion = FAST lookback (3-10d); both hold ~20 D1 bars. lb~=20 is the
//  dead zone (neither effect). Encoded as the per-mode defaults below.
//
//  REGIME = basket BREADTH, computed INTERNALLY from the 5 symbols' own closes:
//    breadth = #symbols with close>SMA200 AND SMA200 rising (vs 20 bars ago).
//    BULL breadth>=4 ; BEAR breadth<=1 ; else CHOP. (no external feed needed.)
//  PROTECTION (each mode flat in the wrong regime -- operator mandate):
//    MOM_LONG trades ONLY bull; MOM_LS skips ONLY chop; MR_LS skips ONLY bull.
//    Asymmetric by design: momentum needs chop-skip (chop is its only loser);
//    MR is the inverse (wants the volatile bear/chop tape) -- see vault
//    [[CrossSectionalIndexRelVal]] + [[omega-mr-asym-bear-veto]].
//  OUTRIGHT INDEX SHORTS ARE DEAD (PF 0.5-0.7 even bear-gated) -- bear coverage
//  comes from the market-NEUTRAL L/S legs (short funded by long), NEVER outright.
//
//  ADVERSE-PROTECTION: hold-to-horizon by design -- backtested. A cold loss-cut
//  or BE-ratchet was NOT added: relative-value/momentum legs give back into the
//  reversion (cf. swing-protection sweep + ImpulseFilter-MUST-NOT-on-MR). The
//  protection IS the regime gate (flat in wrong regime) + the market-neutral
//  construction (paired long/short nets out beta) + the fixed 20-bar horizon.
//  Verdict: trail/cut lowers net; gate+neutrality is the adverse control.
//
//  DESIGN -- D1-driven (on_tick aggregates UTC-day bars per symbol). At the
//  first tick of a new UTC day the PRIOR day closes are finalised, open legs
//  age/exit, then (signal on close[t]) new legs fill at the current price
//  (~open[t+1]). Faithful to xs_*; signal-to-fill is next-open, never same-bar.
//  Portable (no gmtime), MSVC-safe, shadow default. Warm-seed per symbol via
//  seed_from_d1_csv. Faithful harness: backtest/cross_sectional_relval.py +
//  backtest/xs_timeframe_matrix.py.
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot (persist_save_all/persist_restore)
#include "IndexBookBudget.hpp"   // global concurrent-exposure cap for the D1 index book

namespace omega {

enum class XsMode { MOM_LONG, MOM_LS, MR_LS };

struct XsIndexParams {
    XsMode mode        = XsMode::MOM_LS;
    int    lookback    = 250;     // D1 bars for the ranking return
    int    hold_bars   = 20;      // D1 bars to hold each leg
    int    sma_period  = 200;     // regime trend filter
    int    sma_slope_n = 20;      // SMA "rising" = SMA(i) > SMA(i-sma_slope_n)
    int    topk        = 1;       // legs per side
    double target_vol_bps = 60.0; // vol-target sizing
    double max_lot     = 0.50;
    int    atr_period  = 14;
};

class CrossSectionalIndexEngine {
public:
    bool   shadow_mode   = true;
    bool   enabled       = false;
    bool   use_cost_guard= true;   // production gate; harness sets false for python-parity check
    double lot           = 0.01;
    XsIndexParams p;
    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // roster fixed at construction; usd_per_pt per symbol (CFD point value).
    explicit CrossSectionalIndexEngine(XsMode mode,
                                       std::vector<std::string> symbols,
                                       std::vector<double> usd_per_pt)
        : syms_(std::move(symbols)), upp_(std::move(usd_per_pt)) {
        p.mode = mode;
        switch (mode) {                                   // timeframe-law defaults
            case XsMode::MOM_LONG: p.lookback=120; p.hold_bars=20; break;
            case XsMode::MOM_LS:   p.lookback=250; p.hold_bars=20; break;
            case XsMode::MR_LS:    p.lookback=5;   p.hold_bars=3;  break;  // S-2026-06-23 hold 20->3:
        }
        const char* mn = mode==XsMode::MOM_LONG?"MomLong":mode==XsMode::MOM_LS?"MomLS":"MrLS";
        engine_name_ = std::string("XsIndex_") + mn;
        const size_t N = syms_.size();
        if (upp_.size() < N) upp_.resize(N, 1.0);
        S_.resize(N);
    }

    size_t n_open() const noexcept { return legs_.size(); }
    size_t n_pending() const noexcept { return pend_entry_.size()+pend_exit_.size(); }
    const std::string& name() const noexcept { return engine_name_; }
    int symbol_index(const std::string& s) const noexcept {
        for (size_t i=0;i<syms_.size();++i) if (syms_[i]==s) return (int)i; return -1;
    }

    // per-tick, per-symbol. Aggregates D1 closes; the FIRST symbol to cross into
    // a new UTC day finalises the prior day (signal on close[t]) and QUEUES
    // entries/exits, which then fill on each symbol's next tick (= open[t+1]).
    // No same-bar fill: signal-to-fill is always next-open.
    void on_tick(int si, double bid, double ask, int64_t now_ms, OnCloseFn cb) noexcept {
        if (si<0 || si>=(int)syms_.size() || bid<=0.0 || ask<=0.0) return;
        const int64_t day = (now_ms/86400000LL)*86400000LL;
        Sym& s = S_[si];
        if (!day_open_) {                                 // first ever tick
            day_open_=true; cur_day_=day;
            s.last_bid=bid; s.last_ask=ask; s.cur_close=(bid+ask)*0.5; return;
        }
        if (day != cur_day_) {                            // a new UTC day began:
            finalize_day(cur_day_, cb);                   // finalise PRIOR closes (none updated yet today)
            cur_day_ = day;
        }
        s.last_bid=bid; s.last_ask=ask; s.cur_close=(bid+ask)*0.5;
        fill_pending(si, day, cb);                        // fill this symbol's queued legs at current px
    }

    void force_close_all(int64_t day_ms, OnCloseFn cb) noexcept {
        for (auto& lg : legs_) emit_close(lg, day_ms, "FORCE_CLOSE", cb);   // releases each
        // pend_exit_ legs were reserved at open but not emitted here -> release them.
        for (auto& lg : pend_exit_) IndexBookBudget::g().release(lg.dir>0?IdxDir::LONG:IdxDir::SHORT);
        legs_.clear(); pend_exit_.clear(); pend_entry_.clear();
    }
    void cancel() noexcept {
        // release every reserved leg (legs_ + pend_exit_); pend_entry_ was never reserved.
        for (auto& lg : legs_)      IndexBookBudget::g().release(lg.dir>0?IdxDir::LONG:IdxDir::SHORT);
        for (auto& lg : pend_exit_) IndexBookBudget::g().release(lg.dir>0?IdxDir::LONG:IdxDir::SHORT);
        legs_.clear(); pend_exit_.clear(); pend_entry_.clear();
    }

    // ---- restart persistence (S-2026-07-08). Same orphan class as CalendarTom:
    // legs hold 3-20 D1 bars across near-daily restarts. One snapshot per open
    // leg (queued exits included -- they still hold exposure), tag
    // "<base>#<SYM>:<L|S>" for the wire_multicell router. bars_held is
    // LOAD-BEARING (hold_bars horizon) -> re-derived from entry_ts, so an
    // overdue restored leg queues its exit at the next finalize_day.
    void persist_save_all(const char* base, const char* /*sym*/,
                          std::vector<omega::PositionSnapshot>& out) const {
        auto emit = [&](const Leg& lg) {
            omega::PositionSnapshot ps;
            ps.engine   = std::string(base) + "#" + syms_[(size_t)lg.si] + (lg.dir>0?":L":":S");
            ps.symbol   = syms_[(size_t)lg.si];
            ps.side     = lg.dir>0 ? "LONG" : "SHORT";
            ps.size     = lg.lot;
            ps.entry    = lg.entry_px;
            ps.sl       = 0.0; ps.tp = 0.0;
            ps.entry_ts = lg.entry_ts/1000;
            ps.mfe      = lg.mfe; ps.mae = lg.mae;
            out.push_back(ps);
        };
        for (const auto& lg : legs_)      emit(lg);
        for (const auto& lg : pend_exit_) emit(lg);
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        const int si = symbol_index(ps.symbol); if (si < 0) return false;
        const int dir = (ps.side == "LONG") ? +1 : -1;
        for (const auto& lg : legs_)                          // adopt won't double a slot
            if (lg.si == si && lg.dir == dir) return false;
        // re-take the budget slot so emit_close's release() stays paired
        // (observe_only in shadow -> never blocks; return deliberately unchecked).
        IndexBookBudget::g().reserve(dir>0?IdxDir::LONG:IdxDir::SHORT,
                                     engine_name_.c_str(), ps.symbol.c_str());
        Leg lg; lg.si=si; lg.dir=dir; lg.entry_px=ps.entry; lg.lot=ps.size;
        lg.entry_ts=ps.entry_ts*1000; lg.mfe=ps.mfe; lg.mae=ps.mae;
        lg.bars_held=weekdays_between(lg.entry_ts, (int64_t)time(nullptr)*1000LL);
        legs_.push_back(lg);
        return true;
    }

    // warm-seed one symbol's D1 close history (header: ts,o,h,l,c).
    size_t seed_from_d1_csv(int si, const std::string& path) noexcept {
        if (si<0 || si>=(int)syms_.size()) return 0;
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SEED-FATAL] %s %s: cannot open %s\n",engine_name_.c_str(),syms_[si].c_str(),path.c_str()); std::fflush(stdout); return 0; }
        Sym& s=S_[si]; std::string line; std::getline(f,line); size_t n=0;
        while (std::getline(f,line)) {
            double ts=0,o=0,h=0,l=0,c=0;
            if (std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5) continue;
            if (c<=0.0) continue;
            int64_t day_ms=(ts>1e11)?(int64_t)ts:(int64_t)(ts*1000.0);
            { int wd=(int)((((day_ms/86400000LL)%7)+4+7)%7); if(wd==6||wd==0) continue; }  // drop weekend stubs
            push_close(s, c); ++n;
        }
        std::printf("[SEED][%s-%s] %zu D1 closes -- hist=%zu\n",engine_name_.c_str(),syms_[si].c_str(),n,s.closes.size());
        std::fflush(stdout); return n;
    }

private:
    struct Sym {
        std::deque<double> closes;        // D1 close history (cap-bounded)
        double cur_close=0,last_bid=0,last_ask=0;
        double atr=0,atr_sum=0,prev_c=0; int atr_warm=0;
    };
    struct Leg {
        int si=0; int dir=0;              // +1 long, -1 short
        double entry_px=0, lot=0; int64_t entry_ts=0; int bars_held=0; double mfe=0,mae=0;
    };
    struct PendEntry { int si=0; int dir=0; };

    // completed weekday D1 bars strictly between the entry day and `to` day
    static int weekdays_between(int64_t from_ms, int64_t to_ms) noexcept {
        const int64_t a = from_ms/86400000LL, b = to_ms/86400000LL;
        int n = 0;
        for (int64_t z = a+1; z < b; ++z) { int w=(int)(((z%7)+4+7)%7); if (w>=1 && w<=5) ++n; }
        return n;
    }

    void push_close(Sym& s, double c) noexcept {
        // ATR (for vol-target sizing only)
        if (s.prev_c>0.0) {
            double tr=std::fabs(c-s.prev_c);
            if (s.atr_warm<p.atr_period){s.atr_sum+=tr;if(++s.atr_warm==p.atr_period)s.atr=s.atr_sum/p.atr_period;}
            else s.atr=(s.atr*(p.atr_period-1)+tr)/p.atr_period;
        }
        s.prev_c=c;
        s.closes.push_back(c);
        const size_t cap=(size_t)(p.sma_period+p.sma_slope_n+p.lookback+50);
        while (s.closes.size()>cap) s.closes.pop_front();
    }

    static double sma_at(const std::deque<double>& c, int n, int back) noexcept {
        // mean of n closes ending `back` bars from the last element. -1 if short.
        const int sz=(int)c.size(); const int end=sz-1-back;
        if (end<n-1) return -1.0;
        double sum=0; for (int k=end-n+1;k<=end;++k) sum+=c[(size_t)k];
        return sum/n;
    }

    int breadth() const noexcept {
        int cnt=0;
        for (const auto& s : S_) {
            if ((int)s.closes.size() < p.sma_period+p.sma_slope_n+1) continue;
            const double c=s.closes.back();
            const double m=sma_at(s.closes,p.sma_period,0);
            const double m_prev=sma_at(s.closes,p.sma_period,p.sma_slope_n);
            if (m<0||m_prev<0) continue;
            if (c>m && m>m_prev) ++cnt;
        }
        return cnt;
    }
    const char* regime() const noexcept {
        // regime is undefined until EVERY symbol has full SMA history; reporting
        // "BEAR" during warmup would let the non-bull (MR) gate trade the warmup.
        for (const auto& s : S_)
            if ((int)s.closes.size() < p.sma_period+p.sma_slope_n+1) return "WARMUP";
        const int b=breadth();
        return b>=4 ? "BULL" : (b<=1 ? "BEAR" : "CHOP");
    }

    bool gate_ok(const char* reg) const noexcept {
        if (std::string("WARMUP")==reg) return false;   // never trade before regime warmed
        switch (p.mode) {
            case XsMode::MOM_LONG: return std::string("BULL")==reg;     // bull only
            case XsMode::MOM_LS:   return std::string("CHOP")!=reg;     // skip chop
            case XsMode::MR_LS:    return std::string("BULL")!=reg;     // bear+chop
        }
        return false;
    }

    // finalise the day that just ended: push each symbol's (clean, not-yet-
    // updated-today) close, age legs + QUEUE due exits, then (if a fresh cohort
    // is allowed) compute the ranking on close[t] and QUEUE entries. Nothing
    // fills here -- fills happen in fill_pending() on each symbol's next tick.
    void finalize_day(int64_t day_ms, OnCloseFn /*cb*/) noexcept {
        for (auto& s : S_) if (s.cur_close>0.0) push_close(s, s.cur_close);
        pend_day_ = day_ms;

        // age open legs; move those at the hold horizon to the exit queue
        std::vector<Leg> keep;
        for (auto& lg : legs_) {
            Sym& s=S_[(size_t)lg.si]; const double mid=s.cur_close;
            const double fav=lg.dir>0?(mid-lg.entry_px):(lg.entry_px-mid);
            if (fav>lg.mfe) lg.mfe=fav; const double adv=-fav; if (adv>lg.mae) lg.mae=adv;
            ++lg.bars_held;
            if (lg.bars_held >= p.hold_bars) pend_exit_.push_back(lg);
            else keep.push_back(lg);
        }
        legs_.swap(keep);

        // one cohort at a time: no fresh signal while anything is open or queued
        if (!enabled || !legs_.empty() || !pend_exit_.empty() || !pend_entry_.empty()) return;
        const char* reg = regime();
        if (!gate_ok(reg)) return;

        std::vector<std::pair<double,int>> sc;            // (lb-return, si)
        for (size_t i=0;i<S_.size();++i) {
            const auto& c=S_[i].closes; const int sz=(int)c.size();
            if (sz < p.lookback+1) return;                // not warmed -> no trade
            const double c0=c[(size_t)(sz-1-p.lookback)], c1=c.back();
            if (c0<=0.0) return;
            sc.push_back({c1/c0-1.0, (int)i});
        }
        std::sort(sc.begin(), sc.end());                  // ascending by return
        const int n=(int)sc.size(); if (n < p.topk*2) return;
        auto qL=[&](int rp){ pend_entry_.push_back({sc[(size_t)rp].second,+1}); };
        auto qS=[&](int rp){ pend_entry_.push_back({sc[(size_t)rp].second,-1}); };
        for (int k=0;k<p.topk;++k) switch (p.mode) {
            case XsMode::MOM_LONG: qL(n-1-k); break;             // strongest long
            case XsMode::MOM_LS:   qL(n-1-k); qS(k); break;      // strong long / weak short
            case XsMode::MR_LS:    qL(k); qS(n-1-k); break;      // weak long / strong short
        }
        pend_reg_ = reg;
    }

    // fill this symbol's queued exits + entries at its current (next-open) price.
    void fill_pending(int si, int64_t day_ms, OnCloseFn cb) noexcept {
        for (size_t i=0;i<pend_exit_.size();) {
            if (pend_exit_[i].si==si) { emit_close(pend_exit_[i], day_ms, "XS_EXIT", cb);
                pend_exit_.erase(pend_exit_.begin()+(long)i); }
            else ++i;
        }
        for (size_t i=0;i<pend_entry_.size();) {
            if (pend_entry_[i].si==si) {
                if (open_leg(si, pend_entry_[i].dir, day_ms)) {}   // filled (or cost-blocked)
                pend_entry_.erase(pend_entry_.begin()+(long)i);
            } else ++i;
        }
    }

    double sized_lot(int si, double price) const noexcept {
        const double atr=S_[(size_t)si].atr;
        if (atr<=0.0||price<=0.0) return lot;
        const double ab=atr/price*10000.0; if (ab<=0.0) return lot;
        double L=(p.target_vol_bps/ab)*lot; if(L<0.01)L=0.01; if(L>p.max_lot)L=p.max_lot; return L;
    }

    bool open_leg(int si, int dir, int64_t day_ms) noexcept {
        Sym& s=S_[(size_t)si];
        const double bid=s.last_bid, ask=s.last_ask; if(bid<=0.0||ask<=0.0) return false;
        const double L=sized_lot(si, s.cur_close);
        if (use_cost_guard && s.atr>0.0 && !ExecutionCostGuard::is_viable(syms_[(size_t)si].c_str(), ask-bid, s.atr, L, 1.5)) return false;
        // D1 index-book concurrent-exposure cap (per-leg dir; L/S nets ~0 -> neutral legs unthrottled).
        const IdxDir d_ = dir>0 ? IdxDir::LONG : IdxDir::SHORT;
        if (!IndexBookBudget::g().reserve(d_, engine_name_.c_str(), syms_[(size_t)si].c_str())) return false;
        Leg lg; lg.si=si; lg.dir=dir; lg.lot=L; lg.entry_ts=day_ms;
        lg.entry_px = dir>0?ask:bid;                      // long fills ask, short fills bid
        legs_.push_back(lg);
        std::printf("[%s] ENTRY %s %s px=%.2f lot=%.3f reg=%s%s\n",engine_name_.c_str(),
                    dir>0?"LONG":"SHORT",syms_[(size_t)si].c_str(),lg.entry_px,L,pend_reg_.c_str(),shadow_mode?" [SHADOW]":"");
        std::fflush(stdout); return true;
    }

    void emit_close(Leg& lg, int64_t day_ms, const char* why, OnCloseFn cb) noexcept {
        IndexBookBudget::g().release(lg.dir>0 ? IdxDir::LONG : IdxDir::SHORT);   // pair with reserve() in open_leg
        Sym& s=S_[(size_t)lg.si];
        const double bid=(s.last_bid>0.0)?s.last_bid:s.cur_close, ask=(s.last_ask>0.0)?s.last_ask:s.cur_close;
        const double exit_px = lg.dir>0?bid:ask;          // long exits bid, short exits ask
        const double price_bp = lg.dir*(exit_px-lg.entry_px)/lg.entry_px*10000.0;
        const double notional = lg.lot*upp_[(size_t)lg.si], pnl=price_bp/10000.0*notional;
        const double spread=std::fabs(ask-bid), cost=spread/lg.entry_px*notional;
        std::printf("[%s] EXIT %s %s %s bp=%+.1f pnl=%.2f bars=%d%s\n",engine_name_.c_str(),
                    lg.dir>0?"LONG":"SHORT",syms_[(size_t)lg.si].c_str(),why,price_bp,pnl,lg.bars_held,shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
        omega::TradeRecord tr{}; tr.symbol=syms_[(size_t)lg.si]; tr.side=lg.dir>0?"LONG":"SHORT";
        tr.entryPrice=lg.entry_px; tr.exitPrice=exit_px; tr.size=lg.lot; tr.pnl=pnl; tr.net_pnl=pnl-cost;
        tr.entryTs=lg.entry_ts/1000; tr.exitTs=day_ms/1000; tr.engine=engine_name_; tr.exitReason=why;
        tr.spreadAtEntry=spread; tr.shadow=shadow_mode; tr.mfe=lg.mfe; tr.mae=lg.mae;
        if(cb) cb(tr);
    }

    std::vector<std::string> syms_;
    std::vector<double>      upp_;
    std::vector<Sym>         S_;
    std::vector<Leg>         legs_;
    std::vector<Leg>         pend_exit_;     // legs queued to exit on their symbol's next tick
    std::vector<PendEntry>   pend_entry_;    // entries queued to fill on their symbol's next tick
    std::string pend_reg_; int64_t pend_day_=0;
    std::string engine_name_;
    bool    day_open_=false; int64_t cur_day_=0;
};

} // namespace omega
