#pragma once
// ==============================================================================
// EngineLastRegistry.hpp -- Step 3 Omega Terminal: per-engine "last" side-table.
//
// Why this exists:
//   Step 2 added g_engines (EngineRegistry.hpp) so OmegaApiServer.cpp can serve
//   /api/v1/omega/engines. The snapshot lambdas registered in
//   include/engine_init.hpp deliberately returned 0 for last_signal_ts and
//   last_pnl ("per-engine accessors land in Step 3"). Step 3 needs real values
//   so the CC/ENG panels don't show all-zero "Last signal" columns.
//
//   Two designs were considered:
//     (a) Per-engine atomics on every engine class.
//     (b) Central side-table keyed by trade-record engine string.
//   We picked (b): one mutex+map vs ~14 file edits, easier to roll back, and
//   matches how g_omegaLedger is already centrally written from
//   handle_closed_trade in include/trade_lifecycle.hpp.
//
// What the side-table holds:
//   For each tr.engine string that has ever closed a trade, the most recent
//   close timestamp and the corresponding net_pnl.
//
// How it is written:
//   handle_closed_trade calls g_engine_last.record(tr.engine, ts_ms, net_pnl)
//   exactly once per closed trade. Both the shadow branch and the live branch
//   call it -- shadow trades count as "signals" for the purpose of the UI
//   ("when did the engine last act") even though they don't book to ledger.
//
// How it is read:
//   The snapshot lambdas in engine_init.hpp's `reg` helper call
//   g_engine_last.get_latest_for_any({...}) with the engine's tr.engine
//   string(s). For cell-based engines (Tsmom, Donchian, EmaPullback,
//   TrendRider) tr.engine equals the per-cell id (e.g. "Tsmom_H1_long"), so
//   the registry name "Tsmom" is mapped to the prefix "Tsmom_" -- the
//   registry walks all matching keys and returns the latest by ts. For
//   bracket engines tr.engine is a single literal (e.g. "HybridBracketGold")
//   and the lookup is exact.
//
// HBI caveat (HybridSP/NQ/US30/NAS100):
//   All four IndexHybridBracket instances stamp tr.engine = "HybridBracketIndex"
//   uniformly. Until the engine is changed to differentiate by symbol, all
//   four registry entries will show the SAME last_signal_ts/last_pnl. This is
//   accepted for Step 3 -- a follow-up engine-side cleanup will distinguish
//   them, at which point the lookup keys here become symbol-suffixed.
//
// Threading model:
//   mu_-guarded read/write. Writers come from handle_closed_trade (off the
//   tick hot path) and readers come from the OmegaApiServer accept thread
//   (a separate TU). No lock-order conflicts because no engine hot-path lock
//   is taken across record() / get_latest_for_any() boundaries.
// ==============================================================================

#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace omega {

class EngineLastRegistry
{
public:
    struct Entry
    {
        int64_t last_ts_ms = 0;   // unix ms; 0 = never
        double  last_pnl   = 0.0; // net_pnl USD of the trade at last_ts_ms
    };

    // Record the most recent close for `engine_id` (the tr.engine string set
    // by the engine that emitted the trade record). Calls are idempotent in
    // that a record() with an older ts than what we already have is dropped
    // for the timestamp field but still updates the pnl -- this matches the
    // intent that "last_pnl" tracks the latest *recorded* trade. Practically
    // handle_closed_trade is called in trade-time order so this branch is
    // unlikely to fire.
    void record(const std::string& engine_id, int64_t ts_ms, double net_pnl)
    {
        if (engine_id.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        Entry& e = data_[engine_id];
        if (ts_ms >= e.last_ts_ms) {
            e.last_ts_ms = ts_ms;
            e.last_pnl   = net_pnl;
        }
    }

    // Look up the latest entry across any registered engine_id matching one
    // of the supplied patterns. A pattern ending in '_' matches by prefix
    // (so "Tsmom_" matches "Tsmom_H1_long", "Tsmom_H4_short", etc.); any
    // other pattern matches by exact string equality.
    //
    // Returns a zero-initialised Entry if nothing matches (last_ts_ms = 0,
    // last_pnl = 0.0), which the snapshot lambdas serialize transparently.
    Entry get_latest_for_any(std::initializer_list<const char*> patterns) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        Entry latest;
        for (const auto& kv : data_) {
            const std::string& key = kv.first;
            const Entry& val       = kv.second;
            bool match = false;
            for (const char* pat : patterns) {
                if (!pat) continue;
                const std::string p(pat);
                if (p.empty()) continue;
                if (p.back() == '_') {
                    // Prefix match: caller signals "Tsmom_" / "Donchian_" / etc.
                    if (key.size() >= p.size() &&
                        key.compare(0, p.size(), p) == 0) {
                        match = true;
                        break;
                    }
                } else {
                    if (key == p) { match = true; break; }
                }
            }
            if (match && val.last_ts_ms > latest.last_ts_ms) {
                latest = val;
            }
        }
        return latest;
    }

    // Convenience overload for callers that want a single literal lookup.
    Entry get_exact(const std::string& engine_id) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = data_.find(engine_id);
        if (it == data_.end()) return Entry{};
        return it->second;
    }

private:
    mutable std::mutex                          mu_;
    std::unordered_map<std::string, Entry>      data_;
};

} // namespace omega

// File-scope `g_engine_last` -- defined exactly once in include/globals.hpp
// (main.cpp's TU). External linkage so OmegaApiServer.cpp's read of
// last_signal_ts/last_pnl through the snapshot lambdas (which live in
// engine_init.hpp, also main.cpp's TU) resolves to the same instance at link
// time. Mirrors the g_engines pattern in EngineRegistry.hpp.
extern omega::EngineLastRegistry g_engine_last;
