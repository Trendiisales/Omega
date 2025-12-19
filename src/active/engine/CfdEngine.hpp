// =============================================================================
// CfdEngine.hpp - cTrader FIX Trading Engine (ISOLATED) v6.4
// =============================================================================
// ARCHITECTURE:
//   - Dedicated thread pinned to CPU 2
//   - Processes ONLY cTrader FIX ticks (no Binance crossover)
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
//   cTrader FIX → CTraderFIXClient → TickFull → CfdEngine::processTick()
//   Strategy → Intent → Arbiter → Approved Order → submitOrder()
//
// INVARIANTS:
//   - NO shared ticks with CryptoEngine
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
//   - FIX State machine:
//       Startup → UNAVAILABLE
//       Logon ACK → DEGRADED
//       Stable heartbeats → HEALTHY
//       Heartbeat delay → DEGRADED
//       ResendRequest → protocol_errors++, DEGRADED
//       Reject burst → recent_rejects++, DEGRADED
//       Session timeout → UNAVAILABLE
//   - Staleness guard: 20ms
//   - Arbiter enforcement: urgency-gated (DEGRADED blocks urgency > threshold)
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
#include "../fix/client/CTraderFIXClient.hpp"
#include "../venue/VenueHealth.hpp"
#include "../arbiter/Arbiter.hpp"
#include "../arbiter/Intent.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace Chimera {

// =============================================================================
// CfdEngine Statistics v6.4
// =============================================================================
struct CfdEngineStats {
    std::atomic<uint64_t> ticks_processed{0};
    std::atomic<uint64_t> ticks_rejected_unavailable{0};   // v6.4: Rejected due to UNAVAILABLE
    std::atomic<uint64_t> signals_generated{0};
    std::atomic<uint64_t> orders_sent{0};
    std::atomic<uint64_t> orders_rejected{0};
    std::atomic<uint64_t> total_latency_ns{0};
    std::atomic<uint64_t> max_latency_ns{0};
    std::atomic<uint64_t> fix_messages{0};
    std::atomic<uint64_t> fix_reconnects{0};
    std::atomic<uint64_t> fix_reject_bursts{0};
    std::atomic<uint64_t> fix_protocol_errors{0};          // v6.4: Protocol errors
    std::atomic<uint64_t> vetoed_signals{0};
    std::atomic<uint64_t> arbiter_rejections{0};
    std::atomic<uint64_t> backpressure_rejections{0};
    
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
// FIX Configuration (moved to CTraderConfig)
// =============================================================================
using FIXConfig = CTraderConfig;

// =============================================================================
// CfdEngine - cTrader FIX-Only Trading Engine with 10-Bucket Strategy System v6.4
// =============================================================================
class CfdEngine {
public:
    static constexpr int CPU_CORE = 2;  // Pinned to CPU 2
    static constexpr Venue ENGINE_VENUE = Venue::CTRADER;
    
    // v6.4: Urgency threshold for DEGRADED state (FIX is more tolerant)
    static constexpr double DEGRADED_URGENCY_THRESHOLD = 0.5;
    
    // FIX heartbeat configuration
    static constexpr uint64_t HEARTBEAT_INTERVAL_NS = 30'000'000'000ULL;  // 30s
    
    CfdEngine() 
        : running_(false)
        , connected_(false)
        , first_tick_received_(false)
        , kill_switch_(nullptr)
        , arbiter_(nullptr)
        , execEngine_(centralMicro_)
    {
        forex_symbols_ = {"EURUSD", "GBPUSD", "USDJPY", "AUDUSD"};
        metals_symbols_ = {"XAUUSD", "XAGUSD"};
        indices_symbols_ = {"US30", "US100"};
        venueHealth_.reset();  // v6.4: Start in UNAVAILABLE state
    }
    
    ~CfdEngine() {
        stop();
    }
    
    // =========================================================================
    // Configuration
    // =========================================================================
    void setFIXConfig(const FIXConfig& cfg) { 
        fixConfig_ = cfg;
        fixClient_.setConfig(cfg);
    }
    void setForexSymbols(const std::vector<std::string>& s) { forex_symbols_ = s; }
    void setMetalsSymbols(const std::vector<std::string>& s) { metals_symbols_ = s; }
    void setIndicesSymbols(const std::vector<std::string>& s) { indices_symbols_ = s; }
    void setKillSwitch(GlobalKillSwitch* ks) { kill_switch_ = ks; }
    
    void setArbiter(Arbiter* arb) { 
        arbiter_ = arb; 
        // v6.4: Configure Arbiter with our urgency threshold
        if (arbiter_) {
            arbiter_->setVenue2DegradedUrgency(DEGRADED_URGENCY_THRESHOLD);
        }
    }
    
