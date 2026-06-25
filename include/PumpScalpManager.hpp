#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PumpScalpManager — dynamic-universe owner for PumpScalpEngine.
//
//   Every other Omega engine trades a FIXED symbol. Pump scalping trades whatever
//   explodes today, so this manager holds one 3m engine per pumping symbol,
//   creates it the first time the symbol appears in the feed, routes that
//   symbol's bars/prices to it, and retires cold symbols.
//
//   S-2026-06-11: trio (3/5/15m) collapsed to 3m-only. With one position per
//   symbol the 5m/15m engines could only fire when 3m hadn't — slower entries
//   on the same thrust, no diversification. 3m is the backtested winner
//   (pump_tf_bt.py: 3m n=42 PF 36.4/20.7 @1%/2% slip, catches the SLGB monster
//   AND wins ex-monster). Bridge sends TFS=[180] only.
//
//   Pure C++. Fed by the Python IBKR bridge (data-only, like every Omega IBKR
//   feed). shadow_mode propagates to every engine — no live orders.
// ─────────────────────────────────────────────────────────────────────────────
#include "PumpScalpEngine.hpp"
#include <unordered_map>
#include <memory>
#include <string>
#include <map>
#include <set>

namespace omega {

class PumpScalpManager {
public:
    bool   shadow_mode  = true;
    // 2026-06-12: parameterized so a BIG-CAP instance (5m, distinct label) can reuse
    //   this manager alongside the micro-cap one. Defaults = the micro-cap 3m engine.
    int    tf_sec       = 180;     // entry bar timeframe (180=3m micro, 300=5m big-cap)
    std::string label   = "PumpScalp_3m";   // engine_name on emitted trades
    double day_gate_pct = 100.0;   // extreme-mover gate (see engine: durable edge lives here)
    double hard_pct     = 6.0;     // hard stop from entry (engine HARD_PCT). 2026-06-23: exposed tunable
                                   // for the chop-bleed sweep (tighter = cuts chop losers faster).
    int    min_breadth  = 1;       // S-2026-06-23 cross-sectional BREADTH gate: require >= this many
                                   // DISTINCT names igniting same session-day before any entry fires.
                                   // 1 = off (live). 2 = skip isolated single-name chop false-breakouts
                                   // AND sit out bear (few broad-ignition days). The chop/bear protection.
    double trail_pct    = 2.0;     // hard trailing stop. 3->2 2026-06-11 (pump_exit_bt.py:
                                   // BE2T2 wins every basket day at 1% AND 2% slip)
    double be_arm_pct   = 2.0;     // BE-lock arm (engine BE_ARM_PCT); 0 = off
    double be_floor_pct = 2.0;     // BE-lock stop floor = entry +/- this %
    // ATR-trail (engine ATR_LEN/ATR_MULT): when atr_len>0 the trail rides ATR_MULT*ATR
    // off the peak instead of (or alongside) the % trail. 2026-06-18 bigcap_sweep: ATR-trail
    // (len30 x4) is the best robust BigCap exit (PF4.05 vs %-trail 2.30). 0 = off.
    int    atr_len      = 0;
    double atr_mult     = 0.0;
    // S-2026-06-24 profit-scaled trail + close-based reversal exits (engine fields). All 0/off
    // => live behavior unchanged. Swept via bigcap_momo_faithful BC_* env knobs.
    double atr_mult_tight  = 0.0;  // trail mult once gain>=pscale_full_pct (profit-scaled; 0=flat)
    double pscale_full_pct = 0.0;  // gain% at which trail reaches atr_mult_tight
    double giveback_frac   = 0.0;  // exit on retrace of this frac of peak gain, EVERY TICK (0=off)
    double giveback_close_frac = 0.0; // same but on CLOSE only (noise-proof; the A/B give-back lever)
    int    struct_lb       = 0;    // exit on close below swing-low of last N bars (0=off)
    bool   rollover_ema    = false;// exit on close below EMA9 (momentum-decay, long)
    double cold_cut_sec       = 0.0;  // cut never-green trade after N sec (0=off); shrinks ride-down losers
    double cold_cut_green_pct = 0.5;  // "green" = peak favourable >= this % of entry
    // ride winners past the wall-clock while still net-profitable (engine MAXHOLD_SKIP_IF_PROFIT).
    // 2026-06-18: the 240-min cap was clocking out QURE/NTLA/PRAX mid-run. true = ride to trail turn.
    bool   maxhold_skip_if_profit = false;
    double volx         = 0.0;     // ignition volume-surge condition DISABLED (2026-06-10 A/B:
                                   // v>=3x avg20 self-defeats in a sustained frenzy -- every bar is
                                   // huge so none is 3x its neighbors; blocked CIIT +65% while pooled
                                   // 8-day basket net is EQUAL-OR-BETTER without it (1139 vs 1108,
                                   // PF 28.7/16.1 at 1%/2% slip). pump_variant_bt.py both windows.
    int    pyr_adds     = 0;       // pyramid OFF (conditional leverage, hurts durable regime)
    double notional_usd = 1000.0;  // $ per trade (shares = notional/entry). 5000->1000 2026-06-11
                                   // (operator): smaller order walks the thin book far less =>
                                   // less slippage; keeps real risk per name low.
    double slip_pct     = 1.0;     // %/side haircut in recorded PnL (backtest-equivalent cost)
    double min_dvol_usd = 2.0e6;   // ANTI-SLIPPAGE: entry needs bar close*volume >= $2M (liq_calib.py:
                                   // raises net@2% to +$22.7k AND cuts the @5% tail -$37k->-$7.4k).
    double price_min    = 1.0;     // ANTI-SLIPPAGE: skip sub-$1 names (paper books, halt gaps)
    int    maxhold_bars = 5;       // time-stop = this many 3m bars (5 = 15min). 2026-06-11
                                   // pump_recalib_bt.py: strict 3-min (cap=1) cuts winners
                                   // (worst in every trail row); 15-min trail-to-turn nets more
                                   // on BOTH monster + non-monster names (cap5 > cap1 each trail).
    int    max_symbols  = 12;      // cap concurrent pumps tracked
    double stale_sec    = 120.0;   // WATCHDOG: force-close an open position whose feed has been
                                   // silent this long (2026-06-18: dead AH/halt/bridge feed left
                                   // 7 positions frozen open 200+min — exits were 100% tick-gated).
                                   // 120s = 24x the 5s tick cadence = unambiguously dead.
    int    max_entries_per_day = 2; // RE-ENTRY CAP per name/session (chop-bleed guard).
                                   // reentry_cap_bt.py (16-day basket): cap2 keeps 84% of
                                   // net + best PF (42) vs unlimited chop (PF18) or cap1
                                   // (kills edge, $1.7k). 0 = unlimited (old behaviour).
    double entry_max_ext_pct = 0.0; // 2026-06-23 ANTI-CHASE: skip an ignition LONG whose close is
                                   // already > this% above VWAP (buying the extended top that fades).
                                   // 0 = off. Forwarded to PumpScalpEngine::ENTRY_MAX_EXT_PCT.
    bool   verbose      = false;
    bool   enabled      = true;    // 2026-06-12 KILL-SWITCH: false => ignore all feed, no
                                   // new pump entries arm (stop-bleed). Open sim positions
                                   // still close via on_price. Flip in engine_init.
    PumpScalpEngine::TradeRecordCallback on_trade_record;   // one sink for all engines

