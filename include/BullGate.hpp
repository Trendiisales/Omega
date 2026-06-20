#pragma once
// BullGate.hpp — shared, self-contained BULL/CHOP/BEAR regime gate.
//
// The validated 2026-06-20 classifier (see memory omega-bull-only-regime-gate-
// classifier). A bull-only engine queries `is_bull()` as a HARD entry-AND and
// goes/stays flat the moment it returns false — that is the operator's "trade the
// bull, prevent chop/bear" requirement made mechanical.
//
// Why not just RegimeState::is_bull()? That is close-only EMA200/EMA50 + sustained
// logic — it nails bull YEARS but labels CHOP (2018/2023) ~70-100% bull (both sit
// above a rising EMA200). The discriminator that separates bull from chop is
// ADX(14)>=18 (indices) + a realized-vol cap. This packages the full rule.
//
// Validated label behavior (daily): 2022 bear -> 3-7% bull (sits out), 2018/2023
// chop -> 27-51% bull (mostly out), 2024-26 bull -> 48-83% bull. 0-bar lag exiting
// BULL at tops, low whipsaw. Conservative by design (misses some bull rather than
// trade chop/bear).
//
// Feed DAILY bars: bg.on_daily_bar(o,h,l,c). Gold drops ADX (gold trends at low
// ADX); indices use ADX>=18.
#include <deque>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>

namespace omega {

struct BullGate {
    // ---- config ----
    bool   is_index   = true;     // indices use ADX gate; gold (false) drops it
    int    EMA_FAST   = 50;
    int    EMA_SLOW   = 200;
    int    SLOPE_BARS = 20;       // EMA200 rising/falling over this many bars
    int    ADX_LEN    = 14;
    double ADX_MIN    = 18.0;     // chop-killer (indices)
    int    RV_LEN     = 20;       // realized-vol window (bars)
    int    RV_PCT_WIN = 252;      // trailing window for the vol percentile
    double RV_PCT_MAX = 0.92;     // veto when 20d-vol is in the top 8%
    std::string name  = "IDX";

    // ---- state ----
    double emaF_ = 0, emaS_ = 0; bool have_ema_ = false;
    std::deque<double> emaS_hist_;            // for slope
    std::deque<double> closes_;               // for returns
    // ADX (Wilder)
    double prev_close_ = 0, prev_high_ = 0, prev_low_ = 0; bool have_prev_ = false;
    double atr_ = 0, adm_p_ = 0, adm_m_ = 0; bool adx_seed_ = false;
    double adx_ = 0; bool adx_ready_ = false; int adx_n_ = 0;
    // realized-vol percentile
    std::deque<double> rv_hist_;
    long bars_ = 0;
    int regime_ = 0;                          // -1 bear, 0 chop, +1 bull

    bool warm() const noexcept { return bars_ >= EMA_SLOW + SLOPE_BARS; }
    bool is_bull() const noexcept { return regime_ == +1; }
    bool is_bear() const noexcept { return regime_ == -1; }
    bool is_chop() const noexcept { return regime_ == 0 && warm(); }
    double adx() const noexcept { return adx_; }
    const char* state() const noexcept { return regime_>0?"BULL":(regime_<0?"BEAR":"CHOP"); }

    void on_daily_bar(double /*o*/, double h, double l, double c) noexcept {
        const double kF = 2.0/(EMA_FAST+1), kS = 2.0/(EMA_SLOW+1);
        if (!have_ema_) { emaF_ = emaS_ = c; have_ema_ = true; }
        else { emaF_ += kF*(c-emaF_); emaS_ += kS*(c-emaS_); }
        emaS_hist_.push_back(emaS_);
        while ((int)emaS_hist_.size() > SLOPE_BARS+1) emaS_hist_.pop_front();

        // ADX (Wilder) — needs prior bar
        if (have_prev_) {
            const double up = h - prev_high_, dn = prev_low_ - l;
            const double pDM = (up > dn && up > 0) ? up : 0.0;
            const double mDM = (dn > up && dn > 0) ? dn : 0.0;
            const double tr = std::max(h-l, std::max(std::fabs(h-prev_close_), std::fabs(l-prev_close_)));
            if (!adx_seed_) { atr_ = tr; adm_p_ = pDM; adm_m_ = mDM; adx_seed_ = true; }
            else {
                atr_   = atr_   - atr_/ADX_LEN   + tr;
                adm_p_ = adm_p_ - adm_p_/ADX_LEN + pDM;
                adm_m_ = adm_m_ - adm_m_/ADX_LEN + mDM;
            }
            if (atr_ > 0) {
                const double diP = 100.0*adm_p_/atr_, diM = 100.0*adm_m_/atr_;
                const double dx = (diP+diM)>0 ? 100.0*std::fabs(diP-diM)/(diP+diM) : 0.0;
                if (!adx_ready_) { adx_ = dx; if (++adx_n_ >= ADX_LEN) adx_ready_ = true; }
                else adx_ = (adx_*(ADX_LEN-1) + dx)/ADX_LEN;
            }
        }
        prev_high_ = h; prev_low_ = l; prev_close_ = c; have_prev_ = true;

        // realized-vol percentile (stdev of last RV_LEN log-returns)
        closes_.push_back(c);
        while ((int)closes_.size() > RV_LEN+2) closes_.pop_front();
        double rv = 0;
        if ((int)closes_.size() >= RV_LEN+1) {
            std::vector<double> r; r.reserve(RV_LEN);
            for (int i=(int)closes_.size()-RV_LEN; i<(int)closes_.size(); ++i)
                if (closes_[i-1] > 0) r.push_back(std::log(closes_[i]/closes_[i-1]));
            if (!r.empty()) {
                double m=0; for(double x:r) m+=x; m/=r.size();
                double v=0; for(double x:r) v+=(x-m)*(x-m); rv = std::sqrt(v/r.size());
            }
        }
        rv_hist_.push_back(rv);
        while ((int)rv_hist_.size() > RV_PCT_WIN) rv_hist_.pop_front();
        double rv_pct = 0.0;
        if ((int)rv_hist_.size() >= RV_LEN) {
            int below=0; for(double x:rv_hist_) if (x <= rv) ++below;
            rv_pct = (double)below/rv_hist_.size();
        }

        // ---- classify ----
        ++bars_;
        regime_ = 0;
        if (warm() && (int)emaS_hist_.size() >= SLOPE_BARS+1) {
            const double emaS_old = emaS_hist_.front();
            const bool rising  = emaS_ > emaS_old;
            const bool falling = emaS_ < emaS_old;
            const bool vol_ok  = rv_pct < RV_PCT_MAX;
            const bool adx_ok  = (!is_index) || (adx_ready_ && adx_ >= ADX_MIN);
            if (c > emaS_ && rising && emaF_ > emaS_ && adx_ok && vol_ok) regime_ = +1;       // BULL
            else if (c < emaS_ && falling && emaF_ < emaS_)              regime_ = -1;        // BEAR
            // else CHOP (0)
        }
    }
};

// ---- shared singletons (RegimeState pattern) — feed daily bars, query is_bull() ----
//   In a gold engine:  omega::bull_gate_gold().on_daily_bar(o,h,l,c);
//                      if (!omega::bull_gate_gold().is_bull()) return;   // flat in chop/bear
inline BullGate& bull_gate_gold() noexcept {
    static BullGate g = []{ BullGate b; b.is_index=false; b.name="XAU"; return b; }();
    return g;
}
inline BullGate& bull_gate_index() noexcept {
    static BullGate g = []{ BullGate b; b.is_index=true; b.name="IDX"; return b; }();
    return g;
}

} // namespace omega
