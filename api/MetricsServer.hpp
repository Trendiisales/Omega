// =============================================================================
// MetricsServer.hpp - HTTP Metrics Server for Chimera HFT
// Serves Prometheus-format metrics for remote dashboard access
// =============================================================================
#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <cstring>
#include <cstdio>
#include <functional>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace chimera {

// =============================================================================
// Global Metrics State - Updated by engine components
// =============================================================================
struct ChimeraMetrics {
    // Connection status
    std::atomic<int> binance_connected{0};
    std::atomic<int> fix_quote_connected{0};
    std::atomic<int> fix_trade_connected{0};
    
    // Tick counters
    std::atomic<uint64_t> binance_ticks{0};
    std::atomic<uint64_t> fix_ticks{0};
    std::atomic<uint64_t> fix_messages{0};
    std::atomic<uint64_t> fix_heartbeats{0};
    std::atomic<uint64_t> fix_errors{0};
    
    // Latency (microseconds)
    std::atomic<int64_t> binance_latency_us{200};
    std::atomic<int64_t> fix_quote_latency_us{0};
    std::atomic<int64_t> fix_trade_latency_us{0};
    std::atomic<int64_t> engine_loop_us{50};
    
    // Engine stats
    std::atomic<uint64_t> queue_depth{0};
    std::atomic<uint64_t> heartbeat{0};
    std::atomic<uint64_t> uptime_sec{0};
    
    // =========================================================================
    // Symbol Prices - Metals
    // =========================================================================
    std::atomic<double> xauusd_bid{0};
    std::atomic<double> xauusd_ask{0};
    std::atomic<double> xagusd_bid{0};
    std::atomic<double> xagusd_ask{0};
    
    // =========================================================================
    // Symbol Prices - Forex Majors
    // =========================================================================
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
    
    // =========================================================================
    // Symbol Prices - Crypto (Binance)
    // =========================================================================
    std::atomic<double> btcusdt_bid{0};
    std::atomic<double> btcusdt_ask{0};
    std::atomic<double> ethusdt_bid{0};
    std::atomic<double> ethusdt_ask{0};
    std::atomic<double> solusdt_bid{0};
    std::atomic<double> solusdt_ask{0};
    
    // =========================================================================
    // Symbol Prices - Indices
    // =========================================================================
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

// Global metrics instance
inline ChimeraMetrics g_metrics;

// =============================================================================
// MetricsServer - HTTP server for Prometheus metrics
// =============================================================================
class MetricsServer {
public:
    MetricsServer() = default;
    ~MetricsServer() { stop(); }
    
