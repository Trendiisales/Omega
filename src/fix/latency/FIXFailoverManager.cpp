#include "FIXFailoverManager.hpp"

namespace Omega {

FIXFailoverManager::FIXFailoverManager()
    : portPrimary(0),
      portBackup(0),
      currentState(FailoverState::DISCONNECTED) {}

FIXFailoverManager::~FIXFailoverManager() {}

void FIXFailoverManager::setPrimary(const std::string& host, int port) {
    std::lock_guard<std::mutex> g(lock);
    hostPrimary = host;
    portPrimary = port;
    if (currentState == FailoverState::DISCONNECTED) {
        currentState = FailoverState::PRIMARY;
    }
}

void FIXFailoverManager::setBackup(const std::string& host, int port) {
    std::lock_guard<std::mutex> g(lock);
    hostBackup = host;
    portBackup = port;
}

void FIXFailoverManager::setFailCallback(std::function<void(const std::string&)> cb) {
    onFail = cb;
}

void FIXFailoverManager::markPrimaryDown() {
    FailoverState prev = currentState.exchange(FailoverState::BACKUP);
    if (prev != FailoverState::BACKUP) {
        if (onFail) onFail("PrimaryDown->Backup");
    }
}

void FIXFailoverManager::markPrimaryUp() {
    FailoverState prev = currentState.exchange(FailoverState::PRIMARY);
    if (prev != FailoverState::PRIMARY) {
        if (onFail) onFail("Backup->Primary");
    }
}

void FIXFailoverManager::markBackupDown() {
    currentState = FailoverState::DISCONNECTED;
    if (onFail) onFail("BackupDown->Disconnected");
}

std::string FIXFailoverManager::currentHost() const {
    std::lock_guard<std::mutex> g(lock);
    FailoverState s = currentState;
    if (s == FailoverState::PRIMARY) return hostPrimary;
    if (s == FailoverState::BACKUP) return hostBackup;
    return hostPrimary;  // default to primary
}

int FIXFailoverManager::currentPort() const {
    std::lock_guard<std::mutex> g(lock);
    FailoverState s = currentState;
    if (s == FailoverState::PRIMARY) return portPrimary;
    if (s == FailoverState::BACKUP) return portBackup;
    return portPrimary;
}

FailoverState FIXFailoverManager::state() const {
    return currentState;
}

bool FIXFailoverManager::hasPrimary() const {
    std::lock_guard<std::mutex> g(lock);
    return !hostPrimary.empty() && portPrimary > 0;
}

bool FIXFailoverManager::hasBackup() const {
    std::lock_guard<std::mutex> g(lock);
    return !hostBackup.empty() && portBackup > 0;
}

} // namespace Omega
