#include "PnLTracker.hpp"

namespace Omega {

PnLTracker::PnLTracker()
    : realizedPnL(0.0), totalFees(0.0) {}

void PnLTracker::reset() {
    realizedPnL = 0.0;
    totalFees   = 0.0;
}

void PnLTracker::onExec(const ExecReport& r) {
    // hook point: compute realized PnL from fills + fee tags
    // currently left as additive placeholder
    (void)r;
}

double PnLTracker::realized() const {
    return realizedPnL;
}

double PnLTracker::fees() const {
    return totalFees;
}

} // namespace Omega
