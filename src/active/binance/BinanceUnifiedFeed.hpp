#pragma once

#include <functional>
#include <string>
#include <atomic>
#include <iostream>

#include "BinanceWebSocketController.hpp"
#include "../market/Tick.hpp"

namespace Chimera {

/*
 * BinanceUnifiedFeed
 *
 * Thin coordinator over WebSocket controller.
 * Header-only by design (non-hot-path orchestration).
 */
class BinanceUnifiedFeed {
public:
    using TickCallback  = std::function<void(const Tick&)>;
    using StateCallback = std::function<void(bool)>;

    BinanceUnifiedFeed() noexcept
        : running_(false)
    {}

    inline void setTickCallback(TickCallback cb) {
        tick_cb_ = std::move(cb);
    }

    inline void setStateCallback(StateCallback cb) {
        state_cb_ = std::move(cb);
    }

    inline bool start(const std::string& symbol) {
        if (running_.exchange(true)) {
            return false;
        }

        std::cout << "[BinanceFeed] start " << symbol << std::endl;

        if (state_cb_) {
            state_cb_(true);
        }
        return true;
    }

    inline void stop() {
        if (!running_.exchange(false)) {
            return;
        }

        if (state_cb_) {
            state_cb_(false);
        }

        std::cout << "[BinanceFeed] stopped" << std::endl;
    }

private:
    std::atomic<bool> running_;
    TickCallback  tick_cb_;
    StateCallback state_cb_;
};

} // namespace Chimera
