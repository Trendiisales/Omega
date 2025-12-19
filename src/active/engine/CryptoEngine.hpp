// =============================================================================
// CryptoEngine.hpp - Binance Crypto Trading Engine (ISOLATED) v6.4
// =============================================================================
// ARCHITECTURE:
//   - Dedicated thread pinned to CPU 1
//   - Processes ONLY Binance ticks (no cTrader crossover)
//   - Owns its own strategies, positions, risk limits
//   - Communicates with outside world via atomic counters ONLY
//   - Exports VenueHealth for Arbiter (atomic reads only)
//
// STRATEGY SYSTEM:
//   - 10 Bucket-based voting (not 32 individual)
//   - Each bucket owns ONE category
//   - Strategy outputs Intent (not OrderRequest)
//   - Arbiter decides execution
//
// DATA FLOW:
//   Binance WS → BinanceUnifiedFeed → TickFull → CryptoEngine::processTick()
//   Strategy → Intent → Arbiter → Approved Order → submitOrder()
//
// INVARIANTS:
//   - NO shared ticks with CfdEngine
//   - NO shared order books  
//   - NO shared strategy state
//   - NO mutex in tick processing
//   - ONE symbol router per engine
//
// v6.4 UNIFIED VENUEHEALTH (per spec):
//   - Single VenueHealth struct (venue/VenueHealth.hpp)
//   - VenueState: HEALTHY/DEGRADED/UNAVAILABLE
//   - Backpressure tracking integrated into VenueHealth
//   - Protocol-agnostic Arbiter contract
//   - State machine:
//       UNAVAILABLE = no valid market view (initial, disconnect, before snapshot)
//       WS Connect → UNAVAILABLE
//       Snapshot received → DEGRADED
//       First live tick after snapshot → HEALTHY
//       WS Disconnect → UNAVAILABLE (immediate, no grace period)
//       Stale tick (20ms) → DEGRADED
//   - Staleness guard: 20ms
//   - Arbiter enforcement: urgency-gated (DEGRADED blocks urgency > threshold)
//   - REST rate limit: Predictive backpressure (pre-429)
// =============================================================================
#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <functional>
#include <iostream>
#include <chrono>
#include <cstring>

#include "EngineTypes.hpp"
#include "../market/TickFull.hpp"
#include "../market/Tick.hpp"
#include "../data/UnifiedTick.hpp"
#include "../micro/CentralMicroEngine.hpp"
#include "../micro/MicroEngines_CRTP.hpp"
#include "../strategy/Strategies_Bucket.hpp"
#include "../risk/RiskGuardian.hpp"
#include "../execution/SmartExecutionEngine.hpp"
#include "../binance/BinanceUnifiedFeed.hpp"
#include "../venue/VenueHealth.hpp"
#include "../arbiter/Arbiter.hpp"
#include "../arbiter/Intent.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace Chimera {

// =============================================================================
// CryptoEngine Statistics (atomic, safe to read from other threads)
// =============================================================================
struct CryptoEngineStats {
    std::atomic<uint64_t> ticks_processed{0};
    std::atomic<uint64_t> ticks_rejected_unavailable{0};   // v6.4: Rejected due to UNAVAILABLE
    std::atomic<uint64_t> signals_generated{0};
    std::atomic<uint64_t> orders_sent{0};
    std::atomic<uint64_t> orders_rejected{0};
    std::atomic<uint64_t> orders_rejected_rest_limit{0};   // REST rate limit
    std::atomic<uint64_t> total_latency_ns{0};
    std::atomic<uint64_t> max_latency_ns{0};
    std::atomic<uint64_t> vetoed_signals{0};
    std::atomic<uint64_t> arbiter_rejections{0};
    std::atomic<uint64_t> backpressure_rejections{0};
    std::atomic<uint64_t> ws_disconnects{0};
    std::atomic<uint64_t> ws_reconnects{0};
    
    // Bucket vote counters
    std::atomic<uint64_t> buy_votes{0};
    std::atomic<uint64_t> sell_votes{0};
    std::atomic<uint64_t> consensus_trades{0};
    
    double avgLatencyUs() const {
        uint64_t ticks = ticks_processed.load(std::memory_order_relaxed);
        if (ticks == 0) return 0.0;
        return static_cast<double>(total_latency_ns.load(std::memory_order_relaxed)) / ticks / 1000.0;
    }
};

