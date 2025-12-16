#include "ExposureTracker.hpp"

namespace Omega {

ExposureTracker::ExposureTracker()
    : limitPerSymbol(0.0), limitGlobal(0.0) {}

void ExposureTracker::reset() {
    map.clear();
}

void ExposureTracker::setLimit(double perSymbol, double global) {
    limitPerSymbol = perSymbol;
    limitGlobal    = global;
}

void ExposureTracker::add(const std::string& sym, double notionalDelta) {
    auto& e = map[sym];
    e.notional += notionalDelta;
}

double ExposureTracker::symbolExposure(const std::string& sym) const {
    auto it = map.find(sym);
    if (it == map.end()) return 0.0;
    return it->second.notional;
}

double ExposureTracker::globalExposure() const {
    double sum = 0.0;
    for (const auto& kv : map)
        sum += kv.second.notional;
    return sum;
}

bool ExposureTracker::withinLimits(const std::string& sym, double additional) const {
    double symExp  = symbolExposure(sym);
    double globExp = globalExposure();
    if (limitPerSymbol > 0.0 && symExp + additional > limitPerSymbol) return false;
    if (limitGlobal    > 0.0 && globExp + additional > limitGlobal)    return false;
    return true;
}

} // namespace Omega
