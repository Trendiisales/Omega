// =============================================================================
// EngineIngress.hpp - Single Entry Point for Market Data
// =============================================================================
// Feed → Engine boundary with drop accounting.
// No parsing, no logic, no allocation.
// =============================================================================
#pragma once

#include "market/MarketTypes.hpp"
#include "core/SPSCQueue.hpp"
#include "engine/EngineHealth.hpp"
#include "engine/QueueMetrics.hpp"

namespace chimera {
namespace engine {

// Default capacity: 16384 ticks
template <std::size_t QueueSize = 16384>
class EngineIngress {
public:
    using Tick = chimera::market::Tick;

    // Constructor with health tracking
    EngineIngress(EngineHealth& health, QueueMetrics& metrics) noexcept
        : health_(&health)
        , metrics_(&metrics)
    {}

    // Default constructor (no tracking)
    EngineIngress() noexcept
        : health_(nullptr)
        , metrics_(nullptr)
    {}

    // Feed thread calls this
    inline bool push_tick(const Tick& t) noexcept {
        if (metrics_) {
            metrics_->record_attempt();
        }

        if (health_ && health_->is_killed()) {
            return false;
        }

        if (!queue_.push(t)) {
            if (metrics_) {
                metrics_->record_drop();
            }
            if (health_) {
                health_->tick_drops.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        return true;
    }

    // Engine thread calls this
    inline bool pop_tick(Tick& t) noexcept {
        return queue_.pop(t);
    }

    inline bool empty() const noexcept {
        return queue_.empty();
    }

    inline std::size_t size() const noexcept {
        return queue_.size();
    }

private:
    chimera::core::SPSCQueue<Tick, QueueSize> queue_;
    EngineHealth* health_;
    QueueMetrics* metrics_;
};

} // namespace engine
} // namespace chimera