// =============================================================================
// REST Rate Limiter (Binance-specific, kept in engine not VenueHealth)
// =============================================================================
struct RestRateLimiter {
    std::atomic<uint32_t> calls_this_second{0};
    std::atomic<uint64_t> window_start_ns{0};
    
    static constexpr uint32_t LIMIT_PER_SEC = 1000;  // Binance limit
    
    inline bool check(uint64_t now_ns) noexcept {
        uint64_t window_start = window_start_ns.load(std::memory_order_relaxed);
        
        // Reset window every second
        if (now_ns - window_start > 1'000'000'000) {
            window_start_ns.store(now_ns, std::memory_order_relaxed);
            calls_this_second.store(0, std::memory_order_relaxed);
        }
        
        return calls_this_second.load(std::memory_order_relaxed) < LIMIT_PER_SEC;
    }
    
    inline void increment() noexcept {
        calls_this_second.fetch_add(1, std::memory_order_relaxed);
    }
    
    inline void reset() noexcept {
        calls_this_second.store(0, std::memory_order_relaxed);
        window_start_ns.store(0, std::memory_order_relaxed);
    }
};

// =============================================================================
// CryptoEngine - Binance-Only Trading Engine with 10-Bucket Strategy System v6.4
// =============================================================================
class CryptoEngine {
public:
    static constexpr int CPU_CORE = 1;  // Pinned to CPU 1
    static constexpr Venue ENGINE_VENUE = Venue::BINANCE;
    
    // v6.4: Urgency threshold for DEGRADED state (configured to Arbiter)
    static constexpr double DEGRADED_URGENCY_THRESHOLD = 0.3;
    
    CryptoEngine() 
        : running_(false)
        , snapshot_received_(false)
        , first_tick_received_(false)
        , kill_switch_(nullptr)
        , arbiter_(nullptr)
        , execEngine_(centralMicro_)
    {
        symbols_ = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
        venueHealth_.reset();  // v6.4: reset() sets UNAVAILABLE
        restLimiter_.reset();
    }
    
    ~CryptoEngine() {
        stop();
    }
    
    // =========================================================================
    // Configuration
    // =========================================================================
    void setSymbols(const std::vector<std::string>& symbols) {
        symbols_ = symbols;
    }
    
    void setKillSwitch(GlobalKillSwitch* ks) {
        kill_switch_ = ks;
    }
    
    void setArbiter(Arbiter* arb) {
        arbiter_ = arb;
        // v6.4: Configure Arbiter with our urgency threshold
        if (arbiter_) {
            arbiter_->setVenue1DegradedUrgency(DEGRADED_URGENCY_THRESHOLD);
        }
    }
    
    void setOrderCallback(std::function<void(const char*, int8_t, double)> cb) {
        orderCallback_ = std::move(cb);
    }
    
    void setBucketWeights(const BucketWeights& w) {
        stratPack_.aggregator.setWeights(w);
    }
    
    // =========================================================================
    // VenueHealth Export (for Arbiter - READ ONLY)
    // v6.4: Now using unified VenueHealth from venue/VenueHealth.hpp
    // =========================================================================
    const VenueHealth& getVenueHealth() const { return venueHealth_; }
    VenueHealth& getVenueHealthMutable() { return venueHealth_; }
    
    // =========================================================================
    // Lifecycle
    // =========================================================================
    bool start() {
        if (running_) return false;
        
        running_ = true;
        snapshot_received_ = false;
        first_tick_received_ = false;
        venueHealth_.reset();  // v6.4: Start in UNAVAILABLE state
        restLimiter_.reset();
        riskGuard_.start();
        execEngine_.start();
        
        engineThread_ = std::thread(&CryptoEngine::engineLoop, this);
        pinToCpu(engineThread_, CPU_CORE);
        
        std::cout << "[CryptoEngine] Started on CPU " << CPU_CORE << std::endl;
        std::cout << "[CryptoEngine] Strategy System: 10-BUCKET VOTING + ARBITER" << std::endl;
        std::cout << "[CryptoEngine] v6.4: UNIFIED VenueHealth (UNAVAILABLE→snap→DEGRADED→live→HEALTHY)" << std::endl;
        std::cout << "[CryptoEngine] v6.4: Staleness guard = " 
                  << VenueHealth::STALE_DATA_NS / 1'000'000 << "ms" << std::endl;
        std::cout << "[CryptoEngine] v6.4: DEGRADED urgency threshold = " 
                  << DEGRADED_URGENCY_THRESHOLD << std::endl;
        std::cout << "[CryptoEngine] v6.4: REST rate limit = " 
                  << RestRateLimiter::LIMIT_PER_SEC << "/sec" << std::endl;
        return true;
    }
    
