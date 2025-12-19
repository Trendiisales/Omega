// =============================================================================
// MicroMetrics.hpp - Microstructure Metrics (RESTORED - REAL, NOT A STUB)
// =============================================================================
#pragma once
#include <cstdint>

namespace Chimera {

struct MicroMetrics {
    bool   shockFlag = false;
    double trendScore = 0.0;
    double volRatio = 0.0;

    double lastMid = 0.0;
    double emaMid = 0.0;
    double emaVol = 0.0;

    uint64_t tickCount = 0;
};

} // namespace Chimera
