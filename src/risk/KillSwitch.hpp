#pragma once
#include <atomic>
#include <cstdint>

namespace Omega {

class KillSwitch {
public:
    KillSwitch();

    void trigger();
    void clear();

    bool isTriggered() const;

private:
    std::atomic<bool> tripped;
};

} // namespace Omega
