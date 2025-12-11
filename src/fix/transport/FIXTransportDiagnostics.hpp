#pragma once
#include <string>
#include <atomic>
#include <cstdint>
#include "FIXTransport.hpp"
#include "FIXSocketStats.hpp"

namespace Omega {

class FIXTransportDiagnostics {
public:
    FIXTransportDiagnostics();

    void attach(FIXTransport* t, FIXSocketStats* s);

    // update counters
    void onTx(const std::string& msg);
    void onRx(const std::string& msg);

    // read-only metrics
    uint64_t bytesSent() const;
    uint64_t bytesReceived() const;
    uint64_t reconnectCount() const;

private:
    FIXTransport* tr;
    FIXSocketStats* stats;

    std::atomic<uint64_t> txBytes;
    std::atomic<uint64_t> rxBytes;
};

} // namespace Omega
