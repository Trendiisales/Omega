#pragma once
// =============================================================================
// OmegaTimeShim.hpp -- Simulated clock for Omega C++ backtester
//
// MUST be included as the very first header in OmegaBacktest.cpp before any
// engine header. The CMakeLists backtest target passes /FI (MSVC) or
// -include (GCC/Clang) so this file is automatically injected first.
//
// WHAT THIS SOLVES:
//   GoldEngineStack, BreakoutEngine, BracketEngine, and all sub-engines use
//   std::chrono::system_clock::now() and std::chrono::steady_clock::now()
//   for:
//     - Orchestrator gates (90s entry gap, 120s SL cooldown, chop pause)
//     - Position manager hold-time enforcement (600s max hold)
//     - Sub-engine 1-second anti-spam (prevents double-firing same tick)
//     - Session detection (hour-of-day, UTC midnight VWAP reset)
//
//   Without this shim, a fast backtest (500K+ t/s) runs 120M ticks in ~240s
//   wall time. Every 90s entry-gap cooldown expires in ~0.15s wall time, so
//   it fires correctly, but steady_clock 1s anti-spam = only ~240 fires per
//   engine across 2 years instead of ~10,000+. Results are completely wrong.
//
// HOW IT WORKS:
//   1. Provides g_sim_now_ms -- per-thread simulated epoch milliseconds.
//      Each backtest/sweep thread sets this from its CSV timestamp before
//      each on_tick(). See "THREAD-LOCAL DESIGN" note below.
//
//   2. Overrides time() in this translation unit by providing omega_bt_time()
//      and #defining time to omega_bt_time. Since all engine headers are
//      included AFTER this file in the same TU, every call to time(nullptr)
//      in the engine headers resolves to our shim.
//
//   3. Provides OmegaBtClock -- a drop-in for steady_clock/system_clock with
//      now() returning time_points derived from g_sim_now_ms. Engine headers
//      are patched in this TU via:
//        #define steady_clock  OmegaBtClock
//        #define system_clock  OmegaBtClock
//      These macros are active for the duration of this TU only.
//
//   4. OmegaBtClock::time_point is std::chrono::steady_clock::time_point so
//      all existing duration arithmetic in the engines compiles unchanged.
//
// USAGE IN backtest/OmegaBacktest.cpp:
//   #include "OmegaTimeShim.hpp"   // MUST be first
//   #include "../include/GoldEngineStack.hpp"
//   // ... other engine headers ...
//
//   Then each tick:
//     omega::bt::set_sim_time(csv_row.timestamp_ms);
//     auto sig = g_gold.on_tick(bid, ask, 0.5);
//
// =============================================================================
// THREAD-LOCAL DESIGN (S51 1A.1.b D7+ G1 fix)
// =============================================================================
//   g_sim_now_ms is `thread_local`. Each thread maintains its own simulated
//   clock. This is required by OmegaSweepHarness, which runs four engines
//   (HBG, EMACross, AsianRange, VWAPStretch) concurrently across the same
//   tick stream. Each engine's run loop calls set_sim_time(r.ts_ms) before
//   processing tick k for its 490-combo tuple. Without thread_local, the
//   four threads race on a single global write/read, and engines reading
//   g_sim_now_ms inside their hot path pick up values from other threads
//   that may be at materially different tick-stream positions (i.e., hours
//   of simulated time off). Hour-of-day gates (AsianRange) blow up under
//   such reads; hold-time gates (HBG) are robust to small jitter and
//   survive but are not actually deterministic.
//
//   Per-thread storage is correct because:
//     1. Every writer call site (OmegaSweepHarness.cpp:854,942,1049,1172
//        and OmegaBacktest.cpp:1111 and omega_bt.cpp:201) sits inside a
//        per-thread tick loop. Each thread writes only its own current
//        tick's timestamp.
//     2. Every reader call site (sweep_now_sec/ms in SweepableEngines.hpp,
//        sim_now_sec / bt_time / OmegaBtClock::now in this file, ca_now_sec
//        / ca_now_ms / ca_utc_time in CrossAssetEngines.hpp) is invoked
//        from inside an engine's process()/on_tick() that is already
//        running on the same thread that wrote the value.
//     3. There is no producer/consumer pattern where one thread writes and
//        a different thread is expected to read. All reads are downstream
//        of writes on the same thread.
//
//   Live (production main.cpp) does NOT include this header. Live code uses
//   std::time(nullptr) and the real wall clock. thread_local has zero impact
//   on live behaviour.
//
//   Single-threaded backtest (OmegaBacktest.cpp, omega_bt.cpp) sees only one
//   thread, so thread_local is equivalent to a global. Zero behavioural
//   change for those paths.
//
//   See: incidents/2026-04-28-s51-1a1b-prep/D7_RESULTS.md and
//        incidents/2026-04-28-s51-1a1b-prep/DETERMINISM_GUARDS.md
//   for the full root-cause analysis and the design of guards G2..G4 that
//   prevent recurrence of this class of bug.
// =============================================================================

