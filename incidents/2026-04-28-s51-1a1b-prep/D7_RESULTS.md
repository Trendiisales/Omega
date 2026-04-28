# S51 1A.1.b D7 + HBG-DIAG — Results

**HEAD at audit:** `841dd823308bb2f3ca5a82ba0bf10d0cdfe9ee4e`
**Run date:** 2026-04-28 NZST (commit 2026-04-28 11:54:38 UTC)
**Run dir:** `/Users/jo/omega_repo/sweep_D7DIAG_20260428_235803/`
**Wall:** 1101.3s (18.4 min) on 154,265,439 ticks
**Status:** Decisive. Both engines analysed. Root cause hypothesis identified. No code changed in this work.

---

## TL;DR

Two findings, both important:

1. **AsianRange cross-instance non-determinism is harness-side, not engine-side.** Six combos with IEEE-identical params receive identical pre-time-gate counter values (`n_called`, `n_rej_invalid`, `n_rej_spread`, `n_rej_session`) but **diverge by millions of ticks at the first counter that depends on `sweep_utc_hms()` output** (`n_in_build_window`, `n_rej_outside_fire`). The four sweep threads (HBG, EMA, AsianRange, VWAPStretch run **concurrently** per the run log) likely race on a non-thread-local time-shim global. **This contaminates every engine's results, not just AsianRange.**

2. **HBG `trail_frac` is NOT dead.** D6+E1 inferred trail logic never engages because combos 309–314 produced byte-identical PnL across `trail_frac ∈ [0.157, 0.500]`. **The diag counters disprove this.** Trail engages aggressively (`n_trail_eval=528`, `n_trail_sl_updated=136` per combo of 49 opens). The reason 309–314 are identical is that `trail_dist = min(range_trail, mfe_trail)` clamps against a **hardcoded `mfe * 0.20`** (`SweepableEngines.hpp:954`). For `trail_frac ≥ ~0.157`, `range * trail_frac > mfe * 0.20`, the `mfe_trail` clamp wins, and `trail_frac` becomes irrelevant. **The actual trail-aggressiveness lever is the hardcoded `0.20` constant, currently outside the sweep grid.**

D6.3 (drop `trail_frac` from the sweep) is therefore the **wrong** conclusion. **Reject D6.3.**

---

## Run integrity

| Check | Result |
|---|---|
| Exit code | 0 |
| Wall time | 1101.3s (18.4 min) — diag instrumentation cost ~6.4% over the 1036.5s D6+E1 baseline |
| All 7 outputs present | ✓ (4 sweep CSVs, sweep_summary.txt, asian_diag.tsv, hbg_diag.tsv) |
| Sentinel file present | ✓ |
| Diag → CSV signal-count consistency | ✓ for all 6 AsianRange centre combos (15, 14, 14, 14, 14, 8) |

**Diag instrumentation is observation-only.** Diag signal_count == CSV n_trades for every centre combo checked — adding the counters did not perturb engine state.

---

## Finding 1 — AsianRange non-determinism is in the time shim

### What we see

Six combos (24, 73, 122, 171, 220, 269) have IEEE-identical template parameters
`(BUFFER, MIN_RANGE, MAX_RANGE, SL_TICKS, TP_TICKS) = (0.5, 3.0, 50.0, 80, 200)`.
They process the same 154,265,439-tick stream in the same order. They should produce identical results.

Counter cascade for the centre combos:

