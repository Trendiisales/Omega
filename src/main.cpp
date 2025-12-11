#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#include "engine/OmegaEngine.hpp"
#include "config/ConfigLoader.hpp"
#include "data/UnifiedTick.hpp"
#include "server/OmegaWSServer.hpp"
#include "server/OmegaHttpServer.hpp"

static std::atomic<bool> g_running{true};

void signalHandler(int signum) {
    std::cout << "\n[OMEGA] Received signal " << signum << ", shutting down...\n";
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

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    std::string configPath = "config.ini";
    if (argc > 1) configPath = argv[1];

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║              OMEGA HFT ENGINE v1.0                       ║\n";
    std::cout << "║          High-Frequency Trading System                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";

    std::cout << "[OMEGA] Loading config: " << configPath << "\n";
    
    Omega::ConfigLoader cfg;
    if (!cfg.load(configPath)) {
        std::cerr << "[ERROR] Failed to load config: " << configPath << "\n";
        std::cerr << "[INFO] Using defaults...\n";
    }

    // Get server settings
    int httpPort = cfg.getInt("server.http_port", 8080);
    int wsPort = cfg.getInt("server.ws_port", 8765);
    std::string webRoot = cfg.getString("server.web_root", "www");
    bool openGui = cfg.getBool("server.open_browser", true);

    // Start HTTP server for GUI
    Omega::OmegaHttpServer httpServer;
    std::cout << "[OMEGA] Starting HTTP server on port " << httpPort << "...\n";
    if (!httpServer.start(httpPort, webRoot)) {
        std::cerr << "[WARN] Failed to start HTTP server. GUI will not be available.\n";
        std::cerr << "[INFO] Make sure 'www' folder exists with GUI files.\n";
    } else {
        std::cout << "[OMEGA] GUI available at: http://localhost:" << httpPort << "\n";
    }

    // Start WebSocket server for real-time data
    Omega::OmegaWSServer wsServer;
    std::cout << "[OMEGA] Starting WebSocket server on port " << wsPort << "...\n";
    if (!wsServer.start(wsPort)) {
        std::cerr << "[WARN] Failed to start WebSocket server.\n";
    } else {
        std::cout << "[OMEGA] WebSocket server ready on port " << wsPort << "\n";
    }

    // Initialize trading engine
    Omega::OmegaEngine engine;
    engine.setLogPath(cfg.getString("engine.log_path", "omega.log"));
    engine.setMode(cfg.getString("engine.mode", "sim"));
    engine.setSymbol(cfg.getString("engine.symbol", "BTCUSDT"));

    std::cout << "[OMEGA] Initializing trading engine...\n";
    engine.init();
    
    std::cout << "[OMEGA] Starting trading engine...\n";
    engine.start();

    // Open browser automatically
    if (openGui) {
        std::cout << "[OMEGA] Opening GUI in browser...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        openBrowser("http://localhost:" + std::to_string(httpPort));
    }

    std::cout << "\n[OMEGA] ═══════════════════════════════════════════════════\n";
    std::cout << "[OMEGA] Engine running. Press CTRL+C to exit.\n";
    std::cout << "[OMEGA] GUI: http://localhost:" << httpPort << "\n";
    std::cout << "[OMEGA] ═══════════════════════════════════════════════════\n\n";

    // Main loop - broadcast engine state to GUI
    while (g_running) {
        // Build state JSON for GUI
        std::ostringstream json;
        json << "{";
        json << "\"type\":\"state\",";
        json << "\"running\":" << (g_running ? "true" : "false") << ",";
        json << "\"clients\":" << wsServer.clientCount() << ",";
        json << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json << "}";
        
        wsServer.broadcast(json.str());
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[OMEGA] Shutting down...\n";
    
    engine.stop();
    wsServer.stop();
    httpServer.stop();
    
    std::cout << "[OMEGA] Shutdown complete.\n";
    return 0;
}