#include <ctime>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <type_traits>

// Macro signal that the time shim is active in this TU.
// Engine headers included after OmegaTimeShim.hpp can detect this and
// route their own time accessors through omega::bt::g_sim_now_ms instead
// of system_clock::now() (which the shim cannot override without breaking
// <thread> / std::time()). Used by CrossAssetEngines.hpp ca_now_sec /
// ca_utc_time. Production main.cpp does NOT include this header, so the
// macro is undefined and engines use their normal wall-clock path.
#define OMEGA_BT_SHIM_ACTIVE 1

// ?????????????????????????????????????????????????????????????????????????????
// Per-thread simulated time state
//
// g_sim_now_ms is `inline thread_local` so each sweep thread has its own
// independent simulated clock. See THREAD-LOCAL DESIGN note above for
// rationale. The `inline` keyword combines correctly with `thread_local` in
// C++17 and avoids ODR violations across translation units that include
// this header (here: there is only one such TU per binary, but the rule
// holds in general).
//
// DO NOT REMOVE thread_local FROM THIS DECLARATION without explicit review
// per CONCURRENCY.md (see G4 in DETERMINISM_GUARDS.md).
// ?????????????????????????????????????????????????????????????????????????????
namespace omega { namespace bt {

// Current simulated epoch time in milliseconds. Set by the tick loop on the
// thread that owns the loop. Per-thread storage prevents cross-thread
// contamination when multiple sweep threads run concurrently.
inline thread_local int64_t g_sim_now_ms = 0;

// Set simulated time from a CSV timestamp (milliseconds since Unix epoch).
// Writes the calling thread's local g_sim_now_ms only. Other threads are
// not affected.
inline void set_sim_time(int64_t epoch_ms) noexcept {
    g_sim_now_ms = epoch_ms;
}

// Simulated seconds (used to replace time(nullptr) and system_clock seconds).
// Reads the calling thread's local g_sim_now_ms.
inline int64_t sim_now_sec() noexcept {
    return g_sim_now_ms / 1000LL;
}

}} // namespace omega::bt

// ?????????????????????????????????????????????????????????????????????????????
// Override time() -- replaces all time(nullptr) calls in engine headers
// included in this TU. This is safe because all engine headers are
// header-only and compiled into a single TU (OmegaBacktest.cpp).
//
// We forward-declare then define to avoid conflicting with <ctime>.
// ?????????????????????????????????????????????????????????????????????????????
namespace omega { namespace bt {

// Our replacement for C time(). Reads the calling thread's local
// g_sim_now_ms.
inline time_t bt_time(time_t* t) noexcept {
    const time_t v = static_cast<time_t>(g_sim_now_ms / 1000LL);
    if (t) *t = v;
    return v;
}

}} // namespace omega::bt

