#pragma once
#include <string>
#include <cstdint>
#include "../fix/execution/FIXExecHandler.hpp"

namespace Omega {

struct PositionSnapshot {
    double qty      = 0.0;
    double avgPrice = 0.0;
    double unrealizedPnL = 0.0;
};

class PositionTracker {
public:
    PositionTracker();

    void reset();
    void setSymbol(const std::string& s);

    void update(const ExecReport& r);
    PositionSnapshot snapshot() const;
    
    double position() const { return qty; }
    double avgPx() const { return avgPrice; }

private:
    std::string symbol;
    double qty;
    double avgPrice;
};

} // namespace Omega
