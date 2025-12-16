// =============================================================================
// EngineSupervisor.hpp - Cold-Path Health Policy
// =============================================================================
#pragma once

#include "engine/EngineHealth.hpp"
#include "engine/QueueMetrics.hpp"
#include "engine/BurstDetector.hpp"

namespace chimera {
namespace engine {

struct EngineSupervisor {
    uint64_t max_tick_drops;
    uint64_t max_intent_drops;
    uint64_t max_invalid_ticks;
    BurstDetector ingress_burst;

    EngineSupervisor() noexcept
        : max_tick_drops(10000)
        , max_intent_drops(1000)
        , max_invalid_ticks(1000)
        , ingress_burst()
    {}

    EngineSupervisor(uint64_t tick_drops,
                     uint64_t intent_drops,
                     uint64_t invalid_ticks,
                     uint64_t burst_warn_ppm,
                     uint64_t burst_kill_ppm) noexcept
        : max_tick_drops(tick_drops)
        , max_intent_drops(intent_drops)
        , max_invalid_ticks(invalid_ticks)
        , ingress_burst(burst_warn_ppm, burst_kill_ppm)
    {}

    void set_thresholds(uint64_t tick_drops,
                       uint64_t intent_drops,
                       uint64_t invalid_ticks,
                       uint64_t burst_warn_ppm,
                       uint64_t burst_kill_ppm) noexcept {
        max_tick_drops = tick_drops;
        max_intent_drops = intent_drops;
        max_invalid_ticks = invalid_ticks;
        ingress_burst = BurstDetector(burst_warn_ppm, burst_kill_ppm);
    }

    inline void evaluate(EngineHealth& h, QueueMetrics& ingress_metrics) noexcept {
        if (h.is_killed()) return;

        if (ingress_burst.detect_burst(ingress_metrics)) {
            h.kill(EngineKillReason::TICK_QUEUE_OVERFLOW);
            return;
        }

        if (h.tick_drops.load(std::memory_order_relaxed) > max_tick_drops) {
            h.kill(EngineKillReason::TICK_QUEUE_OVERFLOW);
            return;
        }

        if (h.intent_drops.load(std::memory_order_relaxed) > max_intent_drops) {
            h.kill(EngineKillReason::INTENT_QUEUE_OVERFLOW);
            return;
        }

        if (h.invalid_ticks.load(std::memory_order_relaxed) > max_invalid_ticks) {
            h.kill(EngineKillReason::INVALID_TICK);
            return;
        }
    }

    inline void evaluate(EngineHealth& h) noexcept {
        if (h.is_killed()) return;

        if (h.tick_drops.load(std::memory_order_relaxed) > max_tick_drops) {
            h.kill(EngineKillReason::TICK_QUEUE_OVERFLOW);
            return;
        }

        if (h.intent_drops.load(std::memory_order_relaxed) > max_intent_drops) {
            h.kill(EngineKillReason::INTENT_QUEUE_OVERFLOW);
            return;
        }

        if (h.invalid_ticks.load(std::memory_order_relaxed) > max_invalid_ticks) {
            h.kill(EngineKillReason::INVALID_TICK);
            return;
        }
    }
};

} // namespace engine
} // namespace chimera
