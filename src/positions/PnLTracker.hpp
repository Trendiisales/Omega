#pragma once
#include <cstdint>
#include "../fix/execution/FIXExecHandler.hpp"

namespace Omega {

class PnLTracker {
public:
    PnLTracker();

    void reset();
    void onExec(const ExecReport& r);

    double realized() const;
    double fees() const;

private:
    double realizedPnL;
    double totalFees;
};

} // namespace Omega
