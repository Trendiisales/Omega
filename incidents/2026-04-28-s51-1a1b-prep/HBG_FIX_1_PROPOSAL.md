# HBG-FIX-1 Proposal — Expose hardcoded `mfe * 0.20` as sweep parameter

**Status:** PROPOSAL. No code changed. Awaiting explicit go/no-go from Jo.

**HEAD at write:** `808f473c8638ed0c2cc7b3cbe1406c7ef4f263b8`
**Predecessors:**
- `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_4.md` queued this work as priority 2.
- `incidents/2026-04-28-s51-1a1b-prep/D6E1_G1CLEAN_RESULTS.md` §1 supplies the empirical case for the swap.
- `backtest/CONCURRENCY.md` and `scripts/check_no_globals_in_hotpath.py` constrain the implementation to thread-local-safe state only.

**Risk classification:** **Medium.** First engine-code modification (`include/SweepableEngines.hpp`) since the start of the G1+G2+G4 sequence. Per memory rule "Never modify core code unless instructed clearly," this proposal must be approved by Jo before any edit.

---

## TL;DR

The hardcoded constant `mfe * 0.20` in HBG_T's trail logic (`include/SweepableEngines.hpp:954`) is the actual trail-aggressiveness lever in the current sweep grid. It is not surfaced as a sweep parameter; the existing `TRAIL_FRAC_T` parameter is largely clamped against it. G1CLEAN evidence (D6E1_G1CLEAN combos 308–314) shows 6/7 `trail_frac` values produce byte-identical PnL because they all clamp against the same `mfe * 0.20` ceiling.

This proposal:
1. Adds a 6th template parameter `MFE_LOCK_FRAC_T` to `HBG_T` with default `0.20` (preserves live-equivalence at the default).
2. Replaces `TRAIL_FRAC_T` in the sweep grid with `MFE_LOCK_FRAC_T`. `TRAIL_FRAC_T` becomes a fixed compile-time constant at `0.25` (live default), no longer swept.
3. Keeps the harness combo count at **490 per engine** (5 swept HBG params × pairwise grid). No grid-architecture change.
4. Touches **only HBG**. Other 3 engines (EMACross, AsianRange, VWAPStretch) untouched.
5. **No live code change.** `GoldHybridBracketEngine.hpp` is not modified. The 0.20 in live remains hardcoded — the change is harness-only.

---

## 1. Verified facts (read this session via GitHub contents API)

### 1a. The hardcoded site exists exactly as memo claims

`include/SweepableEngines.hpp:954` (file blob SHA `be9572f6...`):

```cpp
const double mfe_trail = pos.mfe * 0.20;
const double range_trail = range * TRAIL_FRAC_T;
const double trail_dist = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
```

The `0.20` is a fixed-multiplier *ceiling* on aggressiveness: the trailing stop locks at the *minimum* of `range × TRAIL_FRAC_T` and `MFE × 0.20`.

### 1b. The hardcoded site is mirrored in live code

`include/GoldHybridBracketEngine.hpp:466-468` (file blob SHA `61085be9...`):

```cpp
const double mfe_trail = pos.mfe * 0.20;
const double range_trail = range * TRAIL_FRAC;
const double trail_dist = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
```

The harness HBG_T at L954 is a faithful port. **Live behaviour at the default sweep parameters must remain byte-identical after this change.** The proposal achieves this by keeping the harness default `MFE_LOCK_FRAC_T = 0.20`. Live engine itself is not modified.

### 1c. HBG_T template signature (current)

`include/SweepableEngines.hpp:691-697`:

```cpp
template <
    double MIN_RANGE_T  = 6.0,
    double MAX_RANGE_T  = 25.0,
    double SL_FRAC_T    = 0.5,
    double TP_RR_T      = 2.0,
    double TRAIL_FRAC_T = 0.25
>
class HBG_T {
```

