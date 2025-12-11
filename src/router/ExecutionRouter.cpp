#include "ExecutionRouter.hpp"
#include <iostream>
#include <chrono>

namespace Omega {

static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ExecutionRouter::ExecutionRouter()
    : symbol("BTCUSDT"),
      mode("sim"),
      defaultQty(0.001),
      orderCount_(0),
      fillCount_(0),
      orderIdCounter_(1)
{
}

ExecutionRouter::~ExecutionRouter() {}

void ExecutionRouter::setSymbol(const std::string& s) {
    symbol = s;
}

void ExecutionRouter::setMode(const std::string& m) {
    mode = m;
}

void ExecutionRouter::setDefaultQty(double q) {
    defaultQty = q;
}

void ExecutionRouter::setExecutionCallback(std::function<void(const ExecutionResult&)> cb) {
    onExecution = cb;
}

ExecutionResult ExecutionRouter::route(double signal, const UnifiedTick& t) {
    ExecutionResult result;
    
    if (mode == "live") {
        result = routeLive(signal, t);
    } else {
        result = routeSim(signal, t);
    }
    
    if (onExecution) {
        onExecution(result);
    }
    
    return result;
}

ExecutionResult ExecutionRouter::routeLive(double signal, const UnifiedTick& t) {
    ExecutionResult r;
    r.ts = now_us();
    r.qty = defaultQty;
    
    // In live mode, this would connect to actual exchange
    // For now, just log the intent
    
    std::cout << "[LIVE ROUTE] " << symbol
              << " signal=" << signal
              << " bid=" << t.bid
              << " ask=" << t.ask << "\n";
    
    orderCount_++;
    
    r.orderId = "LIVE_" + std::to_string(orderIdCounter_++);
    r.side = signal > 0 ? "BUY" : "SELL";
    r.price = signal > 0 ? t.ask : t.bid;
    r.success = false;  // Would be true after actual execution
    
    return r;
}

ExecutionResult ExecutionRouter::routeSim(double signal, const UnifiedTick& t) {
    ExecutionResult r;
    r.ts = now_us();
    r.qty = defaultQty;
    
    std::cout << "[SIM ROUTE]  " << symbol
              << " signal=" << signal
              << " bid=" << t.bid
              << " ask=" << t.ask << "\n";
    
    orderCount_++;
    fillCount_++;  // Sim always fills
    
    r.orderId = "SIM_" + std::to_string(orderIdCounter_++);
    r.side = signal > 0 ? "BUY" : "SELL";
    r.price = signal > 0 ? t.ask : t.bid;
    r.success = true;
    
    return r;
}

} // namespace Omega
