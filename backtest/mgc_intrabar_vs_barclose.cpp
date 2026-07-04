// ─────────────────────────────────────────────────────────────────────────────
// mgc_intrabar_vs_barclose.cpp
//   Measures whether an INTRA-BAR exit beats the current BAR-CLOSE-only exit for
//   the two live MGC 30m engines. NOTHING varies except exit-check cadence; entry
//   logic + all params are held identical to the real engines.
//
//   ENGINE #1  g_mgc_fastdon = omega::MgcFastDonchian30mEngine
//     - exit = channel-close breach: `if (c < _chan_low(Nout)) _close(c)`  (header L155)
//     - NO on_tick hook -> cannot inject an intra-bar exit through the real class.
//       => the struct is COPIED VERBATIM into sim::MgcFastDonCad with ONE change:
//          an exit_mode_ switch on the pos_active_ exit line. Everything else
//          (entry, Donchian, HVN profile, EMA/ATR, regime gate) is byte-identical.
//          CLOSE mode is validated to reproduce the real engine trade-for-trade.
//       INTRA-BAR: exit the instant bar_low <= chan_low(Nout); fill at chan_low,
//          or at bar OPEN if the bar gapped open below the level.
//
//   ENGINE #2  g_mgc_volbrk = omega::GoldVolBreakoutM30Engine on the MGC feed
//     - has a per-tick SL in on_tick (`if (bid <= pos.sl_px) _close`, header L250)
//       but the MGC feed calls ONLY on_m30_bar -> on_tick never runs -> the SL/trail
//       is DEAD; the only live exit is MAX_HOLD (72 bars, header L225).
//     - So we drive the REAL engine:
//         BAR-CLOSE  = on_m30_bar only (current live: SL dead, MAX_HOLD-only).
//         INTRA-BAR  = additionally feed on_tick(bid=low) each bar BEFORE the close,
//                      so the engine's REAL sl_px/trail check fires against the bar
//                      low (fills at sl_px per the engine's own _close).
//
//   Params replicated (verbatim from the headers, NOT invented):
//     #1: Nin=20, Nout=10, Kov=1.5, use_hvn_skip=1, profile_bins=30, hvn_frac=0.60,
//         use_trend_filter=1, EmaTrend=100, SlopeLB=20, lot=0.01
//         (MgcFastDonchian30mEngine.hpp L34-48, L125,155,157,171)
//     #2: kDonch=20,kAtrP=14,kEmaH1=200,kSlopeLB=5,kStopAtr=1.5,kTrailAtr=3.0,
//         kTrailAfterR=1.55,kImpPos=0.70,kImpRange=2.0,kLateAtr=1.0,kSess[7,20),
//         kMaxHold=72,kCooldown=3, lot=0.01
//         (GoldVolBreakoutM30Engine.hpp L116-129, L221-225,250)
//
//   PnL: engines emit pnl = (exit-entry)*lot RAW points. USD = pnl * mult, gold
//   mult=100 (registry sizing.hpp). With lot=0.01 -> USD_per_point = 0.01*100 = 1.0.
//   Round-trip cost applied in points (default 0.30, matching mgc_engines_audit).
//
//   Build (same flags as the existing MGC harnesses):
//     g++ -std=c++17 -O2 -Iinclude backtest/mgc_intrabar_vs_barclose.cpp \
//         -o backtest/mgc_intrabar_vs_barclose
//   Run: backtest/mgc_intrabar_vs_barclose [cost_pts=0.30] [csv=/Users/jo/Tick/mgc_30m_hist.csv]
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>

#include "MgcFastDonchian30mEngine.hpp"   // real engine #1 (reference/validation)
#include "GoldVolBreakoutM30Engine.hpp"   // real engine #2 (driven for both modes)

// ============================================================================
//  sim::MgcFastDonCad -- VERBATIM copy of omega::MgcFastDonchian30mEngine with a
//  single behavioural change: the pos_active_ exit line honours exit_mode_.
//  (Copied from include/MgcFastDonchian30mEngine.hpp; only the marked block and
//   two extra members differ.)
// ============================================================================
namespace sim {

struct MgcFastDonCad {
    // ---- config (identical defaults) ----
    bool   enabled       = false;
    bool   shadow_mode   = true;
    double lot           = 0.01;
    int    Nin           = 20;
    int    Nout          = 10;
    double Kov           = 1.5;
    bool   use_hvn_skip  = true;
    int    profile_bins  = 30;
    double hvn_frac      = 0.60;
    bool   use_trend_filter = true;
    int    EmaTrend         = 100;
    int    SlopeLB          = 20;

