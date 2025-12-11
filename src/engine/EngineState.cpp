#include "EngineState.hpp"

namespace Omega {

void EngineState::reset() {
    mode = EngineMode::Idle;
    mainLoopIterations = 0;
    tickCount          = 0;
    bookCount          = 0;
    execCount          = 0;
    rejectCount        = 0;
    ordersSent         = 0;
    riskBlocked        = 0;
    routeFailed        = 0;
    lastStartTs        = 0;
    lastDepthUpdateTs  = 0;
}

} // namespace Omega
