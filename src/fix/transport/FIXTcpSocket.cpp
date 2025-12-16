#include "FIXTcpSocket.hpp"
#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#define CLOSESOCK closesocket
typedef int socklen_t;
#else
#define CLOSESOCK ::close
#endif

namespace Omega {

FIXTcpSocket::FIXTcpSocket() : running(false), sock(-1) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

FIXTcpSocket::~FIXTcpSocket() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool FIXTcpSocket::connect(const std::string& host, int port) {
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        CLOSESOCK(sock);
        sock = -1;
        return false;
    }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CLOSESOCK(sock);
        sock = -1;
        return false;
    }

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

    running = true;
    rxThread = std::thread(&FIXTcpSocket::rxLoop, this);
    
    return true;
}

void FIXTcpSocket::disconnect() {
    running = false;
    if (sock >= 0) {
        CLOSESOCK(sock);
        sock = -1;
    }
    if (rxThread.joinable()) {
        rxThread.join();
    }
}

bool FIXTcpSocket::isConnected() const {
    return sock >= 0 && running;
}

bool FIXTcpSocket::sendRaw(const std::string& msg) {
    if (sock < 0) return false;
    int sent = ::send(sock, msg.c_str(), (int)msg.size(), 0);
    return sent == (int)msg.size();
}

int FIXTcpSocket::sendRaw(const char* data, int len) {
    if (sock < 0) return -1;
    return ::send(sock, data, len, 0);
}

int FIXTcpSocket::recvRaw(char* buf, int maxLen) {
    if (sock < 0) return -1;
    return ::recv(sock, buf, maxLen, 0);
}

int FIXTcpSocket::getFd() const {
    return sock;
}

void FIXTcpSocket::rxLoop() {
    char buf[8192];
    while (running && sock >= 0) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (running) {
                running = false;
            }
            break;
        }
        
        std::string msg(buf, n);
        emitRx(msg);
    }
}

} // namespace Omega
