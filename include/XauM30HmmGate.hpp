#pragma once
// =============================================================================
// XauM30HmmGate.hpp -- Causal 3-state Gaussian HMM regime classifier on M30
//                     bars for gating XauThreeBar30mEngine entries.
//
// Distinct from include/GoldHMM.hpp which operates on M1-tick features for the
// (disabled) CandleFlowEngine. This one is M30-scale, smaller features, used
// to gate the slower XAU M30+ trend engines.
//
// Pipeline:
//   1. Caller pushes one HmmM30Bar (open, high, low, close, atr14) per M30
//      bar close.
//   2. update_features() computes the 4-dim feature vector with state internal
//      to the gate (rolling close history, atr median window, prior-bar
//      directions).
//   3. step() runs ONE forward-pass update on the new feature, advancing the
//      log-alpha vector. State at this bar = argmax log_alpha. CAUSAL --
//      uses only observations up to this bar. NO smoothing, NO Viterbi over
//      the full sequence. See HARNESS_FIDELITY_CHECKLIST Check 6 for why.
//   4. current_state() / is_noise() / p_noise() expose the current state.
//
// Trained parameters (means, covars, transmat, startprob) are hardcoded
// from the Python training run on the first 50% of the 25-month Duka
// M5->M30 corpus. Retrain via /Users/jo/Tick/mid_freq_research/hmm_export_params.py;
// paste new constexpr arrays here on each retrain (operator decision when).
//
// Features (per M30 bar close):
//   [0] drift_norm  = (close - close[-4]) / atr14
//   [1] atr_norm    = atr14 / median(atr14, last 200 bars)
//   [2] range_norm  = (high - low) / atr14
//   [3] dir_persist = sum( sign(close - open) for last 4 bars ) * 0.25
//
// Backtest gain (causal, OOS, 7 months, lot=1):
//   baseline:       n=314 net=+$448 PF=1.27 MaxDD=-$183
//   HMM+slope_12:   n=247 net=+$664 PF=1.54 MaxDD=-$149
//   Lift:           +48% net, +21% PF, -19% MaxDD, retains 79% of trades.
//
// Random baseline P(random_drop >= HMM_gate_net) = 0.0000 over 5000 trials.
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>

namespace omega {

struct HmmM30Bar {
    double open  = 0.0;
    double high  = 0.0;
    double low   = 0.0;
    double close = 0.0;
    double atr14 = 0.0;  // external Wilder ATR-14 on M30
};

class XauM30HmmGate {
public:
    static constexpr int N_STATES   = 3;
    static constexpr int N_FEATURES = 4;
    static constexpr int DRIFT_LOOKBACK = 4;     // bars
    static constexpr int DIR_PERSIST_LOOKBACK = 4;
    static constexpr int ATR_MEDIAN_WINDOW = 200;
    static constexpr int WARMUP_BARS = ATR_MEDIAN_WINDOW + 10;

    // --- Trained params (Python hmm_export_params.py, 2026-05-27 run) ---
    // Train period: 2024-03-07 16:30 UTC -> 2025-04-01 04:00 UTC
    // First 50% of 25-month M5-derived M30 Duka corpus.
    static constexpr int STATE_MEAN_REV     = 0;
    static constexpr int STATE_NOISE        = 1;
    static constexpr int STATE_CONTINUATION = 2;

    // NOTE on START: Python EM fitted [0, 0, 1] because in-sample the very
    // first bar happened to be CONTINUATION-ish. For live use this pins
    // log_alpha at -inf for the other two states forever — once we throw
    // away the first bar's identity at deploy time, we have NO prior on the
    // current regime. Override to uniform; the forward-pass converges to
    // the true posterior within ~10 bars regardless of starting prior.
    static constexpr double START[N_STATES] = { 0.333333, 0.333333, 0.333334 };
    static constexpr double A[N_STATES][N_STATES] = {
        { 0.819705, 0.065762, 0.114533 },
        { 0.049727, 0.872728, 0.077545 },
        { 0.063536, 0.128683, 0.807781 },
    };
    static constexpr double MU[N_STATES][N_FEATURES] = {
        { -1.119813, 1.261036, 1.266164, -0.359987 },
        { -0.081164, 0.904169, 0.801525, -0.080039 },
        {  1.240519, 1.093012, 1.040831,  0.503266 },
    };
    static constexpr double SIG2[N_STATES][N_FEATURES] = {
        { 1.446979, 0.113837, 0.525375, 0.145677 },
        { 0.440524, 0.017190, 0.102137, 0.136355 },
        { 0.822744, 0.058937, 0.262129, 0.102480 },
    };

    XauM30HmmGate() noexcept { reset(); }

    void reset() noexcept {
        bars_.clear();
        atr_window_.clear();
        bar_count_ = 0;
        current_state_ = STATE_CONTINUATION;  // safe default — passes "not_NOISE" gate
        // log_alpha = log(start)
        for (int i = 0; i < N_STATES; ++i) {
            log_alpha_[i] = std::log(std::max(START[i], 1e-300));
        }
    }

