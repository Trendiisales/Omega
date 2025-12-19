// =============================================================================
// RiskGuardian.hpp - HFT-Optimized Risk Management
// 
// CONCEPT from documents: Multi-dimensional risk monitoring + kill switch
// IMPLEMENTATION using HFT patterns:
// - Atomic state (no mutex in hot path)
// - yield() instead of sleep_for()
// - Lock-free signal updates
// - Cache-line aligned for zero false sharing
// =============================================================================
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <thread>
#include <chrono>
#include "../micro/CentralMicroEngine.hpp"
#include "../data/UnifiedTick.hpp"

namespace Chimera {

// Cache line size for alignment
constexpr size_t CACHE_LINE_SIZE = 64;

// =============================================================================
// Risk Limits Configuration
// =============================================================================
struct alignas(CACHE_LINE_SIZE) RiskLimits {
    double maxDrawdown = -100.0;          // Maximum allowed drawdown ($)
    double maxPosition = 1.0;             // Maximum position size
    double maxVolatility = 0.05;          // Max volatility threshold
    double maxSpreadBps = 50.0;           // Max spread in basis points
    double maxToxicity = 0.6;             // Max order flow toxicity
    double maxVPIN = 0.7;                 // Max VPIN
    double maxLossPerTrade = -10.0;       // Max loss per trade ($)
    double maxDailyLoss = -500.0;         // Max daily loss ($)
    int maxOpenPositions = 5;             // Max simultaneous positions
    int maxOrdersPerSecond = 100;         // Rate limiting
    uint64_t maxLatencyNs = 1000000;      // Max acceptable latency (1ms)
};

// =============================================================================
// Risk State - Atomic for lock-free reads
// =============================================================================
struct alignas(CACHE_LINE_SIZE) RiskState {
    std::atomic<double> currentPnL{0.0};
    std::atomic<double> currentPosition{0.0};
    std::atomic<double> currentDrawdown{0.0};
    std::atomic<double> dailyPnL{0.0};
    std::atomic<int> openPositions{0};
    std::atomic<int> ordersThisSecond{0};
    std::atomic<uint64_t> lastOrderTime{0};
    std::atomic<uint64_t> lastRiskCheck{0};
    
    // Flags
    std::atomic<bool> globalKillSwitch{false};
    std::atomic<bool> volatilityHalt{false};
    std::atomic<bool> drawdownHalt{false};
    std::atomic<bool> toxicityHalt{false};
    std::atomic<bool> latencyHalt{false};
    std::atomic<bool> rateHalt{false};
};

// =============================================================================
// Risk Event Types
// =============================================================================
enum class RiskEvent : uint8_t {
    NONE = 0,
    VOLATILITY_BREACH,
    DRAWDOWN_BREACH,
    TOXICITY_BREACH,
    SPREAD_BREACH,
    LATENCY_BREACH,
    RATE_LIMIT_BREACH,
    POSITION_LIMIT_BREACH,
    DAILY_LOSS_BREACH,
    MANUAL_HALT
};

// =============================================================================
// RiskGuardian - Lock-free HFT Risk Management
// =============================================================================
class RiskGuardian {
public:
    RiskGuardian() : running_(false) {}
    
    ~RiskGuardian() { stop(); }
    
    // ==========================================================================
    // Configuration
    // ==========================================================================
    inline void setLimits(const RiskLimits& limits) {
        limits_ = limits;
    }
    
