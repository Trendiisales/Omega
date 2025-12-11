#pragma once
#include <thread>
#include <atomic>
#include <string>
#include "FIXTransport.hpp"
#include "FIXReconnectPolicy.hpp"

namespace Omega {

class FIXTcpReconnector {
public:
    FIXTcpReconnector(FIXTransport* t);

    void setTarget(const std::string& h, int p);
    void start();
    void stop();

private:
    void loop();

private:
    FIXTransport* tr;
    std::thread th;
    std::atomic<bool> running;

    std::string host;
    int port;

    FIXReconnectPolicy policy;
};

} // namespace Omega