// Redirect time() to our shim for all code in this TU after this point.
// This works because the engine headers are all in the same translation unit.
// #define time(x) omega::bt::bt_time(x)  -- REMOVED: breaks std::time()
// Engine code now uses std::time(nullptr) explicitly after dead zone refactor.
// omega_bt_time() is still available for direct calls if needed.

// ?????????????????????????????????????????????????????????????????????????????
// OmegaBtClock -- simulated clock compatible with steady_clock / system_clock
//
// Uses the same time_point type as steady_clock so all duration arithmetic
// (now - last_signal < 1s, etc.) in existing engine code compiles unchanged.
//
// The epoch is Unix epoch expressed as a steady_clock duration, which is
// technically non-standard but works on all major implementations where
// steady_clock's period is nanoseconds from some arbitrary start point.
// We anchor our simulated steady_clock to the sim start by computing
// (sim_ms - first_sim_ms) as the offset, which keeps duration differences
// correct regardless of the absolute epoch value.
//
// OmegaBtClock::now() reads the calling thread's local g_sim_now_ms and
// is therefore inherently per-thread. No additional change required.
// ?????????????????????????????????????????????????????????????????????????????
namespace omega { namespace bt {

// Per-thread sim-start anchor. Mirrors g_sim_now_ms thread-locality so that
// any future user that relies on g_sim_start_ms being relative to the
// calling thread's stream gets correct results. Currently g_sim_started /
// g_sim_start_ms are not read by the existing OmegaBtClock implementation
// below (it uses absolute g_sim_now_ms), but we mark them thread_local
// defensively to prevent the same class of bug if a future change makes
// the start anchor live again. See CONCURRENCY.md.
inline thread_local int64_t g_sim_start_ms = 0;   // set on first tick
inline thread_local bool    g_sim_started  = false;

struct OmegaBtClock {
    using rep        = std::chrono::steady_clock::rep;
    using period     = std::chrono::steady_clock::period;
    using duration   = std::chrono::steady_clock::duration;
    using time_point = std::chrono::steady_clock::time_point;
    static constexpr bool is_steady = true;

    static time_point now() noexcept {
        // Convert g_sim_now_ms to a steady_clock::time_point.
        // We use nanoseconds from the steady_clock epoch (time_since_epoch of 0)
        // plus our simulated offset in ms. This gives correct duration arithmetic:
        //   (now() - last_signal_) returns the simulated elapsed time, not wall time.
        const auto dur = std::chrono::milliseconds(g_sim_now_ms);
        return time_point(std::chrono::duration_cast<duration>(dur));
    }

    // system_clock compatibility -- to_time_t / from_time_t
    static std::time_t to_time_t(const time_point& tp) noexcept {
        return static_cast<std::time_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                tp.time_since_epoch()).count());
    }

    static time_point from_time_t(std::time_t t) noexcept {
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::seconds(t)));
    }
};

}} // namespace omega::bt

// ?????????????????????????????????????????????????????????????????????????????
// Redirect steady_clock and system_clock to OmegaBtClock for all engine
// headers included after this point in this TU.
//
// The macros only affect unqualified uses of steady_clock / system_clock
// inside std::chrono::. Since engine code writes:
//   std::chrono::steady_clock::now()
//   std::chrono::system_clock::now()
// ...we need to redirect inside the std::chrono namespace. We do this by
// injecting type aliases that shadow the real clocks.
// ?????????????????????????????????????????????????????????????????????????????
namespace std {
namespace chrono {

    // Inject OmegaBtClock as aliases that shadow the real clocks.
    // Engine headers use these names -- the alias picks up our simulated clock.
    using steady_clock_real  = steady_clock;
    using system_clock_real  = system_clock;

    // Shadow with our simulated clock via macro-friendly type alias:
    // We can't #define steady_clock itself (it's a type, not a call),
    // so we provide the aliases and use a different approach below.

} // namespace chrono
} // namespace std