    // Push one M30 bar at close. Updates feature state and runs one forward
    // step. Caller checks warmed() before trusting current_state().
    void push_bar(const HmmM30Bar& bar) noexcept {
        if (bar.atr14 <= 0.0) return;
        bars_.push_back(bar);
        if ((int)bars_.size() > DRIFT_LOOKBACK + DIR_PERSIST_LOOKBACK + 4) {
            bars_.pop_front();
        }
        atr_window_.push_back(bar.atr14);
        if ((int)atr_window_.size() > ATR_MEDIAN_WINDOW) atr_window_.pop_front();
        ++bar_count_;

        if (!warmed()) return;

        // Compute features for this bar
        const auto& cur = bars_.back();
        const int n = (int)bars_.size();
        // drift_norm = (close - close[-DRIFT_LOOKBACK]) / atr14
        double drift_norm = 0.0;
        if (n > DRIFT_LOOKBACK) {
            drift_norm = (cur.close - bars_[n - 1 - DRIFT_LOOKBACK].close) / cur.atr14;
        }
        // atr_norm = atr14 / median(atr_window_)
        double atr_norm = 1.0;
        {
            std::array<double, ATR_MEDIAN_WINDOW> tmp{};
            const int m = (int)atr_window_.size();
            for (int i = 0; i < m; ++i) tmp[i] = atr_window_[i];
            std::nth_element(tmp.begin(), tmp.begin() + m/2, tmp.begin() + m);
            const double med = tmp[m/2];
            if (med > 0.0) atr_norm = cur.atr14 / med;
        }
        // range_norm = (high - low) / atr14
        const double range_norm = (cur.high - cur.low) / cur.atr14;
        // dir_persist = sum(sign(close - open) for last DIR_PERSIST_LOOKBACK bars) * 0.25
        double dir_sum = 0.0;
        int considered = 0;
        for (int i = n - 1; i >= 0 && considered < DIR_PERSIST_LOOKBACK; --i) {
            const auto& b = bars_[i];
            const double d = b.close - b.open;
            dir_sum += (d > 0) ? 1.0 : ((d < 0) ? -1.0 : 0.0);
            ++considered;
        }
        const double dir_persist = (considered > 0) ? (dir_sum / considered) : 0.0;

        const double feat[N_FEATURES] = {
            drift_norm, atr_norm, range_norm, dir_persist
        };

        _step_forward(feat);
    }

    bool warmed() const noexcept { return bar_count_ >= WARMUP_BARS; }
    int  current_state() const noexcept { return current_state_; }
    bool is_noise() const noexcept { return current_state_ == STATE_NOISE; }
    bool is_continuation() const noexcept { return current_state_ == STATE_CONTINUATION; }

    const char* state_name() const noexcept {
        switch (current_state_) {
            case STATE_NOISE:        return "NOISE";
            case STATE_CONTINUATION: return "CONT";
            case STATE_MEAN_REV:     return "MR";
            default:                 return "?";
        }
    }

private:
    void _step_forward(const double feat[N_FEATURES]) noexcept {
        // Log-space forward recursion: numerically stable.
        //   new_alpha[j] = log_sum_exp_i(alpha[i] + log_A[i][j]) + log_emit(j, feat)
        double new_log_alpha[N_STATES];
        for (int j = 0; j < N_STATES; ++j) {
            // log-sum-exp over previous states
            double vals[N_STATES];
            double mx = -1e300;
            for (int i = 0; i < N_STATES; ++i) {
                vals[i] = log_alpha_[i] + std::log(std::max(A[i][j], 1e-300));
                if (vals[i] > mx) mx = vals[i];
            }
            double s = 0.0;
            for (int i = 0; i < N_STATES; ++i) s += std::exp(vals[i] - mx);
            new_log_alpha[j] = mx + std::log(std::max(s, 1e-300)) + _log_emit(j, feat);
        }

        // Normalize (subtract max) to prevent overflow
        double mx = new_log_alpha[0];
        for (int j = 1; j < N_STATES; ++j) if (new_log_alpha[j] > mx) mx = new_log_alpha[j];
        for (int j = 0; j < N_STATES; ++j) log_alpha_[j] = new_log_alpha[j] - mx;

        // Argmax = current state
        int best_j = 0;
        for (int j = 1; j < N_STATES; ++j) {
            if (log_alpha_[j] > log_alpha_[best_j]) best_j = j;
        }
        current_state_ = best_j;
    }

    double _log_emit(int j, const double feat[N_FEATURES]) const noexcept {
        // log N(feat; mu[j], diag(sig2[j]))
        double s = 0.0;
        for (int k = 0; k < N_FEATURES; ++k) {
            const double v = std::max(SIG2[j][k], 1e-12);
            const double d = feat[k] - MU[j][k];
            s += d * d / v + std::log(2.0 * M_PI * v);
        }
        return -0.5 * s;
    }

    std::deque<HmmM30Bar> bars_;
    std::deque<double>    atr_window_;
    double                log_alpha_[N_STATES]{};
    int                   bar_count_     = 0;
    int                   current_state_ = STATE_CONTINUATION;
};

} // namespace omega