    bool start(uint16_t port = 9001) {
        port_ = port;
        
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::fprintf(stderr, "[MetricsServer] socket() failed\n");
            return false;
        }
        
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;  // Bind to all interfaces for remote access
        addr.sin_port = htons(port);
        
        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "[MetricsServer] bind() failed on port %u\n", port);
            close(server_fd_);
            return false;
        }
        
        if (listen(server_fd_, 10) < 0) {
            std::fprintf(stderr, "[MetricsServer] listen() failed\n");
            close(server_fd_);
            return false;
        }
        
        running_.store(true);
        server_thread_ = std::thread(&MetricsServer::run, this);
        
        std::printf("[MetricsServer] Started on port %u (accessible remotely)\n", port);
        return true;
    }
    
    void stop() {
        running_.store(false);
        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
            server_fd_ = -1;
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
private:
    void run() {
        while (running_.load()) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (running_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                continue;
            }
            
            // Read request (we don't really care about the content)
            char buf[1024];
            ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                
                // Generate metrics
                std::string body = generate_metrics();
                
                // HTTP response with CORS headers for browser access
                std::string response = 
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET\r\n"
                    "Connection: close\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "\r\n" + body;
                
                write(client_fd, response.c_str(), response.size());
            }
            
            close(client_fd);
        }
    }
    
    std::string generate_metrics() {
        std::string out;
        out.reserve(4096);
        
        char buf[256];
        
        // Connection status
        out += "# HELP chimera_binance_connected Binance WebSocket connection status\n";
        out += "# TYPE chimera_binance_connected gauge\n";
        out += "chimera_binance_connected " + std::to_string(g_metrics.binance_connected.load()) + "\n";
        out += "chimera_fix_quote_connected " + std::to_string(g_metrics.fix_quote_connected.load()) + "\n";
        out += "chimera_fix_trade_connected " + std::to_string(g_metrics.fix_trade_connected.load()) + "\n";
        out += "\n";
        
        // Tick counters
        out += "# HELP chimera_binance_ticks Total Binance ticks received\n";
        out += "# TYPE chimera_binance_ticks counter\n";
        out += "chimera_binance_ticks " + std::to_string(g_metrics.binance_ticks.load()) + "\n";
        out += "chimera_fix_ticks " + std::to_string(g_metrics.fix_ticks.load()) + "\n";
        out += "chimera_fix_messages " + std::to_string(g_metrics.fix_messages.load()) + "\n";
        out += "chimera_fix_heartbeats " + std::to_string(g_metrics.fix_heartbeats.load()) + "\n";
        out += "chimera_fix_errors " + std::to_string(g_metrics.fix_errors.load()) + "\n";
        out += "\n";
        
        // Latency
        out += "# HELP chimera_binance_latency_us Latency in microseconds\n";
        out += "# TYPE chimera_binance_latency_us gauge\n";
        out += "chimera_binance_latency_us " + std::to_string(g_metrics.binance_latency_us.load()) + "\n";
        out += "chimera_fix_quote_latency_us " + std::to_string(g_metrics.fix_quote_latency_us.load()) + "\n";
        out += "chimera_fix_trade_latency_us " + std::to_string(g_metrics.fix_trade_latency_us.load()) + "\n";
        out += "\n";
        
        // Engine
        out += "# HELP chimera_engine_loop_us Engine loop time in microseconds\n";
        out += "# TYPE chimera_engine_loop_us gauge\n";
        out += "chimera_engine_loop_us " + std::to_string(g_metrics.engine_loop_us.load()) + "\n";
        out += "chimera_queue_depth " + std::to_string(g_metrics.queue_depth.load()) + "\n";
        out += "chimera_heartbeat " + std::to_string(g_metrics.heartbeat.load()) + "\n";
        out += "chimera_uptime_sec " + std::to_string(g_metrics.uptime_sec.load()) + "\n";
        out += "\n";
        
        // Metals
        out += "# Metals\n";
        std::snprintf(buf, sizeof(buf), "chimera_xauusd_bid %.5f\n", g_metrics.xauusd_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_xauusd_ask %.5f\n", g_metrics.xauusd_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_xagusd_bid %.5f\n", g_metrics.xagusd_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_xagusd_ask %.5f\n", g_metrics.xagusd_ask.load());
        out += buf;
        out += "\n";
        
        // Forex
        out += "# Forex majors\n";
        std::snprintf(buf, sizeof(buf), "chimera_eurusd_bid %.5f\n", g_metrics.eurusd_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_eurusd_ask %.5f\n", g_metrics.eurusd_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_gbpusd_bid %.5f\n", g_metrics.gbpusd_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_gbpusd_ask %.5f\n", g_metrics.gbpusd_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_usdjpy_bid %.5f\n", g_metrics.usdjpy_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_usdjpy_ask %.5f\n", g_metrics.usdjpy_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_audusd_bid %.5f\n", g_metrics.audusd_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_audusd_ask %.5f\n", g_metrics.audusd_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_usdcad_bid %.5f\n", g_metrics.usdcad_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_usdcad_ask %.5f\n", g_metrics.usdcad_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_nzdusd_bid %.5f\n", g_metrics.nzdusd_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_nzdusd_ask %.5f\n", g_metrics.nzdusd_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_usdchf_bid %.5f\n", g_metrics.usdchf_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_usdchf_ask %.5f\n", g_metrics.usdchf_ask.load());
        out += buf;
        out += "\n";
        
        // Crypto
        out += "# Crypto\n";
        std::snprintf(buf, sizeof(buf), "chimera_btcusdt_bid %.2f\n", g_metrics.btcusdt_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_btcusdt_ask %.2f\n", g_metrics.btcusdt_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_ethusdt_bid %.2f\n", g_metrics.ethusdt_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_ethusdt_ask %.2f\n", g_metrics.ethusdt_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_solusdt_bid %.2f\n", g_metrics.solusdt_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_solusdt_ask %.2f\n", g_metrics.solusdt_ask.load());
        out += buf;
        out += "\n";
        
        // Indices
        out += "# Indices\n";
        std::snprintf(buf, sizeof(buf), "chimera_nas100_bid %.2f\n", g_metrics.nas100_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_nas100_ask %.2f\n", g_metrics.nas100_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_spx500_bid %.2f\n", g_metrics.spx500_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_spx500_ask %.2f\n", g_metrics.spx500_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_us30_bid %.2f\n", g_metrics.us30_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_us30_ask %.2f\n", g_metrics.us30_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_ger30_bid %.2f\n", g_metrics.ger30_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_ger30_ask %.2f\n", g_metrics.ger30_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_uk100_bid %.2f\n", g_metrics.uk100_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_uk100_ask %.2f\n", g_metrics.uk100_ask.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_jpn225_bid %.2f\n", g_metrics.jpn225_bid.load());
        out += buf;
        std::snprintf(buf, sizeof(buf), "chimera_jpn225_ask %.2f\n", g_metrics.jpn225_ask.load());
        out += buf;
        
        return out;
    }
    
    uint16_t port_{9001};
    int server_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread server_thread_;
};

} // namespace chimera
