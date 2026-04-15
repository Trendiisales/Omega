#pragma once
// =============================================================================
// GoldHMM.hpp -- Inline 3-state Gaussian HMM for CandleFlowEngine regime gating
//
// Problem: CFE fires on continuation signals (drift spike + RSI + expansion
// candle) but regime-blindly. The same signal in MEAN_REVERSION or NOISE
// regime loses money. This HMM classifies which regime is currently active
// and gates CFE entries accordingly.
//
// States:
//   CONTINUATION (0) -- strong drift, expanding ATR, fast tape, sustained
//                       directional pressure. CFE fires normally.
//   MEAN_REVERSION (1) -- drift spikes then fades, moderate ATR, RSI
//                        churning. CFE blocked -- fade setups not scalps.
//   NOISE (2)         -- near-zero drift, low ATR, slow tape (Asia).
//                        CFE blocked -- no edge.
//
// Features (6-dimensional, computed per M1 bar close):
//   [0] drift_norm      -- |ewm_drift| / atr, clamped [0, 2]
//   [1] atr_norm        -- atr / 5.0, clamped [0, 2]
//   [2] rsi_trend_norm  -- |rsi_trend| / 12.0, clamped [0, 1]
//   [3] tick_rate_norm  -- tick_rate / 8.0, clamped [0, 1]
//   [4] bar_range_ratio -- (bar.high - bar.low) / atr, clamped [0, 3]
//   [5] drift_dur_norm  -- drift_sustained_ms / 90000.0, clamped [0, 2]
//
// Algorithm: Online EM (Baum-Welch variant).
//   - update() called at every M1 bar close with the feature vector
//   - Maintains rolling window of last WINDOW observations
//   - Re-runs forward-backward every UPDATE_INTERVAL bars (every 5 M1 bars)
//   - Updates transition matrix A and emission parameters (mu, sigma²)
//   - Warmup: HMM_WARMUP_BARS bars before gating activates (default 60)
//
// Initial parameters seeded from backtest evidence:
//   - Asia (22:00-07:00 UTC) shows NOISE dominance (sustained-drift disabled)
//   - London/NY (07:00-22:00 UTC) shows CONTINUATION edge (71.4% WR at opt params)
//   - MEAN_REVERSION identified in HIGH_DRIFT + FALLING_ACCEL analysis
//   - Transition priors reflect session length: NOISE sticky (9h Asia runs)
//
// Fail-open: if !warmed(), p_continuation() returns 1.0 (no gating).
// Thread safety: not thread-safe. Called only from the single tick thread.
//
// Usage in CandleFlowEngine:
//   m_hmm.update(feat);            // at M1 bar close
//   if (m_hmm.warmed() && m_hmm.p_continuation(feat) < HMM_MIN_PROB) return;
// =============================================================================

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <algorithm>

namespace omega {

// =============================================================================
// Constants
// =============================================================================
static constexpr int    HMM_STATES         = 3;
static constexpr int    HMM_FEATURES       = 6;
static constexpr int    HMM_WARMUP_BARS    = 60;    // bars before gating active
static constexpr int    HMM_WINDOW         = 120;   // rolling observation window
static constexpr int    HMM_UPDATE_INTERVAL= 5;     // re-run EM every N bars
static constexpr double HMM_MIN_PROB       = 0.55;  // min P(CONTINUATION) to allow entry
static constexpr double HMM_MIN_SIGMA2     = 1e-4;  // floor on variance (numerical stability)
static constexpr double HMM_ALPHA          = 0.05;  // EM learning rate for online update

// State indices
static constexpr int HMM_CONTINUATION   = 0;
static constexpr int HMM_MEAN_REVERSION = 1;
static constexpr int HMM_NOISE          = 2;

// =============================================================================
// HmmFeature -- one observation (one M1 bar close)
// =============================================================================
struct HmmFeature {
    double v[HMM_FEATURES] = {};

