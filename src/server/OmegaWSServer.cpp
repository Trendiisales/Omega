#include "OmegaWSServer.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

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

// Simple SHA1 implementation (no OpenSSL required)
static void sha1(const unsigned char* data, size_t len, unsigned char* hash) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    size_t newLen = len + 1;
    while (newLen % 64 != 56) newLen++;
    
    unsigned char* msg = new unsigned char[newLen + 8]();
    memcpy(msg, data, len);
    msg[len] = 0x80;
    
    uint64_t bitLen = len * 8;
    for (int i = 0; i < 8; i++) {
        msg[newLen + 7 - i] = (bitLen >> (i * 8)) & 0xFF;
    }

    for (size_t chunk = 0; chunk < newLen + 8; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (msg[chunk + i*4] << 24) | (msg[chunk + i*4 + 1] << 16) |
                   (msg[chunk + i*4 + 2] << 8) | msg[chunk + i*4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (t << 1) | (t >> 31);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }

            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    delete[] msg;

    for (int i = 0; i < 4; i++) {
        hash[i] = (h0 >> (24 - i*8)) & 0xFF;
        hash[4 + i] = (h1 >> (24 - i*8)) & 0xFF;
        hash[8 + i] = (h2 >> (24 - i*8)) & 0xFF;
        hash[12 + i] = (h3 >> (24 - i*8)) & 0xFF;
        hash[16 + i] = (h4 >> (24 - i*8)) & 0xFF;
    }
}

// Simple base64 encode (no OpenSSL required)
static std::string base64Encode(const unsigned char* data, size_t len) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        
        result += chars[(n >> 18) & 0x3F];
        result += chars[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? chars[n & 0x3F] : '=';
    }
    return result;
}

static std::string computeAcceptKey(const std::string& key) {
    std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[20];
    sha1((unsigned char*)magic.c_str(), magic.size(), hash);
    return base64Encode(hash, 20);
}

OmegaWSServer::OmegaWSServer() : serverFd(-1), running(false) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

OmegaWSServer::~OmegaWSServer() {
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool OmegaWSServer::start(int port) {
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
    acceptThread = std::thread(&OmegaWSServer::acceptLoop, this);
    return true;
}

void OmegaWSServer::stop() {
    running = false;
    
    if (serverFd >= 0) {
        CLOSESOCK(serverFd);
        serverFd = -1;
    }
    
    {
        std::lock_guard<std::mutex> lock(clientsMtx);
        for (auto& p : clients) {
            CLOSESOCK(p.first);
        }
        clients.clear();
    }
    
    if (acceptThread.joinable()) acceptThread.join();
}

void OmegaWSServer::acceptLoop() {
    while (running && serverFd >= 0) {
        struct sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &len);
        
        if (clientFd < 0) continue;
        
        std::thread([this, clientFd]() {
            clientLoop(clientFd);
        }).detach();
    }
}

void OmegaWSServer::clientLoop(int clientFd) {
    char buf[4096];
    int n = recv(clientFd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        CLOSESOCK(clientFd);
        return;
    }
    buf[n] = 0;
    
    if (!doHandshake(clientFd, buf)) {
        CLOSESOCK(clientFd);
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(clientsMtx);
        clients[clientFd] = {clientFd, true};
    }
    
    while (running) {
        std::string msg = readFrame(clientFd);
        if (msg.empty()) break;
        if (onCommand) onCommand(msg);
    }
    
    {
        std::lock_guard<std::mutex> lock(clientsMtx);
        clients.erase(clientFd);
    }
    CLOSESOCK(clientFd);
}

bool OmegaWSServer::doHandshake(int fd, const std::string& request) {
    size_t keyPos = request.find("Sec-WebSocket-Key:");
    if (keyPos == std::string::npos) return false;
    
    keyPos += 18;
    while (keyPos < request.size() && request[keyPos] == ' ') keyPos++;
    
    size_t keyEnd = request.find("\r\n", keyPos);
    if (keyEnd == std::string::npos) return false;
    
    std::string key = request.substr(keyPos, keyEnd - keyPos);
    std::string acceptKey = computeAcceptKey(key);
    
    std::string response = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
        "\r\n";
    
    send(fd, response.c_str(), (int)response.size(), 0);
    return true;
}

void OmegaWSServer::sendFrame(int fd, const std::string& data) {
    std::vector<char> frame;
    frame.push_back((char)0x81);
    
    if (data.size() < 126) {
        frame.push_back((char)data.size());
    } else if (data.size() < 65536) {
        frame.push_back((char)126);
        frame.push_back((char)((data.size() >> 8) & 0xFF));
        frame.push_back((char)(data.size() & 0xFF));
    } else {
        frame.push_back((char)127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((char)((data.size() >> (i * 8)) & 0xFF));
        }
    }
    
    frame.insert(frame.end(), data.begin(), data.end());
    send(fd, frame.data(), (int)frame.size(), 0);
}

std::string OmegaWSServer::readFrame(int fd) {
    char header[2];
    if (recv(fd, header, 2, 0) != 2) return "";
    
    bool masked = (header[1] & 0x80) != 0;
    size_t len = header[1] & 0x7F;
    
    if (len == 126) {
        char ext[2];
        if (recv(fd, ext, 2, 0) != 2) return "";
        len = ((unsigned char)ext[0] << 8) | (unsigned char)ext[1];
    } else if (len == 127) {
        char ext[8];
        if (recv(fd, ext, 8, 0) != 8) return "";
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | (unsigned char)ext[i];
        }
    }
    
    char mask[4] = {0};
    if (masked) {
        if (recv(fd, mask, 4, 0) != 4) return "";
    }
    
    std::string payload(len, 0);
    size_t received = 0;
    while (received < len) {
        int n = recv(fd, &payload[received], (int)(len - received), 0);
        if (n <= 0) return "";
        received += n;
    }
    
    if (masked) {
        for (size_t i = 0; i < len; i++) {
            payload[i] ^= mask[i % 4];
        }
    }
    
    return payload;
}

void OmegaWSServer::broadcast(const std::string& json) {
    std::lock_guard<std::mutex> lock(clientsMtx);
    for (auto& p : clients) {
        if (p.second.handshook) {
            sendFrame(p.first, json);
        }
    }
}

int OmegaWSServer::clientCount() const {
    std::lock_guard<std::mutex> lock(clientsMtx);
    return (int)clients.size();
}

} // namespace Omega
