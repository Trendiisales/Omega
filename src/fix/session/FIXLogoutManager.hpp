#pragma once
#include <string>
#include <functional>
#include <atomic>
#include "../transport/FIXTransport.hpp"

namespace Omega {

class FIXLogoutManager {
public:
    FIXLogoutManager();
    ~FIXLogoutManager();

    void attach(FIXTransport* t);
    void setCallback(std::function<void()> cb);
    void setReasonCallback(std::function<void(const std::string&)> cb);

    void requestLogout();
    void requestLogout(const std::string& reason);
    void onInbound(const std::string& msg);
    
    bool logoutPending() const;

private:
    FIXTransport* tr;
    std::function<void()> onLogout;
    std::function<void(const std::string&)> onLogoutReason;
    std::atomic<bool> pending;
};

} // namespace Omega
