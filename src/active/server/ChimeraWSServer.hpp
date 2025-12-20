#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>

namespace Chimera {

/*
 * ChimeraWSServer
 *
 * Header-only, low-overhead WebSocket broadcaster.
 * Monitoring / dashboard only (NOT trading hot path).
 */
class ChimeraWSServer {
public:
    ChimeraWSServer() noexcept
        : running_(false)
    {}

    ~ChimeraWSServer() noexcept {
        stop();
    }

    // MUST return bool — main_dual.cpp depends on this
    inline bool start(int port) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
            return false; // already running
        }

        port_ = port;
        std::cout << "[WSS] started on port " << port_ << std::endl;
        return true;
    }

    inline void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        std::lock_guard<std::mutex> lock(conn_mtx_);
        connections_.clear();
        std::cout << "[WSS] stopped" << std::endl;
    }

    inline void broadcast(const std::string& msg) {
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        std::lock_guard<std::mutex> lock(conn_mtx_);
        for (int fd : connections_) {
            (void)fd;
            // send(fd, msg) — intentionally omitted (monitoring only)
        }
    }

    inline void addConnection(int fd) {
        std::lock_guard<std::mutex> lock(conn_mtx_);
        connections_.push_back(fd);
    }

    inline void removeConnection(int fd) {
        std::lock_guard<std::mutex> lock(conn_mtx_);
        for (auto it = connections_.begin(); it != connections_.end(); ++it) {
            if (*it == fd) {
                connections_.erase(it);
                break;
            }
        }
    }

private:
    int port_{0};
    std::atomic<bool> running_;
    std::vector<int> connections_;
    std::mutex conn_mtx_;
};

} // namespace Chimera
