// =============================================================================
// Logger.hpp - HFT-Safe Binary Event Logger
// =============================================================================
// Hot path:
//   - enqueue fixed-size binary event
//   - no allocation
//   - no formatting
//   - no IO
//
// Cold path:
//   - dedicated thread
//   - formats + writes
// =============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "SPSCQueue.hpp"

namespace chimera {
namespace core {

// =============================================================================
// Log Level
// =============================================================================
enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    FATAL = 4
};

// =============================================================================
// Log Event (fixed 48 bytes, cache-friendly)
// =============================================================================
struct LogEvent {
    uint64_t ts_ns;       // 8: monotonic timestamp
    uint64_t a;           // 8: user data
    uint64_t b;           // 8: user data
    uint64_t c;           // 8: user data
    uint32_t thread_id;   // 4: logical thread id
    uint16_t code;        // 2: user-defined event code
    uint8_t  level;       // 1: LogLevel
    uint8_t  _pad[5];     // 5: padding to 48 bytes
};

static_assert(sizeof(LogEvent) == 48, "LogEvent size must be fixed at 48 bytes");

// =============================================================================
// Logger
// =============================================================================
class Logger final {
public:
    static constexpr std::size_t QUEUE_CAPACITY = 1 << 14; // 16384 events

    Logger() noexcept;
    ~Logger() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void start() noexcept;
    void stop() noexcept;

    // =========================================================================
    // Hot-path logging
    // Returns false if queue is full (event dropped)
    // =========================================================================
    inline bool log(uint64_t ts_ns,
                    uint32_t thread_id,
                    LogLevel level,
                    uint16_t code,
                    uint64_t a = 0,
                    uint64_t b = 0,
                    uint64_t c = 0) noexcept
    {
        LogEvent ev;
        ev.ts_ns     = ts_ns;
        ev.a         = a;
        ev.b         = b;
        ev.c         = c;
        ev.thread_id = thread_id;
        ev.code      = code;
        ev.level     = static_cast<uint8_t>(level);
        // Zero padding array
        for (int i = 0; i < 5; ++i) ev._pad[i] = 0;

        return queue_.push(ev);
    }

    // Statistics
    uint64_t events_logged() const noexcept { 
        return events_logged_.load(std::memory_order_relaxed); 
    }
    uint64_t events_dropped() const noexcept { 
        return events_dropped_.load(std::memory_order_relaxed); 
    }

private:
    void run() noexcept;

    std::atomic<bool> running_;
    SPSCQueue<LogEvent, QUEUE_CAPACITY> queue_;
    std::atomic<uint64_t> events_logged_;
    std::atomic<uint64_t> events_dropped_;
};

} // namespace core
} // namespace chimera
