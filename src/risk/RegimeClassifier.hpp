#pragma once
#include "../pipeline/MicroMetrics.hpp"

namespace Omega {

enum class Regime {
    Quiet,
    Trend,
    Volatile,
    Shocked
};

class RegimeClassifier {
public:
    RegimeClassifier();

    Regime classify(const MicroMetrics& m);

private:
    double trendThresh;
    double volThresh;
};

} // namespace Omega
