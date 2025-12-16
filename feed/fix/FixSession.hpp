// =============================================================================
// FixSession.hpp - FIX.4.4 Protocol Session with SSL for cTrader
// Supports QUOTE/TRADE sessions with Security List for symbol ID resolution
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <thread>
#include <mutex>
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
#include <poll.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "core/MonotonicClock.hpp"

namespace chimera {
namespace feed {
namespace fix {

static constexpr char MSG_LOGON = 'A';
static constexpr char MSG_LOGOUT = '5';
static constexpr char MSG_HEARTBEAT = '0';
static constexpr char MSG_TEST_REQUEST = '1';
static constexpr char MSG_RESEND_REQUEST = '2';
static constexpr char MSG_REJECT = '3';
static constexpr char MSG_SEQUENCE_RESET = '4';
static constexpr char MSG_MARKET_DATA_REQUEST = 'V';
static constexpr char MSG_MARKET_DATA_SNAPSHOT = 'W';
static constexpr char MSG_MARKET_DATA_INCREMENTAL = 'X';
static constexpr char MSG_MARKET_DATA_REQUEST_REJECT = 'Y';
static constexpr char MSG_NEW_ORDER_SINGLE = 'D';
static constexpr char MSG_EXECUTION_REPORT = '8';
static constexpr char MSG_SECURITY_LIST_REQUEST = 'x';
static constexpr char MSG_SECURITY_LIST = 'y';

struct FixTick {
    char symbol[16];
    double bid;
    double ask;
    double bid_size;
    double ask_size;
    uint64_t timestamp_ns;
    uint64_t sequence;
};

struct FixStats {
    std::atomic<uint64_t> ticks_received{0};
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> heartbeats_sent{0};
    std::atomic<uint64_t> heartbeats_received{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> last_tick_ns{0};
    std::atomic<int64_t> last_latency_us{0};
    
    // Per-symbol tracking - Metals
    std::atomic<double> xauusd_bid{0};
    std::atomic<double> xauusd_ask{0};
    std::atomic<double> xagusd_bid{0};
    std::atomic<double> xagusd_ask{0};
    
    // Forex majors
    std::atomic<double> eurusd_bid{0};
    std::atomic<double> eurusd_ask{0};
    std::atomic<double> gbpusd_bid{0};
    std::atomic<double> gbpusd_ask{0};
    std::atomic<double> usdjpy_bid{0};
    std::atomic<double> usdjpy_ask{0};
    std::atomic<double> audusd_bid{0};
    std::atomic<double> audusd_ask{0};
    std::atomic<double> usdcad_bid{0};
    std::atomic<double> usdcad_ask{0};
    std::atomic<double> nzdusd_bid{0};
    std::atomic<double> nzdusd_ask{0};
    std::atomic<double> usdchf_bid{0};
    std::atomic<double> usdchf_ask{0};
    
    // Indices
    std::atomic<double> nas100_bid{0};
    std::atomic<double> nas100_ask{0};
    std::atomic<double> spx500_bid{0};
    std::atomic<double> spx500_ask{0};
    std::atomic<double> us30_bid{0};
    std::atomic<double> us30_ask{0};
    std::atomic<double> ger30_bid{0};
    std::atomic<double> ger30_ask{0};
    std::atomic<double> uk100_bid{0};
    std::atomic<double> uk100_ask{0};
    std::atomic<double> jpn225_bid{0};
    std::atomic<double> jpn225_ask{0};
};

using FixTickCallback = std::function<void(const FixTick&)>;
using ExecutionCallback = std::function<void(const char* cl_ord_id, const char* exec_type, 
                                             double fill_price, double fill_qty)>;

class FixSession {
public:
    FixStats stats;

