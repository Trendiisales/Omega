// =============================================================================
// CentralMicroEngine.hpp - HFT-Optimized Centralized Microstructure Engine
// 
// CONCEPT from documents, IMPLEMENTATION using HFT patterns:
// - Fixed arrays (no heap allocation)
// - Running sums for O(1) updates
// - Lock-free access
// - Cache-line aligned
// =============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "../data/UnifiedTick.hpp"

namespace Chimera {

// Cache line size for alignment
constexpr size_t CACHE_LINE = 64;

// =============================================================================
// Pre-computed signals - ALL strategies read from this (zero calculation)
// Aligned to cache line to prevent false sharing
// =============================================================================
struct alignas(CACHE_LINE) MicrostructureSignals {
    // --- Price & Volume ---
    double vwap = 0.0;
    double rollingVolume = 0.0;
    double typicalPrice = 0.0;
    
    // --- Volatility ---
    double realizedVolatility = 0.0;
    double microPriceNoise = 0.0;
    double atr = 0.0;  // Average True Range
    
    // --- Order Flow Imbalance ---
    double orderFlowImbalance = 0.0;   // (Buy - Sell) / Total
    double aggressorRatio = 0.0;       // Buy / Total
    double vpin = 0.0;                 // Volume-synchronized PIN
    double toxicity = 0.0;             // Order flow toxicity
    
    // --- Liquidity & Depth ---
    double tradeIntensity = 0.0;       // Trades per second
    double spreadBps = 0.0;            // Spread in basis points
    double depthImbalance = 0.0;       // Bid depth vs ask depth
    
    // --- Momentum ---
    double momentum = 0.0;
    double acceleration = 0.0;
    double trendStrength = 0.0;
    
    // --- Trade Signatures ---
    bool isLargeTrade = false;
    bool isBuyerInitiated = false;
    bool isHighVolatility = false;
    bool isToxicFlow = false;
    
    // --- Timestamps ---
    uint64_t lastUpdateTs = 0;
    uint64_t signalAge = 0;
};

// =============================================================================
// CentralMicroEngine - Computes ALL signals ONCE per tick
// All strategies get signals via fast pointer lookup
// =============================================================================
class CentralMicroEngine {
    static constexpr size_t MAX_WINDOW = 256;
    static constexpr size_t VWAP_WINDOW = 100;
    static constexpr size_t VOL_WINDOW = 50;
    
public:
    CentralMicroEngine() { reset(); }
    
    // ==========================================================================
    // Main entry point - called ONCE per tick by the engine
    // Updates ALL signals for ALL strategies to consume
    // ==========================================================================
    inline void onTick(const UnifiedTick& t) {
        // Store in circular buffer
        size_t idx = head_;
        prices_[idx] = (t.bid + t.ask) * 0.5;
        volumes_[idx] = t.buyVol + t.sellVol;
        buyVolumes_[idx] = t.buyVol;
        sellVolumes_[idx] = t.sellVol;
        spreads_[idx] = t.spread;
        timestamps_[idx] = t.tsLocal;
        
        // Update running sums (O(1) instead of O(n))
        if (count_ >= VWAP_WINDOW) {
            size_t oldIdx = (head_ + MAX_WINDOW - VWAP_WINDOW) % MAX_WINDOW;
            sumPV_ -= prices_[oldIdx] * volumes_[oldIdx];
            sumVolume_ -= volumes_[oldIdx];
            sumBuyVol_ -= buyVolumes_[oldIdx];
            sumSellVol_ -= sellVolumes_[oldIdx];
        }
        
        double pv = prices_[idx] * volumes_[idx];
        sumPV_ += pv;
        sumVolume_ += volumes_[idx];
        sumBuyVol_ += buyVolumes_[idx];
        sumSellVol_ += sellVolumes_[idx];
        
        head_ = (head_ + 1) % MAX_WINDOW;
        if (count_ < MAX_WINDOW) ++count_;
        
        // Update all signals
        updateVWAP(t);
        updateVolatility();
        updateOrderFlow();
        updateMomentum();
        updateTradeIntensity(t);
        updateTradeSignatures(t);
        
        // Mark update time
        signals_.lastUpdateTs = t.tsLocal;
    }
    
    // ==========================================================================
    // Fast read-only access for all strategies
    // No calculation, just pointer dereference
    // ==========================================================================
    inline const MicrostructureSignals& getSignals() const {
        return signals_;
    }
    
    inline void reset() {
        prices_.fill(0.0);
        volumes_.fill(0.0);
        buyVolumes_.fill(0.0);
        sellVolumes_.fill(0.0);
        spreads_.fill(0.0);
        timestamps_.fill(0);
        
        head_ = 0;
        count_ = 0;
        sumPV_ = 0.0;
        sumVolume_ = 0.0;
        sumBuyVol_ = 0.0;
        sumSellVol_ = 0.0;
        sumSqReturns_ = 0.0;
        
        signals_ = MicrostructureSignals{};
    }

private:
    // ==========================================================================
    // VWAP - O(1) using running sums
    // ==========================================================================
    inline void updateVWAP(const UnifiedTick& t) {
        if (sumVolume_ > 0.0) {
            signals_.vwap = sumPV_ / sumVolume_;
            signals_.rollingVolume = sumVolume_;
        }
        signals_.typicalPrice = (t.bid + t.ask + prices_[(head_ + MAX_WINDOW - 1) % MAX_WINDOW]) / 3.0;
        signals_.spreadBps = (t.spread / signals_.vwap) * 10000.0;
    }
    