    // Construct from CFE live inputs
    // drift:      ewm_drift passed to on_tick()
    // atr:        atr_pts passed to on_tick()
    // rsi_trend:  m_rsi_trend (internal CFE member)
    // tick_rate:  g_bars_gold.m1.ind.tick_rate (from OHLCBarEngine)
    // bar_range:  bar.high - bar.low (BarSnap)
    // drift_dur_ms: drift_sustained_ms computed in on_tick()
    static HmmFeature make(double drift, double atr,
                           double rsi_trend, double tick_rate,
                           double bar_range, int64_t drift_dur_ms) noexcept
    {
        HmmFeature f;
        const double atr_safe = (atr > 0.0) ? atr : 3.0;
        f.v[0] = std::min(2.0, std::fabs(drift)     / atr_safe);
        f.v[1] = std::min(2.0, atr_safe              / 5.0);
        f.v[2] = std::min(1.0, std::fabs(rsi_trend)  / 12.0);
        f.v[3] = std::min(1.0, tick_rate             / 8.0);
        f.v[4] = std::min(3.0, (atr_safe > 0.0 ? bar_range / atr_safe : 0.0));
        f.v[5] = std::min(2.0, static_cast<double>(drift_dur_ms) / 90000.0);
        return f;
    }
};

// =============================================================================
// GoldHMM
// =============================================================================
class GoldHMM {
public:
    GoldHMM() { _init_priors(); }

    // -------------------------------------------------------------------------
    // update() -- call at every M1 bar close with current feature vector.
    // Adds observation to window, re-runs EM every HMM_UPDATE_INTERVAL calls.
    // -------------------------------------------------------------------------
    void update(const HmmFeature& feat) noexcept {
        obs_.push_back(feat);
        if ((int)obs_.size() > HMM_WINDOW)
            obs_.pop_front();
        ++bar_count_;

        if (bar_count_ >= HMM_WARMUP_BARS &&
            (bar_count_ % HMM_UPDATE_INTERVAL == 0) &&
            (int)obs_.size() >= HMM_UPDATE_INTERVAL)
        {
            _run_em();
        }
    }

    // -------------------------------------------------------------------------
    // p_continuation() -- probability of being in CONTINUATION state.
    // Returns 1.0 if not warmed (fail-open).
    // -------------------------------------------------------------------------
    double p_continuation(const HmmFeature& feat) const noexcept {
        if (!warmed()) return 1.0;

        // Forward pass: one step using current transition matrix and priors
        // Use last smoothed state distribution as prior
        double alpha[HMM_STATES];
        double total = 0.0;
        for (int j = 0; j < HMM_STATES; ++j) {
            // Sum over previous states: alpha[j] = sum_i(pi[i] * A[i][j]) * B[j](feat)
            double sum = 0.0;
            for (int i = 0; i < HMM_STATES; ++i)
                sum += pi_[i] * A_[i][j];
            alpha[j] = sum * _emit(j, feat);
            total += alpha[j];
        }
        if (total <= 0.0) return 1.0;  // degenerate -- fail-open

        const double p = alpha[HMM_CONTINUATION] / total;
        return std::isfinite(p) ? p : 1.0;
    }

    bool warmed() const noexcept { return bar_count_ >= HMM_WARMUP_BARS; }
    int  bar_count() const noexcept { return bar_count_; }

    // Current most-likely state (Viterbi on last observation)
    int current_state(const HmmFeature& feat) const noexcept {
        if (!warmed()) return HMM_CONTINUATION;
        double best = -1.0;
        int    best_s = HMM_CONTINUATION;
        for (int j = 0; j < HMM_STATES; ++j) {
            double sum = 0.0;
            for (int i = 0; i < HMM_STATES; ++i)
                sum += pi_[i] * A_[i][j];
            const double v = sum * _emit(j, feat);
            if (v > best) { best = v; best_s = j; }
        }
        return best_s;
    }

    static const char* state_name(int s) noexcept {
        if (s == HMM_CONTINUATION)   return "CONTINUATION";
        if (s == HMM_MEAN_REVERSION) return "MEAN_REV";
        return "NOISE";
    }

    // Log current parameters (call hourly or on state change)
    void log_params(int64_t now_ms) const noexcept {
        static int64_t s_last = 0;
        if (now_ms - s_last < 3600000LL) return;
        s_last = now_ms;
        printf("[HMM] bars=%d warmed=%d  A: C->C=%.2f M->M=%.2f N->N=%.2f\n",
               bar_count_, warmed() ? 1 : 0,
               A_[HMM_CONTINUATION][HMM_CONTINUATION],
               A_[HMM_MEAN_REVERSION][HMM_MEAN_REVERSION],
               A_[HMM_NOISE][HMM_NOISE]);
        printf("[HMM] mu_drift: C=%.2f MR=%.2f N=%.2f\n",
               mu_[HMM_CONTINUATION][0],
               mu_[HMM_MEAN_REVERSION][0],
               mu_[HMM_NOISE][0]);
        fflush(stdout);
    }

private:
    // -------------------------------------------------------------------------
    // Model parameters
    // -------------------------------------------------------------------------
    double A_[HMM_STATES][HMM_STATES] = {};   // transition matrix
    double pi_[HMM_STATES]            = {};   // current state distribution
    double mu_[HMM_STATES][HMM_FEATURES]    = {};  // emission means
    double sig2_[HMM_STATES][HMM_FEATURES]  = {};  // emission variances