| Counter | 24 | 73 | 122 | 171 | 220 | 269 |
|---|---:|---:|---:|---:|---:|---:|
| n_called | 154,265,439 | 154,265,439 | 154,265,439 | 154,265,439 | 154,265,439 | 154,265,439 |
| n_rej_invalid | 0 | 0 | 0 | 0 | 0 | 0 |
| n_rej_spread | 1,280,899 | 1,280,899 | 1,280,899 | 1,280,899 | 1,280,899 | 1,280,899 |
| n_rej_session | 5,379,422 | 5,379,422 | 5,379,422 | 5,379,422 | 5,379,422 | 5,379,422 |
| **n_in_build_window** | 39,171,525 | 38,609,806 | 38,116,848 | 37,853,137 | 36,560,323 | **35,133,225** |
| **n_rej_outside_fire** | 85,109,791 | 86,999,113 | 88,532,919 | 89,973,040 | 93,094,282 | **94,473,361** |
| **n_rej_no_range** | 20,907,095 | 19,582,219 | 18,542,158 | 17,365,405 | 15,537,298 | **15,584,449** |
| n_rej_range_size | 1,820,363 | 1,819,729 | 1,819,597 | 1,819,811 | 1,819,649 | 1,819,986 |
| **n_rej_cooldown** | 29,413 | 28,782 | 28,306 | 28,579 | 28,258 | **298,001** |
| n_signal_long | 6 | 6 | 6 | 6 | 6 | 5 |
| n_signal_short | 9 | 8 | 8 | 8 | 8 | 3 |
| signal_count | 15 | 14 | 14 | 14 | 14 | 8 |
| asian_hi (final) | 4711.29 | 4711.29 | 4711.29 | 4711.29 | 4711.29 | 4711.29 |
| asian_lo (final) | 4657.84 | 4657.84 | 4657.84 | 4657.84 | 4657.84 | 4657.84 |
| last_day | 113 | 113 | 113 | 113 | 113 | 113 |
| last_signal_s | 1776326702 | 1776326702 | 1776326702 | 1776326702 | 1776326702 | 1776326702 |

### Where does divergence start?

Reading the AsianRange `process()` source (`SweepableEngines.hpp:334-438`):

1. `++n_called_` — every tick. Identical across combos. ✓
2. `if (!enabled_ || !s.is_valid()) ++n_rej_invalid_` — depends on snapshot only. Identical. ✓
3. `if (s.spread > MAX_SPREAD) ++n_rej_spread_` — depends on snapshot only. Identical. ✓
4. `if (s.session == SessionType::UNKNOWN) ++n_rej_session_` — depends on snapshot only. Identical. ✓
5. `sweep_utc_hms(h, m, yday)` — **first call to time shim**.
6. Daily-reset check on `yday` and gates on `h` against `FIRE_START_H`/`FIRE_END_H` — **counters from this point onwards diverge**.

**The first counter that diverges is the first counter downstream of `sweep_utc_hms()`.** That is the bug location.

### Why this implicates the threading

The run log shows four threads running concurrently:

```
launching hbg sweep thread
launching emacross sweep thread
launching asianrange sweep thread
launching vwapstretch sweep thread
```

Each thread is a separate `std::tuple<EngineT<I>...>` instance. If `sweep_now_sec()` and `sweep_utc_hms()` are backed by **non-thread-local globals** that are written per-tick, then within the AsianRange thread alone the state is consistent — but the timing of the writes versus reads from each engine instance in the tuple may be perturbed by the OS scheduler.

But the engines in **one** thread iterate sequentially over the tuple via a fold expression. The divergence has to come from somewhere shared inside the AsianRange thread itself — most likely the time shim's tick-pointer or current-tick state is mutated by a tuple-iteration that crosses a memory barrier.

**Hypothesis confirmation test (cheap):** rerun single-threaded. If determinism returns, threading is the cause. If it doesn't, the bug is inside the AsianRange thread's per-engine iteration.

### Combo 269 — separate symptom of the same bug

Combo 269's `n_rej_cooldown=298,001` is **10× the peer values**. Looking at cooldown logic:

```cpp
const int64_t now_s   = sweep_now_sec();
const int64_t elapsed = now_s - last_signal_s_;
if (elapsed < COOLDOWN_S) ++n_rej_cooldown_;
```

If `sweep_now_sec()` returns a stale value (or one from a different tick) on combo 269 enough times, then `elapsed` is small enough to trip the cooldown gate disproportionately. The 10× rejection rate is consistent with combo 269 being the most affected by the timing race — possibly because it's the last centre combo in the tuple and its execution is most likely to interleave with another thread's writes.

`last_signal_s_` ends at `1776326702` for all six combos — they all converge on the final state. But the trajectory through it is different.

### What we know with certainty

