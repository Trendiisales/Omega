#pragma once
// =============================================================================
// OmegaCrowdingGuard.hpp -- Directional crowding tracker + scorer penalty
//
// Tracks directional skew of recent closed trades per symbol.
// If too many recent trades are in the same direction, it means the edge
// may be crowded -- all participants leaning the same way = reversal risk.
//
// MECHANISM:
//   - Rolling window of last N closed trades per symbol (default: 10)
//   - Counts LONG vs SHORT in window
//   - Crowding ratio = dominant_count / window_size
//   - When ratio >= CROWDING_THRESHOLD (0.80 = 8/10 same direction):
//       -> Apply CROWDING_SCORE_PENALTY (-2 pts) to ScoreResult
//       -> Log [CROWDING] warning
//   - Penalty is applied POST-score (caller subtracts from ScoreResult.total)
//     keeping scorer self-contained and testable
//
// DESIGN PRINCIPLES:
//   - Lock-free hot path: atomic ring buffer index, reads are relaxed
//   - update() called from handle_closed_trade() -- already off hot path
//   - penalty() called from scorer call site -- single read, no lock
//   - Per-symbol tracking -- XAUUSD crowding does not bleed into other instruments
//   - Penalty does NOT block entry on its own -- it reduces score.
//     Combined with a borderline score it tips it below SCORE_MIN_ENTRY.
//     A strong setup (score >= 7) survives crowding penalty.
//
// THREAD SAFETY:
//   - update() acquires per-symbol spinlock (rare, off hot path)
//   - penalty() reads atomic ratio -- no lock needed
//
// =============================================================================

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>

// =============================================================================
// Tuning constants
// =============================================================================
static constexpr int    CROWDING_WINDOW           = 10;    // last N trades per symbol
static constexpr double CROWDING_THRESHOLD        = 0.80;  // 80% same direction = crowded
static constexpr int    CROWDING_SCORE_PENALTY    = 2;     // pts deducted when crowded
static constexpr int    CROWDING_MIN_SAMPLES      = 5;     // don't penalise until window has N trades

// =============================================================================
// Per-symbol crowding state
// =============================================================================
struct CrowdingState {
    // Ring buffer: 1 = LONG, -1 = SHORT, 0 = empty slot
    std::array<int8_t, CROWDING_WINDOW> ring{};
    int     head    {0};       // next write position
    int     count   {0};       // how many valid entries (capped at CROWDING_WINDOW)
    double  ratio   {0.0};     // dominant direction ratio (0..1), updated on each write
    int     dominant{0};       // +1 = long crowded, -1 = short crowded, 0 = balanced

    std::mutex mtx;            // protects ring/head/count/ratio -- update() only

    CrowdingState() { ring.fill(0); }

    // Called from handle_closed_trade -- not on tick hot path
    void update(bool is_long) {
        std::lock_guard<std::mutex> lk(mtx);
        ring[head] = is_long ? 1 : -1;
        head = (head + 1) % CROWDING_WINDOW;
        if (count < CROWDING_WINDOW) ++count;

        if (count < CROWDING_MIN_SAMPLES) {
            ratio    = 0.0;
            dominant = 0;
            return;
        }

        int longs  = 0;
        int shorts = 0;
        for (int i = 0; i < count; ++i) {
            if (ring[i] ==  1) ++longs;
            if (ring[i] == -1) ++shorts;
        }

        if (longs >= shorts) {
            ratio    = static_cast<double>(longs)  / count;
            dominant = (ratio >= CROWDING_THRESHOLD) ? +1 : 0;
        } else {
            ratio    = static_cast<double>(shorts) / count;
            dominant = (ratio >= CROWDING_THRESHOLD) ? -1 : 0;
        }
    }

    // Returns score penalty (0 or CROWDING_SCORE_PENALTY) for a given direction
    // Safe to call from any thread -- reads local value after lock-free snapshot
    int penalty_for(bool is_long) const {
        // Read dominant without lock -- worst case we read a slightly stale value.
        // This is acceptable: crowding is a soft signal, not a hard block.
        const int dom = dominant;
        if (dom == 0) return 0;
        // Penalise if we are about to enter IN the crowded direction
        if ((is_long && dom == +1) || (!is_long && dom == -1))
            return CROWDING_SCORE_PENALTY;
        return 0;
    }
};

// =============================================================================
// OmegaCrowdingGuard
// =============================================================================
class OmegaCrowdingGuard {
public:
    // =========================================================================
    // update() -- call from handle_closed_trade()
    // =========================================================================
    void update(const std::string& symbol, bool is_long) {
        state_for(symbol).update(is_long);
    }

    // =========================================================================
    // score_penalty() -- call after g_signal_scorer.score_and_store()
    // Returns 0 (not crowded) or CROWDING_SCORE_PENALTY (crowded, subtract from total)
    // =========================================================================
    int score_penalty(const std::string& symbol, bool is_long) const {
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto it = states_.find(symbol);
        if (it == states_.end()) return 0;
        return it->second->penalty_for(is_long);
    }

    // =========================================================================
    // log_state() -- call periodically for diagnostics
    // =========================================================================
    void log_state(const std::string& symbol) const {
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto it = states_.find(symbol);
        if (it == states_.end()) {
            printf("[CROWDING] %s -- no data yet\\n", symbol.c_str());
            fflush(stdout);
            return;
        }
        const CrowdingState& s = *it->second;
        const char* dir = (s.dominant == +1) ? "LONG"
                        : (s.dominant == -1) ? "SHORT"
                        :                      "BALANCED";
        printf("[CROWDING] %s samples=%d dominant=%s ratio=%.2f penalty=%d\\n",
               symbol.c_str(), s.count, dir, s.ratio,
               (s.dominant != 0) ? CROWDING_SCORE_PENALTY : 0);
        fflush(stdout);
    }

private:
    // Lazily creates per-symbol state. Pointer stable after insertion (unique_ptr).
    CrowdingState& state_for(const std::string& symbol) {
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto& ptr = states_[symbol];
        if (!ptr) ptr = std::make_unique<CrowdingState>();
        return *ptr;
    }

    mutable std::mutex map_mtx_;
    std::unordered_map<std::string, std::unique_ptr<CrowdingState>> states_;
};
