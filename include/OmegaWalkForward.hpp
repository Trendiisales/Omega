#pragma once
// =============================================================================
// OmegaWalkForward.hpp  --  Rolling live walk-forward OOS validation
//
// PURPOSE (RenTec #6):
//   As trades accumulate in the live session, periodically run a 5-fold
//   walk-forward validation against g_omegaLedger.snapshot().
//   If OOS Sharpe OR OOS/IS ratio fall below thresholds, reduce lot size
//   via a scale factor read by AdaptiveRiskManager::adjusted_lot() step 11.
//
// ALGORITHM:
//   Same Two Sigma rule as scripts/shadow_analysis.py --wfo:
//     OOS Sharpe >= WFO_OOS_SHARPE_MIN  (default 0.8)
//     OOS/IS ratio >= WFO_RATIO_MIN      (default 0.40)
//   Both conditions must pass in >= WFO_MIN_PASSING_FOLDS folds (default 3/5).
//
// WHEN IT RUNS:
//   - Triggered by update() called from handle_closed_trade()
//   - Re-runs every WFO_RETRIGGER_TRADES trades (default 20)
//   - Requires >= WFO_MIN_TRADES total closed trades (default 40)
//   - Per-symbol: XAUUSD validated separately from FX/indices
//     (pass symbol="" for portfolio-level, or a specific symbol)
//
// SCALE FACTORS:
//   All folds pass   -> scale = 1.00 (full size)
//   3-4/5 pass       -> scale = WFO_DEGRADED_SCALE  (default 0.75)
//   < 3/5 pass       -> scale = WFO_FAILING_SCALE   (default 0.50)
//   Insufficient data -> scale = 1.00 (no penalty during warmup)
//
// THREAD SAFETY:
//   update() acquires mtx_ -- called off hot path (handle_closed_trade)
//   scale()  reads atomic -- safe from any thread, no lock
//
// =============================================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// Tuning constants
// =============================================================================
static constexpr int    WFO_MIN_TRADES          = 40;    // need this many trades before running
static constexpr int    WFO_RETRIGGER_TRADES    = 20;    // re-run every N new trades
static constexpr int    WFO_N_FOLDS             = 5;     // k-fold splits
static constexpr int    WFO_MIN_OOS_TRADES      = 5;     // skip fold if OOS window < N
static constexpr int    WFO_MIN_IS_TRADES        = 10;   // skip fold if IS window < N
static constexpr double WFO_OOS_SHARPE_MIN      = 0.80;  // Two Sigma rule: OOS Sharpe floor
static constexpr double WFO_RATIO_MIN           = 0.40;  // Two Sigma rule: OOS/IS ratio floor
static constexpr int    WFO_MIN_PASSING_FOLDS   = 3;     // need this many folds to pass
static constexpr double WFO_DEGRADED_SCALE      = 0.75;  // 3-4/5 folds pass
static constexpr double WFO_FAILING_SCALE       = 0.50;  // < 3/5 folds pass

// =============================================================================
// Internal helpers
// =============================================================================
namespace wfo_detail {

inline double sharpe(const std::vector<double>& pnl) noexcept {
    const int n = static_cast<int>(pnl.size());
    if (n < 2) return 0.0;
    double sum = 0.0, sq = 0.0;
    for (double v : pnl) { sum += v; sq += v * v; }
    const double mean = sum / n;
    const double var  = sq / n - mean * mean;
    if (var < 1e-18) return 0.0;
    return (mean / std::sqrt(var)) * std::sqrt(252.0);
}

} // namespace wfo_detail

// =============================================================================
// WfoFoldResult  --  single fold outcome
// =============================================================================
struct WfoFoldResult {
    int    fold        = 0;
    int    is_n        = 0;
    int    oos_n       = 0;
    double is_sharpe   = 0.0;
    double oos_sharpe  = 0.0;
    double ratio       = 0.0;   // oos_sharpe / is_sharpe
    bool   pass        = false;
};

