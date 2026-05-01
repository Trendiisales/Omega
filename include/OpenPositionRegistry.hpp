#pragma once
// ==============================================================================
// OpenPositionRegistry.hpp -- Step 3 Omega Terminal: open-position read-API.
//
// Why this exists:
//   Step 2's OmegaApiServer.cpp returns "[]" for /api/v1/omega/positions
//   ("Step 3 wires up CC/ENG/POS panels which will demand richer accessors").
//   Step 3 needs real values so the omega-terminal POS panel renders. Adding a
//   centralised in-memory registry that engines populate via snapshotter
//   lambdas mirrors the EngineRegistry pattern (Step 2) and keeps the
//   OmegaApiServer translation unit independent of every engine's static
//   global graph.
//
// Scope-down for Step 3:
//   The handoff calls for "real /positions across all engines". The engine
//   surface area to enumerate every open position uniformly is large (Tsmom
//   has private std::vector<Position> per cell, Donchian/EmaPullback/TrendRider
//   similarly, HBI brackets share an open-state struct, etc.). Step 3 ships
//   only the HybridGold (HBG) source; other sources land in a follow-up
//   session with a unified per-engine snapshot interface. The registry shape
//   here accepts any number of sources, so adding the next source later is
//   purely additive.
//
// Field shape:
//   PositionSnapshot mirrors the JSON keys in
//   omega-terminal/src/api/types.ts (Position interface). Field names are
//   byte-identical to the JSON keys -- do NOT rename without the TS side.
//
// Threading model:
//   register_source / snapshot_all are mutex-guarded. Snapshotter callbacks
//   read engine state without taking that engine's tick-path mutex (engines
//   don't currently expose one). This means a tick-path producer can be
//   half-way through mutating pos when the snapshotter copies the fields,
//   producing a torn read. The probability is small (the producer path is a
//   handful of stores per tick, the reader is at 2 Hz from the UI), and the
//   visible failure mode is a one-frame inconsistency (e.g. side from the
//   prior position, entry from the new one). Acceptable for the UI; if it
//   becomes a problem the engines can grow a per-pos mutex without changing
//   the registry shape.
// ==============================================================================

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace omega {

// Mirror of omega-terminal/src/api/types.ts Position interface.
//   symbol           e.g. "EURUSD" / "XAUUSD" / "BTCUSD"
//   side             "LONG" | "SHORT"
//   size             contract / lot size (engine-native units)
//   entry            entry price
//   current          current mid (or last) price -- 0 if unknown
//   unrealized_pnl   unrealized P&L in account currency (USD), 0 if unknown
//   mfe              max favorable excursion observed since entry (USD)
//   mae              max adverse excursion observed since entry (USD)
//   engine           name of the engine that opened the position
struct PositionSnapshot
{
    std::string symbol;
    std::string side;            // "LONG" | "SHORT"
    double      size           = 0.0;
    double      entry          = 0.0;
    double      current        = 0.0;
    double      unrealized_pnl = 0.0;
    double      mfe            = 0.0;
    double      mae            = 0.0;
    std::string engine;
};

class OpenPositionRegistry
{
public:
    using SourceFn = std::function<std::vector<PositionSnapshot>()>;

    void register_source(std::string label, SourceFn fn)
    {
        std::lock_guard<std::mutex> lk(mu_);
        sources_.emplace_back(std::move(label), std::move(fn));
    }

    std::vector<PositionSnapshot> snapshot_all() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<PositionSnapshot> out;
        out.reserve(sources_.size() * 2);
        for (const auto& kv : sources_) {
            if (!kv.second) continue;
            const auto chunk = kv.second();
            for (const auto& ps : chunk) {
                out.push_back(ps);
            }
        }
        return out;
    }

private:
    mutable std::mutex                              mu_;
    std::vector<std::pair<std::string, SourceFn>>   sources_;
};

} // namespace omega

// File-scope `g_open_positions` -- defined exactly once in include/globals.hpp
// (main.cpp's TU), external linkage so OmegaApiServer.cpp's extern below
// resolves to the same instance at link time. Engines self-register their
// snapshotter in init_engines() (include/engine_init.hpp).
extern omega::OpenPositionRegistry g_open_positions;
