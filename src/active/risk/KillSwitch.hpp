// =============================================================================
// KillSwitch.hpp - RESTORED ORIGINAL API (LOCKED)
// =============================================================================
// DO NOT MODIFY - This is the locked API surface
// =============================================================================
#pragma once
#include <atomic>

namespace Chimera {

class KillSwitch {
public:
    static void trigger() noexcept;
    static void clear() noexcept;
    static bool isTriggered() noexcept;

private:
    static std::atomic<bool> triggered;
};

} // namespace Chimera