// =============================================================================
// WfoResult  --  full validation result
// =============================================================================
struct WfoResult {
    int    n_trades        = 0;
    int    n_folds_run     = 0;
    int    n_folds_passed  = 0;
    double avg_oos_sharpe  = 0.0;
    double avg_ratio       = 0.0;
    double size_scale      = 1.0;  // what adjusted_lot multiplies by
    bool   sufficient_data = false;

    std::vector<WfoFoldResult> folds;

    void print(const std::string& label) const noexcept {
        if (!sufficient_data) {
            printf("[WFO] %s -- insufficient data (%d trades, need %d)\n",
                   label.c_str(), n_trades, WFO_MIN_TRADES);
            fflush(stdout);
            return;
        }
        printf("[WFO] %s  n=%d  folds=%d/%d_pass  "
               "avg_oos_sharpe=%.2f  avg_ratio=%.2f  scale=%.2f  %s\n",
               label.c_str(), n_trades, n_folds_passed, n_folds_run,
               avg_oos_sharpe, avg_ratio, size_scale,
               size_scale >= 1.0 ? "PASS" :
               size_scale >= WFO_DEGRADED_SCALE ? "DEGRADED" : "FAILING");
        for (const auto& f : folds) {
            printf("[WFO]   fold=%d  IS=%d(%.2f)  OOS=%d(%.2f)  ratio=%.2f  %s\n",
                   f.fold, f.is_n, f.is_sharpe, f.oos_n, f.oos_sharpe,
                   f.ratio, f.pass ? "pass" : "FAIL");
        }
        fflush(stdout);
    }
};

// =============================================================================
// OmegaWalkForward
// =============================================================================
class OmegaWalkForward {
public:

    // =========================================================================
    // update()
    // Called from handle_closed_trade() with all closed trades for a symbol.
    // Re-runs WFO if enough new trades have arrived since last run.
    // symbol: used for logging and per-symbol state. Pass "" for portfolio-level.
    // =========================================================================
    void update(const std::string& symbol,
                const std::vector<double>& pnl_history) noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);

        const int n = static_cast<int>(pnl_history.size());
        const int last = last_n_.count(symbol) ? last_n_.at(symbol) : 0;

        // Only re-run if enough new trades since last evaluation
        if (n < WFO_MIN_TRADES) {
            // Not enough data yet -- ensure scale stays at 1.0 (no warmup penalty)
            scales_[symbol].store(1.0f, std::memory_order_relaxed);
            last_n_[symbol] = n;
            return;
        }
        if (n - last < WFO_RETRIGGER_TRADES && last >= WFO_MIN_TRADES) {
            return;  // not enough new trades to justify re-run
        }

        const WfoResult result = run_wfo(pnl_history);
        last_n_[symbol] = n;

        // Store scale atomically for lock-free read in adjusted_lot
        scales_[symbol].store(static_cast<float>(result.size_scale),
                               std::memory_order_relaxed);
        last_results_[symbol] = result;

        result.print(symbol.empty() ? "PORTFOLIO" : symbol);
    }

    // =========================================================================
    // scale()
    // Returns the current size scale factor for this symbol.
    // 1.0 = full size, 0.75 = degraded, 0.50 = failing.
    // Safe to call from any thread (atomic read).
    // =========================================================================
    double scale(const std::string& symbol) const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = scales_.find(symbol);
        if (it == scales_.end()) return 1.0;
        return static_cast<double>(it->second.load(std::memory_order_relaxed));
    }

    // =========================================================================
    // last_result()
    // Returns the last computed WfoResult for a symbol (for GUI/logging).
    // =========================================================================
    WfoResult last_result(const std::string& symbol) const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = last_results_.find(symbol);
        if (it == last_results_.end()) return WfoResult{};
        return it->second;
    }

    // =========================================================================
    // log_all()
    // Print current state for all tracked symbols.
    // =========================================================================
    void log_all() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (last_results_.empty()) {
            printf("[WFO] No results yet (insufficient data across all symbols)\n");
            fflush(stdout);
            return;
        }
        for (const auto& kv : last_results_)
            kv.second.print(kv.first.empty() ? "PORTFOLIO" : kv.first);
    }