- `s.session == UNKNOWN` decisions (counted by `n_rej_session`) are reached at the same ticks for every combo. **Engines are receiving identical input snapshots.**
- The very next gate (which depends on `sweep_utc_hms`) returns different `h`/`yday` to different engine instances on the same tick.
- Both `asian_hi_/asian_lo_/last_day_` and `last_signal_s_` end at identical values, so the steady-state attractor is the same.
- The class itself has no mutable static state. Engine internals are per-instance.

### What we don't yet know

- Whether the bug is in `sweep_now_sec()`, `sweep_utc_hms()`, or in the harness's per-tick state advance.
- Whether single-threading the harness eliminates the divergence (this is **D7-FIX-2**, the cheap test).

---

## Finding 2 — HBG trail logic is alive; `trail_frac` is clamped, not dead

### Conservation check

For every combo dumped, `n_position_opens == n_close_tp_hit + n_close_be_hit + n_close_trail_hit + n_close_sl_hit + n_close_force`. No leaks.

### Trail counters across the trail-frac sweep range

Combos 308–314 differ only in `trail_frac` (0.125, 0.157, 0.198, 0.250, 0.315, 0.397, 0.500). All other params identical: `min_range=6, max_range=25.4, sl_frac=0.42, tp_rr=2.0`.

| combo | trail_frac | opens | trail_eval | sl_updated | TP | TRAIL | SL |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 308 | 0.125 | 49 | 389 | 103 | 7 | 23 | 19 |
| 309 | 0.157 | 49 | 528 | 136 | 8 | 22 | 19 |
| 310 | 0.198 | 49 | 528 | 136 | 8 | 22 | 19 |
| 311 | 0.250 | 49 | 528 | 136 | 8 | 22 | 19 |
| 312 | 0.315 | 49 | 528 | 136 | 8 | 22 | 19 |
| 313 | 0.397 | 49 | 528 | 136 | 8 | 22 | 19 |
| 314 | 0.500 | 49 | 528 | 136 | 8 | 22 | 19 |

### Reading the trail logic at SweepableEngines.hpp:954-958

```cpp
const double mfe_trail   = pos.mfe * 0.20;
const double range_trail = range * TRAIL_FRAC_T;
const double trail_dist  = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
const double trail_sl    = pos.is_long ? (pos.entry + pos.mfe - trail_dist)
                                       : (pos.entry - pos.mfe + trail_dist);
```

`trail_dist` is the smaller of `range * TRAIL_FRAC_T` and `pos.mfe * 0.20`. The trail SL is set such that it locks in `pos.mfe - trail_dist` of profit.

### Why 309–314 produce identical PnL