    void stop() {
        if (!running_) return;
        running_ = false;
        venueHealth_.setUnavailable();
        feed_.stop();
        if (engineThread_.joinable()) engineThread_.join();
        execEngine_.stop();
        riskGuard_.stop();
        std::cout << "[CryptoEngine] Stopped. Ticks: " << stats_.ticks_processed.load() << std::endl;
        std::cout << "[CryptoEngine] UNAVAILABLE rejected: " << stats_.ticks_rejected_unavailable.load() << std::endl;
        std::cout << "[CryptoEngine] Consensus trades: " << stats_.consensus_trades.load() 
                  << " Vetoed: " << stats_.vetoed_signals.load() 
                  << " Arbiter rejected: " << stats_.arbiter_rejections.load() 
                  << " Backpressure rejected: " << stats_.backpressure_rejections.load() << std::endl;
        std::cout << "[CryptoEngine] REST limit rejects: " << stats_.orders_rejected_rest_limit.load() << std::endl;
        std::cout << "[CryptoEngine] WS disconnects: " << stats_.ws_disconnects.load() 
                  << " reconnects: " << stats_.ws_reconnects.load() << std::endl;
    }
    
    // =========================================================================
    // HOT PATH - Process tick with bucket voting + Arbiter routing
    // v6.4: UNIFIED VENUEHEALTH per spec (no trading during UNAVAILABLE)
    // =========================================================================
    inline void processTick(const TickFull& tick) {
        if (tick.venue != Venue::BINANCE) return;
        if (kill_switch_ && kill_switch_->isCryptoKilled()) return;
        
        uint64_t start_ns = nowNs();
        
        // v6.4: Staleness guard FIRST (20ms per spec)
        VenueState current_state = venueHealth_.getState();
        if (current_state == VenueState::HEALTHY) {
            if (venueHealth_.checkAndHandleStaleness(start_ns)) {
                current_state = VenueState::DEGRADED;
            }
        }
        
        // v6.4: UNAVAILABLE - NEVER trade
        if (current_state == VenueState::UNAVAILABLE) {
            stats_.ticks_rejected_unavailable.fetch_add(1, std::memory_order_relaxed);
            return;  // Don't process tick during UNAVAILABLE
        }
        
        // Note: DEGRADED ticks are processed but Arbiter enforces urgency gating
        
        // Update venue health (tick received) - this resets staleness timer
        venueHealth_.onDataReceived(start_ns);
        
        // Convert tick
        UnifiedTick ut;
        convertTick(tick, ut);
        
        // Update micro engines
        centralMicro_.onTick(ut);
        updateMicroEngines(ut);
        
        // Get microstructure signals for strategies
        const MicrostructureSignals& micro = centralMicro_.getSignals();
        
        // Run 10-bucket strategy voting
        BucketDecision decision = stratPack_.compute(ut, micro);
        
        stats_.buy_votes.store(decision.buyVotes, std::memory_order_relaxed);
        stats_.sell_votes.store(decision.sellVotes, std::memory_order_relaxed);
        
        // Check for vetoes
        if (decision.vetoed) {
            stats_.vetoed_signals.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Execute if consensus reached
        if (decision.hasConsensus() && decision.avgConfidence > 0.4) {
            // Create Intent from decision
            Intent intent;
            intent.symbol_id = tick.symbol_id;
            intent.side = decision.shouldBuy() ? Side::Buy : Side::Sell;
            intent.size = 0.001 * decision.riskMultiplier;  // Base size * risk
            intent.size = std::clamp(intent.size, 0.0001, 0.005);  // Limits
            intent.urgency = decision.avgConfidence;  // Use confidence as urgency
            intent.confidence = decision.avgConfidence;
            intent.ts_ns = start_ns;
            intent.venue = Venue::BINANCE;  // v6.4: Specify venue preference
            
            // Route through Arbiter (if available)
            bool approved = true;
            if (arbiter_) {
                // v6.4: Use venue-agnostic decide()
                ArbiterDecision arbDecision = arbiter_->decide(intent);
                approved = arbDecision.shouldExecute() && arbDecision.isBinance();
                
                if (!approved) {
                    stats_.arbiter_rejections.fetch_add(1, std::memory_order_relaxed);
                }
            }
            
            // Execute if approved
            if (approved) {
                int8_t side = (intent.side == Side::Buy) ? 1 : -1;
                submitOrder(tick.symbol, side, intent.size);
                stats_.consensus_trades.fetch_add(1, std::memory_order_relaxed);
            }
        }
        
        execEngine_.onTick(ut);
        
        // Update latency stats and feed to latency filter
        uint64_t latency = nowNs() - start_ns;
        stratPack_.updateExecLatency(latency);
        
        // Update venue health with latency
        venueHealth_.updateLatency(latency);
        
        stats_.ticks_processed.fetch_add(1, std::memory_order_relaxed);
        stats_.total_latency_ns.fetch_add(latency, std::memory_order_relaxed);
        uint64_t max_lat = stats_.max_latency_ns.load(std::memory_order_relaxed);
        if (latency > max_lat) stats_.max_latency_ns.store(latency, std::memory_order_relaxed);
        
        if (decision.buyVotes > 0 || decision.sellVotes > 0) {
            stats_.signals_generated.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    // =========================================================================
    // Order completion callbacks (for backpressure tracking via VenueHealth)
    // =========================================================================
    void onOrderFill(const char* /* symbol */, double /* qty */) {
        venueHealth_.onOrderFill();
    }
    
    void onOrderReject(const char* /* symbol */) {
        venueHealth_.onOrderReject();
        stats_.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
    }
    
    void onOrderCancel(const char* /* symbol */) {
        venueHealth_.onOrderCancel();
    }
    
    const CryptoEngineStats& getStats() const { return stats_; }
    const MicrostructureSignals& getSignals() const { return centralMicro_.getSignals(); }
    const BucketWeights& getBucketWeights() const { return stratPack_.aggregator.getWeights(); }
    bool isRunning() const { return running_; }
    
    // v6.4: Expose state for monitoring
    VenueState getVenueState() const { return venueHealth_.getState(); }
    const char* getVenueStateStr() const { return venueHealth_.stateStr(); }
    bool isUnavailable() const { return venueHealth_.isUnavailable(); }
    bool isDegraded() const { return venueHealth_.isDegraded(); }
    bool isHealthy() const { return venueHealth_.isHealthy(); }
    
    // Legacy aliases
    bool isBlindMode() const { return isUnavailable(); }

private:
    // =========================================================================
    // v6.4: WebSocket state callbacks (per UNIFIED VENUEHEALTH spec)
    // =========================================================================
    // State machine:
    //   WS Connect → UNAVAILABLE (no valid market view yet)
    //   Snapshot received → DEGRADED (have data but not confirmed live)
    //   First live tick after snapshot → HEALTHY (confirmed live flow)
    //   WS Disconnect → UNAVAILABLE (immediate, no grace period)
    //   Stale tick (20ms) → DEGRADED
    // =========================================================================
    
    void onWsConnect() {
        // v6.4: Connection alone ≠ valid market. Set UNAVAILABLE.
        std::cout << "[CryptoEngine] WS CONNECTED → UNAVAILABLE (awaiting snapshot)" << std::endl;
        venueHealth_.setUnavailable();  // No valid market view yet
        stats_.ws_reconnects.fetch_add(1, std::memory_order_relaxed);
        snapshot_received_ = false;
        first_tick_received_ = false;
    }
    
    void onWsDisconnect() {
        // v6.4: Immediate UNAVAILABLE. No grace period.
        std::cout << "[CryptoEngine] WS DISCONNECTED → UNAVAILABLE (immediate)" << std::endl;
        venueHealth_.setUnavailable();
        stats_.ws_disconnects.fetch_add(1, std::memory_order_relaxed);
        snapshot_received_ = false;
        first_tick_received_ = false;
    }
    
    void onSnapshotReceived() {
        // v6.4: Snapshot received → DEGRADED (have data, not yet confirmed live)
        if (!snapshot_received_) {
            snapshot_received_ = true;
            if (venueHealth_.getState() == VenueState::UNAVAILABLE) {
                venueHealth_.setDegraded();
                std::cout << "[CryptoEngine] Snapshot received → DEGRADED" << std::endl;
            }
        }
    }
    
    void onFirstLiveTick() {
        // v6.4: First live tick after snapshot → HEALTHY
        if (snapshot_received_ && !first_tick_received_) {
            first_tick_received_ = true;
            venueHealth_.setHealthy();
            std::cout << "[CryptoEngine] First live tick → HEALTHY" << std::endl;
        }
    }
    
    // Legacy compatibility
    void onFirstTick() {
        // v6.4: Two-phase warmup per spec
        if (!snapshot_received_) {
            // First tick = treat as snapshot
            onSnapshotReceived();
        } else if (!first_tick_received_) {
            // Second tick = first live tick after snapshot
            onFirstLiveTick();
        }
    }

    void engineLoop() {
        std::cout << "[CryptoEngine] Loop started" << std::endl;
        std::cout << "[CryptoEngine] v6.4: UNIFIED VenueHealth - 2-phase warmup (snapshot→DEGRADED→live→HEALTHY)" << std::endl;
        std::cout << "[CryptoEngine] v6.4: Staleness guard = 20ms" << std::endl;
        
        feed_.setTickCallback([this](const Tick& t) {
            // v6.4: Two-phase warmup per spec
            // Phase 1: First tick = snapshot → DEGRADED
            // Phase 2: Second tick = live tick → HEALTHY
            VenueState current = venueHealth_.getState();
            if (current == VenueState::UNAVAILABLE) {
                // First tick after connect - treat as snapshot
                onSnapshotReceived();
            } else if (current == VenueState::DEGRADED && snapshot_received_ && !first_tick_received_) {
                // Second tick - confirm live flow
                onFirstLiveTick();
            }
            
            TickFull tick;
            std::strncpy(tick.symbol, t.symbol, 15);
            tick.symbol[15] = '\0';
            tick.venue = Venue::BINANCE;
            tick.ts_ns = nowNs();
            tick.bid = t.bid;
            tick.ask = t.ask;
            tick.bid_size = t.b1;
            tick.ask_size = t.a1;
            tick.buy_vol = t.buyVol;
            tick.sell_vol = t.sellVol;
            tick.flags = TICK_FLAG_BBO_UPDATE;
            tick.bid_depth[0] = t.b1; tick.bid_depth[1] = t.b2;
            tick.bid_depth[2] = t.b3; tick.bid_depth[3] = t.b4;
            tick.ask_depth[0] = t.a1; tick.ask_depth[1] = t.a2;
            tick.ask_depth[2] = t.a3; tick.ask_depth[3] = t.a4;
            processTick(tick);
        });
        
        // Register state callback (fires on WS connect/disconnect)
        feed_.setStateCallback([this](bool connected) {
            if (connected) {
                onWsConnect();
            } else {
                onWsDisconnect();
            }
        });
        
        for (const auto& sym : symbols_) {
            std::cout << "[CryptoEngine] Starting feed: " << sym << std::endl;
            feed_.start(sym);
        }
        
        // Control loop with periodic maintenance
        uint64_t loop_count = 0;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            loop_count++;
            
            // Periodic maintenance (every 100ms)
            // v6.4: Decay counters via unified VenueHealth
            venueHealth_.decayCounters();
            
            // v6.4: Backup staleness check (catches silent WS stalls when no ticks arrive)
            uint64_t now = nowNs();
            if (venueHealth_.getState() == VenueState::HEALTHY) {
                if (venueHealth_.hasStaleData(now)) {
                    std::cout << "[CryptoEngine] Stale tick detected (20ms) → DEGRADED" << std::endl;
                    venueHealth_.setDegraded();
                }
            }
            
            // v6.4: Check reject burst → DEGRADED (via recent_rejects threshold)
            if (venueHealth_.getState() == VenueState::HEALTHY) {
                if (venueHealth_.getRecentRejects() > VenueHealth::MAX_RECENT_REJECTS) {
                    std::cout << "[CryptoEngine] Reject burst detected → DEGRADED" << std::endl;
                    venueHealth_.setDegraded();
                }
            }
        }
    }
    
    inline void convertTick(const TickFull& src, UnifiedTick& dst) {
        std::strncpy(dst.symbol, src.symbol, 15);
        dst.bid = src.bid; dst.ask = src.ask;
        dst.spread = src.spread();
        dst.bidSize = src.bid_size; dst.askSize = src.ask_size;
        dst.buyVol = src.buy_vol; dst.sellVol = src.sell_vol;
        dst.tsLocal = src.ts_ns; dst.tsExchange = src.ts_exchange;
        dst.b1 = src.bid_depth[0]; dst.b2 = src.bid_depth[1];
        dst.b3 = src.bid_depth[2]; dst.b4 = src.bid_depth[3]; dst.b5 = src.bid_depth[4];
        dst.a1 = src.ask_depth[0]; dst.a2 = src.ask_depth[1];
        dst.a3 = src.ask_depth[2]; dst.a4 = src.ask_depth[3]; dst.a5 = src.ask_depth[4];
        dst.computeDepth();
    }
    
    inline void updateMicroEngines(const UnifiedTick& t) {
        micro01_.onTick(t); micro02_.onTick(t); micro03_.onTick(t); micro04_.onTick(t);
        micro05_.onTick(t); micro06_.onTick(t); micro07_.onTick(t); micro08_.onTick(t);
        micro09_.onTick(t); micro10_.onTick(t); micro11_.onTick(t); micro12_.onTick(t);
        micro13_.onTick(t); micro14_.onTick(t); micro15_.onTick(t); micro16_.onTick(t);
        micro17_.onTick(t);
    }
    
    // v6.4: Enhanced order submission with REST rate limit check
    inline void submitOrder(const char* symbol, int8_t side, double qty) {
        uint64_t now = nowNs();
        
        // REST rate limit check
        if (!restLimiter_.check(now)) {
            stats_.orders_rejected_rest_limit.fetch_add(1, std::memory_order_relaxed);
            
            // Rate limit hit → DEGRADED
            if (venueHealth_.getState() == VenueState::HEALTHY) {
                std::cout << "[CryptoEngine] REST rate limit hit → DEGRADED" << std::endl;
                venueHealth_.setDegraded();
            }
            return;
        }
        restLimiter_.increment();
        
        if (!riskGuard_.checkOrder(qty, side)) {
            stats_.orders_rejected.fetch_add(1, std::memory_order_relaxed);
            venueHealth_.onOrderReject();
            return;
        }
        
        // Track order sent (backpressure via VenueHealth)
        venueHealth_.onOrderSent(now);
        
        if (orderCallback_) orderCallback_(symbol, side, qty);
        stats_.orders_sent.fetch_add(1, std::memory_order_relaxed);
        
        // Note: In production, onOrderFill/onOrderReject/onOrderCancel should be 
        // called by the actual execution layer when order status is received
    }
    
    static void pinToCpu(std::thread& t, int cpu) {
#ifdef __linux__
        cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(cpu, &cpuset);
        pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#else
        (void)t; (void)cpu;
#endif
    }
    
    inline uint64_t nowNs() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    std::atomic<bool> running_;
    std::atomic<bool> snapshot_received_;         // Track snapshot for DEGRADED transition
    std::atomic<bool> first_tick_received_;       // Track first live tick for HEALTHY transition
    GlobalKillSwitch* kill_switch_;
    Arbiter* arbiter_;
    std::thread engineThread_;
    std::vector<std::string> symbols_;
    BinanceUnifiedFeed feed_;
    CentralMicroEngine centralMicro_;
    
    // v6.4: Unified VenueHealth (from venue/VenueHealth.hpp)
    VenueHealth venueHealth_;
    
    // REST rate limiter (Binance-specific)
    RestRateLimiter restLimiter_;
    
    // Micro engines (still needed for additional signals)
    MicroEngine01 micro01_; MicroEngine02 micro02_; MicroEngine03 micro03_; MicroEngine04 micro04_;
    MicroEngine05 micro05_; MicroEngine06 micro06_; MicroEngine07 micro07_; MicroEngine08 micro08_;
    MicroEngine09 micro09_; MicroEngine10 micro10_; MicroEngine11 micro11_; MicroEngine12 micro12_;
    MicroEngine13 micro13_; MicroEngine14 micro14_; MicroEngine15 micro15_; MicroEngine16 micro16_;
    MicroEngine17 micro17_;
    
    // 10-Bucket Strategy Pack (replaces 32 individual strategies)
    StrategyPack stratPack_;
    
    RiskGuardian riskGuard_;
    SmartExecutionEngine execEngine_;
    CryptoEngineStats stats_;
    std::function<void(const char*, int8_t, double)> orderCallback_;
};

} // namespace Chimera