    // ==========================================================================
    // FAST PATH - Called on every tick, must be ultra-low latency
    // Returns: true if trading allowed, false if blocked
    // ==========================================================================
    inline bool checkTick(const UnifiedTick& t, const MicrostructureSignals& sig) {
        // Fast path - check kill switch first (single atomic read)
        if (state_.globalKillSwitch.load(std::memory_order_relaxed)) {
            return false;
        }
        
        // Check any halt flags (all atomic, no locks)
        if (state_.volatilityHalt.load(std::memory_order_relaxed) ||
            state_.drawdownHalt.load(std::memory_order_relaxed) ||
            state_.toxicityHalt.load(std::memory_order_relaxed) ||
            state_.latencyHalt.load(std::memory_order_relaxed) ||
            state_.rateHalt.load(std::memory_order_relaxed)) {
            return false;
        }
        
        // Quick volatility check (inline, no function call)
        if (sig.realizedVolatility > limits_.maxVolatility) {
            state_.volatilityHalt.store(true, std::memory_order_relaxed);
            lastEvent_ = RiskEvent::VOLATILITY_BREACH;
            return false;
        }
        
        // Quick toxicity check
        if (sig.toxicity > limits_.maxToxicity || sig.vpin > limits_.maxVPIN) {
            state_.toxicityHalt.store(true, std::memory_order_relaxed);
            lastEvent_ = RiskEvent::TOXICITY_BREACH;
            return false;
        }
        
        // Quick spread check
        if (sig.spreadBps > limits_.maxSpreadBps) {
            lastEvent_ = RiskEvent::SPREAD_BREACH;
            return false;  // Soft halt - don't set flag, just skip this tick
        }
        
        return true;
    }
    
    // ==========================================================================
    // Check before sending order (slightly slower path, more thorough)
    // ==========================================================================
    inline bool checkOrder(double orderSize, int8_t side) {
        // Kill switch
        if (state_.globalKillSwitch.load(std::memory_order_relaxed)) {
            return false;
        }
        
        // Position limit
        double currentPos = state_.currentPosition.load(std::memory_order_relaxed);
        double newPos = currentPos + (side * orderSize);
        if (std::fabs(newPos) > limits_.maxPosition) {
            lastEvent_ = RiskEvent::POSITION_LIMIT_BREACH;
            return false;
        }
        
        // Open positions limit
        if (state_.openPositions.load(std::memory_order_relaxed) >= limits_.maxOpenPositions) {
            lastEvent_ = RiskEvent::POSITION_LIMIT_BREACH;
            return false;
        }
        
        // Rate limiting
        uint64_t now = currentTimeNs();
        uint64_t lastOrder = state_.lastOrderTime.load(std::memory_order_relaxed);
        if (now - lastOrder < 1000000000ULL) { // Within 1 second
            int orders = state_.ordersThisSecond.fetch_add(1, std::memory_order_relaxed);
            if (orders >= limits_.maxOrdersPerSecond) {
                state_.rateHalt.store(true, std::memory_order_relaxed);
                lastEvent_ = RiskEvent::RATE_LIMIT_BREACH;
                return false;
            }
        } else {
            state_.ordersThisSecond.store(1, std::memory_order_relaxed);
            state_.lastOrderTime.store(now, std::memory_order_relaxed);
        }
        
        return true;
    }
    
    // ==========================================================================
    // Update after fill
    // ==========================================================================
    inline void onFill(double fillQty, double fillPrice, int8_t side, double pnl) {
        // Update position
        double currentPos = state_.currentPosition.load(std::memory_order_relaxed);
        state_.currentPosition.store(currentPos + (side * fillQty), std::memory_order_relaxed);
        
        // Update PnL
        double currentPnL = state_.currentPnL.load(std::memory_order_relaxed);
        double newPnL = currentPnL + pnl;
        state_.currentPnL.store(newPnL, std::memory_order_relaxed);
        
        double dailyPnL = state_.dailyPnL.load(std::memory_order_relaxed);
        state_.dailyPnL.store(dailyPnL + pnl, std::memory_order_relaxed);
        
        // Check drawdown
        if (newPnL < limits_.maxDrawdown) {
            state_.drawdownHalt.store(true, std::memory_order_relaxed);
            state_.globalKillSwitch.store(true, std::memory_order_relaxed);
            lastEvent_ = RiskEvent::DRAWDOWN_BREACH;
        }
        
        // Check daily loss
        if (state_.dailyPnL.load(std::memory_order_relaxed) < limits_.maxDailyLoss) {
            state_.drawdownHalt.store(true, std::memory_order_relaxed);
            state_.globalKillSwitch.store(true, std::memory_order_relaxed);
            lastEvent_ = RiskEvent::DAILY_LOSS_BREACH;
        }
    }
    