    // ---- NEW: cadence switch + whipsaw counter ----
    int    exit_mode_    = 0;    // 0 = bar-close (live), 1 = intra-bar
    long   whipsaw_      = 0;    // intra-bar exits where close recovered above the level

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    struct Bar { double o,h,l,c,v; int day; };
    std::deque<Bar> bars_;
    double  atr14_ = 0.0; double prev_close_ = 0.0; bool atr_init_ = false;
    double  ema_t_ = 0.0; bool ema_t_init_ = false;
    int     cur_day_ = -1;
    std::vector<std::pair<double,double>> day_cv_;
    double  day_hi_ = -1e18, day_lo_ = 1e18;
    std::vector<double> prior_hvn_; double prior_poc_ = 0.0; bool prior_ok_ = false;
    bool    hvn_external_ = false;
    bool    pos_active_ = false; double entry_ = 0.0; int64_t entry_ts_ = 0; double mfe_ = 0.0, mae_ = 0.0;
    double  l2_imb_ = 0.5; double l2_gate_ = 0.0;
    void    set_l2_imb(double x) noexcept { l2_imb_ = x; }
    bool has_open_position() const { return pos_active_; }

    void set_prior_hvn(const std::vector<double>& hvn, double poc) {
        prior_hvn_ = hvn; prior_poc_ = poc; prior_ok_ = !hvn.empty(); hvn_external_ = true;
    }

    void _finalize_prior_profile() {
        prior_ok_ = false; prior_hvn_.clear();
        if (day_cv_.empty() || day_hi_ <= day_lo_) return;
        double tv = 0; for (auto& cv : day_cv_) tv += cv.second;
        if (tv <= 0) return;
        const double bs = (day_hi_ - day_lo_) / profile_bins;
        std::vector<double> vb(profile_bins, 0.0);
        for (auto& cv : day_cv_) {
            int bi = (int)((cv.first - day_lo_) / bs);
            bi = std::max(0, std::min(bi, profile_bins - 1));
            vb[bi] += cv.second;
        }
        double mx = 0; int pi = 0;
        for (int j = 0; j < profile_bins; ++j) if (vb[j] > mx) { mx = vb[j]; pi = j; }
        prior_poc_ = day_lo_ + bs * (pi + 0.5);
        for (int j = 0; j < profile_bins; ++j)
            if (vb[j] >= hvn_frac * mx) prior_hvn_.push_back(day_lo_ + bs * (j + 0.5));
        prior_ok_ = true;
    }

    double _chan_high(int n) const {
        if ((int)bars_.size() < n) return 1e18;
        double hh = -1e18; for (int k = (int)bars_.size() - n; k < (int)bars_.size(); ++k) hh = std::max(hh, bars_[k].h);
        return hh;
    }
    double _chan_low(int n) const {
        if ((int)bars_.size() < n) return -1e18;
        double ll = 1e18; for (int k = (int)bars_.size() - n; k < (int)bars_.size(); ++k) ll = std::min(ll, bars_[k].l);
        return ll;
    }

    void _close(double exit_px, int64_t ts, OnCloseFn cb) {
        omega::TradeRecord tr;
        tr.symbol="MGC"; tr.engine="MgcFastDonchian30m"; tr.side="LONG";
        tr.entryPrice=entry_; tr.exitPrice=exit_px; tr.size=lot;
        tr.pnl=(exit_px - entry_) * lot;
        tr.entryTs=entry_ts_; tr.exitTs=ts; tr.shadow=shadow_mode;
        tr.mfe=mfe_; tr.mae=mae_;
        pos_active_ = false;
        if (cb) cb(tr);
    }

