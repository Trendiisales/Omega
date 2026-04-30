# CellEngine Generic Refactor Plan

**Author:** Omega session 2026-04-30 (post commit `1db4408`)
**Status:** APPROVED — user signed off on §7.1 / §7.2 / §7.3 / §7.4 and §6.3 timing override on 2026-04-30
**Scope:** Consolidate the 4 cell-style engines (Tsmom, Donchian, EmaPullback, TrendRider) into a generic `CellEngine<Strategy>` base
**Estimated effort:** 1-2 sessions (refactor) + 1 session (validation) = 3 sessions total

---

## 0. Decisions locked

| Item | Original options | Decision |
|---|---|---|
| §7.1 Multi-position policy | Donchian/Epb stay single-pos OR move to max=10 like Tsmom | **Move to max=10** — align all engines |
| §7.2 Strategy concept enforcement | template errors OR C++20 `concept` | **C++20 `concept`** — cleaner errors, codebase already C++20 |
| §7.3 Sizing override | hardcoded in engine_init.hpp OR fully config-driven | **Hybrid — code defaults + config override** (see §7.3 below for recommendation rationale) |
| §7.4 Engine deletion vs deprecation | delete in Phase 4 OR `#error "deprecated"` for one cycle | **Deprecate** — `#error` stub, deletion in next session after one shipped release |
| §6.3 Timing | defer 2 weeks for TrendRider shadow validation | **Override — proceed immediately**. TrendRider is a rare-event engine (H2 ~67/yr, D1 rare). 2 weeks of shadow data adds little signal vs. cost of stale context. |

---

## 1. Executive summary

The 4 cell-style engines shipped during the 2026-04-30 session share roughly **70% of their structural code** but live as 4 independent files totaling **~2,984 lines**. Each engine reimplements its own `Bar`, `BarSynth`, `ATR14`, multi-position state machine, CSV warmup loader, and Portfolio harness. The trading logic that actually differs between them — entry signal, exit logic, sizing — accounts for only ~30% of each file.

A generic `CellEngine<Strategy>` template would:

- Cut total LOC by ~50% (estimated **~2,984 → ~1,500 lines** including shared base + 4 thin Strategy adapters)
- Make new engine variants a 100-200 line job instead of 700-800
- Eliminate the warmup/synth/ATR drift hazard where one engine fixes a bug and the others don't get it (this has already happened twice — see `set_ewm_drift` injection bug fixed in VWAPStretchReversion 2026-04-09 but never propagated)
- Preserve every byte of existing per-cell behaviour by encoding the entry/exit/sizing differences as pluggable Strategy types

This document is a **design** — it commits to nothing until you approve the trade-offs in §6.

---

## 2. Current state survey

### 2.1 File inventory

| Engine | File | Lines | Cells live | Backtest projection |
|---|---|---:|---:|---:|
| Tsmom | `include/TsmomEngine.hpp` | 728 | 5 (H1/H2/H4/H6/D1 long) | Tier-1, ~$X/yr |
| Donchian | `include/DonchianEngine.hpp` | 776 | 7 (H2L; H4/H6/D1 L+S) | ~$5,620/yr |
| EmaPullback | `include/EmaPullbackEngine.hpp` | 684 | 4 (H1/H2/H4/H6 long) | ~$4,006/yr |
| TrendRider | `include/TrendRiderEngine.hpp` | 796 | 6 (H2/H4 L+S; H6/D1 L) | ~$90K/yr Option B |
| **Total** | | **2,984** | **22** | |

### 2.2 What's duplicated (estimated 70% per file)

Every engine declares near-identical scaffolding:

