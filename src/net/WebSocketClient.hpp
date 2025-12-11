#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>

namespace Omega {

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    // New simple interface (URL only)
    bool connect(const std::string& url);
    
    // Legacy interface (host, path, port)
    bool connect(const std::string& host, const std::string& path, int port);
    
    void disconnect();
    void close() { disconnect(); }

    bool send(const std::string& msg);
    bool sendText(const std::string& txt) { return send(txt); }

    // Callback setters (both names for compatibility)
    void setMessageCallback(std::function<void(const std::string&)> cb);
    void setOnMessage(std::function<void(const std::string&)> cb) { setMessageCallback(cb); }
    void setCallback(std::function<void(const std::string&)> cb) { setMessageCallback(cb); }
    
    void setStateCallback(std::function<void(bool)> cb);
    
    bool isConnected() const { return connected; }

private:
    void readerLoop();
    void writerLoop();
    bool parseUrl(const std::string& url, std::string& host, std::string& path, int& port, bool& ssl);
    bool doConnect(const std::string& host, const std::string& path, int port, bool ssl);

private:
    int sock;
    std::string wsUrl;
    std::atomic<bool> running;
    std::atomic<bool> connected;

    std::thread tReader;
    std::thread tWriter;

    std::queue<std::string> outbox;
    std::mutex outMtx;
    std::condition_variable outCv;
    std::mutex wlock;

    std::function<void(const std::string&)> onMsg;
    std::function<void(bool)> onState;
};

} // namespace Omega