    FixSession()
        : ssl_ctx_(nullptr)
        , ssl_(nullptr)
        , sock_fd_(-1)
        , connected_(false)
        , logged_on_(false)
        , running_(false)
        , security_list_received_(false)
        , seq_num_(1)
        , heartbeat_interval_(30)
        , last_send_time_(0)
        , last_recv_time_(0)
    {
        SSL_library_init();
        SSL_load_error_strings();
        std::memset(sender_comp_id_, 0, sizeof(sender_comp_id_));
        std::memset(target_comp_id_, 0, sizeof(target_comp_id_));
        std::memset(sub_id_, 0, sizeof(sub_id_));
        std::memset(username_, 0, sizeof(username_));
        std::memset(password_, 0, sizeof(password_));
        std::strcpy(sub_id_, "TRADE");
    }

    ~FixSession() { disconnect(); }

    void set_credentials(const char* sender, const char* target,
                        const char* username, const char* password,
                        const char* sub_id = "TRADE") {
        std::strncpy(sender_comp_id_, sender, sizeof(sender_comp_id_) - 1);
        std::strncpy(target_comp_id_, target, sizeof(target_comp_id_) - 1);
        std::strncpy(username_, username, sizeof(username_) - 1);
        std::strncpy(password_, password, sizeof(password_) - 1);
        std::strncpy(sub_id_, sub_id, sizeof(sub_id_) - 1);
    }

    void set_tick_callback(FixTickCallback cb) { tick_callback_ = std::move(cb); }
    void set_execution_callback(ExecutionCallback cb) { exec_callback_ = std::move(cb); }

    bool connect(const char* host, uint16_t port) {
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) return false;
        
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ssl_ctx_);

        hostent* he = gethostbyname(host);
        if (!he) {
            std::fprintf(stderr, "[FIX-%s] gethostbyname failed for %s\n", sub_id_, host);
            return false;
        }

        sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd_ < 0) return false;
        
        int flag = 1;
        setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        std::memcpy(&addr.sin_addr, he->h_addr, he->h_length);

        if (::connect(sock_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "[FIX-%s] connect() failed\n", sub_id_);
            return false;
        }

        ssl_ = SSL_new(ssl_ctx_);
        SSL_set_fd(ssl_, sock_fd_);
        SSL_set_tlsext_host_name(ssl_, host);

        if (SSL_connect(ssl_) != 1) {
            std::fprintf(stderr, "[FIX-%s] SSL_connect failed\n", sub_id_);
            return false;
        }

        connected_.store(true, std::memory_order_release);
        std::printf("[FIX-%s] SSL connected to %s:%d\n", sub_id_, host, port);

        if (!send_logon()) return false;

        std::printf("[FIX-%s] Waiting for logon response...\n", sub_id_);
        for (int i = 0; i < 50; ++i) {
            struct pollfd pfd = { sock_fd_, POLLIN, 0 };
            if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
                char buf[4096];
                int n = SSL_read(ssl_, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    last_recv_time_ = chimera::core::MonotonicClock::now_ns();
                    
                    char debug[4096];
                    std::memcpy(debug, buf, n);
                    for (int j = 0; j < n; ++j)
                        if (debug[j] == '\x01') debug[j] = '|';
                    debug[n] = '\0';
                    std::printf("[FIX-%s] RAW RESPONSE: %s\n", sub_id_, debug);

                    if (std::strstr(buf, "35=A")) {
                        logged_on_.store(true, std::memory_order_release);
                        std::printf("[FIX-%s] Logon successful!\n", sub_id_);
                        return true;
                    }
                    if (std::strstr(buf, "35=5")) {
                        const char* r = std::strstr(buf, "58=");
                        if (r) {
                            char reason[256] = {0};
                            const char* end = std::strchr(r + 3, '\x01');
                            if (end) std::strncpy(reason, r + 3, std::min((size_t)(end - r - 3), sizeof(reason) - 1));
                            std::printf("[FIX-%s] Logout: %s\n", sub_id_, reason);
                        }
                        return false;
                    }
                } else if (n <= 0) {
                    int err = SSL_get_error(ssl_, n);
                    std::printf("[FIX-%s] SSL_read error: %d\n", sub_id_, err);
                    return false;
                }
            }
        }
        std::printf("[FIX-%s] Logon timeout\n", sub_id_);
        return false;
    }

