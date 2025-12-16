// src/core/logger.cpp
#include "core/logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>

Logger& Logger::get() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    stop();
}

void Logger::start(const std::string& filename) {
    m_log_file.open(filename, std::ios::app);
    if (!m_log_file.is_open()) {
        std::cerr << "FATAL: Could not open log file: " << filename << std::endl;
        return;
    }
    m_running = true;
    m_thread = std::thread(&Logger::run, this);
}

void Logger::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_log_file.is_open()) {
        m_log_file.close();
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    if (!m_running) return;
    m_queue.enqueue({level, message, std::chrono::high_resolution_clock::now()});
}

void Logger::run() {
    LogEntry entry;
    while (m_running || m_queue.try_dequeue(entry)) {
        if (m_queue.try_dequeue(entry)) {
            std::string formatted = format_message(entry);
            std::lock_guard<std::mutex> lock(m_file_mutex);
            m_log_file << formatted << std::endl;
            std::cout << formatted << std::endl; // Also print to stdout
        } else {
            std::this_thread::yield();
        }
    }
}

std::string Logger::format_message(const LogEntry& entry) {
    auto time_t = std::chrono::high_resolution_clock::to_time_t(entry.timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(entry.timestamp.time_since_epoch()) % 1000;
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    ss << " [" << (entry.level == LogLevel::INFO ? "INFO" : entry.level == LogLevel::WARN ? "WARN" : "ERROR") << "] ";
    ss << entry.message;
    return ss.str();
}
