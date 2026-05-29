// =============================================================================
// L2LeverageState.hpp -- shared L2 leverage helper for bar-driven engines
// -----------------------------------------------------------------------------
// Encapsulates:
//   - 30-tick ring buffer of microprice + imbalance (updated per on_tick)
//   - Sizing-mult computation from rolling mic_avg
//   - L2-trail flip detection (close when mic flips against side at mfe>=1R)
//
// Used by DonchianEngine, EmaPullbackEngine, XauTrendFollow4hEngine, and
// SurvivorPortfolio cells. IndexSwingEngine has its own inline copy with the
// same params (kept separate for blame-history; future refactor to merge).
//
// Per memory:omega-l2-leverage-points -- L2-trail mfe guard MUST be 1R not
// 0.5R. Replay sweep 2026-05-30 showed 0.5R loses $296 on sample, 1R gains
// $79.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include "L2Globals.hpp"

namespace omega {

struct L2LeverageState {
    static constexpr int BUF_N           = 30;
    static constexpr int CONFIRM_N       = 10;
    static constexpr double SIZE_FACTOR  = 10.0;   // mic_avg * 10, clamped [0.5, 2.0]
    static constexpr double SZ_MIN     = 0.5;
    static constexpr double SZ_MAX     = 2.0;
    static constexpr double TRAIL_FLIP_MIC = 0.10;
    static constexpr double TRAIL_MFE_R  = 1.0;    // require mfe >= 1R before trail

    double mic_buf[BUF_N] = {};
    double imb_buf[BUF_N] = {};
    int    head           = 0;
    int    count          = 0;
    double size_mult_at_entry = 1.0;

    void reset() noexcept {
        std::fill(std::begin(mic_buf), std::end(mic_buf), 0.0);
        std::fill(std::begin(imb_buf), std::end(imb_buf), 0.0);
        head = 0; count = 0;
        size_mult_at_entry = 1.0;
    }

    // Push current L2 atomic state into the ring buffer. Call every on_tick.
    void push(const AtomicL2& src) noexcept {
        mic_buf[head] = src.microprice_bias.load(std::memory_order_relaxed);
        imb_buf[head] = src.imbalance.load(std::memory_order_relaxed);
        head = (head + 1) % BUF_N;
        if (count < BUF_N) ++count;
    }

    // Rolling mic_avg over the last N samples (N capped by count).
    double mic_avg(int n) const noexcept {
        const int use = std::min(count, n);
        if (use <= 0) return 0.0;
        double sum = 0;
        int idx = (head - 1 + BUF_N) % BUF_N;
        for (int k = 0; k < use; ++k) {
            sum += mic_buf[idx];
            idx = (idx - 1 + BUF_N) % BUF_N;
        }
        return sum / use;
    }

    double imb_avg(int n) const noexcept {
        const int use = std::min(count, n);
        if (use <= 0) return 0.5;
        double sum = 0;
        int idx = (head - 1 + BUF_N) % BUF_N;
        for (int k = 0; k < use; ++k) {
            sum += imb_buf[idx];
            idx = (idx - 1 + BUF_N) % BUF_N;
        }
        return sum / use;
    }

    // Compute size multiplier from current N-tick rolling mic_avg. Called at
    // entry time. Strong confirmation -> 2x lot, weak -> 0.5x. Result is
    // stored in size_mult_at_entry for the engine to use in PnL accounting.
    double compute_size_mult(int n = CONFIRM_N) noexcept {
        if (count < n) { size_mult_at_entry = 1.0; return 1.0; }
        const double m = mic_avg(n);
        size_mult_at_entry = std::clamp(std::fabs(m) * SIZE_FACTOR, SZ_MIN, SZ_MAX);
        return size_mult_at_entry;
    }

    // Returns true when L2 trail should fire (close position now).
    //   side: +1 long, -1 short
    //   mfe_pts: current max-favorable-excursion in price units
    //   sl_pts: initial SL distance in price units (= 1R)
    bool check_trail_flip(int side, double mfe_pts, double sl_pts) const noexcept {
        if (count < CONFIRM_N) return false;
        if (mfe_pts < sl_pts * TRAIL_MFE_R) return false;
        const double m = mic_avg(CONFIRM_N);
        if (side > 0  && m <= -TRAIL_FLIP_MIC) return true;
        if (side < 0  && m >=  TRAIL_FLIP_MIC) return true;
        return false;
    }
};

}  // namespace omega
