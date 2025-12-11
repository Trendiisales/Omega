#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <map>

namespace Omega {

struct ClientState {
    int fd = -1;
    bool handshook = false;
};

class OmegaWSServer {
public:
    OmegaWSServer();
    ~OmegaWSServer();

    bool start(int port);
    void stop();

    void broadcast(const std::string& json);
    
    void setOnCommand(std::function<void(const std::string&)> cb) { onCommand = cb; }

    int clientCount() const;

private:
    void acceptLoop();
    void clientLoop(int clientFd);
    bool doHandshake(int fd, const std::string& request);
    void sendFrame(int fd, const std::string& data);
    std::string readFrame(int fd);

private:
    int serverFd;
    std::atomic<bool> running;
    std::thread acceptThread;
    
    std::map<int, ClientState> clients;
    mutable std::mutex clientsMtx;
    
    std::function<void(const std::string&)> onCommand;
};

} // namespace Omega
