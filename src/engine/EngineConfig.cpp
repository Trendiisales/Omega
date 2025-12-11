#include "EngineConfig.hpp"
#include <chrono>

namespace Omega {

static std::uint64_t defaultMonotonicNow() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static std::uint64_t defaultWallClockNow() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

EngineConfig::EngineConfig() {
    yieldHook = [](){};
    monotonicNow = &defaultMonotonicNow;
    wallClockNow = &defaultWallClockNow;
}

} // namespace Omega
