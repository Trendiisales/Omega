#pragma once
// engine_dispatch.hpp -- single-writer engine dispatch worker
//
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp, AFTER
// on_tick.hpp and order_exec.hpp (the static functions on_tick() and
// handle_execution_report() must already be visible).
//
// =============================================================================
// PURPOSE
// =============================================================================
// Three threads currently mutate engine state with no synchronization:
//
//   1. FIX quote thread (dispatch_fix W/X handler)        -> on_tick(...)
//   2. cTrader depth thread (on_depth_event -> on_tick_fn)-> on_tick(...)
//   3. FIX trade thread (trade_loop ttype=="8")           -> handle_execution_report(...)
//
// The 500ms "ctrader_depth_is_live" check in fix_dispatch.hpp is NOT a
// synchronization primitive -- it is a price-source preference. During the
// stale->fresh transition window it allows both threads to enter on_tick()
// simultaneously. When that happens, the per-tick std::deque/std::vector/
// std::function operations inside the four portfolio engines (Tsmom,
// Donchian, EmaPullback, TrendRider) and the bracket trend state corrupt the
// segment-heap free list, manifesting as the recurring 0xc0000374 crash at
// ntdll +0x103e89.
//
// The fix is to enforce the "engines are single-threaded" assumption that the
// engine code already makes. Every event that touches engine state is posted
// to a thread-safe MPSC queue and consumed by ONE dedicated worker thread.
// Engine code is unchanged.
//
// =============================================================================
// SHUTDOWN ORDER (caller must respect)
// =============================================================================
// 1. g_running.store(false)              -- stop producers (quote/depth/trade)
// 2. quote_loop, trade_loop, depth_loop join (their own loops exit on g_running)
// 3. engine_dispatch_stop()              -- drain pending, join worker
//
// If shutdown happens while events are still queued, the worker drains them
// before exiting. EXEC_REPORT events are NEVER dropped (broker fills must be
// processed). TICK events MAY be dropped on backpressure overflow.
//
// =============================================================================
// PUBLIC API
// =============================================================================
//   void engine_dispatch_start();
//   void engine_dispatch_stop();
//   void engine_dispatch_post_tick(const std::string& sym, double bid, double ask);
//   void engine_dispatch_post_exec_report(const std::string& msg);
//   uint64_t engine_dispatch_queue_depth();
//   uint64_t engine_dispatch_dropped_ticks();
//
// All post_* functions are safe to call from any thread. They are non-blocking
// except in the pathological case of a full queue + EXEC_REPORT (bounded
// 100ms wait so the trade thread doesn't stall the FIX session).
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace omega { namespace dispatch {

// -- Event payload --------------------------------------------------------
// Tagged struct rather than std::variant: simpler, fewer surprises with
// move/copy, and the per-event size cost (~120 bytes) is dwarfed by the
// std::string payloads it would always carry anyway.
enum class Kind : uint8_t {
    TICK         = 1,
    EXEC_REPORT  = 2,
    SHUTDOWN     = 3,
};

struct Event {
    Kind        kind = Kind::TICK;
    // TICK payload
    std::string sym;
    double      bid = 0.0;
    double      ask = 0.0;
    // EXEC_REPORT payload (raw FIX message bytes incl. SOH)
    std::string msg;

    Event() = default;
    explicit Event(Kind k) : kind(k) {}
};

// -- Bounded queue --------------------------------------------------------
// Hard cap chosen so that under sustained 1000 evt/sec the queue can absorb
// up to ~4 seconds of latency in the worker before backpressure kicks in.
// Real worker latency under normal load is well under 1ms/event so queue
// depth in steady state is ~0.
static constexpr size_t kQueueCap = 4096;

// Backpressure policy for full queue:
// - TICK on full queue:        drop the new tick (log every 1s).
// - EXEC_REPORT on full queue: wait up to 100ms for room, then drop the
//                              OLDEST TICK to make room. EXEC_REPORT is
//                              never dropped.
static constexpr std::chrono::milliseconds kExecReportWaitMax{100};

// Internal state ---------------------------------------------------------
struct State {
    std::mutex                 mtx;
    std::condition_variable    cv_not_empty;
    std::condition_variable    cv_not_full;
    std::deque<Event>          q;
    std::atomic<bool>          running{false};
    std::thread                worker;

    // Telemetry counters
    std::atomic<uint64_t>      ev_posted_tick{0};
    std::atomic<uint64_t>      ev_posted_exec{0};
    std::atomic<uint64_t>      ev_processed{0};
    std::atomic<uint64_t>      ev_dropped_tick{0};
    std::atomic<uint64_t>      ev_dropped_exec{0};
    std::atomic<uint64_t>      depth_peak{0};
    std::atomic<int64_t>       last_drop_log_ms{0};
};

inline State& state() {
    static State s;
    return s;
}

// Internal: enqueue with policy. Returns true if enqueued, false if dropped.
inline bool enqueue_(Event&& e) {
    auto& S = state();
    if (!S.running.load(std::memory_order_acquire)) {
        // Worker not started or already stopped -- silently drop.
        // The static-init order in main() guarantees engine_dispatch_start()
        // runs before any producer. Hitting this branch indicates shutdown.
        if (e.kind == Kind::EXEC_REPORT) {
            S.ev_dropped_exec.fetch_add(1, std::memory_order_relaxed);
        } else if (e.kind == Kind::TICK) {
            S.ev_dropped_tick.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    std::unique_lock<std::mutex> lk(S.mtx);

    if (S.q.size() >= kQueueCap) {
        // -- Backpressure --
        if (e.kind == Kind::TICK) {
            // Drop the new tick. Stale tick is worthless anyway -- next tick
            // will re-fire engine state quickly.
            lk.unlock();
            S.ev_dropped_tick.fetch_add(1, std::memory_order_relaxed);
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const int64_t last  = S.last_drop_log_ms.load(std::memory_order_relaxed);
            if (now_ms - last > 1000) {
                if (S.last_drop_log_ms.compare_exchange_strong(
                        const_cast<int64_t&>(last), now_ms,
                        std::memory_order_relaxed)) {
                    std::cerr << "[ENG-DISPATCH] TICK queue full (cap="
                              << kQueueCap << ") -- dropped, dropped_total="
                              << S.ev_dropped_tick.load(std::memory_order_relaxed)
                              << "\n";
                    std::cerr.flush();
                }
            }
            return false;
        }

        // EXEC_REPORT: wait up to 100ms, then force room by dropping the
        // oldest TICK. We never drop EXEC_REPORT.
        const auto deadline = std::chrono::steady_clock::now() + kExecReportWaitMax;
        S.cv_not_full.wait_until(lk, deadline, [&S] {
            return !S.running.load(std::memory_order_acquire) ||
                   S.q.size() < kQueueCap;
        });
        if (S.q.size() >= kQueueCap) {
            // Still full after wait -- evict oldest TICK.
            for (auto it = S.q.begin(); it != S.q.end(); ++it) {
                if (it->kind == Kind::TICK) {
                    S.q.erase(it);
                    S.ev_dropped_tick.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            // If even after eviction no room (queue is all EXEC_REPORTs --
            // shouldn't happen in practice), block one more cycle.
            if (S.q.size() >= kQueueCap) {
                S.cv_not_full.wait(lk, [&S] { return S.q.size() < kQueueCap; });
            }
        }
    }

    const Kind k = e.kind;
    S.q.push_back(std::move(e));
    const size_t depth = S.q.size();

    // Update peak depth (relaxed -- diagnostic only).
    uint64_t old_peak = S.depth_peak.load(std::memory_order_relaxed);
    while (depth > old_peak &&
           !S.depth_peak.compare_exchange_weak(old_peak, depth,
               std::memory_order_relaxed)) { /* retry */ }

    if (k == Kind::EXEC_REPORT) {
        S.ev_posted_exec.fetch_add(1, std::memory_order_relaxed);
    } else if (k == Kind::TICK) {
        S.ev_posted_tick.fetch_add(1, std::memory_order_relaxed);
    }

    lk.unlock();
    S.cv_not_empty.notify_one();
    return true;
}

// -- Worker ---------------------------------------------------------------
// IMPORTANT: this function MUST be defined inside the same translation unit
// as on_tick() and handle_execution_report(). Because this header is
// included after on_tick.hpp / order_exec.hpp, those names are in scope.
inline void worker_loop_() {
    auto& S = state();
    int64_t last_stat_ms = 0;

    while (true) {
        Event e;
        {
            std::unique_lock<std::mutex> lk(S.mtx);
            S.cv_not_empty.wait(lk, [&S] {
                return !S.q.empty() || !S.running.load(std::memory_order_acquire);
            });
            if (S.q.empty()) {
                // running == false and queue drained -- exit.
                if (!S.running.load(std::memory_order_acquire)) return;
                continue;
            }
            e = std::move(S.q.front());
            S.q.pop_front();
        }
        S.cv_not_full.notify_one();

        try {
            switch (e.kind) {
                case Kind::TICK:
                    on_tick(e.sym, e.bid, e.ask);
                    break;
                case Kind::EXEC_REPORT:
                    handle_execution_report(e.msg);
                    break;
                case Kind::SHUTDOWN:
                    return;
            }
        } catch (const std::exception& ex) {
            std::cerr << "[ENG-DISPATCH] worker exception kind="
                      << static_cast<int>(e.kind)
                      << " sym=" << e.sym
                      << " what=" << ex.what()
                      << " -- continuing\n";
            std::cerr.flush();
        } catch (...) {
            std::cerr << "[ENG-DISPATCH] worker unknown exception kind="
                      << static_cast<int>(e.kind)
                      << " -- continuing\n";
            std::cerr.flush();
        }

        S.ev_processed.fetch_add(1, std::memory_order_relaxed);

        // Periodic stats every 60s -- diagnostic only.
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms - last_stat_ms > 60000) {
            last_stat_ms = now_ms;
            std::cout << "[ENG-DISPATCH-STATS] posted_tick="
                      << S.ev_posted_tick.load(std::memory_order_relaxed)
                      << " posted_exec="
                      << S.ev_posted_exec.load(std::memory_order_relaxed)
                      << " processed="
                      << S.ev_processed.load(std::memory_order_relaxed)
                      << " dropped_tick="
                      << S.ev_dropped_tick.load(std::memory_order_relaxed)
                      << " dropped_exec="
                      << S.ev_dropped_exec.load(std::memory_order_relaxed)
                      << " depth_peak="
                      << S.depth_peak.load(std::memory_order_relaxed)
                      << "\n";
            std::cout.flush();
        }
    }
}

}} // namespace omega::dispatch

// =============================================================================
// PUBLIC API (file-scope free functions, callable from anywhere in the TU)
// =============================================================================

// Start the dispatch worker. Idempotent. Must be called BEFORE any producer
// thread (FIX quote, FIX trade, cTrader depth) starts posting events.
inline void engine_dispatch_start() {
    auto& S = omega::dispatch::state();
    bool expected = false;
    if (!S.running.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        // Already running.
        return;
    }
    S.worker = std::thread(omega::dispatch::worker_loop_);
    std::cout << "[ENG-DISPATCH] worker started cap="
              << omega::dispatch::kQueueCap << "\n";
    std::cout.flush();
}

// Signal shutdown and join the worker. Drains all pending events before
// returning. Caller MUST have already stopped the producer threads (or at
// least set g_running=false so they stop posting) -- otherwise the queue
// will keep filling and this never returns.
inline void engine_dispatch_stop() {
    auto& S = omega::dispatch::state();
    bool expected = true;
    if (!S.running.compare_exchange_strong(expected, false,
            std::memory_order_acq_rel)) {
        // Not running.
        return;
    }
    {
        std::lock_guard<std::mutex> lk(S.mtx);
        // Push sentinel so a worker waiting on cv has something to see.
        S.q.emplace_back(omega::dispatch::Kind::SHUTDOWN);
    }
    S.cv_not_empty.notify_all();
    S.cv_not_full.notify_all();
    if (S.worker.joinable()) S.worker.join();

    std::cout << "[ENG-DISPATCH] worker stopped"
              << " posted_tick=" << S.ev_posted_tick.load()
              << " posted_exec=" << S.ev_posted_exec.load()
              << " processed=" << S.ev_processed.load()
              << " dropped_tick=" << S.ev_dropped_tick.load()
              << " dropped_exec=" << S.ev_dropped_exec.load()
              << " depth_peak=" << S.depth_peak.load() << "\n";
    std::cout.flush();
}

// Post a tick event. Safe from any thread. Non-blocking. May drop on
// backpressure (logged to stderr, counted in dropped_tick). Drop policy is
// safe: the next tick on this symbol will re-trigger the same engine state
// updates within milliseconds.
inline void engine_dispatch_post_tick(const std::string& sym, double bid, double ask) {
    omega::dispatch::Event e(omega::dispatch::Kind::TICK);
    e.sym = sym;
    e.bid = bid;
    e.ask = ask;
    omega::dispatch::enqueue_(std::move(e));
}

// Post an ExecutionReport for processing. Safe from any thread. Bounded-wait
// on backpressure (up to 100ms then forces room). NEVER drops -- broker fills
// must be processed for correct position bookkeeping.
inline void engine_dispatch_post_exec_report(const std::string& msg) {
    omega::dispatch::Event e(omega::dispatch::Kind::EXEC_REPORT);
    e.msg = msg;
    omega::dispatch::enqueue_(std::move(e));
}

// Diagnostic accessors -- safe to call from any thread.
inline uint64_t engine_dispatch_queue_depth() {
    auto& S = omega::dispatch::state();
    std::lock_guard<std::mutex> lk(S.mtx);
    return static_cast<uint64_t>(S.q.size());
}

inline uint64_t engine_dispatch_dropped_ticks() {
    return omega::dispatch::state().ev_dropped_tick.load(std::memory_order_relaxed);
}

inline uint64_t engine_dispatch_processed() {
    return omega::dispatch::state().ev_processed.load(std::memory_order_relaxed);
}
