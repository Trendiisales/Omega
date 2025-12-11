#pragma once
#include "../transport/FIXTransport.hpp"
#include "../execution/FIXExecHandler.hpp"
#include "../md/FIXMDHandler.hpp"
#include "../FIXMessage.hpp"
#include "../../market/Tick.hpp"
#include <functional>

namespace Omega {

class FIXBridge {
public:
    FIXBridge();
    ~FIXBridge();

    void setTransport(FIXTransport* t);
    void setExecHandler(FIXExecHandler* e);
    void setMarketDataHandler(FIXMDHandler* m);

    void setTickCallback(std::function<void(const Tick&)> cb);
    void setExecCallback(std::function<void(const ExecReport&)> cb);

    bool connect(const std::string& host, int port);
    void disconnect();

    // Process incoming FIX message
    bool process(const FIXMessage& msg);

    bool sendOrder(const std::string& symbol, 
                   const std::string& side,
                   double qty, 
                   double price);
    bool cancelOrder(const std::string& clOrdId);

private:
    FIXTransport* transport;
    FIXExecHandler* exec;
    FIXMDHandler* md;

    std::function<void(const Tick&)> tickCB;
    std::function<void(const ExecReport&)> execCB;
};

} // namespace Omega
