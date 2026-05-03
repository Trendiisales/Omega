#pragma once
// ==============================================================================
// EngineRegistry.hpp -- Step 2 Omega Terminal: thread-safe engine snapshot
// registry consumed by OmegaApiServer (src/api/OmegaApiServer.cpp).
//
// Why a separate header (vs putting this in globals.hpp):
//   globals.hpp is a single-TU include consumed only by main.cpp. It transitively
//   pulls in the full engine universe (SymbolEngines, GoldEngineStack, the cross
//   stack, etc.) and uses static-storage globals throughout. OmegaApiServer.cpp
//   is a separate translation unit that only needs to *read* the registry; it
//   has no reason to pull main.cpp's dep graph in. Splitting the types out lets
//   OmegaApiServer.cpp #include exactly what it uses.
//
//   The instance `omega::g_engines` itself is defined in globals.hpp (file-scope,
//   external linkage so the linker resolves the extern declaration below). Engines
//   self-register from include/engine_init.hpp at startup.
//
// Threading:
//   register_engine() and snapshot_all() are mutex-guarded. Callers (including
//   the snapshot lambdas registered with register_engine) MUST NOT take any lock
//   that an engine hot-path holds while calling back into the registry --
//   otherwise an OmegaApiServer reader inverts a hot-path producer's lock order.
//   The lambdas added in Step 2's engine_init.hpp only read scalar fields off
//   their engine globals (.enabled / .shadow_mode), so this concern is satisfied
//   by inspection.
// ==============================================================================

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace omega {

struct EngineSnapshot
{
    std::string name;
    bool        enabled        = false;
    std::string mode;          // "LIVE" | "SHADOW"
    std::string state;         // "RUNNING" | "IDLE" | "ERR"
    int64_t     last_signal_ts = 0;  // unix ms; 0 = never
    double      last_pnl       = 0.0;
};

class EngineRegistry
{
public:
    using Snapshotter = std::function<EngineSnapshot()>;

    void register_engine(std::string name, Snapshotter fn)
    {
        std::lock_guard<std::mutex> lk(mu_);
        engines_.emplace_back(std::move(name), std::move(fn));
    }

    std::vector<EngineSnapshot> snapshot_all() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<EngineSnapshot> out;
        out.reserve(engines_.size());
        for (const auto& kv : engines_) {
            if (kv.second) out.emplace_back(kv.second());
        }
        return out;
    }

private:
    mutable std::mutex                                mu_;
    std::vector<std::pair<std::string, Snapshotter>>  engines_;
};

} // namespace omega

// File-scope `g_engines` -- matches the convention used elsewhere in
// include/globals.hpp and include/omega_types.hpp where engine and ledger
// globals (e.g. `omega::SpEngine g_eng_sp`, `omega::OmegaTradeLedger
// g_omegaLedger`) sit at file scope while the type lives in `namespace omega`.
//
// Defined exactly once in include/globals.hpp (main.cpp's TU). External
// linkage so OmegaApiServer.cpp's extern below resolves to the same instance
// at link time.
extern omega::EngineRegistry g_engines;
