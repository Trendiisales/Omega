#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include "FIXTransport.hpp"

namespace Omega {

class FIXChannel {
public:
    using MsgHandler   = std::function<void(const std::string&)>;
    using StateHandler = std::function<void(bool)>;

    FIXChannel(FIXTransport* t);
    ~FIXChannel();

    void setMessageHandler(MsgHandler h);
    void setStateHandler(StateHandler h);

    bool connect(const std::string& host, int port);
    void disconnect();

    bool send(const std::string& msg);

private:
    void attachCallbacks();

private:
    FIXTransport* tr;
    MsgHandler onMsg;
    StateHandler onState;
};

} // namespace Omega
