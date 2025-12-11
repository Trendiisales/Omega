#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <atomic>
#include "../data/UnifiedTick.hpp"

namespace Omega {

struct ExecutionResult {
    bool success = false;
    std::string orderId;
    std::string side;
    double price = 0.0;
    double qty = 0.0;
    uint64_t ts = 0;
};

class ExecutionRouter {
public:
    ExecutionRouter();
    ~ExecutionRouter();

    void setSymbol(const std::string& s);
    void setMode(const std::string& m);
    void setDefaultQty(double q);

    void setExecutionCallback(std::function<void(const ExecutionResult&)> cb);

    ExecutionResult route(double signal, const UnifiedTick& t);

    // Stats
    int orderCount() const { return orderCount_; }
    int fillCount() const { return fillCount_; }

private:
    ExecutionResult routeLive(double signal, const UnifiedTick& t);
    ExecutionResult routeSim(double signal, const UnifiedTick& t);

private:
    std::string symbol;
    std::string mode;
    double defaultQty;
    
    std::function<void(const ExecutionResult&)> onExecution;
    
    std::atomic<int> orderCount_;
    std::atomic<int> fillCount_;
    std::atomic<uint64_t> orderIdCounter_;
};

} // namespace Omega
