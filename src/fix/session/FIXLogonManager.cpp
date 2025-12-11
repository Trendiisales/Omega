#include "FIXLogonManager.hpp"
#include "../codec/FIXParser.hpp"

namespace Omega {

FIXLogonManager::FIXLogonManager()
    : tr(nullptr), sessionState(SessionState::DISCONNECTED) {}

FIXLogonManager::~FIXLogonManager() {}

void FIXLogonManager::attach(FIXTransport* t) {
    tr = t;
    if (!tr) return;

    tr->setRxCallback([this](const std::string& msg){
        onRx(msg);
    });
    
    tr->setStateCallback([this](bool up){
        if (!up) {
            sessionState = SessionState::DISCONNECTED;
        }
    });
}

void FIXLogonManager::setLogonCallback(std::function<void()> cb) {
    onLogon = cb;
}

void FIXLogonManager::setLogoutCallback(std::function<void()> cb) {
    onLogout = cb;
}

void FIXLogonManager::setRejectCallback(std::function<void(const std::string&)> cb) {
    onReject = cb;
}

SessionState FIXLogonManager::state() const {
    return sessionState;
}

bool FIXLogonManager::isLoggedIn() const {
    return sessionState == SessionState::LOGGED_IN;
}

void FIXLogonManager::sendLogon(const std::string& user,
                                const std::string& pw)
{
    if (!tr) return;

    sessionState = SessionState::LOGGING_IN;

    std::string msg =
        "8=FIX.4.4\x01"
        "35=A\x01"
        "98=0\x01"           // EncryptMethod = None
        "108=30\x01"         // HeartBtInt = 30s
        "553=" + user + "\x01"
        "554=" + pw + "\x01";

    tr->sendRaw(msg);
}

void FIXLogonManager::sendLogout() {
    if (!tr) return;

    sessionState = SessionState::LOGGING_OUT;

    std::string msg =
        "8=FIX.4.4\x01"
        "35=5\x01";

    tr->sendRaw(msg);
}

void FIXLogonManager::onRx(const std::string& msg) {
    FIXParser p;
    auto tags = p.parse(msg);
    
    auto it = tags.find("35");
    if (it == tags.end()) return;
    
    std::string type = it->second;

    if (type == "A") {  // Logon
        sessionState = SessionState::LOGGED_IN;
        if (onLogon) onLogon();
    }
    else if (type == "5") {  // Logout
        sessionState = SessionState::DISCONNECTED;
        if (onLogout) onLogout();
    }
    else if (type == "3") {  // Reject
        auto textIt = tags.find("58");
        std::string reason = (textIt != tags.end()) ? textIt->second : "Unknown";
        if (onReject) onReject(reason);
    }
}

} // namespace Omega
