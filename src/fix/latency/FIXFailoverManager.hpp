#pragma once
#include <string>
#include <functional>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace Omega {

enum class FailoverState {
    PRIMARY,
    BACKUP,
    DISCONNECTED
};

class FIXFailoverManager {
public:
    FIXFailoverManager();
    ~FIXFailoverManager();

    void setPrimary(const std::string& host, int port);
    void setBackup(const std::string& host, int port);

    void setFailCallback(std::function<void(const std::string&)> cb);

    void markPrimaryDown();
    void markPrimaryUp();
    void markBackupDown();

    std::string currentHost() const;
    int currentPort() const;
    FailoverState state() const;
    
    bool hasPrimary() const;
    bool hasBackup() const;

private:
    mutable std::mutex lock;
    std::string hostPrimary;
    int portPrimary;

    std::string hostBackup;
    int portBackup;

    std::atomic<FailoverState> currentState;

    std::function<void(const std::string&)> onFail;
};

} // namespace Omega
