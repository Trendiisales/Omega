// =============================================================================
// main.cpp - CHIMERA HFT ENGINE (AUTHORITATIVE BUILD)
// Dual FIX sessions with Security List for symbol ID resolution
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <atomic>
#include <type_traits>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

// Core
#include "core/SPSCQueue.hpp"
#include "core/Logger.hpp"
#include "core/LatencyStats.hpp"
#include "core/ThreadPin.hpp"
#include "core/MonotonicClock.hpp"

// Market
#include "market/MarketTypes.hpp"
#include "market/TickValidator.hpp"

// Engine
#include "engine/EngineConfig.hpp"
#include "engine/EngineIngress.hpp"
#include "engine/EngineCore.hpp"
#include "engine/Intent.hpp"
#include "engine/IntentQueue.hpp"
#include "engine/StrategyRunner.hpp"
#include "engine/ExecutionRouter.hpp"
#include "engine/EngineHealth.hpp"
#include "engine/QueueMetrics.hpp"
#include "engine/EngineSupervisor.hpp"
#include "engine/ExecutionHealth.hpp"
#include "engine/NullExecutionRouter.hpp"
#include "engine/IExecutionRouter.hpp"

// Strategies
#include "strategy/momentum/MomentumStrategy.hpp"

// Feed
#include "feed/binance/BinanceTradeNormalizer.hpp"
#include "feed/binance/BinanceWebSocket.hpp"
#include "feed/fix/FixSession.hpp"

// API & Monitoring (new MetricsServer with g_metrics)
#include "api/MetricsServer.hpp"
#include "monitor/AlertEngine.hpp"

// =============================================================================
// COMPILE-TIME GUARDS
// =============================================================================
static_assert(sizeof(chimera::market::Tick) == 64, "Tick must be 64 bytes");
static_assert(std::is_trivially_copyable<chimera::market::Tick>::value);
static_assert(alignof(chimera::market::Tick) == 64);

// =============================================================================
// Global state
// =============================================================================
static std::atomic<bool> g_running{true};
static uint64_t g_start_time_ns = 0;

static std::atomic<uint64_t> g_binance_ticks{0};
static std::atomic<uint64_t> g_fix_ticks{0};

// =============================================================================
// Signal handler
// =============================================================================
static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// =============================================================================
// Simple config
// =============================================================================
struct ChimeraConfig {
    std::vector<std::string> binance_symbols;
    bool binance_enabled = true;

    std::string fix_quote_host;
    uint16_t fix_quote_port = 5211;
    
    std::string fix_trade_host;
    uint16_t fix_trade_port = 5212;
    
    std::string fix_sender;
    std::string fix_target;
    std::string fix_username;
    std::string fix_password;
    std::vector<std::string> fix_symbols;
    bool fix_enabled = true;
};

