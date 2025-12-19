#pragma once
// =============================================================================
// FIXSSLTransport.hpp - SSL-enabled FIX transport for cTrader
// =============================================================================
// Uses OpenSSL for secure FIX 4.4 connections to cTrader
// Required for demo-uk-eqx-02.p.c-trader.com:5212
// =============================================================================
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>
#include <iostream>

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

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "FIXTransport.hpp"

namespace Chimera {

class FIXSSLTransport : public FIXTransport {
public:
    FIXSSLTransport();
    ~FIXSSLTransport() override;

    bool connect(const std::string& host, int port) override;
    void disconnect() override;
    bool sendRaw(const std::string& msg) override;
    
    bool isConnected() const { return connected_.load(); }
    
    // Stats
    uint64_t getBytesSent() const { return bytesSent_.load(); }
    uint64_t getBytesRecv() const { return bytesRecv_.load(); }

private:
    void rxLoop();
    void txLoop();
    bool sslHandshake();
    void cleanupSSL();
    
    int sslRead(char* buf, int len);
    int sslWrite(const char* buf, int len);
    
    void processBuffer();

private:
    int sock_;
    SSL_CTX* sslCtx_;
    SSL* ssl_;
    
    std::string host_;
    int port_;
    
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    
    std::thread rxThread_;
    std::thread txThread_;
    
    std::queue<std::string> txQueue_;
    // COLD_PATH_ONLY: transport state

    std::mutex txMtx_;
    std::condition_variable txCv_;
    // COLD_PATH_ONLY: transport state

    std::mutex writeMtx_;
    
    std::string rxBuffer_;
    // COLD_PATH_ONLY: transport state

    std::mutex rxMtx_;
    
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesRecv_{0};
    
    static std::once_flag sslInitFlag_;
    static void initSSL();
};

} // namespace Chimera