    // ==========================================================================
    // Volatility - Incremental calculation
    // ==========================================================================
    inline void updateVolatility() {
        if (count_ < 3) return;
        
        // Get last few prices for log returns
        size_t idx1 = (head_ + MAX_WINDOW - 1) % MAX_WINDOW;
        size_t idx2 = (head_ + MAX_WINDOW - 2) % MAX_WINDOW;
        size_t idx3 = (head_ + MAX_WINDOW - 3) % MAX_WINDOW;
        
        double p1 = prices_[idx3];
        double p2 = prices_[idx2];
        double p3 = prices_[idx1];
        
        if (p1 > 0 && p2 > 0 && p3 > 0) {
            double r1 = std::log(p2 / p1);
            double r2 = std::log(p3 / p2);
            
            // Exponential moving average of squared returns
            double alpha = 0.1;
            sumSqReturns_ = alpha * (r2 * r2) + (1.0 - alpha) * sumSqReturns_;
            signals_.realizedVolatility = std::sqrt(sumSqReturns_);
            
            // Micro price noise
            signals_.microPriceNoise = std::fabs(r2 - r1);
        }
        
        signals_.isHighVolatility = (signals_.realizedVolatility > 0.001);
    }
    
    // ==========================================================================
    // Order Flow - O(1) using running sums
    // ==========================================================================
    inline void updateOrderFlow() {
        double totalVol = sumBuyVol_ + sumSellVol_;
        
        if (totalVol > 0.0) {
            signals_.orderFlowImbalance = (sumBuyVol_ - sumSellVol_) / totalVol;
            signals_.aggressorRatio = sumBuyVol_ / totalVol;
            
            // VPIN approximation
            double absImbalance = std::fabs(sumBuyVol_ - sumSellVol_);
            signals_.vpin = absImbalance / totalVol;
            
            // Toxicity: high VPIN with directional flow
            signals_.toxicity = signals_.vpin * std::fabs(signals_.orderFlowImbalance);
            signals_.isToxicFlow = (signals_.toxicity > 0.3);
        }
    }
    
    // ==========================================================================
    // Momentum - O(1) from last 3 prices
    // ==========================================================================
    inline void updateMomentum() {
        if (count_ < 3) return;
        
        size_t idx1 = (head_ + MAX_WINDOW - 1) % MAX_WINDOW;
        size_t idx2 = (head_ + MAX_WINDOW - 2) % MAX_WINDOW;
        size_t idx3 = (head_ + MAX_WINDOW - 3) % MAX_WINDOW;
        
        double p1 = prices_[idx3];
        double p2 = prices_[idx2];
        double p3 = prices_[idx1];
        
        double v1 = p2 - p1;
        double v2 = p3 - p2;
        
        signals_.momentum = v2;
        signals_.acceleration = v2 - v1;
        
        // Trend strength: consistent direction
        if (v1 * v2 > 0) {
            signals_.trendStrength = std::min(1.0, std::fabs(v2) / (std::fabs(v1) + 1e-9));
        } else {
            signals_.trendStrength = 0.0;
        }
    }
    
    // ==========================================================================
    // Trade Intensity - trades per second
    // ==========================================================================
    inline void updateTradeIntensity(const UnifiedTick& t) {
        if (count_ < 2) return;
        
        size_t oldestIdx = (head_ + MAX_WINDOW - std::min(count_, VWAP_WINDOW)) % MAX_WINDOW;
        uint64_t timeDiffNs = t.tsLocal - timestamps_[oldestIdx];
        
        if (timeDiffNs > 0) {
            double timeDiffSec = static_cast<double>(timeDiffNs) / 1e9;
            signals_.tradeIntensity = static_cast<double>(std::min(count_, VWAP_WINDOW)) / timeDiffSec;
        }
    }
    
    // ==========================================================================
    // Trade Signatures
    // ==========================================================================
    inline void updateTradeSignatures(const UnifiedTick& t) {
        // Large trade detection
        double avgVol = (count_ > 0) ? sumVolume_ / std::min(count_, VWAP_WINDOW) : 0.0;
        signals_.isLargeTrade = (t.buyVol + t.sellVol) > (avgVol * 3.0);
        
        // Buyer initiated
        signals_.isBuyerInitiated = (t.buyVol > t.sellVol);
        
        // Depth imbalance
        if (t.bidDepth + t.askDepth > 0) {
            signals_.depthImbalance = (t.bidDepth - t.askDepth) / (t.bidDepth + t.askDepth);
        }
    }

private:
    // Fixed-size circular buffers - NO HEAP ALLOCATION
    std::array<double, MAX_WINDOW> prices_;
    std::array<double, MAX_WINDOW> volumes_;
    std::array<double, MAX_WINDOW> buyVolumes_;
    std::array<double, MAX_WINDOW> sellVolumes_;
    std::array<double, MAX_WINDOW> spreads_;
    std::array<uint64_t, MAX_WINDOW> timestamps_;
    
    size_t head_ = 0;
    size_t count_ = 0;
    
    // Running sums for O(1) updates
    double sumPV_ = 0.0;
    double sumVolume_ = 0.0;
    double sumBuyVol_ = 0.0;
    double sumSellVol_ = 0.0;
    double sumSqReturns_ = 0.0;
    
    // The output - all signals pre-computed
    alignas(CACHE_LINE) MicrostructureSignals signals_;
};

} // namespace Chimera
