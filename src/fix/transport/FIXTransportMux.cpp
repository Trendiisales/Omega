#include "FIXTransportMux.hpp"

namespace Omega {

FIXTransportMux::FIXTransportMux() {}
FIXTransportMux::~FIXTransportMux() {}

void FIXTransportMux::add(FIXTransport* t) {
    list.push_back(t);
}

bool FIXTransportMux::sendRaw(const std::string& msg) {
    bool ok = true;
    for (auto* t : list) {
        if (t && !t->sendRaw(msg)) ok = false;
    }
    return ok;
}

} // namespace Omega
