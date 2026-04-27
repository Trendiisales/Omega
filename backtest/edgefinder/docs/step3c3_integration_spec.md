# C6 #1C Step 3c.3 — Live HBG vol_bar60 gate integration spec

Branch context: `c6-1c-step3c-deploy` (HEAD `cdc69538` after 3c.2).
Audit ref: `origin/main` `96cb951d`.

This document specifies how the `vol_bar60 >= 0.000375` gate ships into live
HBG. **No code changes are made by this step.** Implementation happens in a
follow-up session.

---

## 1. Decisions (from C6 #1C session 3 dialogue)

- **Q1 — Compute path: C++ in-process (Path A).** Audit below shows
  `g_bars_gold.m1` (the global `OHLCBarEngine` for XAUUSD) already
  maintains a `std::deque<OHLCBar> bars_` of M1 bars (cap 300) and already
  computes `log_return = ln(close[t]/close[t-1])` per bar. Adding a
  `vol_bar60` indicator costs one rolling-window `std::sqrt`-of-variance
  calculation per closed bar. No IPC, no Python sidecar in the live loop.

- **Q2 — Threshold: hardcoded absolute `0.000375`, plus instrumentation.**
  Backtested constant. Live HBG tick handler also writes per-minute
  `vol_bar60` samples to a sidecar CSV so the gate can be re-validated
  empirically after 30 days of live data before any rolling-percentile
  variant is considered.

- **Q3 — Edge B stays in the deployed portfolio.** 3c.2 found Edge B's
  gated March mean = -0.014 (sharpe -0.0007, n=90). Joint portfolio
  sharpe (0.442) and sum (8485 pts) still beat Edge A alone meaningfully.
  Diversification is real (outer-zerofill correlation -0.072). Ship both.

---

## 2. Audit findings (live HBG bar machinery)

### 2.1 Bar engine — already there

`include/OHLCBarEngine.hpp`:
- L65: `struct OHLCBar { ts, open, high, low, close, volume }`
- L348: `const std::deque<OHLCBar>& get_bars() const noexcept { return bars_; }`
- L441: `std::deque<OHLCBar> bars_;`
- L252: `bars_.push_back(bar); if (bars_.size() > 300) bars_.pop_front();`
- L1041-1091: `_update_ewma_vol()` already computes `log_return[t] =
  ln(close[t]/close[t-1])` and EWMA realised volatility. `bars_per_year`
  constant = 525600 (= minutes per year), confirming **M1 bars are the
  base unit**.

The 60-bar rolling window we need (`vol_bar60`) is well within the 300-bar
cap, so no buffer changes needed.

### 2.2 Per-symbol bar registry

`include/OHLCBarEngine.hpp` L1592-1610:
```
struct SymBarState {
    OHLCBarEngine m1;   // 1-minute bars
    OHLCBarEngine m5;
    OHLCBarEngine m15;
    OHLCBarEngine h1;
    OHLCBarEngine h4;
};
```
Global `g_bars_gold` lives in `main.cpp` (per the comment at L1615-1617).
HBG already accesses `g_bars_gold.m1` from the call site (see 2.4).

### 2.3 HBG gate cluster

`include/GoldHybridBracketEngine.hpp` `on_tick()` sequence inside
`Phase::ARMED` (L272-340), in evaluation order:

| Step | Line | Gate | On fail |
|---|---|---|---|
| 1 | 277 | `range < MIN_RANGE` or `range > MAX_RANGE` | `phase = IDLE` |
| 2 | 287 | `m_inside_ticks < MIN_BREAK_TICKS` | return (await more ticks) |
| 3 | 292 | `tp_dist < min_tp` (COST_FAIL) | log + `phase = IDLE` |
| 4 | 304-333 | ATR-expansion (ATR_GATE_FAIL) | log + `phase = IDLE` |
| 5 | 336+ | record range; size; fire bracket | enter |

**The vol_bar60 gate goes between step 4 and step 5** — after ATR
expansion passes, before recording range and sizing. Rationale:

- ATR-expansion gate is a *range-of-range proxy* (compares this firing's
  range to the median of recent fired-on ranges). It's about the
  compression structure being big enough.
- vol_bar60 is *price-action volatility* (60-bar realised std of log
  returns). It's about the underlying tape regime.
- These measure different things and are not redundant. ATR-expansion
  could pass (compression looks crisp) while vol_bar60 still says the
  tape is too quiet to fire (sub-decile-7 regime).
