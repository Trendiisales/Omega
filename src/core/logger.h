// include/core/logger.h
#pragma once
#include <string>
#include <fstream>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include "lock_free_queue.h"

enum class LogLevel {
    INFO,
    WARN,
    ERROR
};

struct LogEntry {
    LogLevel level;
    std::string message;
    std::chrono::high_resolution_clock::time_point timestamp;
};

class Logger {
public:
    static Logger& get();
    void log(LogLevel level, const std::string& message);
    void start(const std::string& filename);
    void stop();

private:
    Logger() = default;
    ~Logger();

    void run();
    std::string format_message(const LogEntry& entry);

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    LockFreeQueue<LogEntry> m_queue;
    std::ofstream m_log_file;
    std::mutex m_file_mutex; // Only for file operations, not on the critical path
};

// Convenience macros
#define LOG_INFO(msg) Logger::get().log(LogLevel::INFO, msg)
#define LOG_WARN(msg) Logger::get().log(LogLevel::WARN, msg)
#define LOG_ERROR(msg) Logger::get().log(LogLevel::ERROR, msg)
