#pragma once
#include <string>
#include <mutex>
#include "../FIXMessage.hpp"
#include "../FIXSession.hpp"
#include "FIXLatencyMonitor.hpp"
#include "FIXExecThrottle.hpp"

namespace Omega {

class FIXMultiSessionRouter {
public:
    FIXMultiSessionRouter();

    void setPrimary(FIXSession*);
    void setBackup(FIXSession*);

    void setLatency(FIXLatencyMonitor*);
    void setThrottle(FIXExecThrottle*);

    bool routeSend(FIXMessage&);
    void routeRecv(FIXMessage&);

private:
    std::mutex lock;

    FIXSession* primary=nullptr;
    FIXSession* backup=nullptr;

    FIXLatencyMonitor* latency=nullptr;
    FIXExecThrottle* throttle=nullptr;

    bool useBackup=false;
};

}