    void setOrderCallback(std::function<void(const char*, int8_t, double)> cb) { orderCallback_ = std::move(cb); }
    
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
        first_tick_received_ = false;
        venueHealth_.reset();  // v6.4: Start in UNAVAILABLE state
        riskGuard_.start();
        execEngine_.start();
        
        engineThread_ = std::thread(&CfdEngine::engineLoop, this);
        pinToCpu(engineThread_, CPU_CORE);
        
        std::cout << "[CfdEngine] Started on CPU " << CPU_CORE << std::endl;
        std::cout << "[CfdEngine] Strategy System: 10-BUCKET VOTING + ARBITER" << std::endl;
        std::cout << "[CfdEngine] v6.4: UNIFIED VenueHealth (UNAVAILABLE→logon→DEGRADED→heartbeat→HEALTHY)" << std::endl;
        std::cout << "[CfdEngine] v6.4: DEGRADED urgency threshold = " 
                  << DEGRADED_URGENCY_THRESHOLD << std::endl;
        std::cout << "[CfdEngine] Forex: "; for (const auto& s : forex_symbols_) std::cout << s << " "; std::cout << std::endl;
        std::cout << "[CfdEngine] Metals: "; for (const auto& s : metals_symbols_) std::cout << s << " "; std::cout << std::endl;
        return true;
    }
    
    void stop() {
        if (!running_) return;
        running_ = false;
        connected_ = false;
        venueHealth_.setUnavailable();
        fixClient_.disconnect();
        if (engineThread_.joinable()) engineThread_.join();
        execEngine_.stop();
        riskGuard_.stop();
        std::cout << "[CfdEngine] Stopped. Ticks: " << stats_.ticks_processed.load() << std::endl;
        std::cout << "[CfdEngine] UNAVAILABLE rejected: " << stats_.ticks_rejected_unavailable.load() << std::endl;
        std::cout << "[CfdEngine] FIX reconnects: " << stats_.fix_reconnects.load() 
                  << " Protocol errors: " << stats_.fix_protocol_errors.load() << std::endl;
    }
    
