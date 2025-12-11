#include "FIXTransport.hpp"

namespace Omega {

FIXTransport::FIXTransport() {}
FIXTransport::~FIXTransport() {}

void FIXTransport::setRxCallback(RxCallback cb) {
    onRx = cb;
}

void FIXTransport::setStateCallback(StateCallback cb) {
    onState = cb;
}

void FIXTransport::emitRx(const std::string& msg) {
    if (onRx) onRx(msg);
}

void FIXTransport::emitState(bool up) {
    if (onState) onState(up);
}

} // namespace Omega