    struct Cell { PumpScalpEngine e3; int64_t last_ms = 0; };

    // 2026-06-12b: param renamed bar_tf_sec (was tf_sec -- MSVC C4458 shadow = build
    // break) AND the filter now compares against the INSTANCE tf_sec member instead of
    // hardcoded 180 (the hardcode silently dropped every 5m bar for the big-cap
    // instance -- BigCapMomo would never have seen a bar).
    void on_bar(const std::string& sym, int bar_tf_sec,
                double o, double h, double l, double c, double v, int64_t ts_ms, bool is_seed=false) {
        if (!enabled) return;        // disabled -> no cells built, no entries arm
        if (bar_tf_sec != tf_sec) return;  // accept only this instance's TF (bridge may replay old multi-TF seeds)
        Cell& t = ensure(sym, ts_ms);
        t.e3.on_entry_bar(o, h, l, c, v, ts_ms, is_seed);
    }

    // Bridge 'R' line: clean re-warm of an already-tracked symbol ahead of a
    // seed replay (bridge restart / consumer reconnect). No-op if untracked.
    void reset_symbol(const std::string& sym) {
        auto it = m_book.find(sym);
        if (it == m_book.end()) return;
        it->second->e3.reset_for_reseed();
    }

    void on_price(const std::string& sym, double px, int64_t ts_ms) {
        auto it = m_book.find(sym);
        if (it == m_book.end()) return;
        it->second->last_ms = ts_ms;
        it->second->e3.on_price(px, ts_ms);
    }