5 template params. Defaults match live (note: `MAX_RANGE` default is 25.0 in live; the harness D6 rebase to 32.0 lives in the *sweep grid* in `OmegaSweepHarness.cpp:599`, not in the engine's template default).

### 1d. Sweep grid is hard-bound to 5 params per engine

`backtest/OmegaSweepHarness.cpp:378-380`:

```cpp
static constexpr int N_PAIRS  = 10;          // C(5,2) = 10
static constexpr int PAIR_GRID = 7;
static constexpr int N_COMBOS = N_PAIRS * PAIR_GRID * PAIR_GRID;  // 490
```

`N_PAIRS = 10` corresponds to the 10 unordered pairs of 5 parameters. Adding a 6th param to HBG would require:
- `N_PAIRS = 15` (C(6,2)) for HBG, but `N_PAIRS = 10` for other engines → split per-engine combo counts → breaks the cross-engine `N_COMBOS` constant assumed throughout the harness.
- Or moving all four engines to 6 params + 15 pairs × 49 = 735 combos each, ~2940 total → moves toward the MSVC OOM ceiling and is out of scope here.

**Therefore the proposal swaps `TRAIL_FRAC_T` for `MFE_LOCK_FRAC_T` in the sweep, not adds.**

### 1e. Empirical case for swapping `trail_frac` out (G1CLEAN combos 308-314)

From `D6E1_G1CLEAN_RESULTS.md` §1:

| trail_frac | trades | WR | total_pnl | stddev_q | score |
|---:|---:|---:|---:|---:|---:|
| 0.125 | 49 | 61.2% | 0.46 | 0.3149 | 0.3462 |
| 0.158 | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.198 | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.250 | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.315 | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.397 | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.500 | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |

All values `>= 0.158` produce byte-identical PnL because `range × TRAIL_FRAC_T` exceeds `MFE × 0.20` for the regime where MFE > 0, so `min()` picks the MFE-clamp. Only `trail_frac = 0.125` differs slightly (smallest bin, no clamp). `TRAIL_FRAC_T` is essentially a single-value parameter under the current grid.

Conversely, `MFE_LOCK_FRAC_T` is the binding constraint and has no signal in the current sweep because it's not swept.

---

## 2. Proposed changes

### 2a. `include/SweepableEngines.hpp` (HBG_T only)

**Current** (L691-697):
```cpp
template <
    double MIN_RANGE_T  = 6.0,
    double MAX_RANGE_T  = 25.0,
    double SL_FRAC_T    = 0.5,
    double TP_RR_T      = 2.0,
    double TRAIL_FRAC_T = 0.25
>
class HBG_T {
```

**Proposed** (add 6th template parameter, **default unchanged from prior behaviour**):
```cpp
template <
    double MIN_RANGE_T     = 6.0,
    double MAX_RANGE_T     = 25.0,
    double SL_FRAC_T       = 0.5,
    double TP_RR_T         = 2.0,
    double TRAIL_FRAC_T    = 0.25,
    double MFE_LOCK_FRAC_T = 0.20
>
class HBG_T {
```

**Current** (L954):
```cpp
const double mfe_trail = pos.mfe * 0.20;
```

**Proposed**:
```cpp
const double mfe_trail = pos.mfe * MFE_LOCK_FRAC_T;
```

**No other changes to HBG_T.** `manage()` body, `confirm_fill()`, `on_tick()`, all members, all other call sites unchanged.

### 2b. `backtest/OmegaSweepHarness.cpp` HBG sweep grid

The existing 5 swept params (`MIN_RANGE`, `MAX_RANGE`, `SL_FRAC`, `TP_RR`, `TRAIL_FRAC`) become (`MIN_RANGE`, `MAX_RANGE`, `SL_FRAC`, `TP_RR`, **`MFE_LOCK_FRAC`**). `TRAIL_FRAC_T` is fixed at the live default `0.25`.

**Current `HBG_AT` alias** (L596-603):
```cpp
template <std::size_t I>
using HBG_AT = omega::sweep::HBG_T<
    6.0  * mult_for_param(static_cast<int>(I), 0),
    32.0 * mult_for_param(static_cast<int>(I), 1),
    0.42 * mult_for_param(static_cast<int>(I), 2),
    2.0  * mult_for_param(static_cast<int>(I), 3),
    0.25 * mult_for_param(static_cast<int>(I), 4)
>;
```

**Proposed**:
```cpp
template <std::size_t I>
using HBG_AT = omega::sweep::HBG_T<
    6.0  * mult_for_param(static_cast<int>(I), 0),  // MIN_RANGE
    32.0 * mult_for_param(static_cast<int>(I), 1),  // MAX_RANGE (D6 rebase)
    0.42 * mult_for_param(static_cast<int>(I), 2),  // SL_FRAC (D6 rebase)
    2.0  * mult_for_param(static_cast<int>(I), 3),  // TP_RR
    0.25,                                            // TRAIL_FRAC (fixed at live default; no longer swept)
    0.20 * mult_for_param(static_cast<int>(I), 4)   // MFE_LOCK_FRAC (HBG-FIX-1: swept)
>;
```

`hbg_params_for()` at L629-635 and the diag dump at L778-784 must mirror this change. **Param slot 4 in the output CSVs becomes `mfe_lock_frac` instead of `trail_frac`.** The CSV header column name in `write_csv()` for HBG must be updated to match (the harness writes a `p4` column with a header label per engine — I'll verify the exact header line during the implementation phase, not in this proposal).

### 2c. CSV output column naming

The CSV writers will emit param slot 4 with whatever label the existing code uses. I will verify the exact header generation path during implementation. The proposal commits to: **the rebuilt CSV's column 4 header reads `mfe_lock_frac` (or equivalent label) and its values are 0.20 × the grid multiplier**, with the grid spanning `0.10..0.40` geometrically (`0.20 × 0.5 → 0.20 × 2.0`).

### 2d. What is **NOT** changing

- Live `GoldHybridBracketEngine.hpp` — unchanged. `0.20` remains hardcoded in live. Live binary, VPS deployment, all live trading: untouched.
- Other engines in `SweepableEngines.hpp` (EMACrossT, AsianRangeT, VWAPStretchT) — unchanged.
- The pairwise sweep architecture (`N_PAIRS=10`, `PAIR_GRID=7`, `N_COMBOS=490`) — unchanged.
- The 7-multiplier `GRID_MULT` array — unchanged.
- The G1, G2, G4 infrastructure — unchanged.
- `OmegaTimeShim.hpp`, `CONCURRENCY.md`, lint script — unchanged.
- The CMake build target `OmegaSweepHarness`, the `OMEGA_BUILD_SWEEP=ON` gate — unchanged.

### 2e. Default-equivalence proof

For combo `I` where `mult_for_param(I, 4) == 1.0` (i.e., the combo's pair does not include slot 4), the resolved param values become:

- **Before HBG-FIX-1:** `TRAIL_FRAC_T = 0.25 × 1.0 = 0.25`, `mfe_trail = pos.mfe × 0.20` (hardcoded)
- **After HBG-FIX-1:** `TRAIL_FRAC_T = 0.25` (fixed), `MFE_LOCK_FRAC_T = 0.20 × 1.0 = 0.20`, `mfe_trail = pos.mfe × 0.20`

These produce **byte-identical** values into `mfe_trail`, `range_trail`, and `trail_dist`. For any combo where slot 4 is at multiplier 1.0, the engine state evolution is bit-equivalent to the pre-fix version.

Combos where slot 4 is *not* at 1.0 are precisely the ones we want to vary — those are the 84 combos (7 values × 12 pair-positions / 2 because slot 4 is paired with slots 0..3 = 4 pairs × 49 combos = 196, of which 7×7=49 have slot 4 at multiplier 1.0 in the b-axis, and 7×7=49 have it in the a-axis — empirical exact count to be confirmed during implementation).

**Practical impact:** roughly half of the 490 combos previously varied `trail_frac` and now vary `mfe_lock_frac` instead. The other half (combos that didn't pair slot 4 with anything) are unchanged from before, except that their fixed `TRAIL_FRAC_T` is now compile-time constant rather than mult-resolved (which has zero runtime effect).

---

## 3. Verification plan (post-implementation)

After committing the change and rebuilding the Mac binary:

1. **G2 self-test PASS.** Run the harness; if G2 FAULTs, the change introduced a race. The 6th template parameter is a non-type template parameter (compile-time `double`), so it cannot introduce thread-shared mutable state by definition. A FAULT here would indicate a bug elsewhere in the change.

2. **Lint clean.** `python3 scripts/check_no_globals_in_hotpath.py` returns exit 0. The change is class-internal (template parameter + use site), no new namespace globals.

3. **Default-equivalence spot check.** Pick 1-2 HBG combos where `mult_for_param(I, 4) == 1.0` from the G1CLEAN baseline (`incidents/.../D6E1_G1CLEAN_run/sweep_hbg.csv`) and confirm the new sweep produces byte-identical (or RR-equivalent post-CSV-rounding) values for those combos.

4. **New `MFE_LOCK_FRAC` axis has signal.** The new axis at multipliers (0.5, 0.63, 0.79, 1.0, 1.26, 1.59, 2.0) → `mfe_lock_frac` values (0.10, 0.13, 0.16, 0.20, 0.25, 0.32, 0.40) should produce *different* PnL across at least 4 of those 7 values for combos that pair slot 4 with another. If all 7 produce byte-identical PnL, the lever is dead and we have a deeper engine-logic question to investigate. (Strong prior: this won't happen, because the binding constraint analysis predicts the lever is alive.)

---

## 4. What I am NOT proposing in this commit

- **Bundling D6.1, D6.2, E1.1, write_csv fail-loud.** Per handoff #4 these are *separate*, post-HBG-FIX-1 work.
- **Touching live engine.** The 0.20 in live remains hardcoded. Whether to expose it on the live side is a separate, future discussion.
- **Modifying `TRAIL_FRAC_T` semantics.** The parameter still exists, still drives `range_trail`, still has its hardcoded default. We just stop sweeping it.
- **Re-running the full sweep.** The expected sequence is: edit + push + verify + rebuild Mac + re-run G2 smoke + re-run full sweep (1 × ~21min run). The re-run is part of follow-up after this proposal is approved and shipped.

---

## 5. Risk and rollback

**Risk profile:**
- **Live impact:** None. Live binary is unchanged.
- **Backtest correctness risk:** Low. The change is mechanically a parameter-extraction refactor with a default that mirrors the previous hardcoded value.
- **Compile risk:** Low. New template parameter at end of parameter list with a default; existing users of `HBG_T<>` and `omega::sweep::HBGDefault` continue to compile because the new parameter has a default.
- **Determinism risk:** Zero. Non-type template parameters are compile-time constants; cannot introduce thread-shared state.
- **Performance risk:** None. One float multiplication replaced by another float multiplication; both inlined at compile time.

**Rollback plan:** Revert the commit. The harness sweep grid returns to the prior shape, the CSV column names revert, and any cached G1CLEAN baseline data remains valid because pre-fix HBG_T binary semantics were the same as post-fix-with-default semantics.

---

## 6. Authorisation request

Jo — I'm requesting explicit go/no-go on this proposal before I edit any engine code. Specifically:

> **HBG-FIX-1 — "Expose hardcoded `mfe * 0.20` as sweep parameter `mfe_lock_frac`, demote `trail_frac` from sweep to fixed":** authorise / hold / reject?

If you authorise: I will follow the pre-delivery checklist (read every changed file via API, write the changes, push, verify SHA + files via API, byte-equivalence md5(disk) == md5(api)), then signal you to rebuild on Mac and re-run G2 smoke + full sweep.

If you hold: I'll wait for clarification on a specific objection.

If you reject: HBG-FIX-1 is filed as "considered, declined" in this incidents folder and we move on.
