#include "ShockDetector.hpp"
#include <cmath>

namespace Omega {

ShockDetector::ShockDetector() {}

void ShockDetector::setThresholdBp(double bp) {
    st.thresholdBp = bp;
}

bool ShockDetector::update(const Tick& t) {
    double mid = (t.bid + t.ask) * 0.5;
    if (st.lastPrice <= 0.0) {
        st.lastPrice = mid;
        st.inShock = false;
        return false;
    }

    double diff = std::fabs(mid - st.lastPrice);
    double bp   = (st.lastPrice > 0.0) ? (diff / st.lastPrice * 10000.0) : 0.0;

    st.inShock   = (bp >= st.thresholdBp);
    st.lastPrice = mid;
    return st.inShock;
}

bool ShockDetector::isShocked() const {
    return st.inShock;
}

} // namespace Omega