    int  active() const { return (int)m_book.size(); }

    // ── WATCHDOG heartbeat: call from the main loop (NOT the feed thread). Forces
    //   a feed-independent exit on any position whose price feed has gone stale or
    //   that has exceeded MAXHOLD — so a dead feed can never freeze a trade open.
    //   No-op while ticks flow (each engine's m_last_px_ms stays fresh). ─────────
    void on_heartbeat(int64_t now_ms) {
        const int64_t stale_ms = (int64_t)(stale_sec * 1000.0);
        for (auto& kv : m_book) kv.second->e3.watchdog(now_ms, stale_ms);
    }

    // ── GUI visibility: every active pump position across all symbols,
    //   for g_open_positions.register_source -> shows in the live_trades panel + bell.
    std::vector<omega::PositionSnapshot> collect_positions() {
        std::vector<omega::PositionSnapshot> v;
        omega::PositionSnapshot s;
        for (auto& kv : m_book) {
            auto& t = *kv.second;
            if (t.e3.persist_save("PumpScalp_3m", kv.first.c_str(), s)) v.push_back(s);
        }
        return v;
    }

    // ── S-2026-06-26 PERSISTENCE (wire_multicell archetype): this manager showed positions but did NOT
    //   persist -> BigCapMomo/PumpScalp fills vanished on every restart. The per-cell PumpScalpEngine
    //   already has persist_save/persist_restore; these route per-symbol with tag "<base>#<symbol>".
    void persist_save_all(const char* base, const char* /*sym*/, std::vector<omega::PositionSnapshot>& out) {
        omega::PositionSnapshot s;
        for (auto& kv : m_book) {
            std::string tag = std::string(base) + "#" + kv.first;   // base#<symbol> -> restorer routes by base
            if (kv.second->e3.persist_save(tag.c_str(), kv.first.c_str(), s)) out.push_back(s);
        }
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        if (!enabled) return false;                                 // disabled -> no resurrect (phantom guard)
        auto h = ps.engine.find('#');
        std::string sym = (h == std::string::npos) ? ps.symbol : ps.engine.substr(h + 1);
        if (sym.empty()) return false;
        Cell& t = ensure(sym, ps.entry_ts ? ps.entry_ts * 1000 : 0);
        return t.e3.persist_restore(ps);
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
    //   engine sees less than HALF that expansion is COLD-ANCHORED (the
    //   2026-06-10 silent-no-trades class); a thin bar buffer on an armed name
    //   is a warmup hole. Either -> the consumer should request a seed replay. ──
    bool reseed_wanted() const {
        for (const auto& kv : m_cands) {
            const Candidate& cd = kv.second;
            if (cd.up_pct < day_gate_pct) continue;          // only armed names matter
            auto it = m_book.find(kv.first);
            if (it == m_book.end()) continue;                // cell not built yet
            const double eng_up = it->second->e3.day_up_pct();
            if (eng_up >= 0.0 && eng_up < cd.up_pct * 0.5) return true;   // cold anchor
            if (it->second->e3.bars_seen() < 24) return true;             // warmup hole
        }
        return false;
    }

    bool holds_position(const std::string& sym) const {
        auto it = m_book.find(sym);
        if (it == m_book.end()) return false;
        return it->second->e3.has_open_position();
    }

private:
    void configure(Cell& t, const std::string& sym) {
        PumpScalpEngine& e = t.e3;
        e.symbol       = sym;
        e.engine_name  = label;
        e.TF_SEC       = tf_sec;
        e.DAY_GATE_PCT = day_gate_pct;
        e.HARD_PCT     = hard_pct;
        e.TRAIL_PCT    = trail_pct;
        e.BE_ARM_PCT   = be_arm_pct;
        e.BE_FLOOR_PCT = be_floor_pct;
        e.VOLX         = volx;
        e.PYR_ADDS     = pyr_adds;
        e.NOTIONAL_USD = notional_usd;
        e.SLIP_PCT     = slip_pct;
        e.MIN_DVOL_USD = min_dvol_usd;
        e.PRICE_MIN    = price_min;
        e.ATR_LEN      = atr_len;                  // ATR-trail (0 = off)
        e.ATR_MULT     = atr_mult;
        e.ATR_MULT_TIGHT  = atr_mult_tight;        // S-2026-06-24 profit-scaled trail + close exits
        e.PSCALE_FULL_PCT = pscale_full_pct;
        e.GIVEBACK_FRAC   = giveback_frac;
        e.GIVEBACK_CLOSE_FRAC = giveback_close_frac;
        e.STRUCT_LB       = struct_lb;
        e.ROLLOVER_EMA    = rollover_ema;
        e.COLD_CUT_SEC       = cold_cut_sec;
        e.COLD_CUT_GREEN_PCT = cold_cut_green_pct;
        e.MAXHOLD_SEC  = maxhold_bars * tf_sec;   // time-stop; trail exits on the turn first
        e.MAXHOLD_SKIP_IF_PROFIT = maxhold_skip_if_profit;   // ride winners past the clock
        e.MAX_ENTRIES_PER_DAY = max_entries_per_day;   // re-entry cap (chop-bleed guard)
        e.ENTRY_MAX_EXT_PCT   = entry_max_ext_pct;     // anti-chase: skip longs already extended above VWAP
        e.shadow_mode  = shadow_mode;
        e.verbose      = verbose;
        e.on_trade_record = on_trade_record;
        { const char* v;   // S-2026-06-23 research toggles (BT-only; unset = live defaults)
          if((v=getenv("BC_LB")))       e.LB=atoi(v);
          if((v=getenv("BC_STRENGTH"))) e.STRENGTH=atof(v);
          if((v=getenv("BC_VOLREG")))   e.VOL_REG_FILTER=(atoi(v)!=0); }
        if (min_breadth > 1) {
            e.breadth_register = [this](int64_t day, const std::string& s){
                auto& set = m_day_ignis[day]; set.insert(s);
                return (int)set.size() >= min_breadth;
            };
        }
        e.init();
    }

    Cell& ensure(const std::string& sym, int64_t ts_ms) {
        // Seed bars carry HISTORICAL ts; evicting against them finds no victim
        // (everyone looks newer than "now") -> book grew unbounded (active=43
        // vs cap 12, 2026-06-10). Evict against the max event time ever seen.
        if (ts_ms > m_now) m_now = ts_ms;
        auto it = m_book.find(sym);
        if (it == m_book.end()) {
            if ((int)m_book.size() >= max_symbols) evict_coldest(m_now);
            auto t = std::make_unique<Cell>();
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

    std::unordered_map<std::string, std::unique_ptr<Cell>> m_book;
    std::unordered_map<std::string, Candidate> m_cands;   // scanner candidates (for the GUI panel)
    std::map<int64_t, std::set<std::string>> m_day_ignis;  // S-2026-06-23 breadth: distinct igniters per session-day
    int64_t m_now = 0;   // max event ts seen (eviction clock; seed ts are historical)
};

}  // namespace omega