private:
    // =========================================================================
    // run_wfo()
    // Core k-fold walk-forward. pnl is sorted chronologically by caller.
    // =========================================================================
    static WfoResult run_wfo(const std::vector<double>& pnl) noexcept {
        WfoResult res;
        res.n_trades        = static_cast<int>(pnl.size());
        res.sufficient_data = (res.n_trades >= WFO_MIN_TRADES);

        if (!res.sufficient_data) {
            res.size_scale = 1.0;
            return res;
        }

        const int n  = res.n_trades;
        const int fs = n / WFO_N_FOLDS;   // fold size

        double sum_oos_sharpe = 0.0;
        double sum_ratio      = 0.0;
        int    n_valid        = 0;

        for (int fold = 0; fold < WFO_N_FOLDS; ++fold) {
            const int oos_start = fold * fs;
            const int oos_end   = (fold < WFO_N_FOLDS - 1) ? oos_start + fs : n;

            // OOS window: [oos_start, oos_end)
            std::vector<double> oos(pnl.begin() + oos_start,
                                    pnl.begin() + oos_end);

            // IS window: everything EXCEPT this fold
            std::vector<double> is_data;
            is_data.reserve(n - static_cast<int>(oos.size()));
            for (int i = 0; i < oos_start; ++i)  is_data.push_back(pnl[i]);
            for (int i = oos_end; i < n; ++i)     is_data.push_back(pnl[i]);

            if (static_cast<int>(is_data.size()) < WFO_MIN_IS_TRADES ||
                static_cast<int>(oos.size())     < WFO_MIN_OOS_TRADES)
                continue;

            const double is_sh  = wfo_detail::sharpe(is_data);
            const double oos_sh = wfo_detail::sharpe(oos);
            const double ratio  = (std::fabs(is_sh) > 0.10)
                                  ? (oos_sh / is_sh) : 0.0;

            WfoFoldResult fr;
            fr.fold       = fold + 1;
            fr.is_n       = static_cast<int>(is_data.size());
            fr.oos_n      = static_cast<int>(oos.size());
            fr.is_sharpe  = is_sh;
            fr.oos_sharpe = oos_sh;
            fr.ratio      = ratio;
            fr.pass       = (oos_sh >= WFO_OOS_SHARPE_MIN && ratio >= WFO_RATIO_MIN);
            res.folds.push_back(fr);

            if (fr.pass) ++res.n_folds_passed;
            sum_oos_sharpe += oos_sh;
            sum_ratio      += ratio;
            ++n_valid;
            ++res.n_folds_run;
        }

        if (n_valid > 0) {
            res.avg_oos_sharpe = sum_oos_sharpe / n_valid;
            res.avg_ratio      = sum_ratio      / n_valid;
        }

        // Determine scale
        if (res.n_folds_run == 0) {
            res.size_scale = 1.0;   // no valid folds = no penalty
        } else if (res.n_folds_passed >= WFO_MIN_PASSING_FOLDS) {
            res.size_scale = 1.00;
        } else if (res.n_folds_passed >= WFO_MIN_PASSING_FOLDS - 1) {
            res.size_scale = WFO_DEGRADED_SCALE;  // 0.75
        } else {
            res.size_scale = WFO_FAILING_SCALE;   // 0.50
        }

        return res;
    }

    mutable std::mutex mtx_;

    // Per-symbol last trade count when WFO last ran
    std::unordered_map<std::string, int> last_n_;

    // Per-symbol current scale (atomic float for lock-free read in hot path)
    // Stored as float (4B) -- atomic<double> not always lock-free on x86
    mutable std::unordered_map<std::string, std::atomic<float>> scales_;

    // Per-symbol last full result (for GUI / log_all)
    std::unordered_map<std::string, WfoResult> last_results_;
};
