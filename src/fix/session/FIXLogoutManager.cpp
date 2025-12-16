#include "FIXLogoutManager.hpp"
#include "../codec/FIXParser.hpp"

namespace Omega {

FIXLogoutManager::FIXLogoutManager()
    : tr(nullptr), pending(false) {}

FIXLogoutManager::~FIXLogoutManager() {}

void FIXLogoutManager::attach(FIXTransport* t) {
    tr = t;
    if (tr) {
        tr->setRxCallback([this](const std::string& msg){
            onInbound(msg);
        });
    }
}

void FIXLogoutManager::setCallback(std::function<void()> cb) {
    onLogout = cb;
}

void FIXLogoutManager::setReasonCallback(std::function<void(const std::string&)> cb) {
    onLogoutReason = cb;
}

bool FIXLogoutManager::logoutPending() const {
    return pending;
}

void FIXLogoutManager::requestLogout() {
    if (!tr) return;
    pending = true;
    tr->sendRaw("8=FIX.4.4\x01""35=5\x01");
}

void FIXLogoutManager::requestLogout(const std::string& reason) {
    if (!tr) return;
    pending = true;
    std::string msg = "8=FIX.4.4\x01""35=5\x01""58=" + reason + "\x01";
    tr->sendRaw(msg);
}

void FIXLogoutManager::onInbound(const std::string& msg) {
    FIXParser p;
    auto tags = p.parse(msg);
    
    auto it = tags.find("35");
    if (it != tags.end() && it->second == "5") {
        pending = false;
        
        auto reasonIt = tags.find("58");
        if (reasonIt != tags.end() && onLogoutReason) {
            onLogoutReason(reasonIt->second);
        }
        
        if (onLogout) onLogout();
    }
}

} // namespace Omega
