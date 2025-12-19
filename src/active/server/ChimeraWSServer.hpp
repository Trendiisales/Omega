#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <map>

namespace Chimera {

struct ClientState {
    int fd = -1;
    bool handshook = false;
};

class ChimeraWSServer {
public:
    ChimeraWSServer();
    ~ChimeraWSServer();

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
    // COLD_PATH_ONLY: admin / REST API

    mutable std::mutex clientsMtx;
    
    std::function<void(const std::string&)> onCommand;
};

} // namespace Chimera
