// =============================================================================
// EngineCore.hpp - One Instance Per Feed
// =============================================================================
// One engine = one health domain.
// Owns:
//  - strategies
//  - latency stats (thread-local)
//
// Does NOT:
//  - talk to other engines
//  - allocate
//  - log on hot path
// =============================================================================
#pragma once

#include <cstddef>
#include "market/MarketTypes.hpp"
#include "market/TickValidator.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/Intent.hpp"
#include "engine/IntentQueue.hpp"
#include "engine/EngineIngress.hpp"
#include "engine/EngineHealth.hpp"
#include "core/LatencyStats.hpp"
#include "core/MonotonicClock.hpp"

namespace chimera {
namespace engine {

// =============================================================================
// EngineCore
// =============================================================================
template <typename StrategyRunnerT,
          std::size_t IngressQ = 16384,
          std::size_t IntentQ = 4096>
class EngineCore final {
public:
    using Tick = chimera::market::Tick;

    // Full constructor with health and validation
    EngineCore(const EngineConfig& cfg,
               StrategyRunnerT& runner,
               IntentQueue<IntentQ>& intent_q,
               EngineHealth& health,
               const chimera::market::TickValidator& validator) noexcept
        : cfg_(cfg),
          runner_(runner),
          intent_q_(intent_q),
          health_(&health),
          validator_(&validator),
          last_ingress_ts_(0),
          last_exchange_ts_(0),
          last_exchange_update_ts_(0),
          ticks_processed_(0)
    {
    }

    // Simple constructor (no health/validation)
    EngineCore(const EngineConfig& cfg,
               StrategyRunnerT& runner,
               IntentQueue<IntentQ>& intent_q) noexcept
        : cfg_(cfg),
          runner_(runner),
          intent_q_(intent_q),
          health_(nullptr),
          validator_(nullptr),
          last_ingress_ts_(0),
          last_exchange_ts_(0),
          last_exchange_update_ts_(0),
          ticks_processed_(0)
    {
    }

    // =========================================================================
    // Engine main loop step.
    // Called by the feed-dedicated thread.
    // =========================================================================
    inline void poll(EngineIngress<IngressQ>& ingress,
                     chimera::core::ThreadLatencyStats& lat) noexcept
    {
        if (health_ && health_->is_killed()) {
            return;
        }

        Tick t;
        while (ingress.pop_tick(t)) {
            // Validate tick if validator present
            if (validator_ && health_) {
                if (!validator_->validate(t,
                                         last_ingress_ts_,
                                         last_exchange_ts_,
                                         last_exchange_update_ts_,
                                         *health_)) {
                    continue; // Skip invalid tick
                }
            }

            const uint64_t t0 = t.ingress_ts_ns;

            Intent intent;
            const bool generated = runner_.on_tick(t, intent);
            
            const uint64_t t1 = chimera::core::MonotonicClock::now_ns();
            lat.tick_to_signal.add(t1 - t0);

            if (generated) {
                intent.ts_ns = t1;
                if (!intent_q_.push(intent)) {
                    if (health_) {
                        health_->intent_drops.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            ++ticks_processed_;
            if (health_) {
                health_->ticks_processed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    uint64_t ticks_processed() const noexcept { return ticks_processed_; }

private:
    const EngineConfig cfg_;
    StrategyRunnerT& runner_;
    IntentQueue<IntentQ>& intent_q_;
    EngineHealth* health_;
    const chimera::market::TickValidator* validator_;

    uint64_t last_ingress_ts_;
    uint64_t last_exchange_ts_;
    uint64_t last_exchange_update_ts_;
    uint64_t ticks_processed_;
};

} // namespace engine
} // namespace chimera
