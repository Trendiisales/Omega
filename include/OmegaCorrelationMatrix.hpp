// =============================================================================
//  OmegaCorrelationMatrix.hpp
//  EWM (Exponentially Weighted Moving) rolling correlation matrix +
//  volatility-parity position sizing multiplier.
//
//  PURPOSE:
//    The static CorrelationHeatGuard (OmegaAdaptiveRisk.hpp) counts open
//    positions per cluster but does not measure *realised* correlation between
//    assets in real time. Two metals positions can be uncorrelated on a
//    bifurcated day (gold up / silver down) yet still blocked by the count
//    guard. Conversely, two different clusters can become highly correlated
//    during a macro shock. This module measures both problems in real time.
//
//  ARCHITECTURE:
//    1. EWM Correlation Matrix
//       - Tracks EWM variance and covariance for N symbols using a fast
//         running-stat update (O(N²) per tick but N ≤ 16 → negligible).
//       - Returns pairwise Pearson correlation [-1, +1] at any time.
//       - Alpha = 2/(span+1) where span is configurable (default: 120 ticks ≈
//         12s at 10 ticks/s). This gives a half-life of ~84 ticks.
//
//    2. Correlation Gate (replaces static cluster count guard in enter_directional)
//       - Before opening a new position, checks all *currently open* symbols.
//       - If |corr(new_sym, open_sym)| > CORR_BLOCK_THRESHOLD (default 0.85),
//         the new entry is blocked with a log message.
//       - Threshold is configurable; 0.85 is conservative enough to only fire
//         on genuine correlation spikes, not normal co-movement.
//
//    3. Vol-Parity Sizing Multiplier
//       - Returns a scale factor [VOL_PARITY_MIN, VOL_PARITY_MAX] for a symbol
//         based on its EWM volatility relative to the portfolio vol target.
//       - Replaces flat fallback_lot: high-vol assets get smaller lots, low-vol
//         assets get slightly larger lots (within bounds).
//       - Formula: scale = vol_target / ewm_vol(sym), clamped to [0.5, 1.5].
//       - vol_target is auto-calibrated to the median symbol vol over 500 ticks.
//
//  INTEGRATION (main.cpp):
//    1. Include after OmegaAdaptiveRisk.hpp in the include block.
//    2. Declare global: static omega::corr::CorrelationMatrix g_corr_matrix;
//    3. In tick loop for each symbol:
//         g_corr_matrix.on_price(symbol, mid_price);
//    4. In enter_directional (after existing corr_heat_ok check):
//         if (!g_corr_matrix.entry_allowed(esym, open_symbols)) return 0.0;
//         final_lot *= g_corr_matrix.vol_parity_scale(esym);
//    5. In shutdown save block:
//         g_corr_matrix.save_state(log_root_dir() + "/corr_matrix.dat");
//    6. On startup:
//         g_corr_matrix.load_state(log_root_dir() + "/corr_matrix.dat");
//
//  THREAD SAFETY:
//    All public methods are mutex-protected. Safe to call from the tick thread
//    and the risk thread simultaneously.
//
//  NOTE: Does NOT replace g_adaptive_risk.corr_heat. The static heat guard
//  remains as a fast O(1) pre-filter. This module runs after it as a second,
//  more precise layer.
// =============================================================================

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omega { namespace corr {

// ---------------------------------------------------------------------------
//  Config
// ---------------------------------------------------------------------------
static constexpr int    CORR_MAX_SYMBOLS        = 20;    // hard cap on tracked symbols
static constexpr double CORR_EWM_SPAN           = 120.0; // ticks; alpha = 2/(span+1)
static constexpr double CORR_BLOCK_THRESHOLD    = 0.85;  // |corr| above this → block
static constexpr double CORR_WARN_THRESHOLD     = 0.70;  // log warning only
static constexpr double VOL_PARITY_MIN          = 0.50;  // floor on sizing multiplier
static constexpr double VOL_PARITY_MAX          = 1.50;  // ceiling on sizing multiplier
static constexpr int    VOL_CALIBRATION_TICKS   = 500;   // ticks before vol target is set

// ---------------------------------------------------------------------------
//  EwmStats — single-symbol running EWM mean and variance
// ---------------------------------------------------------------------------
struct EwmStats {
    double alpha  = 2.0 / (CORR_EWM_SPAN + 1.0);
    double mean   = 0.0;
    double var    = 0.0;   // EWM variance (not annualised)
    double prev   = 0.0;
    bool   primed = false;
    int    n      = 0;

    // Update with a new log-return observation (not raw price).
    // Returns the current EWM std-dev (volatility).
    double update(double log_ret) {
        if (!primed) {
            mean   = log_ret;
            var    = 0.0;
            primed = true;
            ++n;
            prev   = log_ret;
            return 0.0;
        }
        const double delta = log_ret - mean;
        mean = mean + alpha * delta;
        var  = (1.0 - alpha) * (var + alpha * delta * delta);
        ++n;
        prev = log_ret;
        return std::sqrt(std::max(var, 1e-18));
    }

    double vol() const { return std::sqrt(std::max(var, 1e-18)); }
};

// ---------------------------------------------------------------------------
//  EwmCov — pairwise EWM covariance
// ---------------------------------------------------------------------------
struct EwmCov {
    double alpha = 2.0 / (CORR_EWM_SPAN + 1.0);
    double cov   = 0.0;

    // Update with concurrent log-return residuals for two assets.
    // resid_a = log_ret_a - mean_a, resid_b = log_ret_b - mean_b
    void update(double resid_a, double resid_b) {
        cov = (1.0 - alpha) * cov + alpha * resid_a * resid_b;
    }
};

// ---------------------------------------------------------------------------
//  CorrelationMatrix
// ---------------------------------------------------------------------------
class CorrelationMatrix {
public:
    // Correlation threshold overrides (can be set before first tick)
    double block_threshold = CORR_BLOCK_THRESHOLD;
    double warn_threshold  = CORR_WARN_THRESHOLD;

    // Vol-parity sizing bounds
    double vol_parity_min = VOL_PARITY_MIN;
    double vol_parity_max = VOL_PARITY_MAX;

    // -----------------------------------------------------------------------
    //  on_price — call every tick for each symbol with the current mid price.
    //  Internally converts to log-returns for stationarity.
    // -----------------------------------------------------------------------
    void on_price(const std::string& sym, double mid) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (mid <= 0.0) return;

        auto& st = ensure_symbol(sym);
        const double prev_px = st.prev_price;
        st.prev_price = mid;
        if (prev_px <= 0.0) return;  // first tick — no return yet

        const double log_ret = std::log(mid / prev_px);
        const double residual = log_ret - st.ewm.mean;
        st.ewm.update(log_ret);

        // Update pairwise covariances with all other symbols
        for (int j = 0; j < sym_count_; ++j) {
            if (syms_[j] == sym) continue;
            auto& other = sym_states_[j];
            if (other.prev_price <= 0.0) continue;
            const double other_resid = other.last_log_ret - other.ewm.mean;
            cov_matrix_[st.idx][other.idx].update(residual, other_resid);
            cov_matrix_[other.idx][st.idx].update(other_resid, residual);
        }
        st.last_log_ret = log_ret;

        // Vol-parity calibration: accumulate vol samples
        calibrate_vol_target(sym, st.ewm.vol());
    }

    // -----------------------------------------------------------------------
    //  correlation — returns EWM Pearson correlation between two symbols.
    //  Returns 0.0 if either symbol is unknown or insufficient data.
    // -----------------------------------------------------------------------
    double correlation(const std::string& a, const std::string& b) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return correlation_unlocked(a, b);
    }

    // -----------------------------------------------------------------------
    //  entry_allowed — main gate. Pass the symbol being entered and a list of
    //  currently open position symbols. Returns false (block) if any open
    //  symbol is correlated above block_threshold with the new symbol.
    // -----------------------------------------------------------------------
    bool entry_allowed(const std::string& new_sym,
                       const std::vector<std::string>& open_syms) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& open : open_syms) {
            if (open == new_sym) continue;
            const double c = correlation_unlocked(new_sym, open);
            const double abs_c = std::fabs(c);
            if (abs_c >= block_threshold) {
                static thread_local int64_t s_last_log = 0;
                const int64_t now_s = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                if (now_s - s_last_log > 10) {
                    s_last_log = now_s;
                    std::printf("[CORR-GATE] %s blocked: |corr(%s,%s)|=%.3f >= %.2f\n",
                                new_sym.c_str(), new_sym.c_str(), open.c_str(),
                                abs_c, block_threshold);
                    std::fflush(stdout);
                }
                return false;
            }
            if (abs_c >= warn_threshold) {
                std::printf("[CORR-WARN] %s high corr with open %s: %.3f\n",
                            new_sym.c_str(), open.c_str(), c);
                std::fflush(stdout);
            }
        }
        return true;
    }

    // -----------------------------------------------------------------------
    //  vol_parity_scale — returns lot-size multiplier for a symbol.
    //  scale = vol_target / ewm_vol(sym), clamped to [vol_parity_min, vol_parity_max].
    //  Returns 1.0 if insufficient data or vol_target not yet set.
    // -----------------------------------------------------------------------
    double vol_parity_scale(const std::string& sym) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (vol_target_ <= 0.0) return 1.0;
        auto it = sym_index_.find(sym);
        if (it == sym_index_.end()) return 1.0;
        const double vol = sym_states_[it->second].ewm.vol();
        if (vol <= 1e-12) return 1.0;
        const double scale = vol_target_ / vol;
        const double clamped = std::max(vol_parity_min, std::min(vol_parity_max, scale));
        return clamped;
    }

    // -----------------------------------------------------------------------
    //  covariance -- returns raw EWM covariance between two symbols.
    //  Positive = move together, negative = move opposite, 0 = unrelated.
    //  Returns 0.0 if either symbol unknown or insufficient data.
    // -----------------------------------------------------------------------
    double covariance(const std::string& a, const std::string& b) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it_a = sym_index_.find(a);
        auto it_b = sym_index_.find(b);
        if (it_a == sym_index_.end() || it_b == sym_index_.end()) return 0.0;
        return cov_matrix_[it_a->second][it_b->second].cov;
    }

    // -----------------------------------------------------------------------
    //  covariance_sizing_mult -- lot-size multiplier based on portfolio covariance.
    //  When entering sym, sums |cov(sym, open_sym)| * |open_direction_sign| across
    //  all open positions. High net covariance exposure = reduce size.
    //
    //  open_positions: vector of {symbol, direction_sign (+1=long, -1=short)}
    //  Returns multiplier in [COV_SIZE_MIN, 1.0]:
    //    - 1.0 when no open positions or covariance below threshold
    //    - Scales down proportionally as covariance exposure increases
    //    - Floor of COV_SIZE_MIN (0.50) prevents sizing to zero
    //
    //  Example: long XAUUSD + long XAGUSD simultaneously -- their high positive
    //  covariance means we are doubling metals exposure. The multiplier shrinks
    //  the new entry size proportionally to the net covariance load.
    // -----------------------------------------------------------------------
    static constexpr double COV_SIZE_THRESHOLD = 1e-6; // below this: no adjustment
    static constexpr double COV_SIZE_SCALE      = 2e5;  // maps cov to [0,1] range
    static constexpr double COV_SIZE_MIN        = 0.50; // floor: never cut below 50%

    double covariance_sizing_mult(
        const std::string& sym,
        const std::vector<std::pair<std::string,int>>& open_positions) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (open_positions.empty()) return 1.0;

        auto it_sym = sym_index_.find(sym);
        if (it_sym == sym_index_.end()) return 1.0;
        const int idx_sym = it_sym->second;

        double net_cov_exposure = 0.0;
        for (const auto& op : open_positions) {
            if (op.first == sym) continue;
            auto it_o = sym_index_.find(op.first);
            if (it_o == sym_index_.end()) continue;
            const double cov = cov_matrix_[idx_sym][it_o->second].cov;
            net_cov_exposure += std::fabs(cov * static_cast<double>(op.second));
        }

        if (net_cov_exposure < COV_SIZE_THRESHOLD) return 1.0;

        const double raw_scale = 1.0 - (net_cov_exposure * COV_SIZE_SCALE);
        const double mult = std::max(COV_SIZE_MIN, std::min(1.0, raw_scale));

        static thread_local int64_t s_cov_log = 0;
        const int64_t now_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_s - s_cov_log > 30) {
            s_cov_log = now_s;
            std::printf("[COV-SIZE] %s net_cov=%.2e -> mult=%.3f\n",
                        sym.c_str(), net_cov_exposure, mult);
            std::fflush(stdout);
        }
        return mult;
    }

    // -----------------------------------------------------------------------
    //  trend_day_corr -- returns true when gold/silver correlation confirms
    //  a macro trend day (not a mean-reversion day).
    //
    //  When |corr(XAUUSD, XAGUSD)| > 0.70, both metals are moving together --
    //  characteristic of risk-off/risk-on macro flow, NOT chop.
    //  Use this to gate mean-reversion engines (VWAPStretch, MeanReversion)
    //  so they do not fade a sustained directional move.
    //
    //  Silver is not traded but receives ticks via g_corr_matrix.on_price()
    //  at line 265 in on_tick.hpp -- before the is_active_sym gate blocks
    //  silver execution. Silver price feed is guaranteed to reach the matrix.
    //
    //  For indices: checks sym vs US500.F (both spike together on macro events).
    // -----------------------------------------------------------------------
    static constexpr double TREND_DAY_CORR_THRESHOLD = 0.70;

    bool trend_day_corr(const std::string& sym) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const std::string companion =
            (sym == "XAUUSD" || sym == "XAGUSD") ? "XAGUSD"
            : (sym == "USTEC.F" || sym == "NAS100" || sym == "DJ30.F") ? "US500.F"
            : "";
        if (companion.empty() || companion == sym) return false;
        const double c = correlation_unlocked(sym, companion);
        return std::fabs(c) >= TREND_DAY_CORR_THRESHOLD;
    }

    // -----------------------------------------------------------------------
    //  print_matrix — diagnostic dump of current correlation matrix.
    // -----------------------------------------------------------------------
    void print_matrix() const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (sym_count_ < 2) {
            std::printf("[CORR-MATRIX] Insufficient symbols tracked (%d)\n", sym_count_);
            return;
        }
        std::printf("[CORR-MATRIX] vol_target=%.6f\n", vol_target_);
        std::printf("%-12s", "");
        for (int j = 0; j < sym_count_; ++j)
            std::printf(" %8.8s", syms_[j].c_str());
        std::printf("\n");
        for (int i = 0; i < sym_count_; ++i) {
            std::printf("%-12.12s", syms_[i].c_str());
            for (int j = 0; j < sym_count_; ++j) {
                if (i == j) { std::printf(" %8.3f", 1.0); continue; }
                std::printf(" %8.3f", correlation_raw(i, j));
            }
            std::printf("  vol=%.6f scale=%.3f\n",
                        sym_states_[i].ewm.vol(),
                        (vol_target_ > 0.0 && sym_states_[i].ewm.vol() > 1e-12)
                            ? std::max(vol_parity_min, std::min(vol_parity_max,
                                vol_target_ / sym_states_[i].ewm.vol()))
                            : 1.0);
        }
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    //  save_state / load_state — persist EWM running stats across restarts
    //  so the matrix warms faster on the next session.
    //  Format: simple text, one symbol per line.
    // -----------------------------------------------------------------------
    void save_state(const std::string& path) const {
        std::lock_guard<std::mutex> lk(mtx_);
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) {
            std::printf("[CORR-MATRIX] save_state: cannot open %s\n", path.c_str());
            return;
        }
        std::fprintf(f, "vol_target=%.10f\n", vol_target_);
        for (int i = 0; i < sym_count_; ++i) {
            const auto& st = sym_states_[i];
            std::fprintf(f, "sym=%s mean=%.10f var=%.10f prev=%.10f prev_px=%.6f n=%d\n",
                         syms_[i].c_str(),
                         st.ewm.mean, st.ewm.var, st.ewm.prev,
                         st.prev_price, st.ewm.n);
        }
        // Save covariance matrix
        for (int i = 0; i < sym_count_; ++i) {
            for (int j = i + 1; j < sym_count_; ++j) {
                std::fprintf(f, "cov=%s:%s %.10f\n",
                             syms_[i].c_str(), syms_[j].c_str(),
                             cov_matrix_[i][j].cov);
            }
        }
        std::fclose(f);
        std::printf("[CORR-MATRIX] State saved to %s (%d symbols)\n",
                    path.c_str(), sym_count_);
    }

    void load_state(const std::string& path) {
        std::lock_guard<std::mutex> lk(mtx_);
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return;  // no saved state — start fresh
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            char sym[64]  = {};
            char sym2[64] = {};
            double v1 = 0, v2 = 0, v3 = 0, v4 = 0;
            int    n  = 0;
            if (std::sscanf(line, "vol_target=%lf", &v1) == 1) {
                vol_target_ = v1;
            } else if (std::sscanf(line, "sym=%63s mean=%lf var=%lf prev=%lf prev_px=%lf n=%d",
                                   sym, &v1, &v2, &v3, &v4, &n) == 6) {
                auto& st = ensure_symbol(sym);
                st.ewm.mean      = v1;
                st.ewm.var       = v2;
                st.ewm.prev      = v3;
                st.ewm.primed    = true;
                st.ewm.n         = n;
                st.prev_price    = v4;
            } else if (std::sscanf(line, "cov=%63[^:]:%63s %lf", sym, sym2, &v1) == 3) {
                auto it_a = sym_index_.find(sym);
                auto it_b = sym_index_.find(sym2);
                if (it_a != sym_index_.end() && it_b != sym_index_.end()) {
                    cov_matrix_[it_a->second][it_b->second].cov = v1;
                    cov_matrix_[it_b->second][it_a->second].cov = v1;
                }
            }
        }
        std::fclose(f);
        std::printf("[CORR-MATRIX] State loaded from %s (%d symbols, vol_target=%.6f)\n",
                    path.c_str(), sym_count_, vol_target_);
    }

