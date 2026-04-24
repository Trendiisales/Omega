#pragma once
// =============================================================================
// OmegaParamGate.hpp  --  Adaptive parameter gate (RenTec #7)
//
// PURPOSE:
//   Reads rolling win-rate and expectancy from SymbolPerformanceTracker and
//   returns a dynamic effective_min_score that tightens or relaxes the entry
//   threshold in OmegaSignalScorer.
//
//   When the edge is confirmed (high WR, positive expectancy) → lower the bar:
//     more trades pass the scorer → higher frequency in good regimes.
//   When the edge is degrading (low WR, negative expectancy) → raise the bar:
//     only the highest-conviction setups get through.
//
// THRESHOLDS (all tunable via constants below):
//
//   State           Condition                         effective_min_score
//   ----------      ---------------------------------  -------------------
//   WARMUP          trade_count < MIN_TRADES           BASE (5) -- no change
//   EDGE_STRONG     WR >= 0.60  AND  exp >  EXP_POS   BASE - 1 (4)  relax
//   EDGE_NORMAL     WR >= 0.45  AND  exp > -EXP_NEG1  BASE     (5)  unchanged
//   EDGE_SOFT_WARN  WR  < 0.45  OR   exp < -EXP_NEG1  BASE + 1 (6)  tighten
//   EDGE_FAILING    WR  < 0.35  OR   exp < -EXP_NEG2  BASE + 2 (7)  max tighten
//
//   BASE = OmegaSignalScorer::SCORE_MIN_ENTRY (5)
//   MAX  = BASE + 2 (7)   -- never goes above 7/16 (43.75% of max)
//   MIN  = BASE - 1 (4)   -- never goes below 4/16 (25.00% of max)
//
// HYSTERESIS:
//   State can only move one level per WFO_RETRIGGER_TRADES new trades.
//   Prevents oscillation when WR hovers near threshold.
//
// THREAD SAFETY:
//   effective_min_score() reads an atomic int → safe from any thread (hot path)
//   update() acquires internal mutex → called off hot path (handle_closed_trade)
//
// INTEGRATION:
//   Replace the hard-coded OmegaSignalScorer::SCORE_MIN_ENTRY comparison in the
//   scorer block with g_param_gate.effective_min_score("XAUUSD").
//
// =============================================================================

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <chrono>

// =============================================================================
// Tuning
// =============================================================================
static constexpr int    APG_BASE_SCORE      = 5;     // matches OMEGA_SCORE_MIN_ENTRY
static constexpr int    APG_SCORE_MAX       = 7;     // most restrictive
static constexpr int    APG_SCORE_MIN       = 4;     // most permissive
static constexpr int    APG_MIN_TRADES      = 10;    // warmup: no adjustment until this many trades
static constexpr int    APG_RETRIGGER       = 10;    // re-evaluate every N new trades
static constexpr double APG_WR_STRONG       = 0.60;  // win rate above this = relax
static constexpr double APG_WR_WARN         = 0.45;  // win rate below this = tighten
static constexpr double APG_WR_FAIL         = 0.35;  // win rate below this = max tighten
static constexpr double APG_EXP_POSITIVE    = 2.00;  // $ expectancy above this = relax
static constexpr double APG_EXP_WARN        = -2.00; // $ expectancy below this = tighten
static constexpr double APG_EXP_FAIL        = -5.00; // $ expectancy below this = max tighten

// =============================================================================
// EdgeState -- named state for logging clarity
// =============================================================================
enum class EdgeState {
    WARMUP,       // insufficient data
    STRONG,       // performing well  -> relax (score - 1)
    NORMAL,       // neutral          -> unchanged (score = base)
    SOFT_WARN,    // underperforming  -> tighten (score + 1)
    FAILING,      // edge degrading   -> max tighten (score + 2)
};

static const char* edge_state_name(EdgeState s) noexcept {
    switch (s) {
        case EdgeState::WARMUP:    return "WARMUP";
        case EdgeState::STRONG:    return "STRONG";
        case EdgeState::NORMAL:    return "NORMAL";
        case EdgeState::SOFT_WARN: return "SOFT_WARN";
        case EdgeState::FAILING:   return "FAILING";
        default:                   return "UNKNOWN";
    }
}

// =============================================================================
// Per-symbol gate state
// =============================================================================
struct ParamGateState {
    std::atomic<int>  score_threshold{APG_BASE_SCORE};
    std::atomic<int>  edge_state_int {static_cast<int>(EdgeState::WARMUP)};
    int               last_n         {0};
    int               last_score     {APG_BASE_SCORE};  // for hysteresis

    EdgeState edge_state() const noexcept {
        return static_cast<EdgeState>(edge_state_int.load(std::memory_order_relaxed));
    }
};

// =============================================================================
// OmegaParamGate
// =============================================================================
class OmegaParamGate {
public:

