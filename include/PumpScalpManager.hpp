#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PumpScalpManager — dynamic-universe owner for PumpScalpEngine.
//
//   Every other Omega engine trades a FIXED symbol. Pump scalping trades whatever
//   explodes today, so this manager holds a per-symbol TRIO of engines (5m / 10m
//   / 15m), creates a trio the first time a pumping symbol appears in the feed,
//   routes that symbol's bars/prices to its trio, and retires cold symbols.
//
//   Entry bars (on_bar) go to the matching-timeframe engine; price ticks
//   (on_price) go to ALL THREE so every timeframe exits IMMEDIATELY on the turn.
//   Trade records from all engines forward to one shadow callback.
//
//   Pure C++. Fed by the Python IBKR bridge (data-only, like every Omega IBKR
//   feed). shadow_mode propagates to every engine — no live orders.
// ─────────────────────────────────────────────────────────────────────────────
#include "PumpScalpEngine.hpp"
#include <unordered_map>
#include <memory>
#include <string>

namespace omega {

class PumpScalpManager {
public:
    bool   shadow_mode  = true;
    double day_gate_pct = 100.0;   // extreme-mover gate (see engine: durable edge lives here)
    double trail_pct    = 3.0;     // hard trailing stop (sweep: tighter=better, 3% for AH-slip margin)
    int    pyr_adds     = 0;       // pyramid OFF (conditional leverage, hurts durable regime)
    int    max_symbols  = 12;      // cap concurrent pumps tracked
    bool   verbose      = false;
    PumpScalpEngine::TradeRecordCallback on_trade_record;   // one sink for all engines

    struct Trio { PumpScalpEngine e5, e10, e15; int64_t last_ms = 0; };

    void on_bar(const std::string& sym, int tf_sec,
                double o, double h, double l, double c, double v, int64_t ts_ms) {
        Trio& t = ensure(sym, ts_ms);
        if      (tf_sec == 300) t.e5.on_entry_bar(o, h, l, c, v, ts_ms);
        else if (tf_sec == 600) t.e10.on_entry_bar(o, h, l, c, v, ts_ms);
        else if (tf_sec == 900) t.e15.on_entry_bar(o, h, l, c, v, ts_ms);
    }

    void on_price(const std::string& sym, double px, int64_t ts_ms) {
        auto it = m_book.find(sym);
        if (it == m_book.end()) return;
        it->second->last_ms = ts_ms;
        it->second->e5.on_price(px, ts_ms);
        it->second->e10.on_price(px, ts_ms);
        it->second->e15.on_price(px, ts_ms);
    }

    int  active() const { return (int)m_book.size(); }

    bool holds_position(const std::string& sym) const {
        auto it = m_book.find(sym);
        if (it == m_book.end()) return false;
        return it->second->e5.has_open_position() || it->second->e10.has_open_position()
            || it->second->e15.has_open_position();
    }

private:
    void configure(Trio& t, const std::string& sym) {
        auto setup = [&](PumpScalpEngine& e, int tf, const char* sfx) {
            e.symbol       = sym;
            e.engine_name  = std::string("PumpScalp_") + sfx;
            e.TF_SEC       = tf;
            e.DAY_GATE_PCT = day_gate_pct;
            e.TRAIL_PCT    = trail_pct;
            e.PYR_ADDS     = pyr_adds;
            e.MAXHOLD_SEC  = 30 * tf;       // ~30 bars of this timeframe
            e.shadow_mode  = shadow_mode;
            e.verbose      = verbose;
            e.on_trade_record = on_trade_record;
            e.init();
        };
        setup(t.e5, 300, "5m"); setup(t.e10, 600, "10m"); setup(t.e15, 900, "15m");
    }

    Trio& ensure(const std::string& sym, int64_t ts_ms) {
        auto it = m_book.find(sym);
        if (it == m_book.end()) {
            if ((int)m_book.size() >= max_symbols) evict_coldest(ts_ms);
            auto t = std::make_unique<Trio>();
            configure(*t, sym);
            it = m_book.emplace(sym, std::move(t)).first;
            if (verbose) printf("[PumpMgr] +track %s (active=%d)\n", sym.c_str(), (int)m_book.size());
        }
        it->second->last_ms = ts_ms;
        return *it->second;
    }

    void evict_coldest(int64_t ts_ms) {
        std::string victim; int64_t oldest = ts_ms + 1;
        for (auto& kv : m_book) {                            // never evict a symbol holding a position
            if (holds_position(kv.first)) continue;
            if (kv.second->last_ms < oldest) { oldest = kv.second->last_ms; victim = kv.first; }
        }
        if (!victim.empty()) m_book.erase(victim);
    }

    std::unordered_map<std::string, std::unique_ptr<Trio>> m_book;
};

}  // namespace omega
