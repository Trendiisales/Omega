// =============================================================================
// SSLWebSocketClient.cpp - SSL-enabled WebSocket client implementation
// =============================================================================
// CHIMERA HFT v1.5.3 - FIXED: Thread-safe disconnect (join before cleanup)
// =============================================================================
#include "SSLWebSocketClient.hpp"
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace Chimera {

std::once_flag SSLWebSocketClient::sslInitFlag_;

void SSLWebSocketClient::initSSL() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

SSLWebSocketClient::SSLWebSocketClient()
    : sock_(-1)
    , sslCtx_(nullptr)
    , ssl_(nullptr)
    , useSSL_(false)
    , running_(false)
    , connected_(false)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    std::call_once(sslInitFlag_, initSSL);
}

SSLWebSocketClient::~SSLWebSocketClient() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

void SSLWebSocketClient::setMessageCallback(std::function<void(const std::string&)> cb) {
    onMsg_ = cb;
}

void SSLWebSocketClient::setStateCallback(std::function<void(bool)> cb) {
    onState_ = cb;
}

bool SSLWebSocketClient::parseUrl(const std::string& url, std::string& host, 
                                   std::string& path, int& port, bool& ssl) {
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
    } else if (url[0] == '/') {
        host = "stream.binance.com";
        port = 9443;
        ssl = true;
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
    
    size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = std::stoi(host.substr(colonPos + 1));
        host = host.substr(0, colonPos);
    }
    
    return true;
}

bool SSLWebSocketClient::connect(const std::string& url) {
    std::string host, path;
    int port;
    bool ssl;
    
    if (!parseUrl(url, host, path, port, ssl)) {
        return false;
    }
    
    return doConnect(host, path, port, ssl);
}

bool SSLWebSocketClient::connect(const std::string& host, const std::string& path, 
                                  int port, bool useSSL) {
    return doConnect(host, path, port, useSSL);
}

bool SSLWebSocketClient::doConnect(const std::string& host, const std::string& path, 
                                    int port, bool useSSL) {
    // Disconnect any existing connection first
    if (running_.load() || connected_.load()) {
        disconnect();
    }
    
    wsUrl_ = host + path;
    useSSL_ = useSSL;
    
    // Create socket
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        std::cerr << "[SSLWebSocket] Socket creation failed\n";
        return false;
    }
    
    // Set TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
    
    // Resolve hostname
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        std::cerr << "[SSLWebSocket] DNS resolution failed for: " << host << "\n";
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = -1;
        return false;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    
    std::cout << "[SSLWebSocket] Connecting to " << host << ":" << port 
              << (useSSL ? " (SSL)" : "") << "\n";
    
    if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[SSLWebSocket] TCP connect failed\n";
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = -1;
        return false;
    }
    
    // SSL handshake if needed
    if (useSSL_) {
        if (!sslHandshake(host)) {
            std::cerr << "[SSLWebSocket] SSL handshake failed\n";
#ifdef _WIN32
            closesocket(sock_);
#else
            ::close(sock_);
#endif
            sock_ = -1;
            return false;
        }
    }
    
    // WebSocket handshake
    if (!wsHandshake(host, path)) {
        std::cerr << "[SSLWebSocket] WebSocket handshake failed\n";
        cleanupSSL();
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = -1;
        return false;
    }
    
    running_ = true;
    connected_ = true;
    
    std::cout << "[SSLWebSocket] Connected successfully\n";
    
    if (onState_) onState_(true);
    
    readerThread_ = std::thread(&SSLWebSocketClient::readerLoop, this);
    writerThread_ = std::thread(&SSLWebSocketClient::writerLoop, this);
    
    return true;
}

