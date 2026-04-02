// ==============================================================================
// OmegaHotReload.hpp
// Watches omega_config.ini for changes and re-applies parameters live.
//
// DESIGN:
//   - Background thread polls file mtime every 2s
//   - If mtime changes, calls load_config() then apply_all_engine_configs()
//   - Zero downtime -- no position closes, no reconnect
//   - Thread-safe: uses the same g_cfg mutex path as normal startup
//
// WHAT IS HOT-RELOADABLE (everything in omega_config.ini):
//   - All risk params: daily_loss_limit, risk_per_trade_usd, max_lot_*, etc.
//   - All engine params: tp_pct, sl_pct, compression_threshold, etc.
//   - Gold stack params: trail distances, lock levels, hold times
//   - Session windows: session_start_utc, session_end_utc
//   - Latency edge params: all thresholds
//
// WHAT IS NOT HOT-RELOADABLE (requires recompile):
//   - FIX connection params (host, port, credentials) -- active socket
//   - mode (SHADOW/LIVE) -- structural, not a param
//   - New engine logic / code changes
//
// USAGE in main.cpp:
//   #include "OmegaHotReload.hpp"
//   // After initial load_config() and apply_all_engine_configs():
//   OmegaHotReload::start("omega_config.ini", []() {
//       load_config("omega_config.ini");
//       apply_all_engine_configs();
//   });
//   // On shutdown:
//   OmegaHotReload::stop();
// ==============================================================================
#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <cstdio>

class OmegaHotReload {
public:
    using ReloadFn = std::function<void()>;

    // Start the background watcher thread.
    // path     : path to omega_config.ini (relative or absolute)
    // on_reload: callback -- called on the watcher thread when file changes.
    //            Must be safe to call from a non-tick thread.
    // poll_ms  : how often to check mtime (default 2000ms)
    static void start(const std::string& path, ReloadFn on_reload,
                      int poll_ms = 2000) {
        if (running_.load()) return;  // already started
        path_      = path;
        on_reload_ = std::move(on_reload);
        poll_ms_   = poll_ms;
        last_mtime_ = get_mtime(path_);
        running_.store(true);
        thread_ = std::thread(watch_loop);
        printf("[HOT-RELOAD] Watching %s (poll=%dms)\n", path.c_str(), poll_ms);
        fflush(stdout);
    }

    static void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        printf("[HOT-RELOAD] Stopped\n");
        fflush(stdout);
    }

private:
    static void watch_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
            if (!running_.load()) break;

            const time_t mtime = get_mtime(path_);
            if (mtime == 0) continue;  // file disappeared -- skip

            if (mtime != last_mtime_) {
                last_mtime_ = mtime;
                printf("[HOT-RELOAD] %s changed -- reloading config...\n",
                       path_.c_str());
                fflush(stdout);
                try {
                    if (on_reload_) on_reload_();
                    printf("[HOT-RELOAD] Config reloaded OK\n");
                } catch (const std::exception& e) {
                    printf("[HOT-RELOAD] ERROR during reload: %s\n", e.what());
                } catch (...) {
                    printf("[HOT-RELOAD] ERROR during reload: unknown exception\n");
                }
                fflush(stdout);
            }
        }
    }

    static time_t get_mtime(const std::string& path) noexcept {
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) return 0;
        return st.st_mtime;
    }

    static inline std::string   path_;
    static inline ReloadFn      on_reload_;
    static inline int           poll_ms_   = 2000;
    static inline time_t        last_mtime_ = 0;
    static inline std::atomic<bool> running_{false};
    static inline std::thread   thread_;
};