    void on_30m_bar(double o, double h, double l, double c, double v,
                    int64_t ts_sec, OnCloseFn cb) {
        if (!enabled) return;
        const int day = (int)(ts_sec / 86400);
        if (day != cur_day_) {
            if (cur_day_ >= 0 && !hvn_external_) _finalize_prior_profile();
            cur_day_ = day; day_cv_.clear(); day_hi_ = -1e18; day_lo_ = 1e18;
        }
        day_cv_.push_back({c, v}); day_hi_ = std::max(day_hi_, h); day_lo_ = std::min(day_lo_, l);
        if (atr_init_) { double tr = std::max(h - l, std::max(std::fabs(h - prev_close_), std::fabs(l - prev_close_)));
                         atr14_ += (tr - atr14_) / 14.0; }
        else           { atr14_ = h - l; atr_init_ = true; }
        prev_close_ = c;
        if (!ema_t_init_) { ema_t_ = c; ema_t_init_ = true; }
        else { const double a = 2.0 / (EmaTrend + 1); ema_t_ = a * c + (1.0 - a) * ema_t_; }

        if (pos_active_) {
            { double fav=h-entry_; if(fav>mfe_)mfe_=fav; double adv=entry_-l; if(adv>mae_)mae_=adv; }
            // ---- ONLY behavioural change vs the real engine: exit cadence ----
            const double L = _chan_low(Nout);
            if (exit_mode_ == 0) {                       // BAR-CLOSE (live)
                if (c < L) _close(c, ts_sec, cb);
            } else {                                     // INTRA-BAR (proposed)
                if (l <= L) {                            // channel-low touched during the bar
                    const double fill = (o < L) ? o : L; // gap-through -> fill at open
                    if (c >= L) ++whipsaw_;              // recovered by close (close-mode would NOT exit)
                    _close(fill, ts_sec, cb);
                }
            }
        } else {
            if (c > _chan_high(Nin)) {
                bool skip = false;
                if (omega::gold_regime().long_blocked()) skip = true;
                if (use_trend_filter) {
                    const bool above_ema = ema_t_init_ && c > ema_t_;
                    const bool rising    = (int)bars_.size() >= SlopeLB && c > bars_[bars_.size() - SlopeLB].c;
                    if (!(above_ema && rising)) skip = true;
                }
                if (use_hvn_skip && prior_ok_)
                    for (double hv : prior_hvn_) if (hv > c && hv <= c + Kov * atr14_) { skip = true; break; }
                if (l2_gate_ > 0.0 && l2_imb_ < l2_gate_) skip = true;
                if (!skip) { pos_active_ = true; entry_ = c; entry_ts_ = ts_sec; mfe_ = 0.0; mae_ = 0.0; }
            }
        }
        bars_.push_back({o,h,l,c,v,day});
        while ((int)bars_.size() > 256) bars_.pop_front();
    }
};

} // namespace sim

// ============================================================================
//  Shared plumbing
// ============================================================================
struct Bar { int64_t ts; double o,h,l,c,v; };

static std::vector<Bar> load(const char* path) {
    std::vector<Bar> out; std::ifstream f(path);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", path); return out; }
    std::string ln; std::getline(f, ln);  // header
    while (std::getline(f, ln)) {
        Bar b; char* e=nullptr; const char* s=ln.c_str();
        b.ts=std::strtoll(s,&e,10); if(*e!=',')continue; ++e;
        b.o=std::strtod(e,&e); if(*e!=',')continue; ++e;
        b.h=std::strtod(e,&e); if(*e!=',')continue; ++e;
        b.l=std::strtod(e,&e); if(*e!=',')continue; ++e;
        b.c=std::strtod(e,&e); b.v=(*e==',')?std::strtod(e+1,&e):0.0;
        if(b.h>=b.l && b.o>0) out.push_back(b);
    }
    return out;
}

// One closed trade in points (gross, pre-cost) + exit reason + bar-close at exit.
struct Trade { double entry, exit; std::string reason; double exit_bar_close; };

static const double USD_PER_POINT = 1.0;   // lot 0.01 * gold mult 100
static const double GOLD_MULT     = 100.0;
static const double LOT           = 0.01;

struct Stats { int n; double net_pts, net_usd, worst_usd, maxdd_usd, wr; };

