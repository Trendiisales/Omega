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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
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
    // S-2026-06-02: additive fields for the live_trades publisher (held-time +
    // TP/SL distance). Purely additive — omega-terminal TS ignores extra JSON
    // keys; sources that don't set these leave them 0.
    int64_t     entry_ts       = 0;     // epoch seconds of entry (0 = unknown)
    double      tp             = 0.0;   // take-profit price (0 = none)
    double      sl             = 0.0;   // stop-loss price (0 = none)
    std::string token;                  // S-23a: broker clOrdId (fill-time lookup); "" = book-only
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

    // S-2026-06-09 visibility fix: enumerate the label of every registered GUI
    // source. Mutex-guarded like snapshot_all. Lets a boot-time guardrail
    // cross-check which engines feed the dashboard vs which only persist.
    std::vector<std::string> source_labels() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        out.reserve(sources_.size());
        for (const auto& kv : sources_) out.push_back(kv.first);
        return out;
    }

    // ========================================================================
    // S-2026-06-03: open-position PERSISTENCE (write/restore) so restarts and
    // deploys resume in-flight trades instead of silently dropping RAM-only
    // positions. The read path (snapshot_all) already enumerates every open
    // position uniformly; this adds the symmetric write/restore half.
    //
    // A restorer is a predicate that ATTEMPTS to adopt one snapshot back into
    // its owning engine and returns true if it claimed it. On restore the
    // registry offers each saved position to every restorer until one adopts.
    // Engines register a restorer the same way they register a source.
    // ========================================================================
    using RestoreFn = std::function<bool(const PositionSnapshot&)>;

    void register_restorer(RestoreFn fn)
    {
        std::lock_guard<std::mutex> lk(mu_);
        restorers_.emplace_back(std::move(fn));
    }

    // ========================================================================
    // S-2026-06-13u: AccountingGuard ENFORCEMENT (Phase 2). A closer is a
    // predicate that ATTEMPTS to force-close the position matching one snapshot
    // (book the trade through the engine's own on_trade_record callback + clear
    // the engine's internal slot) and returns true if it claimed+closed it.
    // Mirrors the restorer pattern exactly. AccountingGuard offers a breached
    // snapshot to every closer until one closes it; returns false if NONE
    // claimed (engine has no closer wired -> guard logs "unenforceable", never
    // silently fails). Engines register a closer the same way as a restorer.
    // ========================================================================
    using CloseFn = std::function<bool(const PositionSnapshot&, const char* reason)>;

    void register_closer(CloseFn fn)
    {
        std::lock_guard<std::mutex> lk(mu_);
        closers_.emplace_back(std::move(fn));
    }

    // Offer the breached snapshot to every closer until one force-closes it.
    // Returns true if a closer claimed+closed; false if none could (unenforceable).
    bool close_matching(const PositionSnapshot& ps, const char* reason)
    {
        std::vector<CloseFn> snap;
        { std::lock_guard<std::mutex> lk(mu_); snap = closers_; }
        for (const auto& fn : snap) {
            if (fn && fn(ps, reason)) return true;
        }
        return false;
    }

    // S-2026-06-03: persist-sources carry FULL position state (incl sl/tp), read
    // directly from engine internals — unlike the GUI snapshot sources, many of
    // which omit sl/tp. serialize()/save() prefer these when any are registered.
    // Lets the persistence module own complete state without editing the GUI
    // source lambdas (which live in engine_init.hpp alongside other WIP).
    void register_persist_source(SourceFn fn)
    {
        std::lock_guard<std::mutex> lk(mu_);
        persist_sources_.emplace_back(std::move(fn));
    }

    std::vector<PositionSnapshot> persist_snapshot_all() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (persist_sources_.empty()) {  // fall back to GUI sources (lock released first)
            std::vector<std::pair<std::string, SourceFn>> srcs;
            { srcs = sources_; }
            std::vector<PositionSnapshot> out;
            for (const auto& kv : srcs) if (kv.second) for (auto& ps : kv.second()) out.push_back(ps);
            return out;
        }
        std::vector<PositionSnapshot> out;
        for (const auto& fn : persist_sources_)
            if (fn) for (const auto& ps : fn()) out.push_back(ps);
        return out;
    }

    // One position per line: engine,symbol,side,size,entry,sl,tp,mfe,mae,entry_ts
    // (flat delimited — internal state file, parsed by restore() below).
    std::string serialize() const
    {
        const auto all = persist_snapshot_all();
        std::ostringstream os;
        for (const auto& p : all) {
            char buf[640];
            std::snprintf(buf, sizeof(buf),
                "%s,%s,%s,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%lld\n",
                p.engine.c_str(), p.symbol.c_str(), p.side.c_str(),
                p.size, p.entry, p.sl, p.tp, p.mfe, p.mae,
                static_cast<long long>(p.entry_ts));
            os << buf;
        }
        return os.str();
    }

    // Atomic-ish save: write temp then rename so a mid-write crash cannot leave
    // a half-written file. Returns number of positions written.
    int save(const std::string& path) const
    {
        const std::string body = serialize();
        const std::string tmp  = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) return -1;
            f << body;
            if (!f.good()) return -1;
        }
        std::remove(path.c_str());
        std::rename(tmp.c_str(), path.c_str());
        int n = 0; for (char ch : body) if (ch == '\n') ++n;
        return n;
    }

    // Read the file and offer each position to the registered restorers. Returns
    // {adopted, seen}. Caller logs. Safe to call when file is absent (returns 0,0).
    // S-2026-06-11: entry_ts of every ADOPTED line is recorded in
    // last_restored_entry_ts() so the central phantom-drop guard can exempt
    // legitimately-restored positions (their entry predates boot but they are real).
    std::pair<int,int> restore(const std::string& path)
    {
        restored_entry_ts_.clear();
        std::ifstream f(path);
        if (!f) return {0, 0};
        std::vector<RestoreFn> rs;
        { std::lock_guard<std::mutex> lk(mu_); rs = restorers_; }
        int adopted = 0, seen = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line); std::string t; std::vector<std::string> tok;
            while (std::getline(ss, t, ',')) tok.push_back(t);
            if (tok.size() < 10) continue;
            PositionSnapshot p;
            p.engine   = tok[0]; p.symbol = tok[1]; p.side = tok[2];
            p.size     = std::atof(tok[3].c_str());
            p.entry    = std::atof(tok[4].c_str());
            p.sl       = std::atof(tok[5].c_str());
            p.tp       = std::atof(tok[6].c_str());
            p.mfe      = std::atof(tok[7].c_str());
            p.mae      = std::atof(tok[8].c_str());
            p.entry_ts = std::atoll(tok[9].c_str());
            ++seen;
            for (const auto& r : rs) {
                if (r && r(p)) { ++adopted; restored_entry_ts_.push_back(p.entry_ts); break; }
            }
        }
        return {adopted, seen};
    }

    // entry_ts of every position adopted by the most recent restore() call.
    const std::vector<int64_t>& last_restored_entry_ts() const { return restored_entry_ts_; }

