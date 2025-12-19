// =============================================================================
// SmartExecutionEngine.hpp - HFT-Optimized Execution Algorithms
// 
// CONCEPT from documents: TWAP/VWAP/Iceberg execution algorithms
// IMPLEMENTATION using HFT patterns:
// - Lock-free order queue
// - No mutex in hot path
// - Atomic state management
// - yield() instead of sleep_for()
// =============================================================================
#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <array>
#include <cstdint>
#include "../core/lock_free_queue.h"
#include "../data/UnifiedTick.hpp"
#include "../micro/CentralMicroEngine.hpp"

namespace Chimera {

// =============================================================================
// Execution Algorithm Types
// =============================================================================
enum class ExecAlgo : uint8_t {
    MARKET = 0,      // Immediate market order (original behavior)
    TWAP,            // Time-weighted average price
    VWAP,            // Volume-weighted average price  
    ICEBERG,         // Hidden size orders
    LIQUIDITY_SEEK,  // Aggressive when liquidity available
    SNIPER           // Wait for optimal entry
};

// =============================================================================
// Parent Order - Tracks execution of a large order
// =============================================================================
struct ParentOrder {
    uint64_t id = 0;
    char symbol[16] = {0};
    int8_t side = 0;          // 1 = buy, -1 = sell
    double totalQty = 0.0;
    double filledQty = 0.0;
    double avgFillPrice = 0.0;
    ExecAlgo algo = ExecAlgo::MARKET;
    
    // Algorithm state
    uint64_t startTime = 0;
    uint64_t endTime = 0;
    uint64_t lastChildTime = 0;
    int childOrdersSent = 0;
    int childOrdersFilled = 0;
    double childSize = 0.0;
    
    // Execution parameters
    int numSlices = 10;         // Number of child orders
    uint64_t sliceIntervalNs = 100000000;  // 100ms between slices
    double maxSlippage = 0.001; // 10 bps max slippage
    double priceLimit = 0.0;    // Limit price (0 = no limit)
    
    bool isActive() const { return filledQty < totalQty; }
    double remainingQty() const { return totalQty - filledQty; }
    double fillRate() const { return totalQty > 0 ? filledQty / totalQty : 0.0; }
};

// =============================================================================
// Child Order - Sent to exchange
// =============================================================================
struct ChildOrder {
    uint64_t id = 0;
    uint64_t parentId = 0;
    char symbol[16] = {0};
    int8_t side = 0;
    double qty = 0.0;
    double price = 0.0;
    uint64_t sentTime = 0;
    bool isLimit = false;
};

// =============================================================================
// Smart Execution Engine - Lock-free, HFT-optimized
// =============================================================================
class SmartExecutionEngine {
    static constexpr size_t MAX_PARENT_ORDERS = 64;
    static constexpr size_t MAX_CHILD_ORDERS = 256;
    
public:
    SmartExecutionEngine(const CentralMicroEngine& microEngine)
        : microEngine_(microEngine), running_(false), nextOrderId_(1) {}
    
    ~SmartExecutionEngine() { stop(); }
    
    // ==========================================================================
    // Submit a new parent order for smart execution
    // ==========================================================================
    inline uint64_t submitOrder(const std::string& symbol, int8_t side, 
                                double qty, ExecAlgo algo, double priceLimit = 0.0) {
        ParentOrder order;
        order.id = nextOrderId_.fetch_add(1);
        std::strncpy(order.symbol, symbol.c_str(), 15);
        order.side = side;
        order.totalQty = qty;
        order.algo = algo;
        order.priceLimit = priceLimit;
        order.startTime = now();
        
        // Configure algorithm
        switch (algo) {
            case ExecAlgo::MARKET:
                order.numSlices = 1;
                order.sliceIntervalNs = 0;
                break;
            case ExecAlgo::TWAP:
                order.numSlices = 20;
                order.sliceIntervalNs = 500000000; // 500ms
                break;
            case ExecAlgo::VWAP:
                order.numSlices = 30;
                order.sliceIntervalNs = 0; // Volume-triggered
                break;
            case ExecAlgo::ICEBERG:
                order.numSlices = 50;
                order.sliceIntervalNs = 100000000; // 100ms
                order.childSize = qty / 50.0; // Show only 2% at a time
                break;
            case ExecAlgo::LIQUIDITY_SEEK:
                order.numSlices = 10;
                order.sliceIntervalNs = 0; // Depth-triggered
                break;
            case ExecAlgo::SNIPER:
                order.numSlices = 1;
                order.sliceIntervalNs = 0; // Signal-triggered
                break;
        }
        
        if (order.childSize == 0.0) {
            order.childSize = qty / order.numSlices;
        }
        
        parentOrderQueue_.enqueue(order);
        return order.id;
    }
    
