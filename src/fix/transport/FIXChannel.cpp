#include "FIXChannel.hpp"

namespace Omega {

FIXChannel::FIXChannel(FIXTransport* t)
    : tr(t) {}

FIXChannel::~FIXChannel() {
    disconnect();
}

void FIXChannel::setMessageHandler(MsgHandler h) {
    onMsg = h;
}

void FIXChannel::setStateHandler(StateHandler h) {
    onState = h;
}

void FIXChannel::attachCallbacks() {
    if (!tr) return;

    tr->setRxCallback([this](const std::string& m) {
        if (onMsg) onMsg(m);
    });

    tr->setStateCallback([this](bool up) {
        if (onState) onState(up);
    });
}

bool FIXChannel::connect(const std::string& host, int port) {
    if (!tr) return false;
    attachCallbacks();
    return tr->connect(host, port);
}

void FIXChannel::disconnect() {
    if (tr) tr->disconnect();
}

bool FIXChannel::send(const std::string& msg) {
    if (!tr) return false;
    return tr->sendRaw(msg);
}

} // namespace Omega