static Stats summarize(const std::vector<Trade>& tr, double cost_pts) {
    Stats s{}; s.n=(int)tr.size();
    double eq=0, peak=0, mdd=0, worst=1e18; int wins=0;
    for (auto& t : tr) {
        double pnl_pts = (t.exit - t.entry) - cost_pts;
        double usd = pnl_pts * USD_PER_POINT;
        s.net_pts += pnl_pts; s.net_usd += usd;
        if (usd < worst) worst = usd;
        if (pnl_pts > 0) ++wins;
        eq += usd; if (eq > peak) peak = eq; if (peak - eq > mdd) mdd = peak - eq;
    }
    s.worst_usd = s.n ? worst : 0.0;
    s.maxdd_usd = mdd;
    s.wr = s.n ? 100.0*wins/s.n : 0.0;
    return s;
}

static void print_engine(const char* name,
                         const std::vector<Trade>& close_tr,
                         const std::vector<Trade>& intra_tr,
                         double cost_pts, long whipsaw) {
    Stats c = summarize(close_tr, cost_pts);
    Stats i = summarize(intra_tr, cost_pts);
    double dusd = i.net_usd - c.net_usd;
    double dpct = c.net_usd != 0 ? 100.0*dusd/std::fabs(c.net_usd) : 0.0;
    std::printf("\n===== %s  (cost=%.2f pt round-trip, USD/pt=%.2f) =====\n", name, cost_pts, USD_PER_POINT);
    std::printf("  BAR-CLOSE : n=%-4d WR=%4.1f%%  net=%+9.2f pt = $%+10.2f  worst=$%+9.2f  maxDD=$%9.2f\n",
                c.n, c.wr, c.net_pts, c.net_usd, c.worst_usd, c.maxdd_usd);
    std::printf("  INTRA-BAR : n=%-4d WR=%4.1f%%  net=%+9.2f pt = $%+10.2f  worst=$%+9.2f  maxDD=$%9.2f\n",
                i.n, i.wr, i.net_pts, i.net_usd, i.worst_usd, i.maxdd_usd);
    std::printf("  DELTA (intra - close): $%+10.2f  (%+.1f%%)   whipsaw(exit-then-recover)=%ld\n",
                dusd, dpct, whipsaw);
    const char* verdict = std::fabs(dpct) < 3.0 ? "NEUTRAL" : (dusd > 0 ? "HELPS" : "HURTS");
    std::printf("  VERDICT: intra-bar %s the engine by $%.2f (%+.1f%%)\n", verdict, dusd, dpct);
}