    // ==========================================================================
    // Process tick - called from main engine loop
    // ==========================================================================
    inline void onTick(const UnifiedTick& t) {
        // Process any new parent orders
        ParentOrder newOrder;
        while (parentOrderQueue_.try_dequeue(newOrder)) {
            // Find empty slot
            for (size_t i = 0; i < MAX_PARENT_ORDERS; ++i) {
                if (!activeOrders_[i].isActive()) {
                    activeOrders_[i] = newOrder;
                    break;
                }
            }
        }
        
        // Execute algorithms for all active orders
        for (size_t i = 0; i < MAX_PARENT_ORDERS; ++i) {
            if (activeOrders_[i].isActive()) {
                executeAlgorithm(activeOrders_[i], t);
            }
        }
    }
    
    // ==========================================================================
    // Handle fill acknowledgment
    // ==========================================================================
    inline void onFill(uint64_t parentId, double fillQty, double fillPrice) {
        for (size_t i = 0; i < MAX_PARENT_ORDERS; ++i) {
            if (activeOrders_[i].id == parentId) {
                double prevFilled = activeOrders_[i].filledQty;
                activeOrders_[i].filledQty += fillQty;
                activeOrders_[i].childOrdersFilled++;
                
                // Update average fill price
                if (activeOrders_[i].filledQty > 0) {
                    activeOrders_[i].avgFillPrice = 
                        (activeOrders_[i].avgFillPrice * prevFilled + fillPrice * fillQty) 
                        / activeOrders_[i].filledQty;
                }
                break;
            }
        }
    }
    
    // ==========================================================================
    // Get pending child orders (for order router to send)
    // ==========================================================================
    inline bool getNextChildOrder(ChildOrder& out) {
        return childOrderQueue_.try_dequeue(out);
    }
    
    // ==========================================================================
    // Statistics
    // ==========================================================================
    inline size_t activeOrderCount() const {
        size_t count = 0;
        for (size_t i = 0; i < MAX_PARENT_ORDERS; ++i) {
            if (activeOrders_[i].isActive()) ++count;
        }
        return count;
    }
    
    void start() { running_ = true; }
    void stop() { running_ = false; }

private:
    // ==========================================================================
    // Execute algorithm for a parent order
    // ==========================================================================
    inline void executeAlgorithm(ParentOrder& order, const UnifiedTick& t) {
        switch (order.algo) {
            case ExecAlgo::MARKET:
                executeMARKET(order, t);
                break;
            case ExecAlgo::TWAP:
                executeTWAP(order, t);
                break;
            case ExecAlgo::VWAP:
                executeVWAP(order, t);
                break;
            case ExecAlgo::ICEBERG:
                executeICEBERG(order, t);
                break;
            case ExecAlgo::LIQUIDITY_SEEK:
                executeLIQUIDITY(order, t);
                break;
            case ExecAlgo::SNIPER:
                executeSNIPER(order, t);
                break;
        }
    }
    
    // ==========================================================================
    // MARKET - Send everything immediately
    // ==========================================================================
    inline void executeMARKET(ParentOrder& order, const UnifiedTick& t) {
        if (order.childOrdersSent == 0) {
            sendChildOrder(order, order.remainingQty(), 0.0, false, t);
        }
    }
    
    // ==========================================================================
    // TWAP - Time-weighted slices
    // ==========================================================================
    inline void executeTWAP(ParentOrder& order, const UnifiedTick& t) {
        uint64_t currentTime = now();
        
        if (order.childOrdersSent < order.numSlices &&
            currentTime - order.lastChildTime >= order.sliceIntervalNs) {
            
            double sliceQty = std::min(order.childSize, order.remainingQty());
            if (sliceQty > 0) {
                sendChildOrder(order, sliceQty, 0.0, false, t);
                order.lastChildTime = currentTime;
            }
        }
    }
    
    // ==========================================================================
    // VWAP - Volume-weighted slices (send when volume picks up)
    // ==========================================================================
    inline void executeVWAP(ParentOrder& order, const UnifiedTick& t) {
        const auto& signals = microEngine_.getSignals();
        
        // Send when trade intensity is above average
        if (signals.tradeIntensity > 10.0 && order.remainingQty() > 0) {
            // Participate proportionally to volume
            double participation = 0.1; // 10% of volume
            double volumeSlice = (t.buyVol + t.sellVol) * participation;
            double sliceQty = std::min(volumeSlice, order.remainingQty());
            sliceQty = std::min(sliceQty, order.childSize * 2); // Cap at 2x normal
            
            if (sliceQty > 0 && now() - order.lastChildTime > 50000000) { // 50ms min
                sendChildOrder(order, sliceQty, 0.0, false, t);
                order.lastChildTime = now();
            }
        }
    }
    