- Placing vol_bar60 last means it gates only candidates that already
  passed all structural checks, which is the minimal-impact placement.

### 2.4 HBG access to `g_bars_gold.m1`

`include/tick_gold.hpp` L1980-1988 — HBG `on_tick` is called from inside
the same translation unit that already reads `g_bars_gold.m1.ind.*`
elsewhere (e.g. L99, L267-271, L479, L493). **HBG itself does not currently
take a reference to `g_bars_gold`**: its `on_tick` signature is

```
void on_tick(double bid, double ask, int64_t now_ms,
             bool can_enter,
             bool flow_live, bool flow_be_locked, int flow_trail_stage,
             CloseCallback on_close,
             double book_slope, bool vacuum_ask, bool vacuum_bid,
             bool wall_above, bool wall_below, bool l2_real)
```

The cleanest option is to pass the gate **value** (not a pointer to the
bar engine) into `on_tick`, computed by the caller in `tick_gold.hpp` from
`g_bars_gold.m1`. This keeps HBG decoupled from the bar registry, matches
how every other gate in HBG already works (parameters pushed in by the
caller), and avoids adding a new global dependency to the engine.

---

## 3. Implementation plan

### 3.1 New indicator on `OHLCBarEngine` — `vol_bar60`

`OHLCBarEngine::BarIndicators` (around L77-235, the existing atomic
indicator block):
- Add `std::atomic<double> vol_bar60{0.0};`
- Add `std::atomic<bool>   vol_bar60_ready{false};` (true once 60 closed
  bars have been seen).

`OHLCBarEngine::_update_vol_bar60()` (new private method, called from the
same close-bar code path that already calls `_update_ewma_vol()`):

```
void _update_vol_bar60() {
    const int n = static_cast<int>(bars_.size());
    if (n < 61) {  // need 60 returns -> 61 closes
        ind.vol_bar60_ready.store(false, std::memory_order_relaxed);
        return;
    }
    // 60 most-recent log returns over the last 61 closes
    double sum  = 0.0;
    double sum2 = 0.0;
    int    cnt  = 0;
    for (int i = n - 60; i < n; ++i) {
        const double cp = bars_[i - 1].close;
        const double cc = bars_[i].close;
        if (cp <= 0.0 || cc <= 0.0) continue;
        const double r = std::log(cc / cp);
        sum  += r;
        sum2 += r * r;
        ++cnt;
    }
    if (cnt < 60) {
        ind.vol_bar60_ready.store(false, std::memory_order_relaxed);
        return;
    }
    const double mean = sum / cnt;
    const double var  = (sum2 / cnt) - (mean * mean);
    const double sd   = (var > 0.0) ? std::sqrt(var) : 0.0;
    ind.vol_bar60.store(sd, std::memory_order_relaxed);
    ind.vol_bar60_ready.store(true, std::memory_order_relaxed);
}
```

Population convention: **population std (divide by n)**, not sample std
(n-1). This matches pandas `Series.std(ddof=0)` if 3a/3c.1 used ddof=0,
or `Series.std(ddof=1)` if not. **THIS NEEDS VERIFICATION** before
implementation — see Section 5.

Wire-up: call `_update_vol_bar60()` from the existing close-bar path
right next to the existing `_update_ewma_vol()` call.

Persistence: `save_indicators()` and `load_indicators()` (L1114, L1209)
should serialise `vol_bar60` and `vol_bar60_ready` so warm restart is
instant, matching the existing pattern.

### 3.2 New parameter on `GoldHybridBracketEngine::on_tick`

Add two new defaulted parameters at the end:

```
void on_tick(double bid, double ask, int64_t now_ms,
             bool can_enter,
             bool flow_live, bool flow_be_locked, int flow_trail_stage,
             CloseCallback on_close,
             double book_slope = 0.0,
             bool vacuum_ask = false, bool vacuum_bid = false,
             bool wall_above = false, bool wall_below = false,
             bool l2_real    = false,
             // NEW Step 3c.3
             double vol_bar60       = 0.0,
             bool   vol_bar60_ready = false) noexcept
```

Defaults preserve backward compatibility for any other call site (none
found in audit, but defaults cost nothing).

### 3.3 New gate inside `on_tick` Phase::ARMED

Insert between L333 (end of ATR-expansion gate) and L336 (record-range +
size). Constants live as `static constexpr` members of
`GoldHybridBracketEngine` near the other gate constants:

