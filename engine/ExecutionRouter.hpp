// =============================================================================
// ExecutionRouter.hpp - Execution Thread Intent Consumer
// =============================================================================
// Dedicated execution thread consumes intents.
// No strategy logic here.
// =============================================================================
#pragma once

#include "engine/Intent.hpp"
#include "engine/IntentQueue.hpp"
#include "core/MonotonicClock.hpp"
#include "core/LatencyStats.hpp"

namespace chimera {
namespace engine {

// =============================================================================
// ExecutionRouter
// =============================================================================
template <std::size_t Capacity = 4096>
class ExecutionRouter final {
public:
    explicit ExecutionRouter(IntentQueue<Capacity>& q) noexcept
        : q_(q), intents_processed_(0) {}

    // Pop next intent (non-blocking)
    inline bool pop(Intent& out) noexcept {
        return q_.pop(out);
    }

    // Process all pending intents with latency tracking
    template <typename ExecFunc>
    inline void process_all(ExecFunc&& exec_fn,
                           chimera::core::ThreadLatencyStats& lat) noexcept
    {
        Intent intent;
        while (q_.pop(intent)) {
            const uint64_t t0 = intent.ts_ns;
            
            exec_fn(intent);
            
            const uint64_t t1 = chimera::core::MonotonicClock::now_ns();
            lat.intent_to_exec.add(t1 - t0);
            
            ++intents_processed_;
        }
    }

    uint64_t intents_processed() const noexcept { return intents_processed_; }

private:
    IntentQueue<Capacity>& q_;
    uint64_t intents_processed_;
};

} // namespace engine
} // namespace chimera