    // ==========================================================================
    // ICEBERG - Small visible orders
    // ==========================================================================
    inline void executeICEBERG(ParentOrder& order, const UnifiedTick& t) {
        uint64_t currentTime = now();
        
        // Only send next slice when previous is filled
        if (order.childOrdersSent == order.childOrdersFilled &&
            order.remainingQty() > 0 &&
            currentTime - order.lastChildTime >= order.sliceIntervalNs) {
            
            double sliceQty = std::min(order.childSize, order.remainingQty());
            // Use limit order just inside the spread
            double limitPrice = (order.side > 0) ? t.bid + t.spread * 0.1 : t.ask - t.spread * 0.1;
            
            sendChildOrder(order, sliceQty, limitPrice, true, t);
            order.lastChildTime = currentTime;
        }
    }
    
    // ==========================================================================
    // LIQUIDITY_SEEK - Aggressive when depth is good
    // ==========================================================================
    inline void executeLIQUIDITY(ParentOrder& order, const UnifiedTick& t) {
        const auto& signals = microEngine_.getSignals();
        
        // Check for favorable conditions
        bool goodDepth = (order.side > 0) ? 
            (t.askDepth > order.remainingQty() * 2) : 
            (t.bidDepth > order.remainingQty() * 2);
        
        bool lowToxicity = !signals.isToxicFlow;
        bool goodSpread = signals.spreadBps < 5.0; // Less than 5 bps
        
        if (goodDepth && lowToxicity && goodSpread && order.remainingQty() > 0) {
            // Be aggressive - take available liquidity
            double sliceQty = std::min(order.childSize * 3, order.remainingQty());
            sendChildOrder(order, sliceQty, 0.0, false, t);
            order.lastChildTime = now();
        } else if (now() - order.lastChildTime > 1000000000) { // 1 second timeout
            // Send small slice anyway to make progress
            double sliceQty = std::min(order.childSize * 0.5, order.remainingQty());
            if (sliceQty > 0) {
                sendChildOrder(order, sliceQty, 0.0, false, t);
                order.lastChildTime = now();
            }
        }
    }
    
    // ==========================================================================
    // SNIPER - Wait for optimal entry
    // ==========================================================================
    inline void executeSNIPER(ParentOrder& order, const UnifiedTick& t) {
        const auto& signals = microEngine_.getSignals();
        
        // Wait for confluence of favorable signals
        bool priceBelow = (order.side > 0) && (t.bid < signals.vwap * 0.999);
        bool priceAbove = (order.side < 0) && (t.ask > signals.vwap * 1.001);
        bool favorableFlow = (order.side > 0) ? 
            (signals.orderFlowImbalance > 0.1) : 
            (signals.orderFlowImbalance < -0.1);
        bool lowVolatility = signals.realizedVolatility < 0.0005;
        
        bool entrySignal = (priceBelow || priceAbove) && favorableFlow && lowVolatility;
        
        if (entrySignal && order.childOrdersSent == 0) {
            // All conditions met - execute full order
            sendChildOrder(order, order.totalQty, 0.0, false, t);
        }
        
        // Timeout after 30 seconds - just execute
        if (order.childOrdersSent == 0 && now() - order.startTime > 30000000000ULL) {
            sendChildOrder(order, order.totalQty, 0.0, false, t);
        }
    }
    
    // ==========================================================================
    // Send child order to queue
    // ==========================================================================
    inline void sendChildOrder(ParentOrder& order, double qty, double price, 
                               bool isLimit, const UnifiedTick& t) {
        ChildOrder child;
        child.id = nextOrderId_.fetch_add(1);
        child.parentId = order.id;
        std::strncpy(child.symbol, order.symbol, 15);
        child.side = order.side;
        child.qty = qty;
        child.price = isLimit ? price : ((order.side > 0) ? t.ask : t.bid);
        child.sentTime = now();
        child.isLimit = isLimit;
        
        childOrderQueue_.enqueue(child);
        order.childOrdersSent++;
    }
    
    inline uint64_t now() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

private:
    const CentralMicroEngine& microEngine_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> nextOrderId_;
    
    // Lock-free queues for thread-safe communication
    LockFreeQueue<ParentOrder> parentOrderQueue_;
    LockFreeQueue<ChildOrder> childOrderQueue_;
    
    // Active orders (fixed array, no allocation)
    std::array<ParentOrder, MAX_PARENT_ORDERS> activeOrders_;
};

} // namespace Chimera
