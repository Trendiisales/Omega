#pragma once
// =============================================================================
// SSLWebSocketClient.hpp - SSL-enabled WebSocket client for Binance
// =============================================================================
// Uses OpenSSL for secure connections to wss:// endpoints
// Required for Binance stream.binance.com:9443
// =============================================================================
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace Chimera {

class SSLWebSocketClient {
public:
    SSLWebSocketClient();
    ~SSLWebSocketClient();

    // Connect to WebSocket URL (wss:// or ws://)
    bool connect(const std::string& url);
    
    // Legacy interface
    bool connect(const std::string& host, const std::string& path, int port, bool useSSL = true);
    
    void disconnect();
    void close() { disconnect(); }

    bool send(const std::string& msg);
    bool sendText(const std::string& txt) { return send(txt); }

    void setMessageCallback(std::function<void(const std::string&)> cb);
    void setOnMessage(std::function<void(const std::string&)> cb) { setMessageCallback(cb); }
    void setCallback(std::function<void(const std::string&)> cb) { setMessageCallback(cb); }
    
    void setStateCallback(std::function<void(bool)> cb);
    
    bool isConnected() const { return connected_.load(); }

private:
    void readerLoop();
    void writerLoop();
    bool parseUrl(const std::string& url, std::string& host, std::string& path, int& port, bool& ssl);
    bool doConnect(const std::string& host, const std::string& path, int port, bool useSSL);
    bool sslHandshake();
    bool wsHandshake(const std::string& host, const std::string& path);
    
    int sslRead(char* buf, int len);
    int sslWrite(const char* buf, int len);
    void cleanupSSL();

private:
    int sock_;
    SSL_CTX* sslCtx_;
    SSL* ssl_;
    bool useSSL_;
    
    std::string wsUrl_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;

    std::thread readerThread_;
    std::thread writerThread_;

    std::queue<std::string> outbox_;
    // COLD_PATH_ONLY: WS callback serialization

    std::mutex outMtx_;
    std::condition_variable outCv_;
    // COLD_PATH_ONLY: WS callback serialization

    std::mutex writeLock_;

    std::function<void(const std::string&)> onMsg_;
    std::function<void(bool)> onState_;
    
    static std::once_flag sslInitFlag_;
    static void initSSL();
};

} // namespace Chimera