    std::deque<HmmFeature> obs_;
    int bar_count_ = 0;

    // -------------------------------------------------------------------------
    // Seeded priors -- calibrated from backtest evidence
    // -------------------------------------------------------------------------
    void _init_priors() noexcept {
        // Transition matrix:
        //   CONTINUATION sticky (London/NY runs hours)
        //   NOISE very sticky (Asia = 9h continuous)
        //   MEAN_REVERSION moderately sticky
        A_[HMM_CONTINUATION][HMM_CONTINUATION]   = 0.70;
        A_[HMM_CONTINUATION][HMM_MEAN_REVERSION] = 0.20;
        A_[HMM_CONTINUATION][HMM_NOISE]          = 0.10;

        A_[HMM_MEAN_REVERSION][HMM_CONTINUATION]   = 0.25;
        A_[HMM_MEAN_REVERSION][HMM_MEAN_REVERSION] = 0.55;
        A_[HMM_MEAN_REVERSION][HMM_NOISE]          = 0.20;

        A_[HMM_NOISE][HMM_CONTINUATION]   = 0.15;
        A_[HMM_NOISE][HMM_MEAN_REVERSION] = 0.20;
        A_[HMM_NOISE][HMM_NOISE]          = 0.65;

        // Initial state distribution:
        // Noise slightly dominant -- gold is 24h and Asia = 9/24 hours
        pi_[HMM_CONTINUATION]   = 0.30;
        pi_[HMM_MEAN_REVERSION] = 0.30;
        pi_[HMM_NOISE]          = 0.40;

        // Emission means per state per feature:
        // Feature order: drift_norm, atr_norm, rsi_trend_norm,
        //                tick_rate_norm, bar_range_ratio, drift_dur_norm

        // CONTINUATION: high drift, medium-high ATR, trending RSI,
        //               fast tape, expanding candles, sustained drift
        mu_[HMM_CONTINUATION][0] = 0.60;  // drift_norm high
        mu_[HMM_CONTINUATION][1] = 0.70;  // atr_norm medium-high
        mu_[HMM_CONTINUATION][2] = 0.50;  // rsi_trend_norm medium
        mu_[HMM_CONTINUATION][3] = 0.60;  // tick_rate_norm medium-high
        mu_[HMM_CONTINUATION][4] = 0.70;  // bar_range_ratio expanding
        mu_[HMM_CONTINUATION][5] = 0.60;  // drift_dur_norm sustained

        // MEAN_REVERSION: moderate drift (spike then fade), choppy RSI,
        //                 medium ATR, medium tape, inconsistent range
        mu_[HMM_MEAN_REVERSION][0] = 0.50;  // drift_norm moderate
        mu_[HMM_MEAN_REVERSION][1] = 0.60;  // atr_norm medium
        mu_[HMM_MEAN_REVERSION][2] = 0.40;  // rsi_trend_norm choppy
        mu_[HMM_MEAN_REVERSION][3] = 0.50;  // tick_rate_norm medium
        mu_[HMM_MEAN_REVERSION][4] = 0.50;  // bar_range_ratio mixed
        mu_[HMM_MEAN_REVERSION][5] = 0.30;  // drift_dur_norm short

        // NOISE: near-zero drift, low ATR, weak RSI, slow tape, small candles
        mu_[HMM_NOISE][0] = 0.10;  // drift_norm near zero
        mu_[HMM_NOISE][1] = 0.30;  // atr_norm low
        mu_[HMM_NOISE][2] = 0.10;  // rsi_trend_norm weak
        mu_[HMM_NOISE][3] = 0.20;  // tick_rate_norm slow
        mu_[HMM_NOISE][4] = 0.30;  // bar_range_ratio narrow
        mu_[HMM_NOISE][5] = 0.10;  // drift_dur_norm brief

        // Emission variances (diagonal Gaussian)
        // CONTINUATION: tighter -- it's a well-defined regime
        for (int f = 0; f < HMM_FEATURES; ++f) sig2_[HMM_CONTINUATION][f]   = 0.08;
        // MEAN_REVERSION: wider -- choppy, less consistent
        for (int f = 0; f < HMM_FEATURES; ++f) sig2_[HMM_MEAN_REVERSION][f] = 0.15;
        // NOISE: tight on drift/atr (consistently low), wider on others
        for (int f = 0; f < HMM_FEATURES; ++f) sig2_[HMM_NOISE][f]          = 0.08;
        sig2_[HMM_NOISE][2] = 0.10;  // RSI slightly wider in noise
        sig2_[HMM_NOISE][3] = 0.10;  // tick rate slightly wider
    }