static ChimeraConfig load_config(const char* path) {
    ChimeraConfig cfg;

    cfg.binance_symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    
    // cTrader FIX - BlackBull demo
    cfg.fix_quote_host = "demo-uk-eqx-01.p.c-trader.com";
    cfg.fix_quote_port = 5211;
    
    cfg.fix_trade_host = "demo-uk-eqx-01.p.c-trader.com";
    cfg.fix_trade_port = 5212;
    
    cfg.fix_sender = "demo.blackbull.2067070";
    cfg.fix_target = "cServer";
    cfg.fix_username = "2067070";
    cfg.fix_password = "Bowen6feb";
    cfg.fix_symbols = {"XAUUSD", "EURUSD", "GBPUSD", "USDJPY", "AUDUSD", "USDCAD", "NZDUSD", "USDCHF", "XAGUSD",
                       "NAS100", "SPX500", "US30", "GER30", "UK100", "JPN225"};

    std::ifstream f(path);
    if (!f.is_open()) {
        std::printf("[CONFIG] Using defaults (no config file at %s)\n", path);
        return cfg;
    }

    std::printf("[CONFIG] Loaded from %s\n", path);
    return cfg;
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    g_start_time_ns = chimera::core::MonotonicClock::now_ns();

    const char* config_path = (argc > 1) ? argv[1] : "config/chimera.ini";
    ChimeraConfig app_cfg = load_config(config_path);

    // =========================================================================
    // Metrics server (uses chimera::MetricsServer from new header)
    // =========================================================================
    chimera::MetricsServer metrics_server;
    if (!metrics_server.start(9001)) {
        std::fprintf(stderr, "[ERROR] Metrics server failed\n");
        return 1;
    }
    std::printf("[METRICS] Server started on port 9001\n");

    // =========================================================================
    // Binance WebSocket
    // =========================================================================
    chimera::feed::binance::BinanceTradeNormalizer normalizer(
        chimera::market::VENUE_BINANCE);
    chimera::feed::binance::BinanceWebSocket binance_ws;

    binance_ws.set_callback([&](const chimera::feed::binance::RawTick& raw) {
        chimera::market::Tick tick;
        normalizer.normalize_trade(
            raw.symbol_id,
            raw.exchange_ts_ns,
            raw.local_ts_ns,
            (raw.bid_price + raw.ask_price) * 0.5,
            raw.bid_qty + raw.ask_qty,
            true,
            tick
        );
        g_binance_ticks.fetch_add(1, std::memory_order_relaxed);
        
        // Update crypto prices in g_metrics
        // Symbol IDs: 0=BTCUSDT, 1=ETHUSDT, 2=SOLUSDT (based on subscription order)
        if (raw.symbol_id == 0) {
            chimera::g_metrics.btcusdt_bid.store(raw.bid_price);
            chimera::g_metrics.btcusdt_ask.store(raw.ask_price);
        } else if (raw.symbol_id == 1) {
            chimera::g_metrics.ethusdt_bid.store(raw.bid_price);
            chimera::g_metrics.ethusdt_ask.store(raw.ask_price);
        } else if (raw.symbol_id == 2) {
            chimera::g_metrics.solusdt_bid.store(raw.bid_price);
            chimera::g_metrics.solusdt_ask.store(raw.ask_price);
        }
        
        // Update Binance latency (exchange to local)
        int64_t lat_us = (raw.local_ts_ns - raw.exchange_ts_ns) / 1000;
        if (lat_us > 0 && lat_us < 100000) {
            chimera::g_metrics.binance_latency_us.store(lat_us);
        }
    });

    if (app_cfg.binance_enabled && binance_ws.connect(app_cfg.binance_symbols)) {
        binance_ws.start();
        std::printf("[BINANCE] Feed started\n");
    }

    // =========================================================================
    // FIX QUOTE Session (Market Data)
    // =========================================================================
    chimera::feed::fix::FixSession fix_quote;
    fix_quote.set_credentials(
        app_cfg.fix_sender.c_str(),
        app_cfg.fix_target.c_str(),
        app_cfg.fix_username.c_str(),
        app_cfg.fix_password.c_str(),
        "QUOTE"
    );

    fix_quote.set_tick_callback([&](const chimera::feed::fix::FixTick& raw) {
        chimera::market::Tick tick;
        uint32_t h = 0;
        for (const char* p = raw.symbol; *p; ++p) h = h * 31 + *p;

        normalizer.normalize_trade(
            h,
            raw.timestamp_ns,
            raw.timestamp_ns,
            (raw.bid + raw.ask) * 0.5,
            raw.bid_size + raw.ask_size,
            true,
            tick
        );
        tick.venue = chimera::market::VENUE_CTRADER;
        g_fix_ticks.fetch_add(1, std::memory_order_relaxed);
    });

    // =========================================================================
    // FIX TRADE Session (Order Execution)
    // =========================================================================
    chimera::feed::fix::FixSession fix_trade;
    fix_trade.set_credentials(
        app_cfg.fix_sender.c_str(),
        app_cfg.fix_target.c_str(),
        app_cfg.fix_username.c_str(),
        app_cfg.fix_password.c_str(),
        "TRADE"
    );

    fix_trade.set_execution_callback([](const char* cl_ord_id, const char* exec_type,
                                        double fill_price, double fill_qty) {
        std::printf("[EXEC] Order %s: type=%s price=%.5f qty=%.4f\n",
                   cl_ord_id, exec_type, fill_price, fill_qty);
    });

    // =========================================================================
    // Connect FIX Sessions
    // =========================================================================
    if (app_cfg.fix_enabled) {
        // Connect QUOTE session
        std::printf("[FIX-QUOTE] Connecting to %s:%u...\n",
                    app_cfg.fix_quote_host.c_str(), app_cfg.fix_quote_port);

        if (fix_quote.connect(app_cfg.fix_quote_host.c_str(), app_cfg.fix_quote_port)) {
            fix_quote.start();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!fix_quote.is_logged_on() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (fix_quote.is_logged_on()) {
                std::printf("[FIX-QUOTE] Logon OK, requesting Security List...\n");
                
                // Request Security List to get symbol IDs
                fix_quote.request_security_list();
                
                // Wait for Security List (up to 5 seconds)
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!fix_quote.has_security_list() &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                if (fix_quote.has_security_list()) {
                    std::printf("[FIX-QUOTE] Security List received (%zu symbols), subscribing to market data...\n",
                               fix_quote.symbol_count());
                    fix_quote.subscribe_market_data(app_cfg.fix_symbols);
                } else {
                    std::printf("[FIX-QUOTE] Security List timeout, trying direct subscription...\n");
                    fix_quote.subscribe_market_data(app_cfg.fix_symbols);
                }
            } else {
                std::fprintf(stderr, "[FIX-QUOTE] Logon timeout\n");
            }
        }

        // Connect TRADE session
        std::printf("[FIX-TRADE] Connecting to %s:%u...\n",
                    app_cfg.fix_trade_host.c_str(), app_cfg.fix_trade_port);

        if (fix_trade.connect(app_cfg.fix_trade_host.c_str(), app_cfg.fix_trade_port)) {
            fix_trade.start();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!fix_trade.is_logged_on() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (fix_trade.is_logged_on()) {
                std::printf("[FIX-TRADE] Logon OK, ready for orders\n");
                
                // Request Security List for TRADE session too (for order symbols)
                fix_trade.request_security_list();
            } else {
                std::fprintf(stderr, "[FIX-TRADE] Logon timeout\n");
            }
        }
    }

    std::printf("[MAIN] Running... Press Ctrl+C to stop\n");
    std::printf("[MAIN] Dashboard: http://VPS_IP:8081/chimera_dashboard_v4.html\n");
    std::printf("[MAIN] Metrics: http://VPS_IP:9001/metrics\n");

    auto start_time = std::chrono::steady_clock::now();
    int stats_counter = 0;
    
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // =====================================================================
        // UPDATE GLOBAL METRICS FOR DASHBOARD
        // =====================================================================
        
        // Connection status
        chimera::g_metrics.binance_connected.store(binance_ws.is_connected() ? 1 : 0);
        chimera::g_metrics.fix_quote_connected.store(fix_quote.is_logged_on() ? 1 : 0);
        chimera::g_metrics.fix_trade_connected.store(fix_trade.is_logged_on() ? 1 : 0);
        
        // Tick counters
        uint64_t bn_ticks = g_binance_ticks.load();
        uint64_t fx_ticks = fix_quote.stats.ticks_received.load();
        chimera::g_metrics.binance_ticks.store(bn_ticks);
        chimera::g_metrics.fix_ticks.store(fx_ticks);
        chimera::g_metrics.fix_messages.store(fix_quote.stats.messages_received.load());
        chimera::g_metrics.fix_heartbeats.store(fix_quote.stats.heartbeats_received.load());
        chimera::g_metrics.fix_errors.store(fix_quote.stats.errors.load());
        
        // Latency
        chimera::g_metrics.fix_quote_latency_us.store(fix_quote.stats.last_latency_us.load());
        chimera::g_metrics.fix_trade_latency_us.store(fix_trade.stats.last_latency_us.load());
        
        // Uptime
        auto now = std::chrono::steady_clock::now();
        chimera::g_metrics.uptime_sec.store(
            std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count());
        
        // Symbol prices from FIX
        chimera::g_metrics.xauusd_bid.store(fix_quote.stats.xauusd_bid.load());
        chimera::g_metrics.xauusd_ask.store(fix_quote.stats.xauusd_ask.load());
        chimera::g_metrics.xagusd_bid.store(fix_quote.stats.xagusd_bid.load());
        chimera::g_metrics.xagusd_ask.store(fix_quote.stats.xagusd_ask.load());
        chimera::g_metrics.eurusd_bid.store(fix_quote.stats.eurusd_bid.load());
        chimera::g_metrics.eurusd_ask.store(fix_quote.stats.eurusd_ask.load());
        chimera::g_metrics.gbpusd_bid.store(fix_quote.stats.gbpusd_bid.load());
        chimera::g_metrics.gbpusd_ask.store(fix_quote.stats.gbpusd_ask.load());
        chimera::g_metrics.usdjpy_bid.store(fix_quote.stats.usdjpy_bid.load());
        chimera::g_metrics.usdjpy_ask.store(fix_quote.stats.usdjpy_ask.load());
        chimera::g_metrics.audusd_bid.store(fix_quote.stats.audusd_bid.load());
        chimera::g_metrics.audusd_ask.store(fix_quote.stats.audusd_ask.load());
        chimera::g_metrics.usdcad_bid.store(fix_quote.stats.usdcad_bid.load());
        chimera::g_metrics.usdcad_ask.store(fix_quote.stats.usdcad_ask.load());
        chimera::g_metrics.nzdusd_bid.store(fix_quote.stats.nzdusd_bid.load());
        chimera::g_metrics.nzdusd_ask.store(fix_quote.stats.nzdusd_ask.load());
        chimera::g_metrics.usdchf_bid.store(fix_quote.stats.usdchf_bid.load());
        chimera::g_metrics.usdchf_ask.store(fix_quote.stats.usdchf_ask.load());
        
        // Indices prices from FIX
        chimera::g_metrics.nas100_bid.store(fix_quote.stats.nas100_bid.load());
        chimera::g_metrics.nas100_ask.store(fix_quote.stats.nas100_ask.load());
        chimera::g_metrics.spx500_bid.store(fix_quote.stats.spx500_bid.load());
        chimera::g_metrics.spx500_ask.store(fix_quote.stats.spx500_ask.load());
        chimera::g_metrics.us30_bid.store(fix_quote.stats.us30_bid.load());
        chimera::g_metrics.us30_ask.store(fix_quote.stats.us30_ask.load());
        chimera::g_metrics.ger30_bid.store(fix_quote.stats.ger30_bid.load());
        chimera::g_metrics.ger30_ask.store(fix_quote.stats.ger30_ask.load());
        chimera::g_metrics.uk100_bid.store(fix_quote.stats.uk100_bid.load());
        chimera::g_metrics.uk100_ask.store(fix_quote.stats.uk100_ask.load());
        chimera::g_metrics.jpn225_bid.store(fix_quote.stats.jpn225_bid.load());
        chimera::g_metrics.jpn225_ask.store(fix_quote.stats.jpn225_ask.load());
        
        // Console output
        uint64_t fx_msgs = fix_quote.stats.messages_received.load();
        uint64_t fx_hb = fix_quote.stats.heartbeats_received.load();
        
        double xau_bid = fix_quote.stats.xauusd_bid.load();
        double xau_ask = fix_quote.stats.xauusd_ask.load();
        double eur_bid = fix_quote.stats.eurusd_bid.load();
        double eur_ask = fix_quote.stats.eurusd_ask.load();
        
        std::printf(
            "[STATS] bn=%llu fx=%llu | ws=%s quote=%s trade=%s | msgs=%llu hb=%llu\n",
            (unsigned long long)bn_ticks,
            (unsigned long long)fx_ticks,
            binance_ws.is_connected() ? "OK" : "DOWN",
            fix_quote.is_logged_on() ? "OK" : "DOWN",
            fix_trade.is_logged_on() ? "OK" : "DOWN",
            (unsigned long long)fx_msgs,
            (unsigned long long)fx_hb);
        
        // Every 5 seconds, print prices if available
        stats_counter++;
        if (stats_counter % 5 == 0 && (xau_bid > 0 || eur_bid > 0)) {
            std::printf("[PRICES] XAUUSD: %.2f/%.2f | EURUSD: %.5f/%.5f\n",
                       xau_bid, xau_ask, eur_bid, eur_ask);
        }
    }

    fix_quote.disconnect();
    fix_trade.disconnect();
    binance_ws.stop();
    metrics_server.stop();

    std::printf("[MAIN] Shutdown complete\n");
    return 0;
}
