#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../../execution/OrderIntent.hpp"
#include "../../supervisor/ExecutionSupervisor.hpp"

namespace Omega {

class UnifiedRouter {
public:
    UnifiedRouter();

    void setExecutionSupervisor(ExecutionSupervisor* sup);

    bool routeSpot(const OrderIntent& o);
    bool routeCFD (const OrderIntent& o);
    bool routeFutures(const OrderIntent& o);

private:
    std::mutex lock;
    ExecutionSupervisor* exec = nullptr;
};

} // namespace Omega
