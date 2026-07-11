#pragma once
//  ADVERSE-PROTECTION: channel-exit by design -- backtested (the faithful re-BT PF1.74
//  2024-06..2026-06 that validated Nin40/Nout20 ran with EXACTLY this exit set: Donchian
//  Nout-low close-basis exit, NO SL/BE/trail/time-stop). Entry-side bear protection:
//  gold_regime() long-block + EMA100/slope trend filter (validated vs XAU 2022 bear) +
//  HVN overhead-supply skip + L2 gate. Residual risk = intrabar/overnight gap through the
//  channel (exit fills next 30m close); accepted at 1-MGC-micro size. Verdict per the
//  2026-06-17 swing-protection sweep class: a cold cut on a Donchian runner lowers net.
// =============================================================================
// =============================================================================
//  MgcFastDonchian30mEngine.hpp  (S-2026-06-03)
//  Fast intraday gold breakout on MGC (COMEX micro gold) 30m bars, with a
//  prior-day volume-profile OVERHEAD-SUPPLY gate. Runs entirely on the IBKR/MGC
//  feed (signal + volume from the same instrument) -> no spot/futures basis.
//
//  Signal : long when close > prior Nin-bar high (Donchian breakout)
//  Exit   : close < prior Nout-bar low
//  Gate   : skip the entry if a prior-day HVN (high-volume node) sits within
//           Kov*ATR ABOVE entry (overhead supply = little room). This gate is
//           the edge: backtest (MGC 30m, 2yr, cost-incl, WF) PF 1.36->1.54,
//           maxDD halved, rDD 2.4->5.1, both halves+. See memory
//           omega-gbb-indicators-eval / mgc_vp_backtest.cpp.
//
//  Feeds on REAL COMEX volume (the FIX/spot feed has none). Default shadow +
//  disabled; live activation requires the MGC 30m bar+volume feed wired into
//  Omega (separate integration) + a shadow period.
// =============================================================================
#include "OmegaTradeLedger.hpp"   // omega::TradeRecord
#include "OmegaCostGuard.hpp"     // ::ExecutionCostGuard (GLOBAL scope) -- S-2026-07-11 PHASE 1b
#include "RegimeState.hpp"        // 2026-06-24: gold_regime() price-bear gate (long-only engine)
#include "GoldTrendMimicLadder.hpp" // one-way mimic trigger (fire-and-forget on open)
#include <cstdint>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace omega {

struct MgcFastDonchian30mEngine {
    // ---- config ----
    bool   enabled       = false;   // wire in engine_init once the MGC feed exists
    bool   shadow_mode   = true;    // paper until validated
    double lot           = 0.01;
    int    Nin           = 20;      // breakout channel (prior bars)
    int    Nout          = 10;      // exit channel
    double Kov           = 1.5;     // overhead-supply skip distance (xATR)
    bool   use_hvn_skip  = true;    // the edge
    int    profile_bins  = 30;
    double hvn_frac      = 0.60;    // bins >= frac*max volume = HVN
    // ---- trend filter (S-2026-06-24): only take long breakouts in a confirmed uptrend.
    // close > EMA(EmaTrend) AND close > close[SlopeLB] (EMA-rising proxy, mirrors the
    // GoldVolBreakout trend_ gate). This is the bear protection MGC's 2024-26 data could
    // not test -- validated against XAU-spot 2022 bear (mgc_engines_audit, --bear). ----
    bool   use_trend_filter = true;
    int    EmaTrend         = 100;  // EMA period on 30m closes (~2 trading days)
    int    SlopeLB          = 20;   // rising proxy: close now > close SlopeLB bars ago
    double mgc_spread_pts   = 0.10; // 1 exchange tick (cost-guard input; same as MgcSlowDon)

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // ---- state ----
    struct Bar { double o,h,l,c,v; int day; };
    std::deque<Bar> bars_;               // recent bars for Donchian channels
    double  atr14_ = 0.0; double prev_close_ = 0.0; bool atr_init_ = false;
    double  ema_t_ = 0.0; bool ema_t_init_ = false;   // EMA trend filter state

    int     cur_day_ = -1;
    std::vector<std::pair<double,double>> day_cv_;   // (close,vol) for current day
    double  day_hi_ = -1e18, day_lo_ = 1e18;

    std::vector<double> prior_hvn_; double prior_poc_ = 0.0; bool prior_ok_ = false;
    // LIVE: HVN levels injected externally (from the daily mgc_volprofile producer)
    // instead of built from per-bar volume (the DOM price feed has no trade volume).
    // BACKTEST: leave false -> build internally from the volume fed to on_30m_bar.
    bool    hvn_external_ = false;

    bool    pos_active_ = false; double entry_ = 0.0; int64_t entry_ts_ = 0; double mfe_ = 0.0, mae_ = 0.0;

    // S-2026-06-23 L2 confirmation gate (forward-validating refinement). Live MGC
    // L2 imbalance pushed in via set_l2_imb each poll; block a long breakout when
    // the book is strongly ask-heavy (l2_imb_ < l2_gate_). l2_gate_=0 -> OFF.
    // Backtests never call set_l2_imb -> l2_imb_ stays 0.5 -> gate inert (PF1.74
    // reproduces exactly). The OBI is real-but-sub-cost standalone; as a filter on
    // this already-positive engine it should only cut the worst book-adverse entries.
    double  l2_imb_ = 0.5; double l2_gate_ = 0.0;
    void    set_l2_imb(double x) noexcept { l2_imb_ = x; }

