// =============================================================================
// PnLTracker.hpp - RESTORED ORIGINAL API (LOCKED)
// =============================================================================
// DO NOT MODIFY - This is the locked API surface
// =============================================================================
#pragma once
#include "../fix/execution/FIXExecHandler.hpp"

namespace Chimera {

class PnLTracker {
public:
    void onExec(const ExecReport& r) noexcept;

    double realized() const noexcept;
    double fees() const noexcept;
    
    void reset() noexcept;

private:
    double realizedPnl = 0.0;
    double totalFees = 0.0;
};

} // namespace Chimera
