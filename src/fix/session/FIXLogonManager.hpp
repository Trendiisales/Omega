#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <atomic>
#include "../transport/FIXTransport.hpp"

namespace Omega {

enum class SessionState {
    DISCONNECTED,
    LOGGING_IN,
    LOGGED_IN,
    LOGGING_OUT
};

class FIXLogonManager {
public:
    FIXLogonManager();
    ~FIXLogonManager();

    void attach(FIXTransport* t);

    void setLogonCallback(std::function<void()> cb);
    void setLogoutCallback(std::function<void()> cb);
    void setRejectCallback(std::function<void(const std::string&)> cb);

    void sendLogon(const std::string& username,
                   const std::string& password);

    void sendLogout();
    
    SessionState state() const;
    bool isLoggedIn() const;

private:
    void onRx(const std::string& msg);

private:
    FIXTransport* tr;
    std::atomic<SessionState> sessionState;
    std::function<void()> onLogon;
    std::function<void()> onLogout;
    std::function<void(const std::string&)> onReject;
};

} // namespace Omega
