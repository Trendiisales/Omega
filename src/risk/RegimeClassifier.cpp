#include "RegimeClassifier.hpp"

namespace Omega {

RegimeClassifier::RegimeClassifier()
    : trendThresh(0.7), volThresh(1.5) {}

Regime RegimeClassifier::classify(const MicroMetrics& m) {
    if (m.shockFlag) return Regime::Shocked;
    if (m.trendScore > trendThresh && m.volRatio <= volThresh) return Regime::Trend;
    if (m.volRatio > volThresh) return Regime::Volatile;
    return Regime::Quiet;
}

} // namespace Omega