    // =========================================================================
    // HOT PATH - Process tick with bucket voting + Arbiter routing
    // v6.4: UNIFIED VENUEHEALTH per spec (no trading during UNAVAILABLE)
    // =========================================================================
    inline void processTick(const TickFull& tick) {
        if (tick.venue != Venue::CTRADER) return;
        if (kill_switch_ && kill_switch_->isCfdKilled()) return;
        
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
            intent.size = getLotSize(tick.symbol);  // FIX uses lot sizes
            intent.urgency = decision.avgConfidence;
            intent.confidence = decision.avgConfidence;
            intent.ts_ns = start_ns;
            intent.venue = Venue::CTRADER;  // v6.4: Specify venue preference
            
            // Route through Arbiter (if available)
            bool approved = true;
            if (arbiter_) {
                // v6.4: Use venue-agnostic decide()
                ArbiterDecision arbDecision = arbiter_->decide(intent);
                approved = arbDecision.shouldExecute() && arbDecision.isCTrader();
                
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
        
        // Update latency stats
        uint64_t latency = nowNs() - start_ns;
        stratPack_.updateExecLatency(latency);
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
    
    // =========================================================================
    // v6.4: FIX Protocol Events → VenueHealth Updates
    // =========================================================================
    void onFIXLogonAck() {
        // FIX Logon ACK → DEGRADED (connected but quality unknown)
        std::cout << "[CfdEngine] FIX LOGON ACK → DEGRADED" << std::endl;
        venueHealth_.setDegraded();
        connected_ = true;
    }
    
    void onFIXHeartbeat() {
        // Stable heartbeat → can promote to HEALTHY
        venueHealth_.onDataReceived(nowNs());
        
        // Promote if we're DEGRADED and have received ticks
        if (venueHealth_.getState() == VenueState::DEGRADED && first_tick_received_) {
            venueHealth_.setHealthy();
        }
    }
    
    void onFIXResendRequest() {
        // Sequence gap → protocol error → DEGRADED
        venueHealth_.onProtocolError();
        stats_.fix_protocol_errors.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[CfdEngine] FIX RESEND REQUEST → DEGRADED" << std::endl;
    }
    
    void onFIXSessionTimeout() {
        // Session timeout → UNAVAILABLE
        std::cout << "[CfdEngine] FIX SESSION TIMEOUT → UNAVAILABLE" << std::endl;
        venueHealth_.setUnavailable();
        connected_ = false;
    }
    
    void onFIXLogout() {
        // Logout → UNAVAILABLE
        std::cout << "[CfdEngine] FIX LOGOUT → UNAVAILABLE" << std::endl;
        venueHealth_.setUnavailable();
        connected_ = false;
    }
    
    const CfdEngineStats& getStats() const { return stats_; }
    const MicrostructureSignals& getSignals() const { return centralMicro_.getSignals(); }
    const BucketWeights& getBucketWeights() const { return stratPack_.aggregator.getWeights(); }
    bool isRunning() const { return running_; }
    bool isConnected() const { return connected_; }
    
    // v6.4: Expose state for monitoring
    VenueState getVenueState() const { return venueHealth_.getState(); }
    const char* getVenueStateStr() const { return venueHealth_.stateStr(); }
    bool isUnavailable() const { return venueHealth_.isUnavailable(); }
    bool isDegraded() const { return venueHealth_.isDegraded(); }
    bool isHealthy() const { return venueHealth_.isHealthy(); }
    
    // Legacy aliases
    bool isBlindMode() const { return isUnavailable(); }

private:
    double getLotSize(const char* symbol) const {
        // FIX lot sizes for different asset classes
        if (strncmp(symbol, "XAU", 3) == 0 || strncmp(symbol, "XAG", 3) == 0) {
            return 0.01;  // Metals: 0.01 lot
        } else if (strncmp(symbol, "US", 2) == 0) {
            return 0.1;   // Indices: 0.1 lot
        }
        return 0.01;      // Forex: 0.01 lot (micro lot)
    }

    void engineLoop() {
        std::cout << "[CfdEngine] Loop started" << std::endl;
        std::cout << "[CfdEngine] v6.4: UNIFIED VenueHealth - FIX state machine" << std::endl;
        
        // Set up FIX callbacks for market data
        fixClient_.setMDCallback([this](const std::string& symbol, double bid, double ask, 
                                         double bidSize, double askSize) {
            // Convert to TickFull
            TickFull tick;
            std::strncpy(tick.symbol, symbol.c_str(), 15);
            tick.symbol[15] = '\0';
            tick.venue = Venue::CTRADER;
            tick.ts_ns = nowNs();
            tick.ts_exchange = 0;
            tick.bid = bid;
            tick.ask = ask;
            tick.bid_size = bidSize;
            tick.ask_size = askSize;
            tick.buy_vol = 0;
            tick.sell_vol = 0;
            tick.flags = TICK_FLAG_BBO_UPDATE;
            
            // First tick received flag
            if (!first_tick_received_) {
                first_tick_received_ = true;
                // If we're DEGRADED and got first tick, can promote to HEALTHY
                if (venueHealth_.getState() == VenueState::DEGRADED) {
                    venueHealth_.setHealthy();
                    std::cout << "[CfdEngine] First FIX tick → HEALTHY" << std::endl;
                }
            }
            processTick(tick);
            stats_.fix_messages.fetch_add(1, std::memory_order_relaxed);
        });
        
        // Connect to FIX server
        std::cout << "[CfdEngine] Connecting to FIX server..." << std::endl;
        if (fixClient_.connect()) {
            onFIXLogonAck();  // v6.4: Use protocol callback
            
            // Subscribe to symbols
            for (const auto& sym : forex_symbols_) {
                fixClient_.subscribeMarketData(sym);
            }
            for (const auto& sym : metals_symbols_) {
                fixClient_.subscribeMarketData(sym);
            }
            for (const auto& sym : indices_symbols_) {
                fixClient_.subscribeMarketData(sym);
            }
            
            std::cout << "[CfdEngine] FIX subscribed to market data" << std::endl;
        } else {
            std::cerr << "[CfdEngine] FIX connection failed" << std::endl;
            connected_ = false;
            venueHealth_.setUnavailable();
        }
        
        // Main loop - monitor connection with periodic maintenance
        uint64_t loop_count = 0;
        uint64_t last_heartbeat_check_ns = nowNs();
        
        while (running_) {
            if (!connected_ && running_) {
                // Reconnection logic
                std::cout << "[CfdEngine] Attempting reconnect..." << std::endl;
                stats_.fix_reconnects.fetch_add(1, std::memory_order_relaxed);
                
                if (fixClient_.connect()) {
                    onFIXLogonAck();
                    first_tick_received_ = false;
                    
                    // Resubscribe
                    for (const auto& sym : forex_symbols_) {
                        fixClient_.subscribeMarketData(sym);
                    }
                    for (const auto& sym : metals_symbols_) {
                        fixClient_.subscribeMarketData(sym);
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::seconds(5));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                loop_count++;
                
                uint64_t now = nowNs();
                
                // Periodic maintenance (every 100ms)
                // v6.4: Decay counters via unified VenueHealth
                venueHealth_.decayCounters();
                
                // v6.4: Check for stale ticks → DEGRADED
                if (venueHealth_.getState() == VenueState::HEALTHY) {
                    if (venueHealth_.hasStaleData(now)) {
                        std::cout << "[CfdEngine] Stale FIX tick detected → DEGRADED" << std::endl;
                        venueHealth_.setDegraded();
                    }
                }
                
                // v6.4: Check reject burst → DEGRADED
                if (venueHealth_.getState() == VenueState::HEALTHY) {
                    if (venueHealth_.getRecentRejects() > VenueHealth::MAX_RECENT_REJECTS) {
                        std::cout << "[CfdEngine] Reject burst active → DEGRADED" << std::endl;
                        venueHealth_.setDegraded();
                        stats_.fix_reject_bursts.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                
                // v6.4: Heartbeat-based health check (FIX-specific)
                if ((now - last_heartbeat_check_ns) > HEARTBEAT_INTERVAL_NS) {
                    last_heartbeat_check_ns = now;
                    
                    // Check if we've received any data recently
                    uint64_t last_rx = venueHealth_.getLastRxNs();
                    if (last_rx > 0 && (now - last_rx) > 3 * HEARTBEAT_INTERVAL_NS) {
                        // No data for 3x heartbeat interval → UNAVAILABLE
                        onFIXSessionTimeout();
                    } else if (last_rx > 0 && (now - last_rx) > HEARTBEAT_INTERVAL_NS) {
                        // No data for 1x heartbeat interval → DEGRADED
                        if (venueHealth_.getState() == VenueState::HEALTHY) {
                            std::cout << "[CfdEngine] Heartbeat delay → DEGRADED" << std::endl;
                            venueHealth_.setDegraded();
                        }
                    }
                }
                
                // v6.4: Recovery: If DEGRADED and conditions are good, try to recover
                if (venueHealth_.getState() == VenueState::DEGRADED) {
                    uint64_t last_rx = venueHealth_.getLastRxNs();
                    if (connected_ && first_tick_received_ && 
                        venueHealth_.getRecentRejects() <= VenueHealth::MAX_RECENT_REJECTS / 2 &&
                        venueHealth_.getProtocolErrors() == 0 &&
                        last_rx > 0 && (now - last_rx) < VenueHealth::STALE_DATA_NS) {
                        std::cout << "[CfdEngine] Conditions recovered → HEALTHY" << std::endl;
                        venueHealth_.setHealthy();
                    }
                }
            }
            
            // Update connection status from FIX client
            bool wasConnected = connected_.load();
            connected_ = fixClient_.isLoggedOn();
            if (wasConnected && !connected_) {
                onFIXLogout();
            }
        }
        
        std::cout << "[CfdEngine] Loop stopped" << std::endl;
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
    
    inline void submitOrder(const char* symbol, int8_t side, double qty) {
        if (!riskGuard_.checkOrder(qty, side)) {
            stats_.orders_rejected.fetch_add(1, std::memory_order_relaxed);
            venueHealth_.onOrderReject();
            return;
        }
        
        // Track order sent (backpressure via VenueHealth)
        venueHealth_.onOrderSent(nowNs());
        
        if (orderCallback_) orderCallback_(symbol, side, qty);
        
        // Also send via FIX if connected
        if (connected_ && fixClient_.isLoggedOn()) {
            static int ordId = 1;
            std::string clOrdID = "ORD" + std::to_string(ordId++);
            char fixSide = (side > 0) ? '1' : '2';  // 1=Buy, 2=Sell
            fixClient_.sendNewOrder(clOrdID, symbol, fixSide, qty);
        }
        
        stats_.orders_sent.fetch_add(1, std::memory_order_relaxed);
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
    std::atomic<bool> connected_;
    std::atomic<bool> first_tick_received_;  // Track first tick for HEALTHY transition
    GlobalKillSwitch* kill_switch_;
    Arbiter* arbiter_;
    std::thread engineThread_;
    
    FIXConfig fixConfig_;
    CTraderFIXClient fixClient_;
    
    // v6.4: Unified VenueHealth (from venue/VenueHealth.hpp)
    VenueHealth venueHealth_;
    
    std::vector<std::string> forex_symbols_;
    std::vector<std::string> metals_symbols_;
    std::vector<std::string> indices_symbols_;
    
    CentralMicroEngine centralMicro_;
    
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
    CfdEngineStats stats_;
    std::function<void(const char*, int8_t, double)> orderCallback_;
};

} // namespace Chimera