private:
    mutable std::mutex mtx_;

    // Per-symbol state
    struct SymState {
        EwmStats ewm;
        double   prev_price  = 0.0;
        double   last_log_ret = 0.0;
        int      idx          = 0;
    };

    std::array<SymState, CORR_MAX_SYMBOLS>                      sym_states_{};
    std::array<std::string, CORR_MAX_SYMBOLS>                   syms_{};
    std::array<std::array<EwmCov, CORR_MAX_SYMBOLS>,
               CORR_MAX_SYMBOLS>                                cov_matrix_{};
    std::unordered_map<std::string, int>                        sym_index_;
    int     sym_count_  = 0;
    double  vol_target_ = 0.0;

    // Accumulated vol samples for median calibration
    std::unordered_map<std::string, std::deque<double>> vol_samples_;

    // Ensure symbol is registered, return ref to its state
    SymState& ensure_symbol(const std::string& sym) {
        auto it = sym_index_.find(sym);
        if (it != sym_index_.end()) return sym_states_[it->second];
        if (sym_count_ >= CORR_MAX_SYMBOLS) {
            // Silently return last slot on overflow (should never happen)
            return sym_states_[CORR_MAX_SYMBOLS - 1];
        }
        const int idx = sym_count_++;
        sym_index_[sym] = idx;
        syms_[idx] = sym;
        sym_states_[idx].idx = idx;
        // Init covariance alpha to match EWM span
        for (int j = 0; j < CORR_MAX_SYMBOLS; ++j) {
            cov_matrix_[idx][j].alpha = 2.0 / (CORR_EWM_SPAN + 1.0);
            cov_matrix_[j][idx].alpha = 2.0 / (CORR_EWM_SPAN + 1.0);
        }
        return sym_states_[idx];
    }

    double correlation_raw(int i, int j) const {
        const double vol_i = sym_states_[i].ewm.vol();
        const double vol_j = sym_states_[j].ewm.vol();
        if (vol_i < 1e-18 || vol_j < 1e-18) return 0.0;
        const double c = cov_matrix_[i][j].cov / (vol_i * vol_j);
        return std::max(-1.0, std::min(1.0, c));
    }

    double correlation_unlocked(const std::string& a, const std::string& b) const {
        auto it_a = sym_index_.find(a);
        auto it_b = sym_index_.find(b);
        if (it_a == sym_index_.end() || it_b == sym_index_.end()) return 0.0;
        return correlation_raw(it_a->second, it_b->second);
    }

    void calibrate_vol_target(const std::string& sym, double vol) {
        if (vol <= 1e-12) return;
        auto& dq = vol_samples_[sym];
        dq.push_back(vol);
        if ((int)dq.size() > VOL_CALIBRATION_TICKS) dq.pop_front();

        // Recalculate vol_target as median of per-symbol median vols
        // Do this only every 50 ticks to avoid unnecessary work
        static int s_cal_tick = 0;
        if (++s_cal_tick % 50 != 0) return;

        std::vector<double> medians;
        medians.reserve(sym_count_);
        for (int i = 0; i < sym_count_; ++i) {
            auto jt = vol_samples_.find(syms_[i]);
            if (jt == vol_samples_.end() || jt->second.empty()) continue;
            std::vector<double> v(jt->second.begin(), jt->second.end());
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            medians.push_back(v[v.size() / 2]);
        }
        if (medians.empty()) return;
        std::nth_element(medians.begin(), medians.begin() + medians.size() / 2, medians.end());
        vol_target_ = medians[medians.size() / 2];
    }
};

}} // namespace omega::corr