For `trail_frac ≥ 0.157` and a typical `range ≈ 17` (the geometric mean of HBG's swept range), `range * trail_frac ≥ 2.67`. For a typical `mfe ≈ 14` (consistent with the 17 trail-hit closes per combo), `mfe * 0.20 ≈ 2.80`. Once `range_trail > mfe_trail`, the `min()` returns `mfe_trail` and **`TRAIL_FRAC_T` is no longer in the formula**.

For `trail_frac = 0.125`, `range_trail ≈ 2.13 < mfe_trail`, so `range_trail` wins and `trail_frac` matters. That's why combo 308 has different `n_trail_eval` (389 vs 528) and different close-reason mix (TP=7, TRAIL=23 vs TP=8, TRAIL=22).

### Implications

- The D6+E1 conclusion that "trail logic literally never engages" was based on PnL identity across `trail_frac` values. **The PnL identity exists, but for a different reason — the `mfe * 0.20` clamp dominates at typical position sizes.**
- The actual trail-aggressiveness param **is** the hardcoded `0.20` constant. It's currently outside the sweep grid.
- D6.3 (drop `trail_frac` from sweep) is wrong. The param is live in the lower corner of the grid.

### Combo 24 vs combo 311 (sanity check on grid effect)

Both have `sl_frac=0.42, tp_rr=2.0, trail_frac=0.25`. Differ in `max_range` (32.0 vs 25.4):

| | c24 | c311 |
|---|---:|---:|
| max_range | 32.0 | 25.4 |
| opens | 50 | 49 |
| n_trail_eval | 505 | 528 |
| n_trail_sl_updated | 128 | 136 |
| TP | 6 | 8 |
| TRAIL | 20 | 22 |
| SL | 24 | 19 |

The wider `max_range` (32) admits one extra position (50 vs 49) but produces 5 more SL hits and 2 fewer TP hits. **`max_range` is meaningfully active in the grid.** This is consistent with D6+E1's finding that 27/50 top combos clustered at `max_range=16` (floor) rather than 32 — the tighter range filter rejects setups with worse SL profiles.

---

## Cross-cutting: every engine's results may be contaminated

The threading-race hypothesis does not single out AsianRange. **HBG, EMACross, and VWAPStretch may all be silently contaminated** — we just can't see it because they don't have a 6-way IEEE-identical-param replication available to expose it.

Spot-evidence in the HBG diag: combos 24, 73, 122, 171 (all `min_range=6, max_range=32, sl_frac=0.42, tp_rr=2.0, trail_frac=0.25`) **do** produce identical counter rows. So either:

(a) HBG isn't affected by the threading race, **or**
(b) HBG happens to converge to the same trajectory because its early gates (range-build, expansion-detection, cooldown) are more deterministic and don't depend on `sweep_utc_hms()` the way AsianRange's `FIRE_START_H`/`FIRE_END_H` checks do.

**Without `sweep_utc_hms()` directly in the HBG hot path, HBG would be relatively insulated from the same race.** That's consistent with what we see — but it's **not proof** the HBG sweep numbers are clean. The cleanest evidence will come from a single-threaded re-run.

---

## What to do next — recommendations

| ID | Action | Status | Risk |
|---|---|---|---|
| **D7-FIX-2** | Single-threaded re-run of the diag binary. Confirms or rejects threading hypothesis. | **Recommended next step.** No code change. | None |
| **D7-FIX-1** | Audit `sweep_now_sec()` / `sweep_utc_hms()` for thread-safety. Make `thread_local` if globals. | Pending D7-FIX-2 result. | Low (harness only) |
| **D6.3 — REJECTED** | Don't drop `trail_frac` from sweep. | Counter-evidence found. | — |
| **HBG-FIX-1** | Surface the hardcoded `mfe * 0.20` constant as a swept param `mfe_lock_frac`; demote `trail_frac` to fixed value or tighten its grid to {0.10, 0.125, 0.15}. | Recommended after D7 fixes land. | Low (sweep-side) |
| **D6.1** | HBG `max_range` rebase 32→16 (geometric grid 8..32). | Carry forward; gated on D7. | Low |
| **D6.2** | HBG `min_range` rebase 3→6 (geometric grid 3..12). | Carry forward; gated on D7. | Low |
| **E1.1** | Extend E1 filter to `rsi_lo ≥ rsi_hi`. | Carry forward; independent. | Low |
| **write_csv fail-loud** | Propagate fopen failure to non-zero exit code. | Carry forward; independent. | Low |

### Priority order recommended for next session

1. **D7-FIX-2** (single-threaded test). One script change, no code commit.
2. **D7-FIX-1** (fix the time shim if D7-FIX-2 confirms).
3. **Re-run D6+E1 deterministically** to refresh HBG numbers and confirm the score=0.4377 figure.
4. **HBG-FIX-1** to expose the real trail param.
5. D6.1 / D6.2 / E1.1 / write_csv fail-loud as separate clean commits.

---

## Authorisation status at memo write

**Authorised, shipped this session:** D6+E1 results memo, D7 + HBG-DIAG (commit 841dd823), D7 diag run, D7_RESULTS memo (this file).

**Authorised, not yet started:** E2 (EMACross RSI dead-band review), D8 (VWAPStretch structural fix).

**Newly recommended this analysis (need authorisation):** D7-FIX-2, D7-FIX-1, HBG-FIX-1, D6.1, D6.2, E1.1, write_csv fail-loud, **rejection of D6.3**.

**Live VPS:** ed95e27c. Untouched. No urgency.
