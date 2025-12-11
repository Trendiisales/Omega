#pragma once
#include <string>
#include <thread>
#include <atomic>
#include "FIXTransport.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

namespace Omega {

class FIXTcpSocket : public FIXTransport {
public:
    FIXTcpSocket();
    ~FIXTcpSocket();

    bool connect(const std::string& host, int port) override;
    void disconnect() override;

    bool sendRaw(const std::string& msg) override;
    
    // Additional methods
    bool isConnected() const;
    int sendRaw(const char* data, int len);
    int recvRaw(char* buf, int maxLen);
    int getFd() const;

private:
    void rxLoop();

private:
    std::thread rxThread;
    std::atomic<bool> running;
    int sock;
};

} // namespace Omega