private:
    mutable std::mutex                              mu_;
    std::vector<std::pair<std::string, SourceFn>>   sources_;
    std::vector<RestoreFn>                          restorers_;
    std::vector<CloseFn>                            closers_;             // S-2026-06-13u AccountingGuard Phase 2
    std::vector<SourceFn>                           persist_sources_;
    std::vector<int64_t>                            restored_entry_ts_;   // S-2026-06-11
};

} // namespace omega

// File-scope `g_open_positions` -- defined exactly once in include/globals.hpp
// (main.cpp's TU), external linkage so OmegaApiServer.cpp's extern below
// resolves to the same instance at link time. Engines self-register their
// snapshotter in init_engines() (include/engine_init.hpp).
extern omega::OpenPositionRegistry g_open_positions;

// ── Manual KILL-ALL panic flatten (S-2026-06-30) ──────────────────────────
// Set true by the GUI panic button (POST /api/flatten, OmegaTelemetryServer).
// Consumed exactly once on the TRADING thread in on_tick (next to the
// catastrophe net) -- NEVER acted on from the HTTP thread, which would race
// engine state mid-tick. On consume, every open position is flattened with a
// real opposing MKT order (send_live_order, itself hard SHADOW-gated) plus
// close_matching() to clear the engine's internal slot and book the close.
// inline => one definition shared across every TU that includes this header.
inline std::atomic<bool> g_flatten_all_request{false};
