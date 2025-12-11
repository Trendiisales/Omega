#include "FIXTransportDiagnostics.hpp"

namespace Omega {

FIXTransportDiagnostics::FIXTransportDiagnostics()
    : tr(nullptr),
      stats(nullptr),
      txBytes(0),
      rxBytes(0) {}

void FIXTransportDiagnostics::attach(FIXTransport* t, FIXSocketStats* s) {
    tr = t;
    stats = s;

    if (!tr) return;

    tr->setRxCallback([this](const std::string& msg) {
        rxBytes.fetch_add(msg.size(), std::memory_order_relaxed);
        if (stats) stats->bytesRx.fetch_add(msg.size(), std::memory_order_relaxed);
    });
}

void FIXTransportDiagnostics::onTx(const std::string& msg) {
    txBytes.fetch_add(msg.size(), std::memory_order_relaxed);
    if (stats) stats->bytesTx.fetch_add(msg.size(), std::memory_order_relaxed);
}

void FIXTransportDiagnostics::onRx(const std::string& msg) {
    rxBytes.fetch_add(msg.size(), std::memory_order_relaxed);
    if (stats) stats->bytesRx.fetch_add(msg.size(), std::memory_order_relaxed);
}

uint64_t FIXTransportDiagnostics::bytesSent() const {
    return txBytes.load(std::memory_order_relaxed);
}

uint64_t FIXTransportDiagnostics::bytesReceived() const {
    return rxBytes.load(std::memory_order_relaxed);
}

uint64_t FIXTransportDiagnostics::reconnectCount() const {
    if (!stats) return 0;
    return stats->reconnects.load(std::memory_order_relaxed);
}

} // namespace Omega
