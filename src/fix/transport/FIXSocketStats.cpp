#include "FIXSocketStats.hpp"

namespace Omega {

FIXSocketStats::FIXSocketStats() {
    reset();
}

void FIXSocketStats::reset() {
    bytesTx.store(0);
    bytesRx.store(0);
    reconnects.store(0);
    heartbeatsTx.store(0);
    heartbeatsRx.store(0);
}

} // namespace Omega