    // -------------------------------------------------------------------------
    // Gaussian emission probability: product of univariate Gaussians
    // Returns log-probability to avoid underflow, exponentiated at end
    // -------------------------------------------------------------------------
    double _emit(int s, const HmmFeature& x) const noexcept {
        double log_p = 0.0;
        for (int f = 0; f < HMM_FEATURES; ++f) {
            const double diff  = x.v[f] - mu_[s][f];
            const double sig2  = std::max(HMM_MIN_SIGMA2, sig2_[s][f]);
            // log N(x; mu, sigma2) = -0.5*log(2*pi*sigma2) - 0.5*(x-mu)^2/sigma2
            // 2*pi hardcoded: M_PI not defined in MSVC without _USE_MATH_DEFINES
            static constexpr double TWO_PI = 6.28318530717958647692;
            log_p += -0.5 * std::log(TWO_PI * sig2)
                     -0.5 * (diff * diff) / sig2;
        }
        const double p = std::exp(log_p);
        return std::isfinite(p) && p > 0.0 ? p : 1e-300;
    }

    // -------------------------------------------------------------------------
    // _run_em() -- one pass of forward-backward + M-step on rolling window
    // Online EM: parameters updated as weighted average with learning rate alpha
    // -------------------------------------------------------------------------
    void _run_em() noexcept {
        const int T = static_cast<int>(obs_.size());
        if (T < 2) return;

        // ── Forward pass ─────────────────────────────────────────────────────
        // alpha[t][s] = P(o_1..o_t, q_t=s)
        // Use scaled version to prevent underflow
        std::vector<std::array<double, HMM_STATES>> alpha(T);
        std::vector<double> scale(T, 0.0);

        // t=0
        for (int s = 0; s < HMM_STATES; ++s) {
            alpha[0][s] = pi_[s] * _emit(s, obs_[0]);
            scale[0] += alpha[0][s];
        }
        if (scale[0] <= 0.0) return;
        for (int s = 0; s < HMM_STATES; ++s) alpha[0][s] /= scale[0];

        // t=1..T-1
        for (int t = 1; t < T; ++t) {
            scale[t] = 0.0;
            for (int j = 0; j < HMM_STATES; ++j) {
                double sum = 0.0;
                for (int i = 0; i < HMM_STATES; ++i)
                    sum += alpha[t-1][i] * A_[i][j];
                alpha[t][j] = sum * _emit(j, obs_[t]);
                scale[t] += alpha[t][j];
            }
            if (scale[t] <= 0.0) return;
            for (int j = 0; j < HMM_STATES; ++j) alpha[t][j] /= scale[t];
        }

        // ── Backward pass ─────────────────────────────────────────────────────
        // beta[t][s] = P(o_{t+1}..o_T | q_t=s), scaled
        std::vector<std::array<double, HMM_STATES>> beta(T);
        for (int s = 0; s < HMM_STATES; ++s) beta[T-1][s] = 1.0;

        for (int t = T-2; t >= 0; --t) {
            double sc = 0.0;
            for (int i = 0; i < HMM_STATES; ++i) {
                beta[t][i] = 0.0;
                for (int j = 0; j < HMM_STATES; ++j)
                    beta[t][i] += A_[i][j] * _emit(j, obs_[t+1]) * beta[t+1][j];
                sc += beta[t][i];
            }
            if (sc <= 0.0) return;
            for (int i = 0; i < HMM_STATES; ++i) beta[t][i] /= sc;
        }

        // ── E-step: compute gamma (state occupancy) ────────────────────────
        // gamma[t][s] = P(q_t=s | O)
        std::vector<std::array<double, HMM_STATES>> gamma(T);
        for (int t = 0; t < T; ++t) {
            double tot = 0.0;
            for (int s = 0; s < HMM_STATES; ++s) {
                gamma[t][s] = alpha[t][s] * beta[t][s];
                tot += gamma[t][s];
            }
            if (tot <= 0.0) return;
            for (int s = 0; s < HMM_STATES; ++s) gamma[t][s] /= tot;
        }

        // xi[t][i][j] = P(q_t=i, q_{t+1}=j | O)
        // Compute on-the-fly for A update to save memory
        double A_new[HMM_STATES][HMM_STATES] = {};
        double A_denom[HMM_STATES] = {};

        for (int t = 0; t < T-1; ++t) {
            const double denom_xi = [&]() {
                double d = 0.0;
                for (int i = 0; i < HMM_STATES; ++i)
                    for (int j = 0; j < HMM_STATES; ++j)
                        d += alpha[t][i] * A_[i][j] * _emit(j, obs_[t+1]) * beta[t+1][j];
                return d;
            }();
            if (denom_xi <= 0.0) continue;
            for (int i = 0; i < HMM_STATES; ++i) {
                for (int j = 0; j < HMM_STATES; ++j) {
                    const double xi_tij = alpha[t][i] * A_[i][j]
                                        * _emit(j, obs_[t+1]) * beta[t+1][j]
                                        / denom_xi;
                    A_new[i][j]  += xi_tij;
                }
                A_denom[i] += gamma[t][i];
            }
        }

        // ── M-step: update parameters with online EM (weighted average) ──────

        // Update transition matrix A
        for (int i = 0; i < HMM_STATES; ++i) {
            if (A_denom[i] <= 0.0) continue;
            double row_sum = 0.0;
            double A_hat[HMM_STATES];
            for (int j = 0; j < HMM_STATES; ++j) {
                A_hat[j] = A_new[i][j] / A_denom[i];
                row_sum += A_hat[j];
            }
            if (row_sum <= 0.0) continue;
            // Online update: blend toward new estimate
            for (int j = 0; j < HMM_STATES; ++j) {
                const double a_hat_norm = A_hat[j] / row_sum;
                A_[i][j] = (1.0 - HMM_ALPHA) * A_[i][j] + HMM_ALPHA * a_hat_norm;
            }
            // Renormalise row
            double rs = 0.0;
            for (int j = 0; j < HMM_STATES; ++j) rs += A_[i][j];
            if (rs > 0.0) for (int j = 0; j < HMM_STATES; ++j) A_[i][j] /= rs;
        }

        // Update emission means and variances
        for (int s = 0; s < HMM_STATES; ++s) {
            double gamma_sum = 0.0;
            for (int t = 0; t < T; ++t) gamma_sum += gamma[t][s];
            if (gamma_sum <= 0.0) continue;

            double mu_hat[HMM_FEATURES]   = {};
            double sig2_hat[HMM_FEATURES] = {};

            for (int t = 0; t < T; ++t) {
                for (int f = 0; f < HMM_FEATURES; ++f)
                    mu_hat[f] += gamma[t][s] * obs_[t].v[f];
            }
            for (int f = 0; f < HMM_FEATURES; ++f) mu_hat[f] /= gamma_sum;

            for (int t = 0; t < T; ++t) {
                for (int f = 0; f < HMM_FEATURES; ++f) {
                    const double diff = obs_[t].v[f] - mu_hat[f];
                    sig2_hat[f] += gamma[t][s] * diff * diff;
                }
            }
            for (int f = 0; f < HMM_FEATURES; ++f) {
                sig2_hat[f] /= gamma_sum;
                sig2_hat[f]  = std::max(HMM_MIN_SIGMA2, sig2_hat[f]);
            }

            // Online update
            for (int f = 0; f < HMM_FEATURES; ++f) {
                mu_  [s][f] = (1.0 - HMM_ALPHA) * mu_  [s][f] + HMM_ALPHA * mu_hat[f];
                sig2_[s][f] = (1.0 - HMM_ALPHA) * sig2_[s][f] + HMM_ALPHA * sig2_hat[f];
                sig2_[s][f] = std::max(HMM_MIN_SIGMA2, sig2_[s][f]);
            }
        }

        // Update pi from t=T-1 gamma
        double pi_sum = 0.0;
        for (int s = 0; s < HMM_STATES; ++s) pi_sum += gamma[T-1][s];
        if (pi_sum > 0.0) {
            for (int s = 0; s < HMM_STATES; ++s) {
                const double pi_hat = gamma[T-1][s] / pi_sum;
                pi_[s] = (1.0 - HMM_ALPHA) * pi_[s] + HMM_ALPHA * pi_hat;
            }
            // Renormalise
            double ps = 0.0;
            for (int s = 0; s < HMM_STATES; ++s) ps += pi_[s];
            if (ps > 0.0) for (int s = 0; s < HMM_STATES; ++s) pi_[s] /= ps;
        }
    }
};

} // namespace omega
