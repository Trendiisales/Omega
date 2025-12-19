// =============================================================================
// main.cpp - OMEGA HFT Dual Engine Entry Point (v6.1 INSTITUTIONAL)
// =============================================================================
// ARCHITECTURE:
//   CryptoEngine (CPU 1) ←→ Binance WebSocket
//   CfdEngine (CPU 2)    ←→ cTrader FIX 4.4
//   Arbiter              ←→ Venue routing (latency-aware, stateful, backpressure)
//
// SHARED STATE (allowed):
//   - atomic<bool> g_kill_all (global kill switch)
//   - atomic<bool> g_running (shutdown signal)
//   - atomic counters for stats
//   - VenueHealth (atomic) for Arbiter reads
//   - ExecutionPressure (atomic) for Arbiter backpressure
//
// NOT SHARED:
//   - Ticks (each engine has its own)
//   - Order books (each engine has its own)
//   - Strategy state (each engine has its own)
//   - Positions (each engine has its own)
//   - Risk limits (each engine has its own)
//
// DATA FLOW:
//   Binance WS → BinanceUnifiedFeed → TickFull → CryptoEngine::processTick()
//   cTrader FIX → CTraderFIXClient → TickFull → CfdEngine::processTick()
//   Strategy → Intent → Arbiter → Venue → Wire
//   NO CROSSOVER. EVER.
//
// v6.1 UPGRADES:
//   - Arbiter statefulness (anti-flap, MIN_HOLD_NS)
//   - Execution backpressure (pending_orders, recent_rejects)
//   - Intent expiry (MAX_INTENT_AGE_NS)
//   - Tail latency tracking (latency_max_window)
// =============================================================================

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <sstream>

#include "engine/CryptoEngine.hpp"
#include "engine/CfdEngine.hpp"
#include "config/ConfigLoader.hpp"
#include "server/ChimeraWSServer.hpp"

// API Lock - Compile-time interface verification
#include "api/CHIMERA_API_ASSERTS.hpp"
#include "server/ChimeraHttpServer.hpp"
#include "risk/KillSwitch.hpp"
#include "arbiter/Arbiter.hpp"

// =============================================================================
// GLOBAL STATE (minimal, only what MUST be shared)
// =============================================================================
static std::atomic<bool> g_running{true};
static Chimera::GlobalKillSwitch g_kill_switch;

void signalHandler(int signum) {
    std::cout << "\n[OMEGA] Signal " << signum << " received - initiating shutdown\n";
    g_running = false;
}

void openBrowser(const std::string& url) {
#ifdef _WIN32
    std::string cmd = "start " + url;
    system(cmd.c_str());
#elif __APPLE__
    std::string cmd = "open " + url;
    system(cmd.c_str());
#else
    std::string cmd = "xdg-open " + url + " &";
    system(cmd.c_str());
#endif
}

