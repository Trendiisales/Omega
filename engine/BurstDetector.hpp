// =============================================================================
// BurstDetector.hpp - Burst Detection Logic
// =============================================================================
// Cold-path: evaluates drop rate over rolling windows
// =============================================================================
#pragma once

#include <cstdint>
#include "engine/QueueMetrics.hpp"

namespace chimera {
namespace engine {

struct BurstDetector {
    uint64_t warn_drop_ratio_ppm;  // Warning threshold (drops per million)
    uint64_t kill_drop_ratio_ppm;  // Kill threshold (drops per million)

    BurstDetector() noexcept
        : warn_drop_ratio_ppm(1000)    // 0.1% warn
        , kill_drop_ratio_ppm(10000)   // 1% kill
    {}

    BurstDetector(uint64_t warn_ppm, uint64_t kill_ppm) noexcept
        : warn_drop_ratio_ppm(warn_ppm)
        , kill_drop_ratio_ppm(kill_ppm)
    {}

    // Returns true if kill threshold exceeded
    inline bool detect_burst(QueueMetrics& m) noexcept {
        const uint64_t ratio_ppm = m.get_window_drop_rate_ppm();
        return ratio_ppm >= kill_drop_ratio_ppm;
    }

    // Returns true if warning threshold exceeded
    inline bool detect_warning(QueueMetrics& m) noexcept {
        const uint64_t ratio_ppm = m.get_window_drop_rate_ppm();
        return ratio_ppm >= warn_drop_ratio_ppm;
    }
};

} // namespace engine
} // namespace chimera