```
// Step 3c.3 — vol_bar60 gate. Threshold from C6 #1C 3a recommendation
// (decile-7-of-OOS, absolute). See backtest/edgefinder/output/paper_trade/
// step3c_gate_summary.md for justification. Gate is bypassed during
// warmup (first 60 closed M1 bars after engine start or restart).
static constexpr double VOL_BAR60_GATE = 0.000375;
```

Gate body:

```
// ── Step 3c.3: vol_bar60 gate ─────────────────────────────────────
if (vol_bar60_ready && vol_bar60 < VOL_BAR60_GATE) {
    char _buf[512];
    snprintf(_buf, sizeof(_buf),
        "[HYBRID-GOLD] VOL_GATE_FAIL vol_bar60=%.6f threshold=%.6f\n",
        vol_bar60, VOL_BAR60_GATE);
    std::cout << _buf;
    std::cout.flush();
    phase = Phase::IDLE;
    bracket_high = bracket_low = 0.0;
    return;
}
// On warmup (vol_bar60_ready == false), gate is pass-through. Logged
// once per fire so we can audit warmup transitions.
if (!vol_bar60_ready) {
    char _buf[256];
    snprintf(_buf, sizeof(_buf),
        "[HYBRID-GOLD] VOL_GATE_WARMUP bypass (bars<60)\n");
    std::cout << _buf;
    std::cout.flush();
}
```

Warmup behaviour matches the ATR-expansion gate's pass-through-during-
warmup pattern exactly (L310 `if ((int)m_range_history.size() >=
EXPANSION_MIN_HISTORY)`).

### 3.4 Caller wiring in `tick_gold.hpp`

At the HBG call sites (L1929 and L1980), pass the two new args:

```
const double v60 = g_bars_gold.m1.ind.vol_bar60.load(std::memory_order_relaxed);
const bool   v60_ok = g_bars_gold.m1.ind.vol_bar60_ready.load(std::memory_order_relaxed);
g_hybrid_gold.on_tick(bid, ask, now_ms_g,
                      hybrid_can_enter, false, false, 0,
                      bracket_on_close,
                      g_macro_ctx.gold_slope,
                      g_macro_ctx.gold_vacuum_ask,
                      g_macro_ctx.gold_vacuum_bid,
                      g_macro_ctx.gold_wall_above,
                      g_macro_ctx.gold_wall_below,
                      g_macro_ctx.gold_l2_real,
                      v60, v60_ok);
```

Apply at both the position-management call (L1929) and the new-entry call
(L1980). The position-management path doesn't fire entries, but consistency
prevents the optional-arg defaults from quietly firing if anyone ever
restructures the path.

### 3.5 Sidecar logging for re-validation (Q2 instrumentation)

New per-tick CSV writer, on closed-M1-bar boundary only (not per tick) to
keep volume sane (~1 row/min during gold trading hours, ~0 outside).

Path: `logs\gold\omega_vol_bar60_YYYY-MM-DD.csv` (matches existing
`logs\gold\omega_gold_trade_closes_YYYY-MM-DD.csv` pattern).

Schema:
```
ts_utc,close_price,vol_bar60,vol_bar60_ready,gate_passes
```
- `ts_utc` — bar close timestamp, ISO-8601 UTC.
- `close_price` — `bars_.back().close` at write time.
- `vol_bar60` — current value.
- `vol_bar60_ready` — bool, "warmup or live".
- `gate_passes` — `vol_bar60 >= 0.000375` (1/0).

Implementation: a small writer hooked into the close-bar path of
`g_bars_gold.m1`, gated by an `OMEGA_LOG_VOL_BAR60` env or config flag so
it's easy to disable. Open file lazily on first write, reopen on UTC date
roll. Buffered append, no fsync per row.

Re-evaluation policy: after 30 calendar days of live data with
non-warmup `vol_bar60_ready == true`, run a small Python script over the
sidecar CSV to compute the live distribution and compare:
- live decile-7 cutoff vs 0.000375 (drift)
- gate fire rate (rows/day where `gate_passes==1`)
- co-occurrence with HBG actual fires from `omega_gold_trade_closes_*.csv`

If live decile-7 cutoff drifts more than ±25% from 0.000375, escalate to
either re-tuning the absolute threshold or shipping the rolling-percentile
variant. Until then, the absolute threshold stands.

### 3.6 Edge mapping — confirm before code

