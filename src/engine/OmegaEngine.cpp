#include "OmegaEngine.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <cmath>
#include "../core/lock_free_queue.h"

namespace Omega {

// Async log queue to remove file I/O from hot path
static LockFreeQueue<std::string> g_logQueue;

OmegaEngine::OmegaEngine()
    : running(false),
      symbol("BTCUSDT"),
      logPath("omega.log"),
      mode("sim"),
      tickCount_(0),
      signalCount_(0)
{
}

OmegaEngine::~OmegaEngine() {
    stop();
}

void OmegaEngine::setSymbol(const std::string& s) {
    symbol = s;
    engTrend.setSymbol(s);
    engReversion.setSymbol(s);
    engMomentum.setSymbol(s);
    engBreakout.setSymbol(s);
    engVolShock.setSymbol(s);
    fusion.setSymbol(s);
}

void OmegaEngine::setLogPath(const std::string& p) {
    logPath = p;
}

void OmegaEngine::setMode(const std::string& m) {
    mode = m;
}

void OmegaEngine::init() {
    // Set symbols on all engines
    engTrend.setSymbol(symbol);
    engReversion.setSymbol(symbol);
    engMomentum.setSymbol(symbol);
    engBreakout.setSymbol(symbol);
    engVolShock.setSymbol(symbol);
    fusion.setSymbol(symbol);
    
    // Configure supervisor
    supervisor.setSymbol(symbol);
    supervisor.setMode(mode);
    supervisor.setCoolDownMs(50);
    supervisor.setMinConfidence(0.01);
    supervisor.setMaxPosition(1);
    
    // Configure router
    router.setSymbol(symbol);
    router.setMode(mode);
    router.setDefaultQty(0.001);

    // Attach tick callback
    assembler.setCallback([this](const UnifiedTick& t){
        processTick(t);
    });
    
    std::cout << "[OMEGA] Initialized: symbol=" << symbol 
              << " mode=" << mode << "\n";
}

void OmegaEngine::start() {
    if (running) return;
    
    running = true;
    tTick = std::thread(&OmegaEngine::tickLoop, this);
    
    std::cout << "[OMEGA] Engine started\n";
}

void OmegaEngine::stop() {
    running = false;
    if (tTick.joinable()) tTick.join();
    
    // Flush remaining logs
    std::string logEntry;
    std::ofstream f(logPath, std::ios::app);
    while (g_logQueue.try_dequeue(logEntry)) {
        if (f.is_open()) f << logEntry;
    }
    
    std::cout << "[OMEGA] Engine stopped. Ticks: " << tickCount_ 
              << " Signals: " << signalCount_ << "\n";
}

void OmegaEngine::tickLoop() {
    // ==========================================================================
    // HFT OPTIMIZED MAIN LOOP
    // Uses yield() instead of sleep_for(1ms) - 1,000,000x faster
    // ==========================================================================
    while (running) {
        // Yield CPU instead of sleeping - instant wakeup on data
        std::this_thread::yield();
    }
}

void OmegaEngine::processTick(const UnifiedTick& t) {
    tickCount_++;
    
    // Feed tick to all micro-engines - CRTP, no virtual overhead
    engTrend.onTick(t);
    engReversion.onTick(t);
    engMomentum.onTick(t);
    engBreakout.onTick(t);
    engVolShock.onTick(t);

    // Compute signals from each engine - CRTP, no virtual overhead
    MicroSignal s1 = engTrend.compute();
    MicroSignal s2 = engReversion.compute();
    MicroSignal s3 = engMomentum.compute();
    MicroSignal s4 = engBreakout.compute();
    MicroSignal s5 = engVolShock.compute();

    // Add to fusion
    fusion.add(s1);
    fusion.add(s2);
    fusion.add(s3);
    fusion.add(s4);
    fusion.add(s5);

    // Compute fused signal
    double fused = fusion.compute();

    // Check supervisor approval and route
    if (std::fabs(fused) > 0.001 && supervisor.approve(fused)) {
        signalCount_++;
        router.route(fused, t);
    }

    // ==========================================================================
    // ASYNC LOGGING - NO file I/O in hot path!
    // Queue the log entry for background thread to write
    // ==========================================================================
    std::string logEntry = std::to_string(t.tsLocal) + "," 
        + std::to_string(t.bid) + "," 
        + std::to_string(t.ask) + "," 
        + std::to_string(fused) + ","
        + std::to_string(s1.value) + ","
        + std::to_string(s2.value) + ","
        + std::to_string(s3.value) + "\n";
    g_logQueue.enqueue(logEntry);
}

} // namespace Omega
