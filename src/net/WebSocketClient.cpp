#include "WebSocketClient.hpp"
#include <chrono>
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

namespace Omega {

WebSocketClient::WebSocketClient()
    : sock(-1), running(false), connected(false) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

WebSocketClient::~WebSocketClient() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

void WebSocketClient::setMessageCallback(std::function<void(const std::string&)> cb) {
    onMsg = cb;
}

void WebSocketClient::setStateCallback(std::function<void(bool)> cb) {
    onState = cb;
}

bool WebSocketClient::parseUrl(const std::string& url, std::string& host, std::string& path, int& port, bool& ssl) {
    ssl = false;
    port = 80;
    
    size_t pos = 0;
    if (url.substr(0, 6) == "wss://") {
        ssl = true;
        port = 443;
        pos = 6;
    } else if (url.substr(0, 5) == "ws://") {
        ssl = false;
        port = 80;
        pos = 5;
    } else if (url.substr(0, 1) == "/") {
        // Just a path, assume localhost
        host = "localhost";
        path = url;
        return true;
    }
    
    size_t pathStart = url.find('/', pos);
    if (pathStart == std::string::npos) {
        host = url.substr(pos);
        path = "/";
    } else {
        host = url.substr(pos, pathStart - pos);
        path = url.substr(pathStart);
    }
    
    // Check for port in host
    size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = std::stoi(host.substr(colonPos + 1));
        host = host.substr(0, colonPos);
    }
    
    return true;
}

bool WebSocketClient::connect(const std::string& url) {
    std::string host, path;
    int port;
    bool ssl;
    
    if (!parseUrl(url, host, path, port, ssl)) {
        return false;
    }
    
    // For Binance streams that start with /ws/
    if (url[0] == '/') {
        host = "stream.binance.com";
        port = 9443;
        ssl = true;
        path = url;
    }
    
    return doConnect(host, path, port, ssl);
}

bool WebSocketClient::connect(const std::string& host, const std::string& path, int port) {
    return doConnect(host, path, port, port == 443 || port == 9443);
}

bool WebSocketClient::doConnect(const std::string& host, const std::string& path, int port, bool ssl) {
    wsUrl = host + path;
    
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        sock = -1;
        return false;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    
    if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        sock = -1;
        return false;
    }
    
    // Send WebSocket handshake
    std::string handshake = 
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    
    ::send(sock, handshake.c_str(), handshake.size(), 0);
    
    // Read handshake response
    char buf[1024];
    int n = recv(sock, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        disconnect();
        return false;
    }
    buf[n] = 0;
    
    // Check for 101 Switching Protocols
    if (strstr(buf, "101") == nullptr) {
        disconnect();
        return false;
    }
    
    running = true;
    connected = true;
    
    if (onState) onState(true);
    
    tReader = std::thread(&WebSocketClient::readerLoop, this);
    tWriter = std::thread(&WebSocketClient::writerLoop, this);
    
    return true;
}

void WebSocketClient::disconnect() {
    running = false;
    connected = false;
    
    if (sock >= 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        sock = -1;
    }
    
    outCv.notify_all();
    
    if (tReader.joinable()) tReader.join();
    if (tWriter.joinable()) tWriter.join();
    
    if (onState) onState(false);
}

bool WebSocketClient::send(const std::string& msg) {
    if (!running || !connected) return false;
    
    {
        std::lock_guard<std::mutex> lock(outMtx);
        outbox.push(msg);
    }
    outCv.notify_one();
    return true;
}

void WebSocketClient::readerLoop() {
    char buf[8192];
    
    while (running && sock >= 0) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (running) {
                connected = false;
                if (onState) onState(false);
            }
            break;
        }
        
        // Simple WebSocket frame parsing (text frames only)
        if (n >= 2) {
            int opcode = buf[0] & 0x0F;
            if (opcode == 0x01) {  // Text frame
                bool masked = (buf[1] & 0x80) != 0;
                size_t len = buf[1] & 0x7F;
                size_t offset = 2;
                
                if (len == 126 && n >= 4) {
                    len = (buf[2] << 8) | buf[3];
                    offset = 4;
                } else if (len == 127 && n >= 10) {
                    // 64-bit length - just use lower 32 bits
                    len = (buf[6] << 24) | (buf[7] << 16) | (buf[8] << 8) | buf[9];
                    offset = 10;
                }
                
                if (masked) offset += 4;  // Skip mask
                
                if (offset + len <= (size_t)n) {
                    std::string payload(buf + offset, len);
                    if (onMsg) onMsg(payload);
                }
            } else if (opcode == 0x08) {  // Close
                disconnect();
                break;
            } else if (opcode == 0x09) {  // Ping
                // Send pong
                char pong[2] = {(char)0x8A, 0x00};
                ::send(sock, pong, 2, 0);
            }
        }
    }
}

void WebSocketClient::writerLoop() {
    while (running) {
        std::unique_lock<std::mutex> lock(outMtx);
        outCv.wait(lock, [&]{ return !outbox.empty() || !running; });
        
        while (!outbox.empty() && running) {
            std::string msg = outbox.front();
            outbox.pop();
            lock.unlock();
            
            // Create WebSocket text frame
            std::vector<char> frame;
            frame.push_back(0x81);  // FIN + Text opcode
            
            if (msg.size() < 126) {
                frame.push_back(0x80 | msg.size());  // Masked + length
            } else if (msg.size() < 65536) {
                frame.push_back(0x80 | 126);
                frame.push_back((msg.size() >> 8) & 0xFF);
                frame.push_back(msg.size() & 0xFF);
            } else {
                frame.push_back(0x80 | 127);
                for (int i = 7; i >= 0; i--) {
                    frame.push_back((msg.size() >> (i*8)) & 0xFF);
                }
            }
            
            // Add mask (required for client -> server)
            char mask[4] = {0x12, 0x34, 0x56, 0x78};
            frame.insert(frame.end(), mask, mask + 4);
            
            // Add masked payload
            for (size_t i = 0; i < msg.size(); i++) {
                frame.push_back(msg[i] ^ mask[i % 4]);
            }
            
            std::lock_guard<std::mutex> wg(wlock);
            ::send(sock, frame.data(), frame.size(), 0);
            
            lock.lock();
        }
    }
}

} // namespace Omega