    void start() {
        if (!connected_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        recv_thread_ = std::thread([this] { recv_loop(); });
        heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });
    }

    void disconnect() {
        running_.store(false, std::memory_order_release);
        if (ssl_) SSL_shutdown(ssl_);
        if (recv_thread_.joinable()) recv_thread_.join();
        if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
        if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
        if (sock_fd_ >= 0) { close(sock_fd_); sock_fd_ = -1; }
        if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
        connected_.store(false, std::memory_order_release);
        logged_on_.store(false, std::memory_order_release);
    }

    // Request Security List to get symbol IDs
    bool request_security_list() {
        if (!logged_on_.load(std::memory_order_acquire)) return false;
        
        char extra[64];
        int len = std::snprintf(extra, sizeof(extra),
            "320=SECLIST1\x01"    // SecurityReqID
            "559=0\x01");         // SecurityListRequestType: 0=All Securities
        
        return send_message(MSG_SECURITY_LIST_REQUEST, extra, len);
    }

    // Get symbol ID by name (returns 0 if not found)
    int get_symbol_id(const std::string& name) const {
        std::lock_guard<std::mutex> lock(symbol_mutex_);
        auto it = symbol_name_to_id_.find(name);
        if (it != symbol_name_to_id_.end()) {
            return it->second;
        }
        return 0;
    }

    // Get symbol name by ID
    std::string get_symbol_name(int id) const {
        std::lock_guard<std::mutex> lock(symbol_mutex_);
        auto it = symbol_id_to_name_.find(id);
        if (it != symbol_id_to_name_.end()) {
            return it->second;
        }
        return "";
    }

    bool has_security_list() const { return security_list_received_.load(); }

    // Subscribe using symbol names (will be converted to IDs)
    bool subscribe_market_data(const std::vector<std::string>& symbols) {
        if (!logged_on_.load(std::memory_order_acquire)) return false;
        
        for (const auto& sym : symbols) {
            int sym_id = get_symbol_id(sym);
            if (sym_id == 0) {
                std::printf("[FIX-%s] Warning: No ID found for symbol %s, using name\n", 
                           sub_id_, sym.c_str());
                // Try anyway with name (will fail, but shows the error)
                char extra[256];
                int len = std::snprintf(extra, sizeof(extra),
                    "262=%s\x01"
                    "263=1\x01"
                    "264=1\x01"
                    "267=2\x01"
                    "269=0\x01"
                    "269=1\x01"
                    "146=1\x01"
                    "55=%s\x01",
                    sym.c_str(), sym.c_str());
                send_message(MSG_MARKET_DATA_REQUEST, extra, len);
            } else {
                std::printf("[FIX-%s] Subscribing to %s (ID=%d)\n", sub_id_, sym.c_str(), sym_id);
                char extra[256];
                int len = std::snprintf(extra, sizeof(extra),
                    "262=%s\x01"       // MDReqID = symbol name for reference
                    "263=1\x01"        // SubscriptionRequestType: 1=Snapshot+Updates
                    "264=1\x01"        // MarketDepth: 1=Top of Book
                    "267=2\x01"        // NoMDEntryTypes: 2
                    "269=0\x01"        // MDEntryType: 0=Bid
                    "269=1\x01"        // MDEntryType: 1=Offer
                    "146=1\x01"        // NoRelatedSym: 1
                    "55=%d\x01",       // Symbol = numeric ID
                    sym.c_str(), sym_id);
                send_message(MSG_MARKET_DATA_REQUEST, extra, len);
            }
        }
        return true;
    }

    bool send_new_order(const char* cl_ord_id, const char* symbol,
                       char side, double qty, double price) {
        if (!logged_on_.load(std::memory_order_acquire)) return false;
        
        int sym_id = get_symbol_id(symbol);
        if (sym_id == 0) {
            std::printf("[FIX-%s] Cannot send order: no ID for %s\n", sub_id_, symbol);
            return false;
        }
        
        char extra[512];
        int len = std::snprintf(extra, sizeof(extra),
            "11=%s\x01" "55=%d\x01" "54=%c\x01" "38=%.8f\x01"
            "40=2\x01" "44=%.5f\x01" "59=0\x01" "60=%s\x01",
            cl_ord_id, sym_id, side, qty, price, timestamp());
        return send_message(MSG_NEW_ORDER_SINGLE, extra, len);
    }

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }
    bool is_logged_on() const { return logged_on_.load(std::memory_order_acquire); }
    uint64_t messages_sent() const { return stats.messages_sent.load(); }
    uint64_t messages_received() const { return stats.messages_received.load(); }
    uint64_t ticks_received() const { return stats.ticks_received.load(); }
    const char* session_type() const { return sub_id_; }
    
    size_t symbol_count() const {
        std::lock_guard<std::mutex> lock(symbol_mutex_);
        return symbol_name_to_id_.size();
    }