bool SSLWebSocketClient::sslHandshake(const std::string& host) {
    sslCtx_ = SSL_CTX_new(TLS_client_method());
    if (!sslCtx_) {
        std::cerr << "[SSLWebSocket] SSL_CTX_new failed\n";
        return false;
    }
    
    ssl_ = SSL_new(sslCtx_);
    if (!ssl_) {
        std::cerr << "[SSLWebSocket] SSL_new failed\n";
        SSL_CTX_free(sslCtx_);
        sslCtx_ = nullptr;
        return false;
    }
    
    // SNI - required for Binance
    SSL_set_tlsext_host_name(ssl_, host.c_str());
    
    SSL_set_fd(ssl_, sock_);
    
    if (SSL_connect(ssl_) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl_);
        ssl_ = nullptr;
        SSL_CTX_free(sslCtx_);
        sslCtx_ = nullptr;
        return false;
    }
    
    std::cout << "[SSLWebSocket] SSL handshake complete, cipher: " 
              << SSL_get_cipher(ssl_) << "\n";
    
    return true;
}

bool SSLWebSocketClient::wsHandshake(const std::string& host, const std::string& path) {
    // Generate random WebSocket key
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    char keyBytes[16];
    for (int i = 0; i < 16; i++) {
        keyBytes[i] = static_cast<char>(dis(gen));
    }
    
    // Base64 encode the key (simplified)
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string wsKey;
    for (int i = 0; i < 16; i += 3) {
        int n = (static_cast<unsigned char>(keyBytes[i]) << 16) | 
                (static_cast<unsigned char>(keyBytes[i+1]) << 8) | 
                static_cast<unsigned char>(keyBytes[i+2]);
        wsKey += b64[(n >> 18) & 63];
        wsKey += b64[(n >> 12) & 63];
        wsKey += b64[(n >> 6) & 63];
        wsKey += b64[n & 63];
    }
    wsKey = wsKey.substr(0, 22) + "==";
    
    std::string handshake = 
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + wsKey + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    
    if (sslWrite(handshake.c_str(), handshake.size()) <= 0) {
        return false;
    }
    
    // Read response
    char buf[1024];
    int n = sslRead(buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = 0;
    
    // Check for 101 Switching Protocols
    if (strstr(buf, "101") == nullptr) {
        std::cerr << "[SSLWebSocket] Handshake response: " << buf << "\n";
        return false;
    }
    
    return true;
}

int SSLWebSocketClient::sslRead(char* buf, int len) {
    if (useSSL_ && ssl_) {
        return SSL_read(ssl_, buf, len);
    } else {
        return recv(sock_, buf, len, 0);
    }
}

int SSLWebSocketClient::sslWrite(const char* buf, int len) {
    if (useSSL_ && ssl_) {
        return SSL_write(ssl_, buf, len);
    } else {
        return ::send(sock_, buf, len, 0);
    }
}

void SSLWebSocketClient::cleanupSSL() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (sslCtx_) {
        SSL_CTX_free(sslCtx_);
        sslCtx_ = nullptr;
    }
}

// =============================================================================
// disconnect - FIXED: Join threads BEFORE cleaning up SSL
// =============================================================================
// Previous bug: cleanupSSL() was called while reader/writer threads were still
// running, causing use-after-free crash on ssl_ pointer.
// Fix: Close socket first (unblocks SSL_read), join threads, THEN cleanup SSL.
// =============================================================================
void SSLWebSocketClient::disconnect() {
    // Step 1: Signal threads to stop
    running_ = false;
    connected_ = false;
    
    // Step 2: Wake up writer thread
    outCv_.notify_all();
    
    // Step 3: Close socket FIRST - this unblocks any pending SSL_read()
    if (sock_ >= 0) {
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = -1;
    }
    
    // Step 4: Join threads - they will exit now that socket is closed
    if (readerThread_.joinable()) readerThread_.join();
    if (writerThread_.joinable()) writerThread_.join();
    
    // Step 5: NOW safe to cleanup SSL - no threads using it
    cleanupSSL();
    
    // Step 6: Notify state
    if (onState_) onState_(false);
}

bool SSLWebSocketClient::send(const std::string& msg) {
    if (!running_ || !connected_) return false;
    
    {
        std::lock_guard<std::mutex> lock(outMtx_);
        outbox_.push(msg);
    }
    outCv_.notify_one();
    return true;
}

