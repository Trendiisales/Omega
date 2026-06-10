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
    double trail_pct    = 2.0;     // hard trailing stop. 3->2 2026-06-11 (pump_exit_bt.py:
                                   // BE2T2 wins every basket day at 1% AND 2% slip)
    double be_arm_pct   = 2.0;     // BE-lock arm (engine BE_ARM_PCT); 0 = off
    double be_floor_pct = 2.0;     // BE-lock stop floor = entry +/- this %
    double volx         = 0.0;     // ignition volume-surge condition DISABLED (2026-06-10 A/B:
                                   // v>=3x avg20 self-defeats in a sustained frenzy -- every bar is
                                   // huge so none is 3x its neighbors; blocked CIIT +65% while pooled
                                   // 8-day basket net is EQUAL-OR-BETTER without it (1139 vs 1108,
                                   // PF 28.7/16.1 at 1%/2% slip). pump_variant_bt.py both windows.
    int    pyr_adds     = 0;       // pyramid OFF (conditional leverage, hurts durable regime)
    int    max_symbols  = 12;      // cap concurrent pumps tracked
    bool   single_position_per_symbol = true;  // S-2026-06-11: trio shares one slot per symbol
                                   // (3 TFs taking the same thrust = 3x concentration).
                                   // Tradeoff: per-TF A/B becomes first-to-fire, accepted.
    bool   verbose      = false;
    PumpScalpEngine::TradeRecordCallback on_trade_record;   // one sink for all engines

    struct Trio { PumpScalpEngine e5, e10, e15; int64_t last_ms = 0; };

    void on_bar(const std::string& sym, int tf_sec,
                double o, double h, double l, double c, double v, int64_t ts_ms, bool is_seed=false) {
        Trio& t = ensure(sym, ts_ms);
        if      (tf_sec == 300) t.e5.on_entry_bar(o, h, l, c, v, ts_ms, is_seed);
        else if (tf_sec == 600) t.e10.on_entry_bar(o, h, l, c, v, ts_ms, is_seed);
        else if (tf_sec == 900) t.e15.on_entry_bar(o, h, l, c, v, ts_ms, is_seed);
    }

    // Bridge 'R' line: clean re-warm of an already-tracked symbol ahead of a
    // seed replay (bridge restart / consumer reconnect). No-op if untracked.
    void reset_symbol(const std::string& sym) {
        auto it = m_book.find(sym);
        if (it == m_book.end()) return;
        it->second->e5.reset_for_reseed();
        it->second->e10.reset_for_reseed();
        it->second->e15.reset_for_reseed();
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

    // ── GUI visibility: every active pump position across all symbols/timeframes,
    //   for g_open_positions.register_source -> shows in the live_trades panel + bell.
    std::vector<omega::PositionSnapshot> collect_positions() {
        std::vector<omega::PositionSnapshot> v;
        omega::PositionSnapshot s;
        for (auto& kv : m_book) {
            auto& t = *kv.second;
            if (t.e5.persist_save ("PumpScalp_5m",  kv.first.c_str(), s)) v.push_back(s);
            if (t.e10.persist_save("PumpScalp_10m", kv.first.c_str(), s)) v.push_back(s);
            if (t.e15.persist_save("PumpScalp_15m", kv.first.c_str(), s)) v.push_back(s);
        }
        return v;
    }

    // ── Scanner visibility: current pump candidates the bridge is tracking
    //   (symbol, price, % up from open). Served to the GUI scanner panel. ───────
    struct Candidate { std::string sym; double px=0, day_open=0, up_pct=0; int64_t ts=0; };
    void set_candidate(const std::string& sym, double px, double day_open, double up_pct, int64_t ts) {
        m_cands[sym] = Candidate{sym, px, day_open, up_pct, ts};
    }
    std::vector<Candidate> candidates() const {
        std::vector<Candidate> v; v.reserve(m_cands.size());
        for (auto& kv : m_cands) v.push_back(kv.second);
        return v;
    }

    // ── Health: bridge truth (candidates carry the bridge's day_open/up%) vs
    //   the engines' own view. A symbol the bridge says is past the gate whose
    //   engines see less than HALF that expansion is COLD-ANCHORED (the
    //   2026-06-10 silent-no-trades class); a thin bar buffer on an armed name
    //   is a warmup hole. Either -> the consumer should request a seed replay. ──
    bool reseed_wanted() const {
        for (const auto& kv : m_cands) {
            const Candidate& cd = kv.second;
            if (cd.up_pct < day_gate_pct) continue;          // only armed names matter
            auto it = m_book.find(kv.first);
            if (it == m_book.end()) continue;                // trio not built yet
            const double eng_up = it->second->e5.day_up_pct();
            if (eng_up >= 0.0 && eng_up < cd.up_pct * 0.5) return true;   // cold anchor
            if (it->second->e5.bars_seen() < 24) return true;             // warmup hole
        }
        return false;
    }

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
            e.BE_ARM_PCT   = be_arm_pct;
            e.BE_FLOOR_PCT = be_floor_pct;
            e.VOLX         = volx;
            e.PYR_ADDS     = pyr_adds;
            e.MAXHOLD_SEC  = 30 * tf;       // ~30 bars of this timeframe
            e.shadow_mode  = shadow_mode;
            e.verbose      = verbose;
            e.on_trade_record = on_trade_record;
            e.init();
        };
        setup(t.e5, 300, "5m"); setup(t.e10, 600, "10m"); setup(t.e15, 900, "15m");
        // S-2026-06-11: one position per SYMBOL across the trio. holds_position()
        // covers all three TF engines; the asking engine's own pos.active is
        // false at entry-check time, so only siblings count.
        if (single_position_per_symbol) {
            t.e5.entry_permit  = [this, sym]() { return !holds_position(sym); };
            t.e10.entry_permit = [this, sym]() { return !holds_position(sym); };
            t.e15.entry_permit = [this, sym]() { return !holds_position(sym); };
        }
    }

    Trio& ensure(const std::string& sym, int64_t ts_ms) {
        // Seed bars carry HISTORICAL ts; evicting against them finds no victim
        // (everyone looks newer than "now") -> book grew unbounded (active=43
        // vs cap 12, 2026-06-10). Evict against the max event time ever seen.
        if (ts_ms > m_now) m_now = ts_ms;
        auto it = m_book.find(sym);
        if (it == m_book.end()) {
            if ((int)m_book.size() >= max_symbols) evict_coldest(m_now);
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
    std::unordered_map<std::string, Candidate> m_cands;   // scanner candidates (for the GUI panel)
    int64_t m_now = 0;   // max event ts seen (eviction clock; seed ts are historical)
};

}  // namespace omega