// =============================================================================
// MAIN ENTRY POINT
// =============================================================================
int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    std::string configPath = "config.ini";
    if (argc > 1) configPath = argv[1];

    // =========================================================================
    // BANNER
    // =========================================================================
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║      OMEGA HFT ENGINE v6.1 - INSTITUTIONAL GRADE ARBITER         ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Engine 1: CryptoEngine (Binance WebSocket)  → CPU 1             ║\n";
    std::cout << "║  Engine 2: CfdEngine (cTrader FIX 4.4)       → CPU 2             ║\n";
    std::cout << "║  Arbiter:  Stateful + Backpressure-Aware Routing                 ║\n";
    std::cout << "║                                                                  ║\n";
    std::cout << "║  UPGRADES:                                                       ║\n";
    std::cout << "║    ✓ Anti-flap (10ms venue hold)                                 ║\n";
    std::cout << "║    ✓ Backpressure (pending orders, reject rate)                  ║\n";
    std::cout << "║    ✓ Intent expiry (50ms freshness)                              ║\n";
    std::cout << "║    ✓ Tail latency tracking (p99 proxy)                           ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";

    // =========================================================================
    // LOAD CONFIG
    // =========================================================================
    std::cout << "[INIT] Loading config: " << configPath << "\n";
    
    Chimera::ConfigLoader cfg;
    if (!cfg.load(configPath)) {
        std::cerr << "[WARN] Failed to load config - using defaults\n";
    }

    // Server settings
    int httpPort = cfg.getInt("server.http_port", 8080);
    int wsPort = cfg.getInt("server.ws_port", 8765);
    std::string webRoot = cfg.getString("server.web_root", "www");
    bool openGui = cfg.getBool("server.open_browser", false);
    
    // Binance settings
    bool binanceEnabled = cfg.getBool("binance.enabled", true);
    std::string binanceSymbols = cfg.getString("binance.symbols", "BTCUSDT,ETHUSDT,SOLUSDT");
    
    // cTrader settings
    bool ctraderEnabled = cfg.getBool("ctrader.enabled", true);
    std::string ctraderHost = cfg.getString("ctrader.host", "demo-uk-eqx-02.p.c-trader.com");
    int ctraderPort = cfg.getInt("ctrader.port_ssl", 5212);
    std::string ctraderSender = cfg.getString("ctrader.sender_comp_id", "demo.blackbull.2067070");
    std::string ctraderTarget = cfg.getString("ctrader.target_comp_id", "cServer");
    std::string ctraderPassword = cfg.getString("ctrader.password", "TQ$2UbnHJcwVm7@");

    // =========================================================================
    // START HTTP SERVER (for GUI)
    // =========================================================================
    Chimera::ChimeraHttpServer httpServer;
    std::cout << "[INIT] Starting HTTP server on port " << httpPort << "...\n";
    if (!httpServer.start(httpPort, webRoot)) {
        std::cerr << "[WARN] Failed to start HTTP server\n";
    }

    // =========================================================================
    // START WEBSOCKET SERVER (for real-time data)
    // =========================================================================
    Chimera::ChimeraWSServer wsServer;
    std::cout << "[INIT] Starting WebSocket server on port " << wsPort << "...\n";
    if (!wsServer.start(wsPort)) {
        std::cerr << "[WARN] Failed to start WebSocket server\n";
    }

    // =========================================================================
    // CREATE CRYPTO ENGINE (Binance)
    // =========================================================================
    Chimera::CryptoEngine cryptoEngine;
    
    if (binanceEnabled) {
        std::cout << "[INIT] Configuring CryptoEngine (Binance)...\n";
        
        // Parse symbols
        std::vector<std::string> symbols;
        std::stringstream ss(binanceSymbols);
        std::string sym;
        while (std::getline(ss, sym, ',')) {
            symbols.push_back(sym);
        }
        
        cryptoEngine.setSymbols(symbols);
        cryptoEngine.setKillSwitch(&g_kill_switch);
        
        // Order callback (for execution tracking)
        cryptoEngine.setOrderCallback([&wsServer](const char* symbol, int8_t side, double qty) {
            std::ostringstream json;
            json << "{\"type\":\"order\",\"engine\":\"crypto\",\"symbol\":\"" << symbol 
                 << "\",\"side\":" << (int)side << ",\"qty\":" << qty << "}";
            wsServer.broadcast(json.str());
        });
    }

    // =========================================================================
    // CREATE CFD ENGINE (cTrader FIX)
    // =========================================================================
    Chimera::CfdEngine cfdEngine;
    
    if (ctraderEnabled) {
        std::cout << "[INIT] Configuring CfdEngine (cTrader FIX)...\n";
        
        Chimera::FIXConfig fixCfg;
        fixCfg.host = ctraderHost;
        fixCfg.port = ctraderPort;
        fixCfg.senderCompID = ctraderSender;
        fixCfg.targetCompID = ctraderTarget;
        fixCfg.password = ctraderPassword;
        fixCfg.username = ctraderSender.substr(ctraderSender.rfind('.') + 1);  // Extract account number
        
        cfdEngine.setFIXConfig(fixCfg);
        cfdEngine.setKillSwitch(&g_kill_switch);
        
        // Order callback
        cfdEngine.setOrderCallback([&wsServer](const char* symbol, int8_t side, double qty) {
            std::ostringstream json;
            json << "{\"type\":\"order\",\"engine\":\"cfd\",\"symbol\":\"" << symbol 
                 << "\",\"side\":" << (int)side << ",\"qty\":" << qty << "}";
            wsServer.broadcast(json.str());
        });
    }

    // =========================================================================
    // CREATE ARBITER (Venue Routing with Unified VenueHealth)
    // =========================================================================
    std::cout << "[INIT] Creating Arbiter (v6.4 unified VenueHealth)...\n";
    
    // Get VenueHealth references from engines
    Chimera::VenueHealth& binanceHealth = cryptoEngine.getVenueHealthMutable();
    Chimera::VenueHealth& ctraderHealth = cfdEngine.getVenueHealthMutable();
    
    // Create Arbiter with references to venue health and global kill switch
    // v6.4: No separate ExecutionPressure - backpressure is in VenueHealth
    Chimera::Arbiter arbiter(binanceHealth, ctraderHealth, g_kill_switch.kill_all);
    
    // Wire Arbiter to engines
    cryptoEngine.setArbiter(&arbiter);
    cfdEngine.setArbiter(&arbiter);
    
    std::cout << "[INIT] Arbiter configured:\n";
    std::cout << "[INIT]   Max latency EWMA: " << Chimera::Arbiter::MAX_LATENCY_NS / 1000000.0 << " ms\n";
    std::cout << "[INIT]   Max tail latency: " << Chimera::Arbiter::MAX_TAIL_NS / 1000000.0 << " ms\n";
    std::cout << "[INIT]   Min venue hold: " << Chimera::Arbiter::MIN_HOLD_NS / 1000000.0 << " ms\n";
    std::cout << "[INIT]   Max intent age: " << Chimera::Arbiter::MAX_INTENT_AGE_NS / 1000000.0 << " ms\n";
    std::cout << "[INIT]   Min confidence: " << Chimera::Arbiter::MIN_CONFIDENCE << "\n";
    std::cout << "[INIT]   Max pending orders: " << Chimera::VenueHealth::MAX_PENDING_ORDERS << "\n";
    std::cout << "[INIT]   Max recent rejects: " << Chimera::VenueHealth::MAX_RECENT_REJECTS << "\n";

    // =========================================================================
    // START ENGINES
    // =========================================================================
    std::cout << "\n[START] ═══════════════════════════════════════════════════════\n";
    
    if (binanceEnabled) {
        std::cout << "[START] Starting CryptoEngine (CPU 1)...\n";
        if (!cryptoEngine.start()) {
            std::cerr << "[ERROR] Failed to start CryptoEngine\n";
        }
    }
    
    if (ctraderEnabled) {
        std::cout << "[START] Starting CfdEngine (CPU 2)...\n";
        if (!cfdEngine.start()) {
            std::cerr << "[ERROR] Failed to start CfdEngine\n";
        }
    }

    std::cout << "[START] ═══════════════════════════════════════════════════════\n\n";

    // Open browser
    if (openGui) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        openBrowser("http://localhost:" + std::to_string(httpPort));
    }

    std::cout << "[OMEGA] ═══════════════════════════════════════════════════════\n";
    std::cout << "[OMEGA] DUAL ENGINE + INSTITUTIONAL ARBITER RUNNING\n";
    std::cout << "[OMEGA]   CryptoEngine: " << (binanceEnabled ? "ACTIVE" : "DISABLED") << "\n";
    std::cout << "[OMEGA]   CfdEngine:    " << (ctraderEnabled ? "ACTIVE" : "DISABLED") << "\n";
    std::cout << "[OMEGA]   Arbiter:      ACTIVE (stateful + backpressure)\n";
    std::cout << "[OMEGA]   GUI: http://localhost:" << httpPort << "\n";
    std::cout << "[OMEGA]   Press CTRL+C to exit\n";
    std::cout << "[OMEGA] ═══════════════════════════════════════════════════════\n\n";

    // =========================================================================
    // MAIN LOOP - Stats broadcast only, NO tick processing here
    // =========================================================================
    uint64_t loopCount = 0;
    auto startTime = std::chrono::steady_clock::now();
    
    while (g_running) {
        loopCount++;
        
        // Build combined stats JSON
        const auto& cryptoStats = cryptoEngine.getStats();
        const auto& cfdStats = cfdEngine.getStats();
        
        // Get arbiter state
        const char* lastVenueStr = "None";
        auto lastVenue = arbiter.getLastVenue();
        if (lastVenue == Chimera::ArbiterVenue::Binance) lastVenueStr = "Binance";
        else if (lastVenue == Chimera::ArbiterVenue::CTrader) lastVenueStr = "CTrader";
        
        std::ostringstream json;
        json << "{";
        json << "\"type\":\"stats\",";
        json << "\"uptime\":" << std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count() << ",";
        json << "\"kill_all\":" << (g_kill_switch.kill_all.load() ? "true" : "false") << ",";
        
        // Arbiter state
        json << "\"arbiter\":{";
        json << "\"last_venue\":\"" << lastVenueStr << "\",";
        json << "\"binance_pending\":" << arbiter.getBinancePending() << ",";
        json << "\"binance_rejects\":" << arbiter.getBinanceRejects() << ",";
        json << "\"ctrader_pending\":" << arbiter.getCTraderPending() << ",";
        json << "\"ctrader_rejects\":" << arbiter.getCTraderRejects();
        json << "},";
        
        // Crypto stats
        json << "\"crypto\":{";
        json << "\"ticks\":" << cryptoStats.ticks_processed.load() << ",";
        json << "\"signals\":" << cryptoStats.signals_generated.load() << ",";
        json << "\"orders\":" << cryptoStats.orders_sent.load() << ",";
        json << "\"arbiter_rejects\":" << cryptoStats.arbiter_rejections.load() << ",";
        json << "\"backpressure_rejects\":" << cryptoStats.backpressure_rejections.load() << ",";
        json << "\"latency_us\":" << cryptoStats.avgLatencyUs() << ",";
        json << "\"venue_latency_ns\":" << arbiter.getBinanceLatencyNs() << ",";
        json << "\"venue_tail_ns\":" << arbiter.getBinanceTailNs() << ",";
        json << "\"venue_state\":\"" << arbiter.getBinanceStateStr() << "\",";
        json << "\"venue_alive\":" << (arbiter.isBinanceAlive() ? "true" : "false");
        json << "},";
        
        // CFD stats
        json << "\"cfd\":{";
        json << "\"ticks\":" << cfdStats.ticks_processed.load() << ",";
        json << "\"signals\":" << cfdStats.signals_generated.load() << ",";
        json << "\"orders\":" << cfdStats.orders_sent.load() << ",";
        json << "\"arbiter_rejects\":" << cfdStats.arbiter_rejections.load() << ",";
        json << "\"backpressure_rejects\":" << cfdStats.backpressure_rejections.load() << ",";
        json << "\"latency_us\":" << cfdStats.avgLatencyUs() << ",";
        json << "\"venue_latency_ns\":" << arbiter.getCTraderLatencyNs() << ",";
        json << "\"venue_tail_ns\":" << arbiter.getCTraderTailNs() << ",";
        json << "\"venue_state\":\"" << arbiter.getCTraderStateStr() << "\",";
        json << "\"venue_alive\":" << (arbiter.isCTraderAlive() ? "true" : "false") << ",";
        json << "\"fix_msgs\":" << cfdStats.fix_messages.load() << ",";
        json << "\"connected\":" << (cfdEngine.isConnected() ? "true" : "false");
        json << "}";
        
        json << "}";
        
        wsServer.broadcast(json.str());
        
        // Print stats every 5 seconds
        if (loopCount % 50 == 0) {
            std::cout << "[STATS] Crypto: " << cryptoStats.ticks_processed.load() 
                      << " ticks, " << cryptoStats.avgLatencyUs() << "us avg"
                      << ", arb_rej=" << cryptoStats.arbiter_rejections.load()
                      << ", bp_rej=" << cryptoStats.backpressure_rejections.load()
                      << " | CFD: " << cfdStats.ticks_processed.load() 
                      << " ticks, " << cfdStats.avgLatencyUs() << "us avg"
                      << ", arb_rej=" << cfdStats.arbiter_rejections.load()
                      << ", bp_rej=" << cfdStats.backpressure_rejections.load()
                      << " | Arbiter: " << lastVenueStr
                      << " (pend=" << arbiter.getBinancePending() << "/" << arbiter.getCTraderPending() << ")"
                      << " | Kill: " << (g_kill_switch.kill_all.load() ? "YES" : "NO") << std::endl;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // =========================================================================
    // SHUTDOWN
    // =========================================================================
    std::cout << "\n[SHUTDOWN] ═══════════════════════════════════════════════════\n";
    
    std::cout << "[SHUTDOWN] Stopping CryptoEngine...\n";
    cryptoEngine.stop();
    
    std::cout << "[SHUTDOWN] Stopping CfdEngine...\n";
    cfdEngine.stop();
    
    std::cout << "[SHUTDOWN] Stopping servers...\n";
    wsServer.stop();
    httpServer.stop();
    
    // Final stats
    const auto& finalCrypto = cryptoEngine.getStats();
    const auto& finalCfd = cfdEngine.getStats();
    
    std::cout << "\n[FINAL] ═══════════════════════════════════════════════════════\n";
    std::cout << "[FINAL] CryptoEngine:\n";
    std::cout << "[FINAL]   Ticks: " << finalCrypto.ticks_processed.load() << "\n";
    std::cout << "[FINAL]   Signals: " << finalCrypto.signals_generated.load() << "\n";
    std::cout << "[FINAL]   Orders: " << finalCrypto.orders_sent.load() << "\n";
    std::cout << "[FINAL]   Arbiter Rejections: " << finalCrypto.arbiter_rejections.load() << "\n";
    std::cout << "[FINAL]   Backpressure Rejections: " << finalCrypto.backpressure_rejections.load() << "\n";
    std::cout << "[FINAL]   Avg Latency: " << finalCrypto.avgLatencyUs() << " us\n";
    std::cout << "[FINAL] CfdEngine:\n";
    std::cout << "[FINAL]   Ticks: " << finalCfd.ticks_processed.load() << "\n";
    std::cout << "[FINAL]   Signals: " << finalCfd.signals_generated.load() << "\n";
    std::cout << "[FINAL]   Orders: " << finalCfd.orders_sent.load() << "\n";
    std::cout << "[FINAL]   Arbiter Rejections: " << finalCfd.arbiter_rejections.load() << "\n";
    std::cout << "[FINAL]   Backpressure Rejections: " << finalCfd.backpressure_rejections.load() << "\n";
    std::cout << "[FINAL]   Avg Latency: " << finalCfd.avgLatencyUs() << " us\n";
    std::cout << "[FINAL]   FIX Messages: " << finalCfd.fix_messages.load() << "\n";
    std::cout << "[FINAL] ═══════════════════════════════════════════════════════\n";
    
    std::cout << "\n[OMEGA] Shutdown complete.\n";
    return 0;
}
