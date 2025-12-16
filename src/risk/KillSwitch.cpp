#include "KillSwitch.hpp"

namespace Omega {

KillSwitch::KillSwitch()
    : tripped(false) {}

void KillSwitch::trigger() {
    tripped.store(true, std::memory_order_release);
}

void KillSwitch::clear() {
    tripped.store(false, std::memory_order_release);
}

bool KillSwitch::isTriggered() const {
    return tripped.load(std::memory_order_acquire);
}

} // namespace Omega