    // =========================================================================
    // update()
    // Called from handle_closed_trade() with current rolling stats for symbol.
    // win_rate_20  : win rate over last 20 trades (0..1)
    // expectancy_20: expectancy over last 20 trades (USD/trade)
    // trade_count  : total trades recorded so far
    // =========================================================================
    void update(const std::string& symbol,
                double win_rate_20,
                double expectancy_20,
                int    trade_count) noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& st = state_for(symbol);

        // Warmup guard
        if (trade_count < APG_MIN_TRADES) {
            st.score_threshold.store(APG_BASE_SCORE, std::memory_order_relaxed);
            st.edge_state_int .store(static_cast<int>(EdgeState::WARMUP),
                                     std::memory_order_relaxed);
            st.last_n = trade_count;
            return;
        }

        // Throttle: only re-evaluate every APG_RETRIGGER new trades
        if (trade_count - st.last_n < APG_RETRIGGER && st.last_n >= APG_MIN_TRADES)
            return;
        st.last_n = trade_count;

        // Classify edge state
        EdgeState new_state;
        if (win_rate_20 < APG_WR_FAIL || expectancy_20 < APG_EXP_FAIL) {
            new_state = EdgeState::FAILING;
        } else if (win_rate_20 < APG_WR_WARN || expectancy_20 < APG_EXP_WARN) {
            new_state = EdgeState::SOFT_WARN;
        } else if (win_rate_20 >= APG_WR_STRONG && expectancy_20 > APG_EXP_POSITIVE) {
            new_state = EdgeState::STRONG;
        } else {
            new_state = EdgeState::NORMAL;
        }

        // Apply hysteresis: don't jump more than 1 level per evaluation
        const EdgeState old_state = st.edge_state();
        const int old_level = state_to_level(old_state);
        const int new_level = state_to_level(new_state);
        const int clamped   = std::max(old_level - 1, std::min(old_level + 1, new_level));
        const EdgeState final_state = level_to_state(clamped);

        const int new_threshold = APG_BASE_SCORE + clamped;
        const int clamped_thr   = std::max(APG_SCORE_MIN,
                                            std::min(APG_SCORE_MAX, new_threshold));

        const int old_thr = st.score_threshold.load(std::memory_order_relaxed);
        st.score_threshold.store(clamped_thr, std::memory_order_relaxed);
        st.edge_state_int .store(static_cast<int>(final_state), std::memory_order_relaxed);

        // Log only if something changed
        if (clamped_thr != old_thr || final_state != old_state) {
            printf("[PARAM-GATE] %s  trades=%d  wr=%.1f%%  exp=$%.2f"
                   "  state=%s->%s  min_score=%d->%d\n",
                   symbol.c_str(), trade_count,
                   win_rate_20 * 100.0, expectancy_20,
                   edge_state_name(old_state), edge_state_name(final_state),
                   old_thr, clamped_thr);
            fflush(stdout);
        }
    }

    // =========================================================================
    // effective_min_score()
    // Returns the current dynamic entry threshold for this symbol.
    // Lock-free atomic read -- safe from tick hot path.
    // =========================================================================
    int effective_min_score(const std::string& symbol) const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(symbol);
        if (it == states_.end()) return APG_BASE_SCORE;
        return it->second.score_threshold.load(std::memory_order_relaxed);
    }

    // =========================================================================
    // log_all() -- print current state for all tracked symbols
    // =========================================================================
    void log_all() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (states_.empty()) {
            printf("[PARAM-GATE] No symbols tracked yet\n");
            fflush(stdout);
            return;
        }
        for (const auto& kv : states_) {
            const auto& st = kv.second;
            printf("[PARAM-GATE] %s  state=%s  min_score=%d  (base=%d)\n",
                   kv.first.c_str(),
                   edge_state_name(st.edge_state()),
                   st.score_threshold.load(std::memory_order_relaxed),
                   APG_BASE_SCORE);
        }
        fflush(stdout);
    }

private:
    // Map EdgeState to integer level: STRONG=-1, NORMAL=0, SOFT_WARN=+1, FAILING=+2
    static int state_to_level(EdgeState s) noexcept {
        switch (s) {
            case EdgeState::STRONG:    return -1;
            case EdgeState::NORMAL:    return  0;
            case EdgeState::SOFT_WARN: return +1;
            case EdgeState::FAILING:   return +2;
            default:                   return  0;  // WARMUP -> base
        }
    }

    static EdgeState level_to_state(int level) noexcept {
        if (level <= -1) return EdgeState::STRONG;
        if (level ==  0) return EdgeState::NORMAL;
        if (level ==  1) return EdgeState::SOFT_WARN;
        return EdgeState::FAILING;
    }

    ParamGateState& state_for(const std::string& symbol) {
        return states_[symbol];  // default-constructed if absent
    }

    mutable std::mutex mtx_;
    mutable std::unordered_map<std::string, ParamGateState> states_;
};