    bool has_open_position() const { return pos_active_; }

    // LIVE injection of prior-day HVN levels (spot-adjusted if needed by caller).
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
        if ((int)bars_.size() < n) return 1e18;     // not warm -> no breakout
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
        tr.symbol     = "MGC";
        tr.engine     = "MgcFastDonchian30m";
        tr.side       = "LONG";
        tr.entryPrice = entry_;
        tr.exitPrice  = exit_px;
        tr.size       = lot;
        tr.pnl        = (exit_px - entry_) * lot;   // points*lot; mult applied downstream
        tr.entryTs    = entry_ts_;
        tr.exitTs     = ts;
        tr.shadow     = shadow_mode;
        tr.mfe        = mfe_; tr.mae = mae_;
        pos_active_ = false;
        if (cb) cb(tr);
    }

    // Call on each CLOSED 30m MGC bar (OHLCV with real volume).
    void on_30m_bar(double o, double h, double l, double c, double v,
                    int64_t ts_sec, OnCloseFn cb) {
        if (!enabled) return;
        const int day = (int)(ts_sec / 86400);
        if (day != cur_day_) {                  // day flip -> finalize prior profile
            if (cur_day_ >= 0 && !hvn_external_) _finalize_prior_profile();  // external HVN: caller injects
            cur_day_ = day; day_cv_.clear(); day_hi_ = -1e18; day_lo_ = 1e18;
        }
        day_cv_.push_back({c, v}); day_hi_ = std::max(day_hi_, h); day_lo_ = std::min(day_lo_, l);

        if (atr_init_) { double tr = std::max(h - l, std::max(std::fabs(h - prev_close_), std::fabs(l - prev_close_)));
                         atr14_ += (tr - atr14_) / 14.0; }
        else           { atr14_ = h - l; atr_init_ = true; }
        prev_close_ = c;
        // EMA trend filter update (on close)
        if (!ema_t_init_) { ema_t_ = c; ema_t_init_ = true; }
        else { const double a = 2.0 / (EmaTrend + 1); ema_t_ = a * c + (1.0 - a) * ema_t_; }

        if (pos_active_) {
            { double fav=h-entry_; if(fav>mfe_)mfe_=fav; double adv=entry_-l; if(adv>mae_)mae_=adv; }  // LONG excursion
            if (c < _chan_low(Nout)) _close(c, ts_sec, cb);
        } else {
            if (c > _chan_high(Nin)) {
                bool skip = false;
                // S-2026-06-24 PRICE-BEAR GATE: long-only Donchian breakout has no trend
                // filter -> would bleed in a gold bear. Block longs when gold_regime() is a
                // confirmed price-bear (fail-open until the regime brain is warm). Audit:
                // PF1.55 both-halves+ on MGC 2024-26 bull window, but that window has NO 2022
                // bear -> this gate is the bear protection the data couldn't test.
                if (omega::gold_regime().long_blocked()) skip = true;
                // TREND FILTER: only long in a confirmed uptrend (close>EMA + rising).
                if (use_trend_filter) {
                    const bool above_ema = ema_t_init_ && c > ema_t_;
                    const bool rising    = (int)bars_.size() >= SlopeLB && c > bars_[bars_.size() - SlopeLB].c;
                    if (!(above_ema && rising)) skip = true;
                }
                if (use_hvn_skip && prior_ok_)
                    for (double hv : prior_hvn_) if (hv > c && hv <= c + Kov * atr14_) { skip = true; break; }
                if (l2_gate_ > 0.0 && l2_imb_ < l2_gate_) { skip = true;
                    std::printf("[MGC-L2-GATE] MgcFastDon LONG skipped (l2_imb=%.2f < %.2f)\n", l2_imb_, l2_gate_); std::fflush(stdout); }
                // S-2026-07-11 GOLD PHASE 1b: ExecutionCostGuard on the explicit MGC
                // row ($10/pt/contract, $2.08 RT comm, 1-tick slip) -- FastDon was the
                // one MGC entry engine with NO cost gate (Phase-1 finding). Distance
                // proxy = the structural exit distance (close - Nout channel low),
                // the same role 3xATR plays for MgcSlowDon. Backtest-verified
                // NEAR-INERT on honest MGC costs: block threshold 0.612pt vs channel
                // widths >= ATR14 (min 8.4pt on certified 2024-26 data) -- 0 of 183
                // faithful-BT entries blocked (fastdon_runner pre==post identical).
                if (!skip) {
                    const double exit_dist = c - _chan_low(Nout);
                    if (!::ExecutionCostGuard::is_viable("MGC", mgc_spread_pts,
                                                         exit_dist, lot, 1.5)) skip = true;
                }
                if (!skip) { pos_active_ = true; entry_ = c; entry_ts_ = ts_sec; mfe_ = 0.0; mae_ = 0.0;
                    // one-way mimic notify (fire-and-forget; never reads/touches this position)
                    omega::gold_trend_mimic().on_trend_open("MgcFastDon", +1, c, ts_sec); }
            }
        }
        bars_.push_back({o,h,l,c,v,day});
        while ((int)bars_.size() > 256) bars_.pop_front();
        // SPECIFIC FEED: drive this engine's mimic book on the M30 bar (its backtested cadence).
        omega::gold_trend_mimic().on_bar("MgcFastDon", h, l, c, ts_sec);
    }
};

} // namespace omega
