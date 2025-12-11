#include "FIXBridge.hpp"

namespace Omega {

FIXBridge::FIXBridge()
    : transport(nullptr), exec(nullptr), md(nullptr)
{}

FIXBridge::~FIXBridge() {}

void FIXBridge::setTransport(FIXTransport* t) {
    transport = t;
}

void FIXBridge::setExecHandler(FIXExecHandler* e) {
    exec = e;
    if (exec) {
        exec->setExecCallback([this](const ExecReport& r) {
            if (execCB) execCB(r);
        });
    }
}

void FIXBridge::setMarketDataHandler(FIXMDHandler* m) {
    md = m;
}

void FIXBridge::setTickCallback(std::function<void(const Tick&)> cb) {
    tickCB = cb;
}

void FIXBridge::setExecCallback(std::function<void(const ExecReport&)> cb) {
    execCB = cb;
}

bool FIXBridge::connect(const std::string& host, int port) {
    if (!transport) return false;
    return transport->connect(host, port);
}

void FIXBridge::disconnect() {
    if (transport) transport->disconnect();
}

bool FIXBridge::process(const FIXMessage& msg) {
    // Route message based on type (tag 35)
    std::string msgType = msg.get(35);
    
    if (msgType == "W" || msgType == "X") {
        // Market data snapshot/update
        if (md) {
            // md->process(msg);
        }
        return true;
    }
    else if (msgType == "8") {
        // Execution report - handled by exec callback
        return true;
    }
    else if (msgType == "9") {
        // Order cancel reject
        return true;
    }
    
    return true;
}

bool FIXBridge::sendOrder(const std::string& symbol, 
                          const std::string& side,
                          double qty, 
                          double price) {
    if (!exec) return false;
    return exec->sendNewOrder(symbol, side, qty, price);
}

bool FIXBridge::cancelOrder(const std::string& clOrdId) {
    if (!exec) return false;
    return exec->sendCancel(clOrdId);
}

} // namespace Omega
