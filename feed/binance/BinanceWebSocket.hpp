// =============================================================================
// BinanceWebSocket.hpp - Real SSL WebSocket Feed to Binance
// =============================================================================
// HFT-optimized WebSocket client for Binance market data
// Uses OpenSSL for TLS, direct syscalls for minimum latency
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>

#include "core/MonotonicClock.hpp"

namespace chimera {
namespace feed {
namespace binance {

// =============================================================================
// Build configuration - PRODUCTION
// =============================================================================
static constexpr const char* WS_HOST = "stream.binance.com";
static constexpr int WS_PORT = 443;

// =============================================================================
// Tick callback type
// =============================================================================
struct RawTick {
    uint32_t symbol_id;      // Index in subscription order (0=BTCUSDT, 1=ETHUSDT, etc)
    uint64_t exchange_ts_ns;
    uint64_t local_ts_ns;
    double bid_price;
    double bid_qty;
    double ask_price;
    double ask_qty;
    char symbol[16];         // Symbol name for reference
};

using TickCallback = std::function<void(const RawTick&)>;

// =============================================================================
// BinanceWebSocket
// =============================================================================
class BinanceWebSocket {
public:
    BinanceWebSocket() noexcept
        : ssl_ctx_(nullptr)
        , ssl_(nullptr)
        , sock_fd_(-1)
        , running_(false)
        , connected_(false)
        , messages_received_(0)
        , last_latency_us_(0)
    {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    }

    ~BinanceWebSocket() {
        stop();
        cleanup();
    }

    void set_callback(TickCallback cb) { callback_ = std::move(cb); }

    bool connect(const std::vector<std::string>& symbols) {
        symbols_ = symbols;
        
        // Build symbol -> index map for O(1) lookup
        symbol_to_id_.clear();
        for (size_t i = 0; i < symbols.size(); ++i) {
            symbol_to_id_[symbols[i]] = static_cast<uint32_t>(i);
        }
        
        // Create SSL context
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            std::fprintf(stderr, "[BINANCE] SSL_CTX_new failed\n");
            return false;
        }
        
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ssl_ctx_);
        
        // Resolve host
        struct hostent* he = gethostbyname(WS_HOST);
        if (!he) {
            std::fprintf(stderr, "[BINANCE] gethostbyname failed for %s\n", WS_HOST);
            return false;
        }
        
        // Create socket
        sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd_ < 0) {
            std::fprintf(stderr, "[BINANCE] socket() failed\n");
            return false;
        }
        
        // TCP optimizations
        int flag = 1;
        setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        // Connect
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(WS_PORT);
        std::memcpy(&addr.sin_addr, he->h_addr, he->h_length);
        
        // Debug: show what we're connecting to
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
        std::printf("[BINANCE] Connecting to %s (%s:%d)...\n", WS_HOST, ip_str, WS_PORT);
        
        if (::connect(sock_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "[BINANCE] connect() failed: %s (errno=%d)\n", 
                        strerror(errno), errno);
            close(sock_fd_);
            sock_fd_ = -1;
            return false;
        }
        
        // SSL handshake
        ssl_ = SSL_new(ssl_ctx_);
        SSL_set_fd(ssl_, sock_fd_);
        SSL_set_tlsext_host_name(ssl_, WS_HOST);
        
        if (SSL_connect(ssl_) != 1) {
            std::fprintf(stderr, "[BINANCE] SSL_connect failed\n");
            ERR_print_errors_fp(stderr);
            return false;
        }
        
        // WebSocket handshake
        if (!ws_handshake()) {
            std::fprintf(stderr, "[BINANCE] WebSocket handshake failed\n");
            return false;
        }
        
        connected_.store(true, std::memory_order_release);
        std::printf("[BINANCE] Connected to %s:%d (%zu symbols)\n", 
                   WS_HOST, WS_PORT, symbols.size());
        return true;
    }

