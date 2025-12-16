// =============================================================================
// Logger.cpp - HFT-Safe Binary Event Logger Implementation
// =============================================================================
#include "Logger.hpp"

#include <cstdio>
#include <thread>
#include <chrono>

namespace chimera {
namespace core {

static const char* level_to_str(uint8_t lvl) noexcept {
    switch (lvl) {
        case 0: return "DEBUG";
        case 1: return "INFO ";
        case 2: return "WARN ";
        case 3: return "ERROR";
        case 4: return "FATAL";
        default: return "?????";
    }
}

Logger::Logger() noexcept
    : running_(false)
    , events_logged_(0)
    , events_dropped_(0)
{
}

Logger::~Logger() noexcept {
    stop();
}

void Logger::start() noexcept {
    if (running_.load(std::memory_order_acquire)) return;
    
    running_.store(true, std::memory_order_release);
    std::thread(&Logger::run, this).detach();
}

void Logger::stop() noexcept {
    running_.store(false, std::memory_order_release);
    // Give logger thread time to flush
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void Logger::run() noexcept {
    LogEvent ev;

    while (running_.load(std::memory_order_acquire)) {
        while (queue_.pop(ev)) {
            std::fprintf(
                stdout,
                "[%llu] T%u %s code=%u a=%llu b=%llu c=%llu\n",
                (unsigned long long)ev.ts_ns,
                ev.thread_id,
                level_to_str(ev.level),
                ev.code,
                (unsigned long long)ev.a,
                (unsigned long long)ev.b,
                (unsigned long long)ev.c
            );
            events_logged_.fetch_add(1, std::memory_order_relaxed);
        }

        // cold-path sleep; NEVER used by hot path
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Final drain
    while (queue_.pop(ev)) {
        std::fprintf(
            stdout,
            "[%llu] T%u %s code=%u a=%llu b=%llu c=%llu\n",
            (unsigned long long)ev.ts_ns,
            ev.thread_id,
            level_to_str(ev.level),
            ev.code,
            (unsigned long long)ev.a,
            (unsigned long long)ev.b,
            (unsigned long long)ev.c
        );
        events_logged_.fetch_add(1, std::memory_order_relaxed);
    }
    
    std::fflush(stdout);
}

} // namespace core
} // namespace chimera
