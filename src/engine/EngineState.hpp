#pragma once
#include <cstdint>

namespace Omega {

enum class EngineMode {
    Idle,
    Running,
    Stopping,
    Stopped
};

struct EngineState {
    EngineMode mode = EngineMode::Idle;

    std::uint64_t mainLoopIterations = 0;
    std::uint64_t tickCount          = 0;
    std::uint64_t bookCount          = 0;
    std::uint64_t execCount          = 0;
    std::uint64_t rejectCount        = 0;
    std::uint64_t ordersSent         = 0;
    std::uint64_t riskBlocked        = 0;
    std::uint64_t routeFailed        = 0;

    std::uint64_t lastStartTs        = 0;
    std::uint64_t lastDepthUpdateTs  = 0;

    void reset();
};

} // namespace Omega