    void start() {
        if (!connected_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        
        recv_thread_ = std::thread([this]() {
            recv_loop();
        });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
    }

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }
    uint64_t messages_received() const { return messages_received_; }
    int64_t last_latency_us() const { return last_latency_us_; }

private:
    SSL_CTX* ssl_ctx_;
    SSL* ssl_;
    int sock_fd_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::thread recv_thread_;
    TickCallback callback_;
    std::vector<std::string> symbols_;
    std::unordered_map<std::string, uint32_t> symbol_to_id_;  // Symbol name -> index
    uint64_t messages_received_;
    int64_t last_latency_us_;
    
    char recv_buf_[65536];
    char frame_buf_[65536];

    void cleanup() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (sock_fd_ >= 0) { close(sock_fd_); sock_fd_ = -1; }
        if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
        connected_.store(false, std::memory_order_release);
    }

    bool ws_handshake() {
        // Build stream path - using bookTicker for bid/ask prices
        std::string path = "/stream?streams=";
        for (size_t i = 0; i < symbols_.size(); ++i) {
            if (i > 0) path += "/";
            // Convert to lowercase for Binance
            std::string sym = symbols_[i];
            for (char& c : sym) c = std::tolower(c);
            path += sym + "@bookTicker";
        }
        
        // Generate random key
        unsigned char key_bytes[16];
        for (int i = 0; i < 16; ++i) key_bytes[i] = rand() & 0xFF;
        char key_b64[32];
        base64_encode(key_bytes, 16, key_b64);
        
        // Send HTTP upgrade
        char request[2048];
        int len = std::snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n",
            path.c_str(), WS_HOST, key_b64);
        
        if (SSL_write(ssl_, request, len) != len) {
            return false;
        }
        
        // Read response
        int n = SSL_read(ssl_, recv_buf_, sizeof(recv_buf_) - 1);
        if (n <= 0) {
            std::fprintf(stderr, "[BINANCE] SSL_read failed in handshake\n");
            return false;
        }
        recv_buf_[n] = '\0';
        
        return std::strstr(recv_buf_, "101") != nullptr;
    }

    void recv_loop() {
        while (running_.load(std::memory_order_acquire)) {
            int n = SSL_read(ssl_, recv_buf_, sizeof(recv_buf_));
            if (n <= 0) {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                std::fprintf(stderr, "[BINANCE] SSL_read error: %d\n", err);
                connected_.store(false, std::memory_order_release);
                break;
            }
            
            const uint64_t local_ts = chimera::core::MonotonicClock::now_ns();
            parse_frames(recv_buf_, n, local_ts);
        }
    }

    void parse_frames(const char* data, int len, uint64_t local_ts) {
        int offset = 0;
        while (offset < len) {
            if (offset + 2 > len) break;
            
            uint8_t b0 = data[offset];
            uint8_t b1 = data[offset + 1];
            
            // bool fin = (b0 & 0x80) != 0;
            uint8_t opcode = b0 & 0x0F;
            bool masked = (b1 & 0x80) != 0;
            uint64_t payload_len = b1 & 0x7F;
            
            int header_len = 2;
            if (payload_len == 126) {
                if (offset + 4 > len) break;
                payload_len = (uint16_t(data[offset + 2]) << 8) | uint8_t(data[offset + 3]);
                header_len = 4;
            } else if (payload_len == 127) {
                if (offset + 10 > len) break;
                payload_len = 0;
                for (int i = 0; i < 8; ++i) {
                    payload_len = (payload_len << 8) | uint8_t(data[offset + 2 + i]);
                }
                header_len = 10;
            }
            
            if (masked) header_len += 4;
            
            if (offset + header_len + (int)payload_len > len) break;
            
            const char* payload = data + offset + header_len;
            
            if (opcode == 0x1) { // Text frame
                parse_json(payload, payload_len, local_ts);
            } else if (opcode == 0x9) { // Ping
                send_pong(payload, payload_len);
            } else if (opcode == 0x8) { // Close
                connected_.store(false, std::memory_order_release);
                break;
            }
            
            offset += header_len + payload_len;
        }
    }

    void parse_json(const char* json, size_t len, uint64_t local_ts) {
        // Copy to null-terminate
        if (len >= sizeof(frame_buf_)) return;
        std::memcpy(frame_buf_, json, len);
        frame_buf_[len] = '\0';
        
        // Fast JSON parsing for bookTicker format:
        // {"stream":"btcusdt@bookTicker","data":{"s":"BTCUSDT","b":"50000.00","B":"1.5","a":"50001.00","A":"2.0","T":1234567890123}}
        
        const char* s_ptr = std::strstr(frame_buf_, "\"s\":\"");
        const char* b_ptr = std::strstr(frame_buf_, "\"b\":\"");
        const char* B_ptr = std::strstr(frame_buf_, "\"B\":\"");
        const char* a_ptr = std::strstr(frame_buf_, "\"a\":\"");
        const char* A_ptr = std::strstr(frame_buf_, "\"A\":\"");
        const char* T_ptr = std::strstr(frame_buf_, "\"T\":");
        
        if (!s_ptr || !b_ptr || !B_ptr || !a_ptr || !A_ptr) return;
        
        RawTick tick{};
        tick.local_ts_ns = local_ts;
        std::memset(tick.symbol, 0, sizeof(tick.symbol));
        
        // Parse symbol name
        s_ptr += 5;
        const char* s_end = std::strchr(s_ptr, '"');
        if (!s_end) return;
        
        size_t slen = s_end - s_ptr;
        if (slen >= sizeof(tick.symbol)) slen = sizeof(tick.symbol) - 1;
        std::memcpy(tick.symbol, s_ptr, slen);
        tick.symbol[slen] = '\0';
        
        // Look up symbol ID from subscription order
        auto it = symbol_to_id_.find(tick.symbol);
        if (it != symbol_to_id_.end()) {
            tick.symbol_id = it->second;
        } else {
            // Unknown symbol - shouldn't happen
            tick.symbol_id = 0xFFFFFFFF;
        }
        
        // Parse prices
        tick.bid_price = std::atof(b_ptr + 5);
        tick.bid_qty = std::atof(B_ptr + 5);
        tick.ask_price = std::atof(a_ptr + 5);
        tick.ask_qty = std::atof(A_ptr + 5);
        
        // Parse exchange timestamp
        if (T_ptr) {
            tick.exchange_ts_ns = std::atoll(T_ptr + 4) * 1000000ULL; // ms to ns
        }
        
        // Calculate latency
        if (tick.exchange_ts_ns > 0) {
            last_latency_us_ = (local_ts - tick.exchange_ts_ns) / 1000;
        }
        
        ++messages_received_;
        
        // Debug first few ticks
        static int debug_count = 0;
        if (debug_count < 5) {
            std::printf("[BN] %s id=%u bid=%.2f ask=%.2f lat=%lldus\n",
                       tick.symbol, tick.symbol_id, tick.bid_price, tick.ask_price,
                       (long long)last_latency_us_);
            ++debug_count;
        }
        
        if (callback_) {
            callback_(tick);
        }
    }

    void send_pong(const char* payload, size_t len) {
        uint8_t frame[128];
        frame[0] = 0x8A; // FIN + PONG
        frame[1] = 0x80 | len; // Masked + length
        
        // Random mask
        uint8_t mask[4] = {(uint8_t)(rand()&0xFF), (uint8_t)(rand()&0xFF), 
                          (uint8_t)(rand()&0xFF), (uint8_t)(rand()&0xFF)};
        std::memcpy(frame + 2, mask, 4);
        
        // Masked payload
        for (size_t i = 0; i < len; ++i) {
            frame[6 + i] = payload[i] ^ mask[i % 4];
        }
        
        SSL_write(ssl_, frame, 6 + len);
    }

    static void base64_encode(const unsigned char* in, size_t len, char* out) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t j = 0;
        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = (uint32_t)in[i] << 16;
            if (i + 1 < len) n |= (uint32_t)in[i + 1] << 8;
            if (i + 2 < len) n |= in[i + 2];
            out[j++] = chars[(n >> 18) & 0x3F];
            out[j++] = chars[(n >> 12) & 0x3F];
            out[j++] = (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
            out[j++] = (i + 2 < len) ? chars[n & 0x3F] : '=';
        }
        out[j] = '\0';
    }
};

} // namespace binance
} // namespace feed
} // namespace chimera