int main(int argc, char** argv) {
    const double cost = argc>1 ? std::atof(argv[1]) : 0.30;
    const char* path  = argc>2 ? argv[2] : "/Users/jo/Tick/mgc_30m_hist.csv";
    auto bars = load(path);
    std::printf("# MGC intrabar-vs-barclose: %zu 30m bars from %s\n", bars.size(), path);
    std::printf("# multiplier: gold=%.0f, lot=%.2f -> USD_per_point=%.2f\n", GOLD_MULT, LOT, USD_PER_POINT);
    if (bars.size() < 500) { std::fprintf(stderr,"too few bars\n"); return 1; }

    // ------------------------------------------------------------------
    //  ENGINE #1  MgcFastDonchian30m
    //  (a) fidelity: real engine vs sim CLOSE-mode must match trade-for-trade
    // ------------------------------------------------------------------
    std::vector<Trade> real1;
    {
        omega::MgcFastDonchian30mEngine eng; eng.enabled=true; eng.lot=LOT;
        auto cb=[&](const omega::TradeRecord& t){ real1.push_back({t.entryPrice,t.exitPrice,"",0}); };
        for (auto& b: bars) eng.on_30m_bar(b.o,b.h,b.l,b.c,b.v,b.ts,cb);
    }
    std::vector<Trade> close1, intra1; long whip1 = 0;
    {
        sim::MgcFastDonCad eng; eng.enabled=true; eng.lot=LOT; eng.exit_mode_=0;
        auto cb=[&](const omega::TradeRecord& t){ close1.push_back({t.entryPrice,t.exitPrice,"",0}); };
        for (auto& b: bars) eng.on_30m_bar(b.o,b.h,b.l,b.c,b.v,b.ts,cb);
    }
    {
        sim::MgcFastDonCad eng; eng.enabled=true; eng.lot=LOT; eng.exit_mode_=1;
        auto cb=[&](const omega::TradeRecord& t){ intra1.push_back({t.entryPrice,t.exitPrice,"",0}); };
        for (auto& b: bars) eng.on_30m_bar(b.o,b.h,b.l,b.c,b.v,b.ts,cb);
        whip1 = eng.whipsaw_;
    }
    // fidelity check
    bool match = (real1.size()==close1.size());
    if (match) for (size_t k=0;k<real1.size();++k)
        if (std::fabs(real1[k].entry-close1[k].entry)>1e-9 || std::fabs(real1[k].exit-close1[k].exit)>1e-9) { match=false; break; }
    std::printf("\n[FIDELITY #1] real engine trades=%zu  sim CLOSE trades=%zu  -> %s\n",
                real1.size(), close1.size(), match ? "MATCH (sim is faithful)" : "MISMATCH!!");

    // ------------------------------------------------------------------
    //  ENGINE #2  GoldVolBreakoutM30 on MGC feed
    //  BAR-CLOSE = on_m30_bar only (SL dead, MAX_HOLD-only, current live)
    //  INTRA-BAR = additionally feed on_tick(bid=low) each bar (real SL fires)
    // ------------------------------------------------------------------
    // H1 close series (last close per hour bucket) -- identical to mgc_engines_audit
    std::vector<std::pair<int64_t,double>> h1;
    { int64_t cur=-1; double cc=0;
      for (auto& b: bars){ int64_t hb=(b.ts/3600)*3600; if(hb!=cur){ if(cur>=0)h1.push_back({cur,cc}); cur=hb; } cc=b.c; }
      if(cur>=0)h1.push_back({cur,cc}); }
    const double half = 0.15;

    std::vector<Trade> close2, intra2; long whip2 = 0;
    {   // BAR-CLOSE: no on_tick
        omega::GoldVolBreakoutM30Engine eng; eng.enabled=true; eng.lot=LOT; eng.init(); eng.enabled=true;
        auto cb=[&](const omega::TradeRecord& t){ close2.push_back({t.entryPrice,t.exitPrice,t.exitReason,0}); };
        size_t ih=0;
        for (auto& b: bars) {
            while (ih < h1.size() && h1[ih].first <= b.ts) { eng.on_h1_close(h1[ih].second); ++ih; }
            eng.on_m30_bar(b.h,b.l,b.c, b.c-half, b.c+half, b.ts*1000, cb);
        }
    }
    {   // INTRA-BAR: feed on_tick(bid=low) BEFORE the close so the real SL/trail fires
        omega::GoldVolBreakoutM30Engine eng; eng.enabled=true; eng.lot=LOT; eng.init(); eng.enabled=true;
        double cur_bar_close = 0.0;
        auto cb=[&](const omega::TradeRecord& t){
            std::string r = t.exitReason;
            intra2.push_back({t.entryPrice,t.exitPrice,r,cur_bar_close});
            // whipsaw: SL/trail exit whose bar closed back ABOVE the stop level
            if (r.find("STOP")!=std::string::npos && cur_bar_close > t.exitPrice) ++whip2;
        };
        size_t ih=0;
        for (auto& b: bars) {
            while (ih < h1.size() && h1[ih].first <= b.ts) { eng.on_h1_close(h1[ih].second); ++ih; }
            cur_bar_close = b.c;
            eng.on_tick(b.l - half, b.l + half, b.ts*1000, cb);   // intra-bar SL check vs bar low
            eng.on_m30_bar(b.h,b.l,b.c, b.c-half, b.c+half, b.ts*1000, cb);
        }
    }

    // ------------------------------------------------------------------
    //  Reports
    // ------------------------------------------------------------------
    print_engine("ENGINE #1  MgcFastDonchian30m (Donchian channel-close exit)",
                 close1, intra1, cost, whip1);
    print_engine("ENGINE #2  GoldVolBreakoutM30 on MGC (SL dead -> MAX_HOLD only vs real SL)",
                 close2, intra2, cost, whip2);

    // exit-reason breakdown for #2
    auto reason_count = [](const std::vector<Trade>& v, const char* key){ int n=0; for(auto&t:v) if(t.reason.find(key)!=std::string::npos)++n; return n; };
    std::printf("\n  [#2 exit mix] CLOSE: MAX_HOLD=%d STOP=%d | INTRA: MAX_HOLD=%d STOP=%d\n",
                reason_count(close2,"MAX_HOLD"), reason_count(close2,"STOP"),
                reason_count(intra2,"MAX_HOLD"), reason_count(intra2,"STOP"));
    return 0;
}
