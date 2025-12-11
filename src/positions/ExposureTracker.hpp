#pragma once
#include <string>
#include <unordered_map>

namespace Omega {

struct ExposureRecord {
    double notional = 0.0;
};

class ExposureTracker {
public:
    ExposureTracker();

    void reset();
    void setLimit(double perSymbol, double global);

    void add(const std::string& sym, double notionalDelta);
    double symbolExposure(const std::string& sym) const;
    double globalExposure() const;

    bool withinLimits(const std::string& sym, double additional) const;

private:
    double limitPerSymbol;
    double limitGlobal;
    std::unordered_map<std::string, ExposureRecord> map;
};

} // namespace Omega