private:
    SSL_CTX* ssl_ctx_;
    SSL* ssl_;
    int sock_fd_;
    std::atomic<bool> connected_;
    std::atomic<bool> logged_on_;
    std::atomic<bool> running_;
    std::atomic<bool> security_list_received_;
    std::thread recv_thread_;
    std::thread heartbeat_thread_;

    char sender_comp_id_[64];
    char target_comp_id_[64];
    char sub_id_[32];
    char username_[64];
    char password_[64];

    uint64_t seq_num_;
    int heartbeat_interval_;
    uint64_t last_send_time_;
    uint64_t last_recv_time_;

    FixTickCallback tick_callback_;
    ExecutionCallback exec_callback_;

    // Symbol ID mappings
    mutable std::mutex symbol_mutex_;
    std::unordered_map<std::string, int> symbol_name_to_id_;
    std::unordered_map<int, std::string> symbol_id_to_name_;

    char send_buf_[4096];
    char recv_buf_[65536];  // Larger for security list
    int recv_len_ = 0;

    void parse_fix_message(const char* msg, int len, std::unordered_map<int, std::string>& fields) {
        fields.clear();
        const char* p = msg;
        const char* end = msg + len;
        
        while (p < end) {
            const char* eq = (const char*)std::memchr(p, '=', end - p);
            if (!eq) break;
            
            int tag = std::atoi(p);
            
            const char* soh = (const char*)std::memchr(eq + 1, '\x01', end - eq - 1);
            if (!soh) soh = end;
            
            if (tag > 0) {
                fields[tag] = std::string(eq + 1, soh - eq - 1);
            }
            
            p = soh + 1;
        }
    }

    double get_double(const std::unordered_map<int, std::string>& fields, int tag) {
        auto it = fields.find(tag);
        if (it == fields.end()) return 0.0;
        return std::atof(it->second.c_str());
    }

    std::string get_string(const std::unordered_map<int, std::string>& fields, int tag) {
        auto it = fields.find(tag);
        if (it == fields.end()) return "";
        return it->second;
    }

    // Parse Security List response to build symbol map
    void process_security_list(const char* msg, int len) {
        std::lock_guard<std::mutex> lock(symbol_mutex_);
        
        // Parse repeating groups: 55=id followed by 1007=name
        const char* p = msg;
        const char* end = msg + len;
        
        int current_id = 0;
        std::string current_name;
        
        while (p < end) {
            const char* eq = (const char*)std::memchr(p, '=', end - p);
            if (!eq) break;
            
            int tag = std::atoi(p);
            
            const char* soh = (const char*)std::memchr(eq + 1, '\x01', end - eq - 1);
            if (!soh) soh = end;
            
            std::string val(eq + 1, soh - eq - 1);
            
            if (tag == 55) {  // Symbol ID
                // If we have a pending pair, save it
                if (current_id > 0 && !current_name.empty()) {
                    symbol_name_to_id_[current_name] = current_id;
                    symbol_id_to_name_[current_id] = current_name;
                }
                current_id = std::atoi(val.c_str());
                current_name.clear();
            }
            else if (tag == 1007) {  // Symbol Name
                current_name = val;
                // Save pair immediately
                if (current_id > 0) {
                    symbol_name_to_id_[current_name] = current_id;
                    symbol_id_to_name_[current_id] = current_name;
                }
            }
            
            p = soh + 1;
        }
        
        // Save last pair if any
        if (current_id > 0 && !current_name.empty()) {
            symbol_name_to_id_[current_name] = current_id;
            symbol_id_to_name_[current_id] = current_name;
        }
        
        security_list_received_.store(true, std::memory_order_release);
        std::printf("[FIX-%s] Security list received: %zu symbols\n", 
                   sub_id_, symbol_name_to_id_.size());
        
        // Debug: print a few key symbols
        for (const auto& name : {"EURUSD", "XAUUSD", "GBPUSD", "USDJPY"}) {
            auto it = symbol_name_to_id_.find(name);
            if (it != symbol_name_to_id_.end()) {
                std::printf("[FIX-%s] Symbol: %s = %d\n", sub_id_, name, it->second);
            }
        }
    }

    // Parse market data from raw message to handle repeating groups
    void process_market_data_raw(const char* msg, int len) {
        uint64_t now_ns = chimera::core::MonotonicClock::now_ns();
        
        // Extract key fields by scanning the raw message
        const char* p = msg;
        const char* end = msg + len;
        
        int sym_id = 0;
        std::string md_req_id;
        double bid = 0.0, ask = 0.0;
        double bid_size = 0.0, ask_size = 0.0;
        
        int current_entry_type = -1;  // 269: 0=Bid, 1=Offer
        
        while (p < end) {
            const char* eq = (const char*)std::memchr(p, '=', end - p);
            if (!eq) break;
            
            int tag = std::atoi(p);
            
            const char* soh = (const char*)std::memchr(eq + 1, '\x01', end - eq - 1);
            if (!soh) soh = end;
            
            std::string val(eq + 1, soh - eq - 1);
            
            switch (tag) {
                case 55:  // Symbol
                    sym_id = std::atoi(val.c_str());
                    break;
                case 262:  // MDReqID
                    md_req_id = val;
                    break;
                case 269:  // MDEntryType: 0=Bid, 1=Offer
                    current_entry_type = std::atoi(val.c_str());
                    break;
                case 270:  // MDEntryPx (price)
                    if (current_entry_type == 0) {
                        bid = std::atof(val.c_str());
                    } else if (current_entry_type == 1) {
                        ask = std::atof(val.c_str());
                    }
                    break;
                case 271:  // MDEntrySize
                    if (current_entry_type == 0) {
                        bid_size = std::atof(val.c_str());
                    } else if (current_entry_type == 1) {
                        ask_size = std::atof(val.c_str());
                    }
                    break;
                case 132:  // BidPx (simple format)
                    bid = std::atof(val.c_str());
                    break;
                case 133:  // OfferPx (simple format)
                    ask = std::atof(val.c_str());
                    break;
                case 134:  // BidSize
                    bid_size = std::atof(val.c_str());
                    break;
                case 135:  // OfferSize
                    ask_size = std::atof(val.c_str());
                    break;
            }
            
            p = soh + 1;
        }
        
        // Get symbol name
        std::string symbol = get_symbol_name(sym_id);
        if (symbol.empty() && !md_req_id.empty()) {
            symbol = md_req_id;
        }
        if (symbol.empty() && sym_id > 0) {
            symbol = "ID:" + std::to_string(sym_id);
        }
        if (symbol.empty()) return;
        
        FixTick tick{};
        std::strncpy(tick.symbol, symbol.c_str(), sizeof(tick.symbol) - 1);
        tick.timestamp_ns = now_ns;
        tick.sequence = stats.ticks_received.load();
        tick.bid = bid;
        tick.ask = ask;
        tick.bid_size = bid_size;
        tick.ask_size = ask_size;
        
        // Update per-symbol stats
        if (symbol == "XAUUSD" || symbol.find("XAU") != std::string::npos) {
            if (tick.bid > 0) stats.xauusd_bid.store(tick.bid);
            if (tick.ask > 0) stats.xauusd_ask.store(tick.ask);
        } else if (symbol == "XAGUSD" || symbol.find("XAG") != std::string::npos) {
            if (tick.bid > 0) stats.xagusd_bid.store(tick.bid);
            if (tick.ask > 0) stats.xagusd_ask.store(tick.ask);
        } else if (symbol == "EURUSD") {
            if (tick.bid > 0) stats.eurusd_bid.store(tick.bid);
            if (tick.ask > 0) stats.eurusd_ask.store(tick.ask);
        } else if (symbol == "GBPUSD") {
            if (tick.bid > 0) stats.gbpusd_bid.store(tick.bid);
            if (tick.ask > 0) stats.gbpusd_ask.store(tick.ask);
        } else if (symbol == "USDJPY") {
            if (tick.bid > 0) stats.usdjpy_bid.store(tick.bid);
            if (tick.ask > 0) stats.usdjpy_ask.store(tick.ask);
        } else if (symbol == "AUDUSD") {
            if (tick.bid > 0) stats.audusd_bid.store(tick.bid);
            if (tick.ask > 0) stats.audusd_ask.store(tick.ask);
        } else if (symbol == "USDCAD") {
            if (tick.bid > 0) stats.usdcad_bid.store(tick.bid);
            if (tick.ask > 0) stats.usdcad_ask.store(tick.ask);
        } else if (symbol == "NZDUSD") {
            if (tick.bid > 0) stats.nzdusd_bid.store(tick.bid);
            if (tick.ask > 0) stats.nzdusd_ask.store(tick.ask);
        } else if (symbol == "USDCHF") {
            if (tick.bid > 0) stats.usdchf_bid.store(tick.bid);
            if (tick.ask > 0) stats.usdchf_ask.store(tick.ask);
        } else if (symbol == "NAS100") {
            if (tick.bid > 0) stats.nas100_bid.store(tick.bid);
            if (tick.ask > 0) stats.nas100_ask.store(tick.ask);
        } else if (symbol == "SPX500") {
            if (tick.bid > 0) stats.spx500_bid.store(tick.bid);
            if (tick.ask > 0) stats.spx500_ask.store(tick.ask);
        } else if (symbol == "US30") {
            if (tick.bid > 0) stats.us30_bid.store(tick.bid);
            if (tick.ask > 0) stats.us30_ask.store(tick.ask);
        } else if (symbol == "GER30") {
            if (tick.bid > 0) stats.ger30_bid.store(tick.bid);
            if (tick.ask > 0) stats.ger30_ask.store(tick.ask);
        } else if (symbol == "UK100") {
            if (tick.bid > 0) stats.uk100_bid.store(tick.bid);
            if (tick.ask > 0) stats.uk100_ask.store(tick.ask);
        } else if (symbol == "JPN225") {
            if (tick.bid > 0) stats.jpn225_bid.store(tick.bid);
            if (tick.ask > 0) stats.jpn225_ask.store(tick.ask);
        }
        
        // Calculate and store latency
        stats.last_latency_us.store((now_ns - tick.timestamp_ns) / 1000, std::memory_order_relaxed);
        
        stats.ticks_received.fetch_add(1, std::memory_order_relaxed);
        stats.last_tick_ns.store(now_ns, std::memory_order_relaxed);
        
        // Debug output for first few ticks
        static int debug_count = 0;
        if (debug_count < 20) {
            std::printf("[FIX-MD] %s bid=%.5f ask=%.5f (spread=%.5f)\n",
                       tick.symbol, tick.bid, tick.ask, tick.ask - tick.bid);
            debug_count++;
        }
        
        if (tick_callback_ && (tick.bid > 0 || tick.ask > 0)) {
            tick_callback_(tick);
        }
    }

    void process_message(const char* msg, int len) {
        std::unordered_map<int, std::string> fields;
        parse_fix_message(msg, len, fields);
        
        stats.messages_received.fetch_add(1, std::memory_order_relaxed);
        
        std::string msg_type = get_string(fields, 35);
        
        if (msg_type == "y") {
            // Security List Response
            process_security_list(msg, len);
        }
        else if (msg_type == "W" || msg_type == "X") {
            process_market_data_raw(msg, len);
        }
        else if (msg_type == "A") {
            logged_on_.store(true, std::memory_order_release);
        }
        else if (msg_type == "5") {
            logged_on_.store(false, std::memory_order_release);
            std::string reason = get_string(fields, 58);
            std::printf("[FIX-%s] Logout received: %s\n", sub_id_, reason.c_str());
        }
        else if (msg_type == "0") {
            stats.heartbeats_received.fetch_add(1, std::memory_order_relaxed);
        }
        else if (msg_type == "1") {
            std::string test_req_id = get_string(fields, 112);
            send_heartbeat(test_req_id.c_str());
        }
        else if (msg_type == "8") {
            if (exec_callback_) {
                std::string cl_ord_id = get_string(fields, 11);
                std::string exec_type = get_string(fields, 150);
                double fill_price = get_double(fields, 31);
                double fill_qty = get_double(fields, 32);
                exec_callback_(cl_ord_id.c_str(), exec_type.c_str(), fill_price, fill_qty);
            }
        }
        else if (msg_type == "Y") {
            std::string reason = get_string(fields, 58);
            std::string req_id = get_string(fields, 262);
            std::printf("[FIX-%s] MD Request Reject for %s: %s\n", sub_id_, req_id.c_str(), reason.c_str());
            stats.errors.fetch_add(1, std::memory_order_relaxed);
        }
        else if (msg_type == "3") {
            std::string reason = get_string(fields, 58);
            std::printf("[FIX-%s] Reject: %s\n", sub_id_, reason.c_str());
            stats.errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool send_logon() {
        seq_num_ = 1;
        const char* ts = timestamp();

        char body[512];
        int body_len = std::snprintf(body, sizeof(body),
            "35=A\x01"
            "49=%s\x01"
            "56=%s\x01"
            "34=%llu\x01"
            "52=%s\x01"
            "57=%s\x01"
            "50=%s\x01"
            "98=0\x01"
            "108=%d\x01"
            "141=Y\x01"
            "553=%s\x01"
            "554=%s\x01",
            sender_comp_id_,
            target_comp_id_,
            (unsigned long long)seq_num_++,
            ts,
            sub_id_,
            sub_id_,
            heartbeat_interval_,
            username_,
            password_);

        return send_raw(body, body_len);
    }

    bool send_heartbeat(const char* test_req_id = nullptr) {
        const char* ts = timestamp();
        char body[256];
        int body_len;
        
        if (test_req_id && test_req_id[0]) {
            body_len = std::snprintf(body, sizeof(body),
                "35=0\x01"
                "49=%s\x01"
                "56=%s\x01"
                "34=%llu\x01"
                "52=%s\x01"
                "57=%s\x01"
                "50=%s\x01"
                "112=%s\x01",
                sender_comp_id_,
                target_comp_id_,
                (unsigned long long)seq_num_++,
                ts,
                sub_id_,
                sub_id_,
                test_req_id);
        } else {
            body_len = std::snprintf(body, sizeof(body),
                "35=0\x01"
                "49=%s\x01"
                "56=%s\x01"
                "34=%llu\x01"
                "52=%s\x01"
                "57=%s\x01"
                "50=%s\x01",
                sender_comp_id_,
                target_comp_id_,
                (unsigned long long)seq_num_++,
                ts,
                sub_id_,
                sub_id_);
        }
        
        stats.heartbeats_sent.fetch_add(1, std::memory_order_relaxed);
        return send_raw(body, body_len);
    }

    bool send_message(char type, const char* extra, int extra_len) {
        const char* ts = timestamp();
        char body[512];
        int body_len = std::snprintf(body, sizeof(body),
            "35=%c\x01"
            "49=%s\x01"
            "56=%s\x01"
            "34=%llu\x01"
            "52=%s\x01"
            "57=%s\x01"
            "50=%s\x01",
            type,
            sender_comp_id_,
            target_comp_id_,
            (unsigned long long)seq_num_++,
            ts,
            sub_id_,
            sub_id_);

        if (extra && extra_len > 0) {
            std::memcpy(body + body_len, extra, extra_len);
            body_len += extra_len;
        }
        return send_raw(body, body_len);
    }

    bool send_raw(const char* body, int body_len) {
        int offset = std::snprintf(send_buf_, sizeof(send_buf_),
            "8=FIX.4.4\x01"
            "9=%d\x01",
            body_len);

        std::memcpy(send_buf_ + offset, body, body_len);
        offset += body_len;

        int checksum = 0;
        for (int i = 0; i < offset; ++i)
            checksum += (unsigned char)send_buf_[i];
        checksum %= 256;

        offset += std::snprintf(send_buf_ + offset, sizeof(send_buf_) - offset,
            "10=%03d\x01", checksum);

        const char* msg_type = std::strstr(send_buf_, "35=");
        bool is_heartbeat = msg_type && msg_type[3] == '0' && msg_type[4] == '\x01';
        
        if (!is_heartbeat) {
            char debug[2048];
            std::memcpy(debug, send_buf_, std::min(offset, 2047));
            for (int i = 0; i < offset && i < 2047; ++i)
                if (debug[i] == '\x01') debug[i] = '|';
            debug[std::min(offset, 2047)] = '\0';
            std::printf("[FIX-%s] Sending (body_len=%d): %s\n", sub_id_, body_len, debug);
        }

        int n = SSL_write(ssl_, send_buf_, offset);
        if (n == offset) {
            stats.messages_sent.fetch_add(1, std::memory_order_relaxed);
            last_send_time_ = chimera::core::MonotonicClock::now_ns();
            return true;
        }
        return false;
    }

    void recv_loop() {
        while (running_.load(std::memory_order_acquire)) {
            struct pollfd pfd = { sock_fd_, POLLIN, 0 };
            int ret = poll(&pfd, 1, 100);
            
            if (ret <= 0) continue;
            if (!(pfd.revents & POLLIN)) continue;
            
            int n = SSL_read(ssl_, recv_buf_ + recv_len_, sizeof(recv_buf_) - recv_len_ - 1);
            if (n <= 0) {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                std::printf("[FIX-%s] SSL_read error: %d\n", sub_id_, err);
                logged_on_.store(false, std::memory_order_release);
                break;
            }
            
            recv_len_ += n;
            recv_buf_[recv_len_] = '\0';
            last_recv_time_ = chimera::core::MonotonicClock::now_ns();
            
            char* start = recv_buf_;
            char* end = recv_buf_ + recv_len_;
            
            while (start < end) {
                char* msg_start = std::strstr(start, "8=FIX");
                if (!msg_start) break;
                
                char* checksum = std::strstr(msg_start, "\x01" "10=");
                if (!checksum) break;
                
                char* msg_end = std::strchr(checksum + 4, '\x01');
                if (!msg_end) break;
                
                msg_end++;
                
                int msg_len = msg_end - msg_start;
                process_message(msg_start, msg_len);
                
                start = msg_end;
            }
            
            if (start > recv_buf_ && start < end) {
                recv_len_ = end - start;
                std::memmove(recv_buf_, start, recv_len_);
            } else if (start >= end) {
                recv_len_ = 0;
            }
        }
    }

    void heartbeat_loop() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(heartbeat_interval_ / 2));
            
            if (!logged_on_.load(std::memory_order_acquire)) continue;
            
            uint64_t now = chimera::core::MonotonicClock::now_ns();
            uint64_t since_send = (now - last_send_time_) / 1000000000ULL;
            
            if (since_send >= (uint64_t)heartbeat_interval_) {
                send_heartbeat();
            }
        }
    }

    const char* timestamp() {
        static thread_local char buf[32];
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H:%M:%S", &tm);
        return buf;
    }
};

} // namespace fix
} // namespace feed
} // namespace chimera