3c.2 deployable edges: pid 57226 SHORT b5 (Edge A), pid 51274 LONG b5
(Edge B). The edge-finder pipeline produced these IDs from a panel that
likely covers multiple bracket-style engines. **Before implementing, Jo
must confirm that `bracket_id=5` in the edge-finder panel maps to
`GoldHybridBracketEngine` specifically — and not to `BracketEngine` or
`IndexHybridBracketEngine`.** If it maps to a different engine, the gate
needs to live there instead.

If the bracket-engine mapping is ambiguous, the safest path is a
git-grep / panel-schema audit to confirm the source. Out of scope for
this spec; flag-only.

---

## 4. Build sequence (when implementation runs in next session)

1. Edit `include/OHLCBarEngine.hpp` — add `vol_bar60` indicator + update
   method + persistence hooks.
2. Edit `include/GoldHybridBracketEngine.hpp` — add gate constant, two
   `on_tick` parameters, gate body.
3. Edit `include/tick_gold.hpp` — wire args at two call sites.
4. Add sidecar writer (location TBD; could live in `OHLCBarEngine` itself
   keyed by config flag, or in a separate `omega_vol_logger.hpp`).
5. Clean rebuild on Mac:
   ```
   cd build && rm -rf * && cmake .. && make
   ```
6. VPS clean rebuild after Mac smoke test passes:
   ```
   cd C:\Omega\build && rm -rf * && cmake .. && cmake --build . --config Release
   ```
   Use VPS cmake at `C:\vcpkg\downloads\tools\cmake-3.31.10-windows\
   cmake-3.31.10-windows-x86_64\bin\cmake.exe`.
7. QUICK_RESTART must include `.obj`/`.pch` delete + post-build timestamp
   check (per Jo's mandatory checklist) since `.hpp` files changed.
8. VERIFY_STARTUP must include hash-vs-HEAD check (per Jo's mandatory
   checklist).

The spec ends here. **No commits to core .hpp files happen during this
session.** Implementation requires explicit go-ahead in the next session
plus the bracket-engine mapping confirmation from §3.6.

---

## 5. Open items / risks before implementation

1. **Std convention (ddof=0 vs ddof=1).** The 3a regrade module computes
   `vol_bar60` from raw ticks via the regime-diagnostic pipeline. Need to
   read `regime_diagnostic.py` (commit `3b383074`, branch
   `c6-1c-step3-regime-diagnostic`) to confirm whether it uses pandas'
   default (ddof=1, sample) or ddof=0 (population). The C++ implementation
   must match exactly. Mismatch produces a small bias that shifts the
   effective threshold and invalidates the backtest reconciliation.

2. **vol_bar60 input source.** Spec uses `bars_[i].close`. The 3a
   computation may use mid (`(bid+ask)/2`) sampled at bar close, or
   midprice trade-by-trade then aggregated. Need to verify
   `regime_diagnostic.py` uses the same bar-close basis.

3. **Time alignment.** The diagnostic journal uses `ts_close` as the
   trade exit time, but the `vol_bar60` joined onto each trade is
   evaluated at *entry time*, not exit. Need to confirm that
   `regime_diagnostic.py` computes `vol_bar60` at the entry moment using
   only past data. If it accidentally uses future bars relative to entry,
   the backtest is contaminated and the live gate will not replicate.

4. **Bracket-engine mapping.** §3.6 above.

5. **Warmup contamination.** During warmup (first 60 bars after restart),
   the gate is pass-through. On a cold start with no persistence load,
   that's the first 60 minutes of trading — during which HBG can fire
   without the gate. Persistence (§3.1) makes warmup near-instant on
   warm restart, so this only matters on cold start. Document explicitly
   for ops.

6. **Threshold from a 3-month backtest sample.** The 3a recommendation
   `0.000375` came from decile-7 of an OOS population spanning 2024-03 to
   2026-04. If gold's vol regime structurally shifts (say, post-tariff
   normalisation), the live decile-7 cutoff drifts. The 30-day sidecar
   re-evaluation in §3.5 is the planned mitigation.

---

## 6. What this spec does NOT do

- No code is committed to `.hpp` files. The spec is a planning artefact
  only.
- No backtest re-validation against new gate code (impossible until code
  exists). The 3c.1 reconciliation (gate_apply.py against
  regrade_v3_per_edge.csv) is the ground truth that any C++ implementation
  must match within numerical tolerance.
- No live-deployment authorisation. Even after implementation, gating
  HBG live will require Jo's explicit go-ahead per the standing rule on
  not modifying core code without instruction.