void SSLWebSocketClient::readerLoop() {
    char buf[65536];
    std::vector<char> frameBuffer;
    
    while (running_ && sock_ >= 0) {
        int n = sslRead(buf, sizeof(buf));
        if (n <= 0) {
            if (running_) {
                connected_ = false;
                if (onState_) onState_(false);
            }
            break;
        }
        
        // Append to frame buffer
        frameBuffer.insert(frameBuffer.end(), buf, buf + n);
        
        // Process complete frames
        while (frameBuffer.size() >= 2 && running_) {
            int opcode = frameBuffer[0] & 0x0F;
            bool fin = (frameBuffer[0] & 0x80) != 0;
            bool masked = (frameBuffer[1] & 0x80) != 0;
            size_t len = frameBuffer[1] & 0x7F;
            size_t headerLen = 2;
            
            if (len == 126) {
                if (frameBuffer.size() < 4) break;
                len = (static_cast<unsigned char>(frameBuffer[2]) << 8) | 
                      static_cast<unsigned char>(frameBuffer[3]);
                headerLen = 4;
            } else if (len == 127) {
                if (frameBuffer.size() < 10) break;
                len = 0;
                for (int i = 2; i < 10; i++) {
                    len = (len << 8) | static_cast<unsigned char>(frameBuffer[i]);
                }
                headerLen = 10;
            }
            
            if (masked) headerLen += 4;
            
            if (frameBuffer.size() < headerLen + len) break;
            
            // Extract payload
            std::string payload;
            if (masked) {
                char mask[4];
                memcpy(mask, &frameBuffer[headerLen - 4], 4);
                payload.resize(len);
                for (size_t i = 0; i < len; i++) {
                    payload[i] = frameBuffer[headerLen + i] ^ mask[i % 4];
                }
            } else {
                payload.assign(&frameBuffer[headerLen], &frameBuffer[headerLen + len]);
            }
            
            // Handle frame
            if (opcode == 0x01 && fin) {  // Text frame
                if (onMsg_) onMsg_(payload);
            } else if (opcode == 0x08) {  // Close
                running_ = false;
                return;
            } else if (opcode == 0x09) {  // Ping
                // Send pong
                std::vector<char> pong;
                pong.push_back(static_cast<char>(0x8A));  // FIN + Pong
                pong.push_back(static_cast<char>(0x80 | len));  // Masked + length
                char mask[4] = {0x12, 0x34, 0x56, 0x78};
                pong.insert(pong.end(), mask, mask + 4);
                for (size_t i = 0; i < len; i++) {
                    pong.push_back(payload[i] ^ mask[i % 4]);
                }
                std::lock_guard<std::mutex> lock(writeLock_);
                sslWrite(pong.data(), pong.size());
            }
            
            // Remove processed frame
            frameBuffer.erase(frameBuffer.begin(), frameBuffer.begin() + headerLen + len);
        }
    }
}

void SSLWebSocketClient::writerLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(outMtx_);
        outCv_.wait(lock, [&]{ return !outbox_.empty() || !running_; });
        
        while (!outbox_.empty() && running_) {
            std::string msg = outbox_.front();
            outbox_.pop();
            lock.unlock();
            
            // Create WebSocket text frame
            std::vector<char> frame;
            frame.push_back(static_cast<char>(0x81));  // FIN + Text opcode
            
            if (msg.size() < 126) {
                frame.push_back(static_cast<char>(0x80 | msg.size()));
            } else if (msg.size() < 65536) {
                frame.push_back(static_cast<char>(0x80 | 126));
                frame.push_back(static_cast<char>((msg.size() >> 8) & 0xFF));
                frame.push_back(static_cast<char>(msg.size() & 0xFF));
            } else {
                frame.push_back(static_cast<char>(0x80 | 127));
                for (int i = 7; i >= 0; i--) {
                    frame.push_back(static_cast<char>((msg.size() >> (i * 8)) & 0xFF));
                }
            }
            
            // Add mask
            char mask[4] = {0x12, 0x34, 0x56, 0x78};
            frame.insert(frame.end(), mask, mask + 4);
            
            // Add masked payload
            for (size_t i = 0; i < msg.size(); i++) {
                frame.push_back(msg[i] ^ mask[i % 4]);
            }
            
            std::lock_guard<std::mutex> wg(writeLock_);
            sslWrite(frame.data(), frame.size());
            
            lock.lock();
        }
    }
}

} // namespace Chimera
