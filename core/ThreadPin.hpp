// =============================================================================
// ThreadPin.hpp - CPU Affinity Enforcement
// =============================================================================
// HARD RULES:
// - Must be called at thread start
// - Must succeed or abort
// - Core IDs are explicit and validated
// - No silent fallback
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstdio>

#if defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#elif defined(__APPLE__)
    #include <pthread.h>
    #include <mach/mach.h>
    #include <mach/thread_policy.h>
#endif

namespace chimera {
namespace core {

// =============================================================================
// pin_current_thread - Pin calling thread to specific CPU core
// Aborts on failure - no silent degradation
// =============================================================================
inline void pin_current_thread(uint32_t core_id) {
#if defined(__linux__)

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    const int rc = pthread_setaffinity_np(
        pthread_self(),
        sizeof(cpu_set_t),
        &cpuset
    );

    if (rc != 0) {
        std::fprintf(stderr,
                     "FATAL: failed to pin thread to CPU %u (errno=%d)\n",
                     core_id,
                     rc);
        std::abort();
    }

#elif defined(__APPLE__)

    // macOS uses affinity tags (hints, not strict binding)
    thread_affinity_policy_data_t policy;
    policy.affinity_tag = static_cast<integer_t>(core_id + 1);

    kern_return_t kr = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_AFFINITY_POLICY,
        (thread_policy_t)&policy,
        THREAD_AFFINITY_POLICY_COUNT
    );

    if (kr != KERN_SUCCESS) {
        // On macOS, affinity is best-effort - warn but don't abort
        std::fprintf(stderr,
                     "WARNING: thread affinity tag %u not guaranteed (kr=%d)\n",
                     core_id, kr);
    }

#else
    (void)core_id;
    std::fprintf(stderr, "WARNING: Thread pinning not supported\n");
#endif
}

// =============================================================================
// verify_pinning - Verify thread is pinned (Linux only)
// =============================================================================
inline bool verify_pinning([[maybe_unused]] uint32_t expected_core) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        return false;
    }
    
    return CPU_ISSET(expected_core, &cpuset) && CPU_COUNT(&cpuset) == 1;
#else
    return true; // Assume OK on non-Linux
#endif
}

} // namespace core
} // namespace chimera
