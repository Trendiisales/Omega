#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <functional>

namespace Chimera {

class ChimeraHttpServer {
public:
    ChimeraHttpServer();
    ~ChimeraHttpServer();

    bool start(int port, const std::string& webRoot);
    void stop();
    
    int clientCount() const;

private:
    void acceptLoop();
    void handleClient(int clientFd);
    std::string getMimeType(const std::string& path);
    std::string readFile(const std::string& path);
    void sendResponse(int fd, int status, const std::string& contentType, const std::string& body);
    void send404(int fd);

private:
    int serverFd;
    std::atomic<bool> running;
    std::thread acceptThread;
    std::string root;
    
    std::atomic<int> clients;
};

} // namespace Chimera