| Component | Tsmom | Donchian | Epb | TrendRider | Notes |
|---|:-:|:-:|:-:|:-:|---|
| `XBar` (OHLC bar) | YES | YES | YES | YES | Identical fields: `open`, `high`, `low`, `close`, `bar_open_ms`, `bar_close_ms` |
| `XBarSynth` (H1→H2/H4/H6/D1) | YES | YES | YES | YES | Identical aggregation logic; UTC bucket math copy-pasted 4× |
| `XATR14` (Wilder's ATR) | YES | YES | YES | YES | Identical math, different namespace |
| Multi-position `Position` struct | YES | YES | YES | YES | Same 9 fields: entry, sl, size, atr, entry_ms, bars_held, mfe, mae, spread_at, id |
| `closes_` rolling window | YES | YES | YES | YES | Used by entry-signal evaluation |
| `positions_` vector | YES | YES | YES | YES | |
| `has_open_position()`, `n_open()` | YES | YES | YES | YES | |
| `on_bar` SL/MFE/MAE/time-exit loop | YES | YES | YES | YES | Lines 215-239 in Tsmom; structurally identical elsewhere |
| `on_tick` intrabar SL fill loop | YES | YES | YES | YES | Same loop body |
| `_close()` helper | YES | YES | YES | YES | Builds TradeRecord, fires callback |
| `warmup_from_csv()` H1 reader | YES | YES | YES | YES | ~80-line CSV parser duplicated 4× |
| Synth-driven on_bar dispatch (H2 every 2 H1, H4 every 4, etc.) | YES | YES | YES | YES | Lines 553-580 in Tsmom mirror Donchian/Epb/TrendRider exactly |
| `XPortfolio` harness | YES | YES | YES | YES | Holds N cells, drives runtime_cb wrapping |
| Per-cell `wrap(runtime_cb, cell_idx)` adapter | YES | YES | YES | YES | |
| Trade ledger emission | YES | YES | YES | YES | |

### 2.3 What's actually different (estimated 30% per file)

| Aspect | Tsmom | Donchian | EmaPullback | TrendRider |
|---|---|---|---|---|
| **Entry signal** | `closes_[t] - closes_[t-lookback]` direction | 20-bar high/low breakout | 9/21 EMA crossover + pullback | 40-bar high/low breakout |
| **Entry indicator** state | none beyond `closes_` | Donchian channel (max/min over period) | `EpbEMA` 9 + 21 | Donchian channel (40-bar) |
| **Exit logic** | `bars_held >= hold_bars` (time exit) | TP at `entry ± tp_r * sl_pts` | TP at `entry ± tp_r * sl_pts` | **Stage trail only** (no TP, no time exit) |
| **SL placement** | `entry ± hard_sl_atr * atr` | `entry ± sl_atr * atr` | `entry ± sl_atr * atr` | `entry ± sl_n * atr` |
| **Sizing** | baseline risk_pct=0.005, max_lot_cap=0.05 | baseline | baseline | **Option B**: risk_pct=0.040, max_lot_cap=0.50 |
| **Cell config struct** | `lookback`, `hold_bars`, `hard_sl_atr` | `period`, `sl_atr`, `tp_r`, `max_hold` | `ema_fast`, `ema_slow`, `sl_atr`, `tp_r` | `donchian_n`, `sl_n`, `stage_trail[]` |
| **Stage trail** (TrendRider-only) | n/a | n/a | n/a | `[(2.0, 1.5), (5.0, 2.5), (10.0, 3.5)]` |
| **EMA tracker** (Epb-only) | n/a | n/a | `EpbEMA` struct | n/a |
| **Direction support** | long-only | long+short | long-only | long+short |

### 2.4 Bugs already caused by duplication

This is the strongest case for refactoring — bugs that would not have happened with shared scaffolding:

1. **VWAPStretchReversion ewm_drift injection** (2026-04-09): code path was wired into MeanReversion but copy-paste forgot the equivalent for VWAPStretchReversion. The engine ran for an unknown period with `ewm_drift_ = 0.0` permanently, firing 3 wrong-side trades on a -87pt trend day before the fix landed. (Note: this lived in `GoldEngineStack.hpp` not the cell engines, but it's the same class of bug the refactor prevents.)
2. **Cold-start CSV warmup** (2026-04-30): had to be added to all 4 engines in lockstep. Each got its own `warmup_from_csv()` implementation with its own CSV path field and its own H1/H2/H4/H6/D1 dispatch. Any future fix has to ship to 4 places.
3. **Multi-position refactor** (Tsmom only, 2026-04-30): `max_positions_per_cell=10` was added to Tsmom but Donchian/Epb still single-position by default. If we want Donchian to match, that's another 4-file change.

---

## 3. Proposed architecture

### 3.1 Approach: Static polymorphism via templates (CRTP-light)

Two viable approaches were considered:

| Approach | Pros | Cons |
|---|---|---|
| **A. Virtual base class** | Familiar OO, single header per engine | Virtual call per tick (~1ns each), heap-allocated cells, less inlining |
| **B. Template (CRTP / Strategy)** | Zero virtual dispatch, full inlining, compile-time errors for missing hooks | Header-only, slightly higher compile time, template error messages |

**Recommendation: B (template).** The hot path is `on_tick → for each cell → SL check`. Virtual dispatch adds ~5-15ns per cell per tick which compounds with 22 cells × multi-position iteration. Template approach matches the existing zero-cost-abstraction style of the codebase (see `MinMaxCircularBuffer<T,N>` in `GoldEngineStack.hpp`).

### 3.2 Layered abstraction

```
+-------------------------------------------------------------+
|  CellPortfolio<Strategy>                                    |  N=1
|    holds N cells, dispatches H1/H2/H4/H6/D1, runs warmup    |
+-------------------------------------------------------------+
                          |
                          v
+-------------------------------------------------------------+
|  CellBase<Strategy>                                         |  N=22 (one per live cell)
|    on_bar(): manage positions, then Strategy::evaluate()    |
|    on_tick(): intrabar SL/trail check                       |
|    multi-position state, _close(), trade ledger emission    |
+-------------------------------------------------------------+
                          |
                          v
+-------------------------------------------------------------+
|  Strategy concept (4 implementations)                       |
|    TsmomStrategy : returns sig_dir from closes_ momentum    |
|    DonchianStrategy : returns sig_dir from N-bar breakout   |
|    EpbStrategy : EMA pullback signal + EMA state            |
|    TrendRiderStrategy : 40-bar breakout + stage trail mgmt  |
+-------------------------------------------------------------+
                          |
                          v
+-------------------------------------------------------------+
|  Shared primitives (single implementation each)             |
|    Bar (POD), BarSynth, ATR14, EMA, Position                |
|    CSV warmup loader (one impl, parameterised by Strategy)  |
+-------------------------------------------------------------+
```

### 3.3 The Strategy concept

A Strategy is a struct that exposes (compile-time, no inheritance required):

```cpp
struct StrategyConcept {
    // ---- Required types ----
    struct Config { /* engine-specific knobs */ };
    struct State  { /* engine-specific running state, e.g. EMA, Donchian channel */ };

    // ---- Required methods (each one called by CellBase at the right moment) ----

    // (1) Per-bar state update: called BEFORE evaluate(). Updates State given new bar.
    static void on_bar_update(State& s, const Bar& b, const Config& cfg) noexcept;

    // (2) Entry evaluation: called after position management + pre-fire gates.
    // Returns +1 LONG, -1 SHORT, 0 NO_SIGNAL. Pure function of state.
    static int evaluate(const State& s, const Config& cfg, int direction) noexcept;

    // (3) SL placement: called when opening a new position.
    // Returns absolute SL price. atr14 is the value AT signal time.
    static double initial_sl(double entry_px, int direction, double atr14,
                             const Config& cfg) noexcept;

    // (4) TP placement: called when opening. Return 0.0 to mean "no TP".
    // (TrendRiderStrategy returns 0.0 — pure trail.)
    static double initial_tp(double entry_px, int direction, double atr14,
                             const Config& cfg) noexcept;

    // (5) Per-tick position management hook: called on every tick for each open
    // position. Lets the strategy adjust SL (trail), update internal state, or
    // request a forced close. Returns true if position should be closed at
    // current price. TsmomStrategy/DonchianStrategy/EpbStrategy return false.
    // TrendRiderStrategy advances stage trail here.
    static bool on_tick_manage(Position& p, double bid, double ask,
                               const State& s, const Config& cfg) noexcept;

    // (6) Time-exit check: called per bar. Default impl checks bars_held vs cfg.
    // TrendRiderStrategy returns false (no time exit).
    static bool time_exit(const Position& p, const Config& cfg) noexcept;

    // (7) Sizing override: returns risk_pct + max_lot_cap. Most strategies use
    // baseline values from cfg; TrendRiderStrategy returns Option B.
    static SizingParams sizing(const Config& cfg) noexcept;
};
```

This concept is enforced at compile time when `CellBase<Strategy>` instantiates each call. Missing methods produce template errors at the call site, not link-time mysteries.

### 3.4 CellBase<Strategy>

```cpp
template <typename Strategy>
struct CellBase {
    using Cfg   = typename Strategy::Config;
    using State = typename Strategy::State;

    // ---- Identity ----
    int          direction       = 1;
    std::string  timeframe       = "H1";
    std::string  symbol          = "XAUUSD";
    std::string  cell_id         = "";

    // ---- Common config ----
    bool   shadow_mode             = true;
    bool   enabled                 = true;
    int    max_positions_per_cell  = 10;
    int    cooldown_bars           = 0;
    double max_spread_pt           = 1.5;

    // ---- Strategy config + state ----
    Cfg    cfg;
    State  state;

    // ---- Multi-position state ----
    std::vector<Position> positions_;
    int    trade_id_      = 0;
    int    bar_count_     = 0;
    int    cooldown_left_ = 0;

    int  on_bar(const Bar& b, double bid, double ask, double atr14_at_signal,
                int64_t now_ms, double size_lot, OnCloseCb on_close) noexcept {
        ++bar_count_;

        // 1. Manage open positions (SL/MFE/MAE/time-exit) — IDENTICAL across all 4 engines
        //    except time_exit dispatches to Strategy::time_exit().
        manage_positions_(b, now_ms, on_close);

        // 2. Strategy-specific state update
        Strategy::on_bar_update(state, b, cfg);

        // 3. Decrement cooldown (legacy)
        if (cooldown_left_ > 0) --cooldown_left_;

        // 4. Pre-fire gates — IDENTICAL
        if (!enabled) return 0;
        if ((int)positions_.size() >= max_positions_per_cell) return 0;
        if (cooldown_left_ > 0) return 0;
        if (!std::isfinite(atr14_at_signal) || atr14_at_signal <= 0.0) return 0;
        const double spread_pt = ask - bid;
        if (!std::isfinite(spread_pt) || spread_pt < 0.0) return 0;
        if (spread_pt > max_spread_pt) return 0;
        if (size_lot <= 0.0) return 0;

        // 5. Strategy entry evaluation
        const int sig_dir = Strategy::evaluate(state, cfg, direction);
        if (sig_dir == 0 || sig_dir != direction) return 0;

        // 6. Open position via Strategy hooks
        const double entry_px = direction == 1 ? ask : bid;
        Position p;
        p.entry      = entry_px;
        p.sl         = Strategy::initial_sl(entry_px, direction, atr14_at_signal, cfg);
        p.tp         = Strategy::initial_tp(entry_px, direction, atr14_at_signal, cfg);
        p.size       = size_lot;
        p.atr        = atr14_at_signal;
        p.entry_ms   = now_ms;
        p.spread_at  = spread_pt;
        p.id         = ++trade_id_;
        positions_.push_back(p);

        if (cooldown_bars > 0) cooldown_left_ = cooldown_bars;
        log_entry_(p, sig_dir, atr14_at_signal, spread_pt);
        return 1;
    }

    void on_tick(double bid, double ask, int64_t now_ms, OnCloseCb on_close) noexcept {
        for (auto it = positions_.begin(); it != positions_.end(); ) {
            Position& p = *it;
            // Strategy hook: advance trail, check forced exits, etc.
            const bool should_close = Strategy::on_tick_manage(p, bid, ask, state, cfg);
            if (should_close) {
                _close(p, direction == 1 ? bid : ask, "STRATEGY_CLOSE", now_ms, on_close);
                it = positions_.erase(it);
                continue;
            }
            // SL fill — IDENTICAL across engines
            const double px = direction == 1 ? bid : ask;
            const bool sl_hit = direction == 1 ? (px <= p.sl) : (px >= p.sl);
            if (sl_hit) {
                _close(p, p.sl, "SL_HIT_TICK", now_ms, on_close);
                it = positions_.erase(it);
                continue;
            }
            ++it;
        }
    }
};
```

### 3.5 Strategy implementation example: TsmomStrategy

```cpp
struct TsmomStrategy {
    struct Config {
        int    lookback      = 20;
        int    hold_bars     = 12;
        double hard_sl_atr   = 3.0;
    };
    struct State {
        std::deque<double> closes_;
    };

    static void on_bar_update(State& s, const Bar& b, const Config& cfg) noexcept {
        s.closes_.push_back(b.close);
        const std::size_t cap = static_cast<std::size_t>(cfg.lookback) + 1;
        while (s.closes_.size() > cap) s.closes_.pop_front();
    }

    static int evaluate(const State& s, const Config& cfg, int direction) noexcept {
        if ((int)s.closes_.size() < cfg.lookback + 1) return 0;
        const double cur     = s.closes_.back();
        const double earlier = s.closes_[s.closes_.size() - 1 - cfg.lookback];
        const double ret_n   = cur - earlier;
        if (ret_n > 0.0) return +1;
        if (ret_n < 0.0) return -1;
        return 0;
    }

    static double initial_sl(double entry, int dir, double atr14, const Config& cfg) noexcept {
        return entry - dir * (atr14 * cfg.hard_sl_atr);
    }
    static double initial_tp(double, int, double, const Config&) noexcept {
        return 0.0;  // Tsmom uses time exit, no TP
    }
    static bool on_tick_manage(Position&, double, double, const State&, const Config&) noexcept {
        return false;  // Tsmom does no per-tick adjustment
    }
    static bool time_exit(const Position& p, const Config& cfg) noexcept {
        return p.bars_held >= cfg.hold_bars;
    }
    static SizingParams sizing(const Config&) noexcept {
        return {0.005, 0.05};  // baseline
    }
};
```

That's ~50 lines. Compare to TsmomCell + TsmomBar + TsmomBarSynth + TsmomATR14 + TsmomPortfolio (~728 lines).

### 3.6 Strategy implementation example: TrendRiderStrategy (the interesting one)

```cpp
struct TrendRiderStrategy {
    struct Config {
        int    donchian_n     = 40;
        double sl_n           = 1.5;
        // Stage trail: each entry is (R-multiple-trigger, ATR-multiple-trail-distance)
        std::array<std::pair<double, double>, 3> stages = {{
            {2.0, 1.5}, {5.0, 2.5}, {10.0, 3.5}
        }};
    };
    struct State {
        DonchianChannel<40> channel;  // shared primitive
    };

    static void on_bar_update(State& s, const Bar& b, const Config&) noexcept {
        s.channel.push(b);
    }
    static int evaluate(const State& s, const Config& cfg, int direction) noexcept {
        if (!s.channel.ready()) return 0;
        // 40-bar breakout: long if close > prior_high, short if close < prior_low
        if (s.channel.close_above_prior_high()) return +1;
        if (s.channel.close_below_prior_low())  return -1;
        return 0;
    }
    static double initial_sl(double entry, int dir, double atr14, const Config& cfg) noexcept {
        return entry - dir * (atr14 * cfg.sl_n);
    }
    static double initial_tp(double, int, double, const Config&) noexcept {
        return 0.0;  // pure trail
    }
    static bool on_tick_manage(Position& p, double bid, double ask,
                               const State&, const Config& cfg) noexcept {
        // Advance stage trail. R-multiple = (current_move) / (initial_risk).
        const int    dir       = p.entry > p.sl ? +1 : -1;  // recover dir from sl side
        const double cur_px    = dir == 1 ? bid : ask;
        const double move      = dir * (cur_px - p.entry);
        const double initial_R = std::fabs(p.entry - p.initial_sl);  // need to store initial_sl
        if (initial_R <= 0.0) return false;
        const double r_mult = move / initial_R;
        // Walk stages from highest R-trigger downward; first match wins.
        for (auto it = cfg.stages.rbegin(); it != cfg.stages.rend(); ++it) {
            if (r_mult >= it->first) {
                const double new_sl = cur_px - dir * (it->second * p.atr);
                // Trail only in profit direction — never widen.
                if (dir == 1 && new_sl > p.sl) p.sl = new_sl;
                if (dir == -1 && new_sl < p.sl) p.sl = new_sl;
                break;
            }
        }
        return false;
    }
    static bool time_exit(const Position&, const Config&) noexcept {
        return false;  // TrendRider has no time exit
    }
    static SizingParams sizing(const Config&) noexcept {
        return {0.040, 0.50};  // Option B
    }
};
```

This shows the design works for the most complex of the 4 engines. The stage trail logic — currently ~80 lines spread across `TrendRiderCell::on_tick` — collapses to ~15 lines of strategy code.

Note: Position struct needs one new field — `initial_sl` — to compute R-multiple after the SL has trailed. That's a 1-line addition to the shared Position.

### 3.7 Shared primitives

Single implementations replace the 4× duplicates:

```cpp
struct Bar { double open, high, low, close; int64_t bar_open_ms, bar_close_ms; };

struct BarSynth {
    void on_h1_close(const Bar& h1);
    bool h2_close() const;  bool h2_bar(Bar& out) const;
    bool h4_close() const;  bool h4_bar(Bar& out) const;
    bool h6_close() const;  bool h6_bar(Bar& out) const;
    bool d1_close() const;  bool d1_bar(Bar& out) const;
    // Internal: UTC bucket math copy-pasted 4× currently
};

struct ATR14 {
    void on_bar(const Bar& b) noexcept;
    double value() const noexcept;
    bool ready() const noexcept;
};

struct Position {
    double  entry, sl, tp, initial_sl, size, atr;
    int64_t entry_ms;
    int     bars_held = 0;
    double  mfe = 0.0, mae = 0.0, spread_at = 0.0;
    int     id = 0;
};
```

### 3.8 CellPortfolio harness

```cpp
template <typename Strategy>
struct CellPortfolio {
    std::vector<CellBase<Strategy>> cells_;
    BarSynth synth_;
    ATR14 atr_h1_, atr_h2_, atr_h4_, atr_h6_, atr_d1_;
    std::string warmup_csv_path;

    void on_h1_bar(const Bar& h1, double bid, double ask, int64_t now_ms,
                   const RuntimeCb& runtime_cb) {
        // Drive ATR + cells per timeframe — IDENTICAL logic across 4 engines today
        atr_h1_.on_bar(h1);
        for (auto& c : cells_h1_) c.on_bar(h1, bid, ask, atr_h1_.value(), now_ms, /*lot=*/0, ...);
        synth_.on_h1_close(h1);
        if (synth_.h2_close()) { Bar b; synth_.h2_bar(b); atr_h2_.on_bar(b); /* ... */ }
        // ... H4, H6, D1
    }
    void on_tick(double bid, double ask, int64_t now_ms, const RuntimeCb& runtime_cb) {
        for (auto& c : cells_) c.on_tick(bid, ask, now_ms, /* wrapped cb */);
    }
    int warmup_from_csv(const std::string& path) noexcept;  // single impl
};
```

### 3.9 Engine-as-typedef

After refactor, each engine's public header collapses to:

```cpp
// TsmomEngine.hpp (new)
#pragma once
#include "CellEngine.hpp"
#include "TsmomStrategy.hpp"
namespace omega {
    using TsmomPortfolio = CellPortfolio<TsmomStrategy>;
}
```

That's the public surface. globals.hpp's `static omega::TsmomPortfolio g_tsmom;` still compiles unchanged.

---

## 4. Migration plan

The refactor MUST preserve every byte of existing trading behaviour. We get there in 4 phases, each independently shippable, with the live system continuing to run unchanged after each one.

### Phase 1: Extract shared primitives (1 session, low risk)

Create `include/CellPrimitives.hpp` with:
- `Bar` struct (POD)
- `BarSynth`
- `ATR14`
- `Position` (with new `initial_sl` field)
- `EMA9_21` (used only by EmaPullback today, but generally useful)

Each of the 4 existing engines keeps its own private types but adds a `static_assert(sizeof(TsmomBar) == sizeof(omega::Bar))` etc. as a structural sanity check. **No behaviour change.** Branch: `cell-refactor-phase-1`.

Validation: existing tests pass, live shadow run unchanged for 24h.

### Phase 2: Introduce CellEngine.hpp + TsmomStrategy (1 session, medium risk)

Create `include/CellEngine.hpp` with `CellBase<Strategy>` and `CellPortfolio<Strategy>`. Implement `TsmomStrategy`. Add `using TsmomPortfolioV2 = CellPortfolio<TsmomStrategy>;` and a parallel `g_tsmom_v2` instance running in **shadow alongside** the existing `g_tsmom`.

Both engines see the same H1 bars, atomic L2, etc. **Compare ledgers daily.**

**Two-stage validation protocol (locked per §7.1 multi-position policy change):**

Phase 2a — *Refactor-correctness validation* (V1 vs V2 must produce identical
ledgers):
1. Run V2 with `max_positions_per_cell = 1` (matches V1's effective behaviour,
   since V1 wasn't actually opening multiple positions per cell).
2. V1 and V2 must produce byte-identical entries, SLs, TPs, and exit reasons
   for at least 5 trading days.
3. Any divergence requires explanation and fix before proceeding.

Phase 2b — *Multi-position policy rollout* (intentional behaviour change):
4. Once Phase 2a passes, flip V2 to `max_positions_per_cell = 10`. V2 will
   now open positions that V1 wouldn't have. This is the §7.1 policy change.
5. Observe new V2 trade behaviour for at least N=5 additional trading days
   before retiring V1.
6. Capture the policy-change effect (additional trades, drawdown impact, PnL
   delta) in a separate ledger comparison so it's never confused with
   refactor-correctness validation.

This separation is non-negotiable: mixing refactor-correctness validation
with a policy change makes it impossible to attribute any divergence
correctly. Phase 2a validates the refactor; Phase 2b validates the policy.

This phase is the highest-risk in the refactor because it's where any
subtle behaviour drift surfaces. Differences in Phase 2a ≠ refactor failure,
but each one needs explanation before progressing.

### Phase 3: Migrate Donchian, EmaPullback, TrendRider (1 session, medium risk)

Once TsmomStrategy validates byte-for-byte, the remaining 3 strategies go in fast — they have no design unknowns left.

Per engine:
1. Implement the Strategy struct
2. Add `using XPortfolioV2 = CellPortfolio<XStrategy>;`
3. Run V2 in shadow alongside V1 for 3-5 days
4. Verify identical ledgers
5. Cut over: replace `g_x` with `g_x_v2`, delete the old engine file

TrendRider is the riskiest of the three because of stage-trail complexity. Schedule it last so the patterns are well-established.

### Phase 4: Delete dead code (1 session, low risk)

Remove `TsmomEngine.hpp`, `DonchianEngine.hpp`, `EmaPullbackEngine.hpp`, `TrendRiderEngine.hpp`. Update includes in `globals.hpp`. Verify build, deploy, monitor for 1 day.

---

## 5. Compile-time and binary-size considerations

- **Compile time:** Templates make headers heavier. The 4 engine includes in `globals.hpp` total ~3,000 lines today; post-refactor, `CellEngine.hpp` + 4 strategies is probably ~1,500 lines but every TU that includes it pays the full template cost. Mitigation: `#include "CellEngine.hpp"` only from `globals.hpp` (single TU), strategies live in their own headers. We already have `// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp` precedent.
- **Binary size:** Template instantiation happens 4× (once per Strategy). Each instantiation produces fully inlined code — no shared code path. Net binary change is approximately neutral (less duplicate source → similar inlined output).
- **Inlining:** All Strategy hook calls inline to direct function calls in optimised builds. Zero virtual dispatch. The CRTP pattern's main runtime advantage.
- **Debug build:** Template error messages can be verbose. Mitigate with `static_assert` checks on the Strategy concept at the top of `CellBase`.

---

## 6. Risks and decision points

### 6.1 Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Behaviour drift in Phase 2 (V2 produces different trades than V1) | Medium | High — invalidates 4-week shadow validation | Side-by-side shadow runs in Phase 2-3, byte-level ledger diff before cutover |
| Template error messages confuse future debugging | Medium | Medium | `static_assert` on Strategy concept; document concept clearly |
| TrendRider stage-trail logic doesn't fit cleanly into `on_tick_manage` hook | Low | Medium | Stage trail is the riskiest abstraction; full prototype in Phase 2 alongside Tsmom |
| Refactor consumes time better spent on new alpha research | High | Variable | This is the biggest non-technical risk — see §6.2 |
| Compile time grows enough to slow iteration | Low | Low | Header guards already isolate to single TU; measure post-Phase 2 |
| Multi-position semantic differences (Tsmom 10 / others 1) | Already exists | Low | Pre-refactor: bring Donchian/Epb to max=10 to match Tsmom OR keep per-cell config (preferred — already a Cfg field) |

### 6.2 The biggest decision: is this worth doing now?

The refactor is **architecturally correct** but **tactically debatable**. Arguments for and against:

**FOR doing it now:**
- Bug-prevention: every duplication-driven bug from this point gets prevented
- Adding Tier-5/6 engines becomes trivial (100 LOC instead of 700)
- Engineering velocity compounds — every engine config change touches 1 place not 4
- The 4 engines just shipped and you have full context on them; in 2 months that context degrades

**AGAINST doing it now:**
- The 4 engines are working. "If it ain't broke, don't refactor it" is a real rule
- TrendRider just shipped at Option B sizing and is unvalidated. Refactoring during shadow validation adds noise to the validation
- Total time: 3 sessions minimum, possibly 4-5 if Phase 2 reveals subtle drift. That's 3-5 sessions you could spend on new alpha
- The bugs duplication caused so far have been small/recoverable

### 6.3 Recommended path — SUPERSEDED 2026-04-30

**Original recommendation:** Defer the refactor by 2 weeks pending TrendRider
shadow validation.

**User override (2026-04-30):** Proceed immediately. Rationale:

1. **TrendRider is a rare-event engine.** H2 ~67/yr expected, H4/H6 less, D1
   rare. A 2-week shadow window is unlikely to produce statistically
   meaningful trade count regardless. Deferring trades engineering velocity
   for shadow data we can't actually observe.
2. **Context decay is real.** The 4 engines just shipped and the architectural
   context is fresh. In 2 weeks, that context degrades and the refactor gets
   meaningfully harder.
3. **Phase 2 shadow validation gates the actual cutover anyway.** The byte-
   level ledger diff between V1 and V2 in Phase 2-3 is the real
   safety-critical step. Deferring Phase 1 (extract shared primitives, no
   behaviour change) buys nothing.

**Constraints to honour as we proceed:**

- No new engine cells until refactor lands (don't add code to files we're
  about to consolidate).
- 3 consecutive sessions ideal — minimise the V1/V2 parallel-run window.
- Phase 2 byte-for-byte ledger validation is non-negotiable.
- If install/deploy issues persist on VPS, refactor commits accumulate
  unbuilt — flag this aggressively rather than letting drift compound.

---

## 7. Open questions — RESOLVED 2026-04-30

### 7.1 Multi-position policy — RESOLVED: align to max=10

All cell engines move to `max_positions_per_cell=10`, matching Tsmom and
roughly matching TrendRider's `max_concurrent=6`. Donchian and EmaPullback
were single-position by default; they migrate to multi-position semantics.

**Implication:** Donchian/EmaPullback live behaviour will diverge from the
pre-refactor 1db4408 behaviour the moment they cut over to V2. This is an
intentional behaviour change, NOT a refactor bug. Phase 2 byte-for-byte
ledger validation must be done with `max=1` first to isolate refactor-
correctness from policy-change effects, THEN switch to `max=10` after the
identical-trades validation passes.

**Phase 2 protocol updated:**
1. V1 vs V2 with `max_positions_per_cell=1` for both → must produce identical
   ledgers for at least 5 trading days.
2. Once identical, flip V2 to `max_positions_per_cell=10` and observe new
   trade behaviour for further N days before V1 retirement.

### 7.2 Strategy concept enforcement — RESOLVED: C++20 `concept`

Codebase compiles at C++20 already (`set(CMAKE_CXX_STANDARD 20)` in
CMakeLists.txt). Use `concept` keyword to express the Strategy contract:

```cpp
template <typename S>
concept CellStrategy = requires(typename S::State& state,
                                const typename S::Config& cfg,
                                const Bar& b, Position& p,
                                double bid, double ask, double atr14,
                                int direction) {
    // Required nested types
    typename S::Config;
    typename S::State;

    // Required static methods
    { S::on_bar_update(state, b, cfg) } -> std::same_as<void>;
    { S::evaluate(std::as_const(state), cfg, direction) } -> std::same_as<int>;
    { S::initial_sl(0.0, direction, atr14, cfg) } -> std::same_as<double>;
    { S::initial_tp(0.0, direction, atr14, cfg) } -> std::same_as<double>;
    { S::on_tick_manage(p, bid, ask, std::as_const(state), cfg) } -> std::same_as<bool>;
    { S::time_exit(std::as_const(p), cfg) } -> std::same_as<bool>;
    { S::sizing(cfg) } -> std::same_as<SizingParams>;
};

template <CellStrategy Strategy>
struct CellBase { /* ... */ };
```

Missing or wrong-typed methods now produce a clean
`'concept CellStrategy<TsmomStrategy>' is not satisfied` error pointing
at the exact missing method, instead of a 200-line template instantiation
trace.

### 7.3 Sizing override — RESOLVED: hybrid (code defaults + config override)

Pure config-driven sizing has a real safety cost: an accidental
`omega_config.ini` edit could silently amplify risk (TrendRider Option B
is 8x baseline; a config typo could push it to 80x without rebuild or
review). Pure hardcoded loses tuning velocity.

Hybrid approach:

```cpp
struct SizingParams { double risk_pct; double max_lot_cap; };

// Inside each Strategy:
static SizingParams sizing(const Config& cfg) noexcept {
    return {
        cfg.risk_pct_override     > 0.0 ? cfg.risk_pct_override     : DEFAULT_RISK_PCT,
        cfg.max_lot_cap_override  > 0.0 ? cfg.max_lot_cap_override  : DEFAULT_MAX_LOT_CAP
    };
}
```

Per-strategy `DEFAULT_RISK_PCT` and `DEFAULT_MAX_LOT_CAP` are constants in
the Strategy header — visible, code-reviewed, never amplified by config.
Config can override per-cell via `[trend_rider.h2_long]` etc., but only
with explicit positive values. Default `0.0` in config = no-op = safe.

**Concrete defaults (locked at refactor time):**

| Engine | DEFAULT_RISK_PCT | DEFAULT_MAX_LOT_CAP |
|---|---:|---:|
| Tsmom | 0.005 | 0.05 |
| Donchian | 0.005 | 0.05 |
| EmaPullback | 0.005 | 0.05 |
| TrendRider | 0.040 | 0.50 |

These match current production. Config overrides are opt-in.

### 7.4 Engine deletion vs deprecation — RESOLVED: deprecate

Phase 4 keeps the old files with a deprecation stub instead of deleting
them. Each old header becomes a one-liner that includes the new path:

```cpp
// TsmomEngine.hpp (after Phase 4 — deprecation stub)
#pragma once
#warning "TsmomEngine.hpp is deprecated; include CellEngine.hpp directly. \
This stub will be removed in the next session after the refactor ships."
#include "CellEngine.hpp"
#include "TsmomStrategy.hpp"
namespace omega {
    using TsmomPortfolio = CellPortfolio<TsmomStrategy>;
}
```

Rationale:
- Any forgotten `#include "TsmomEngine.hpp"` in some out-of-scope file
  still compiles. No silent breakage.
- `#warning` (not `#error`) so the build doesn't fail — just nags. After
  one shipped release confirms no breakage, delete in the following session.
- Single-line file means zero maintenance cost during the deprecation window.

---

## 8. Summary

| | Before | After |
|---|---|---|
| Lines of cell-engine code | ~2,984 | ~1,500 |
| Files | 4 | 1 base + 4 strategy headers |
| New engine variant cost | ~700 LOC | ~150 LOC |
| Fix-once-applies-to-all bugs | 0 (bugs need 4 fixes) | All shared-primitive bugs |
| Runtime overhead | 0 | 0 (template inlines) |
| Risk to current production | n/a | Medium during Phase 2-3 |
| Total session cost | 0 | 3-5 sessions |

**This document is a plan. Nothing in `include/` has been changed by the refactor planning task. Next step: user approval of the trade-offs in §6.2 before any Phase 1 code lands.**
