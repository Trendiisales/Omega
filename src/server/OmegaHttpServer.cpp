#include "OmegaHttpServer.hpp"
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSESOCK closesocket
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define CLOSESOCK ::close
#endif

namespace Omega {

OmegaHttpServer::OmegaHttpServer() : serverFd(-1), running(false), clients(0) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

OmegaHttpServer::~OmegaHttpServer() {
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool OmegaHttpServer::start(int port, const std::string& webRoot) {
    root = webRoot;
    
    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) return false;
    
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CLOSESOCK(serverFd);
        serverFd = -1;
        return false;
    }
    
    if (listen(serverFd, 10) < 0) {
        CLOSESOCK(serverFd);
        serverFd = -1;
        return false;
    }
    
    running = true;
    acceptThread = std::thread(&OmegaHttpServer::acceptLoop, this);
    return true;
}

void OmegaHttpServer::stop() {
    running = false;
    if (serverFd >= 0) {
        CLOSESOCK(serverFd);
        serverFd = -1;
    }
    if (acceptThread.joinable()) acceptThread.join();
}

void OmegaHttpServer::acceptLoop() {
    while (running && serverFd >= 0) {
        struct sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &len);
        
        if (clientFd < 0) continue;
        
        clients++;
        std::thread([this, clientFd]() {
            handleClient(clientFd);
            clients--;
        }).detach();
    }
}

void OmegaHttpServer::handleClient(int clientFd) {
    char buf[4096];
    int n = recv(clientFd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        CLOSESOCK(clientFd);
        return;
    }
    buf[n] = 0;
    
    // Parse GET request
    std::string request(buf);
    if (request.substr(0, 3) != "GET") {
        send404(clientFd);
        CLOSESOCK(clientFd);
        return;
    }
    
    size_t pathStart = request.find(' ') + 1;
    size_t pathEnd = request.find(' ', pathStart);
    std::string path = request.substr(pathStart, pathEnd - pathStart);
    
    // Default to index.html
    if (path == "/") path = "/index.html";
    
    // Security: prevent directory traversal
    if (path.find("..") != std::string::npos) {
        send404(clientFd);
        CLOSESOCK(clientFd);
        return;
    }
    
    std::string fullPath = root + path;
    std::string content = readFile(fullPath);
    
    if (content.empty()) {
        // Try index.html for SPA routing
        content = readFile(root + "/index.html");
        if (content.empty()) {
            send404(clientFd);
            CLOSESOCK(clientFd);
            return;
        }
        path = "/index.html";
    }
    
    sendResponse(clientFd, 200, getMimeType(path), content);
    CLOSESOCK(clientFd);
}

std::string OmegaHttpServer::readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string OmegaHttpServer::getMimeType(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    if (path.find(".json") != std::string::npos) return "application/json";
    if (path.find(".png") != std::string::npos) return "image/png";
    if (path.find(".jpg") != std::string::npos) return "image/jpeg";
    if (path.find(".svg") != std::string::npos) return "image/svg+xml";
    if (path.find(".ico") != std::string::npos) return "image/x-icon";
    if (path.find(".woff") != std::string::npos) return "font/woff";
    if (path.find(".woff2") != std::string::npos) return "font/woff2";
    return "text/plain";
}

void OmegaHttpServer::sendResponse(int fd, int status, const std::string& contentType, const std::string& body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << status << " OK\r\n";
    ss << "Content-Type: " << contentType << "\r\n";
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << "Access-Control-Allow-Origin: *\r\n";
    ss << "Connection: close\r\n";
    ss << "\r\n";
    ss << body;
    
    std::string response = ss.str();
    send(fd, response.c_str(), (int)response.size(), 0);
}

void OmegaHttpServer::send404(int fd) {
    std::string body = "<html><body><h1>404 Not Found</h1></body></html>";
    std::ostringstream ss;
    ss << "HTTP/1.1 404 Not Found\r\n";
    ss << "Content-Type: text/html\r\n";
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << "Connection: close\r\n";
    ss << "\r\n";
    ss << body;
    
    std::string response = ss.str();
    send(fd, response.c_str(), (int)response.size(), 0);
}

int OmegaHttpServer::clientCount() const {
    return clients.load();
}

} // namespace Omega