    // ==========================================================================
    // Manual controls
    // ==========================================================================
    inline void emergencyStop() {
        state_.globalKillSwitch.store(true, std::memory_order_relaxed);
        lastEvent_ = RiskEvent::MANUAL_HALT;
    }
    
    inline void resume() {
        // Only resume if no critical breaches
        if (state_.currentPnL.load(std::memory_order_relaxed) > limits_.maxDrawdown &&
            state_.dailyPnL.load(std::memory_order_relaxed) > limits_.maxDailyLoss) {
            state_.globalKillSwitch.store(false, std::memory_order_relaxed);
            state_.volatilityHalt.store(false, std::memory_order_relaxed);
            state_.toxicityHalt.store(false, std::memory_order_relaxed);
            state_.latencyHalt.store(false, std::memory_order_relaxed);
            state_.rateHalt.store(false, std::memory_order_relaxed);
            lastEvent_ = RiskEvent::NONE;
        }
    }
    
    inline void resetDaily() {
        state_.dailyPnL.store(0.0, std::memory_order_relaxed);
        state_.ordersThisSecond.store(0, std::memory_order_relaxed);
    }
    
    // ==========================================================================
    // State queries
    // ==========================================================================
    inline bool isTradingAllowed() const {
        return !state_.globalKillSwitch.load(std::memory_order_relaxed);
    }
    
    inline double getCurrentPnL() const {
        return state_.currentPnL.load(std::memory_order_relaxed);
    }
    
    inline double getCurrentPosition() const {
        return state_.currentPosition.load(std::memory_order_relaxed);
    }
    
    inline RiskEvent getLastEvent() const {
        return lastEvent_;
    }
    
    inline const RiskState& getState() const {
        return state_;
    }
    
    // ==========================================================================
    // Background monitoring thread (for slower checks)
    // ==========================================================================
    void start() {
        if (running_) return;
        running_ = true;
        monitorThread_ = std::thread([this]() {
            while (running_) {
                // Slower risk checks run here
                checkLatency();
                checkRateReset();
                
                // Use yield() instead of sleep_for() for faster response
                for (int i = 0; i < 1000 && running_; ++i) {
                    std::this_thread::yield();
                }
            }
        });
    }
    
    void stop() {
        running_ = false;
        if (monitorThread_.joinable()) {
            monitorThread_.join();
        }
    }

private:
    inline void checkLatency() {
        uint64_t now = currentTimeNs();
        uint64_t lastCheck = state_.lastRiskCheck.load(std::memory_order_relaxed);
        uint64_t latency = now - lastCheck;
        
        if (latency > limits_.maxLatencyNs && lastCheck > 0) {
            state_.latencyHalt.store(true, std::memory_order_relaxed);
            lastEvent_ = RiskEvent::LATENCY_BREACH;
        }
        
        state_.lastRiskCheck.store(now, std::memory_order_relaxed);
    }
    
    inline void checkRateReset() {
        // Reset rate limit every second
        uint64_t now = currentTimeNs();
        uint64_t lastOrder = state_.lastOrderTime.load(std::memory_order_relaxed);
        if (now - lastOrder > 1000000000ULL) {
            state_.ordersThisSecond.store(0, std::memory_order_relaxed);
            state_.rateHalt.store(false, std::memory_order_relaxed);
        }
    }
    
    inline uint64_t currentTimeNs() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

private:
    RiskLimits limits_;
    RiskState state_;
    RiskEvent lastEvent_ = RiskEvent::NONE;
    
    std::atomic<bool> running_;
    std::thread monitorThread_;
};

} // namespace Chimera