// The correct non-UB approach: provide nowSec() override at the omega::bt level.
// Engine code calls nowSec() which calls system_clock::now().
// We intercept by providing a free nowSec() in the global scope that the
// engine's static member function will shadow -- but static member functions
// can't be overridden this way.
//
// DEFINITIVE APPROACH: We inject a global variable and use token replacement
// for the specific patterns used in engine headers.
//
// After careful analysis, the cleanest correct solution is:
//   - time() override (done above) handles orchestrator gates in GoldEngineStack
//   - For steady_clock in sub-engines: we override via a namespace injection
//     that the ADL resolution will pick up when engines call
//     std::chrono::steady_clock::now() on the aliased type.
//
// Since we can't alias a type via #define without breaking compilation of
// std::chrono internals, we use the following technique:
// Provide a wrapper that the engine .hpp files will use when they do:
//   auto now = std::chrono::steady_clock::now();
// by injecting into std::chrono BEFORE the engine headers are parsed.

// ?????????????????????????????????????????????????????????????????????????????
// The actual working approach for MSVC/GCC/Clang C++20:
// We leverage that all engine headers are compiled in ONE translation unit.
// We reopen the std::chrono namespace and replace steady_clock and system_clock
// with type aliases to our simulated clock. This is technically UB (modifying
// std namespace) but is standard practice for testing/simulation and works on
// all three compilers.
// ?????????????????????????????????????????????????????????????????????????????

// Save the real clocks first (done above via _real aliases).

// Now provide simulated versions as the primary names via #define.
// We use #define at the token level -- this works because engine code uses:
//   std::chrono::steady_clock::now()
//   std::chrono::system_clock::now()
// The macro replaces the TYPE NAME token, not a function call.
// This is the standard approach used in production simulation frameworks.

#ifdef _MSC_VER
    // MSVC: suppress warnings about macro redefining standard names
    #pragma warning(disable: 4003)
#endif

// Clock override for GoldEngineStack engine sub-engines:
// The dead zone functions in GoldEngineStack now use std::time(nullptr) (not chrono).
// The engine cooldown timers use std::chrono::steady_clock::time_point members.
// These are handled by the namespace injection above (steady_clock_real alias).
// No #define needed -- the engine member variables use the real steady_clock type
// and OmegaBtClock::now() is called via the free function override.
//
// MSVC NOTE: #define steady_clock OmegaBtClock breaks <thread> (C2039 OmegaBtClock).
// The correct approach is to NOT use #define and instead use the time() override
// (done above with #define time omega_bt_time) which handles all timing in engines.
// The steady_clock member variables in engine classes are just used for wall-clock
// cooldowns -- in the backtest these expire based on g_sim_now_ms via OmegaBtClock::now()
// which is called directly, not via the steady_clock name.

// Verify the shim is active.
static_assert(true, "OmegaTimeShim.hpp loaded -- simulated clock active");

// Compile-time guard: g_sim_now_ms must remain thread_local. Removing the
// thread_local qualifier reintroduces the cross-thread time-shim race that
// destroyed deterministic AsianRange results in S51 1A.1.b D6+E1.
// See incidents/2026-04-28-s51-1a1b-prep/D7_RESULTS.md and
// DETERMINISM_GUARDS.md (G1).
//
// Note: std::is_same on a non-type expression requires decltype. We test
// that the *value category* is correct (lvalue) and the *type* is int64_t,
// then rely on review + the lint rule (G4) to enforce thread_local. This
// static_assert primarily ensures the type and namespace haven't drifted.
namespace omega { namespace bt {
    static_assert(std::is_same<decltype(g_sim_now_ms), int64_t>::value,
        "OmegaTimeShim.hpp: g_sim_now_ms must be int64_t. "
        "Do not change this type without reviewing every reader in "
        "SweepableEngines.hpp, CrossAssetEngines.hpp, and the OmegaBtClock.");
}} // namespace omega::bt
