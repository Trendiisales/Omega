#include "UnifiedRouter.hpp"

namespace Omega {

UnifiedRouter::UnifiedRouter()
    : exec(nullptr) {}

void UnifiedRouter::setExecutionSupervisor(ExecutionSupervisor* sup) {
    std::lock_guard<std::mutex> g(lock);
    exec = sup;
}

bool UnifiedRouter::routeSpot(const OrderIntent& o) {
    std::lock_guard<std::mutex> g(lock);
    if (!exec) return false;
    exec->route(o);
    return true;
}

bool UnifiedRouter::routeCFD(const OrderIntent& o) {
    std::lock_guard<std::mutex> g(lock);
    if (!exec) return false;
    exec->route(o);
    return true;
}

bool UnifiedRouter::routeFutures(const OrderIntent& o) {
    std::lock_guard<std::mutex> g(lock);
    if (!exec) return false;
    exec->route(o);
    return true;
}

} // namespace Omega
