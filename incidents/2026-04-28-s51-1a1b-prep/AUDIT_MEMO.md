# S51 Stage 1A.1.b — Sweep Prep Audit & Decision Log

**Status:** D4 shipped this commit. D1/D2/D3 parked. D5 next.
**HEAD pre-commit:** `2d29241e131037b8f3988187ba49f1d1ce6c6350`
**Date:** 2026-04-28

---

## Why this file exists in the repo

Session-scoped scratchpads (`/home/claude/...` on Claude side, individual chat
threads on Jo side) do **not** survive between sessions. The Stage 1A.1.b
audit was performed in a prior session and the working notes were lost when
that session ended; only the user-pasted summary survived.

This file is the canonical record of:

1. The five-finding audit performed against HEAD `2d29241e1`.
2. The decision log resolving each open point (D1–D5).
3. Authority trail for each commit in the 1A.1.b chain.

Every future session should read this file first when picking up S51 1A.1.b
work, **before** assuming any state from memory rules or chat history.

---

## Audit findings (verified byte-exact at HEAD `2d29241e1`)

### Finding 1 — Memory rule #30 disagrees with source

Memory rule #30 currently says:
> "S51 sweep harness spec (locked): 5 params/engine, geometric grid 0.5x–2.0x
> of default, 7 values/param → 16,807 combos/engine."

**Source disagrees.** `backtest/OmegaSweepHarness.cpp` L2-19, L377-379:

- Pairwise 2-factor design, **X3** (replaced X2 on 2026-04-27).
- For each engine, vary 2 of 5 params on a 7×7 geometric grid; hold other 3 at default.
- C(5,2) = 10 pairs × 49 = **490 combos / engine**.
- 4 engines × 490 = **1,960 total** (DXY skipped — no DXY tick stream).

The 16,807-combo (X2) full grid was rejected because template instantiation
OOMs the compiler past ~2,000 entries. Memory should be updated to reflect X3.

**Authority for X3 design:** explicit user authorisation, 2026-04-27.

### Finding 2 — `MIN_TRAIL_ARM_PTS_STRONG` does not exist

Parked sweep plan referenced a "strong" trailing-arm tier as one of the
sweepable params. Source has only flat:

- `include/GoldHybridBracketEngine.hpp:93` → `MIN_TRAIL_ARM_PTS = 1.5`
- `include/SweepableEngines.hpp:607` → `MIN_TRAIL_ARM_PTS = 1.5` (HBG_T mirror)

Adding a `_STRONG` tier requires:
1. New constant `MIN_TRAIL_ARM_PTS_STRONG`
2. Defined activation rule (pre-BE-lock? MFE-tier? hold-tier?)
3. `manage()` change to consume tiered logic

This modifies live engine behaviour → **core code modification, requires
explicit authorisation.** Tracked as **D1**.

### Finding 3 — `DIR_SL_COOLDOWN_S` is dead code

Both live HBG and HBG_T set `m_sl_cooldown_ts` on SL_HIT but never read it:

- `include/GoldHybridBracketEngine.hpp` — declared L520, written L548, **never read**.
- `include/SweepableEngines.hpp` (HBG_T) — declared L875, written L889, **never read**.

Sweeping `DIR_SL_COOLDOWN_S` produces zero behavioural variance until a
consumer gate is wired. Cleanest spot: reject same-direction fills in
`confirm_fill` while `now_s < m_sl_cooldown_ts`.

This modifies live engine behaviour → **core code modification, requires
explicit authorisation.** Tracked as **D2**.

### Finding 4 — Grid-shape mismatch

`GRID_MULT[7]` (OmegaSweepHarness.cpp L357-359) is fixed geometric:
`{0.5, 0.63, 0.79, 1.0, 1.26, 1.59, 2.0}`.

If/when D1+D2 expand to 6 or 7 sweepable params, the parked linear value sets
({2.0, 2.5, 3.0, 4.0, 5.0} for `_STRONG`; {60, 90, 120, 150, 180} for
cooldown) **do not** map onto the geometric grid. Three paths:

- **Path A (recommended):** absorb new params into the existing geometric
  grid around chosen base values. Sweep numerics differ slightly from
  parked plan but harness stays uniform.
- **Path B:** rebuild grid as per-param custom array. More flexible, more
  complexity, breaks current uniform `GRID_MULT` lookup.
- **Path C:** keep geometric, choose base values such that 0.5×–2× covers
  the desired range (e.g. base=3.0 → range {1.5, 1.89, 2.38, 3.0, 3.78,
  4.76, 6.0} covers parked {2.0..5.0}).

Tracked as **D3** (pure harness change, no live-engine impact, deferred until
D1/D2 resolve).

### Finding 5 — HBG_T spread fidelity drift (RESOLVED THIS COMMIT)

`include/SweepableEngines.hpp:905` (pre-1A.1.b) hardcoded
`tr.spreadAtEntry = 0.0;`. Live HBG was fixed at S51 1A.1.a but HBG_T drifted.

This is a strict mirror of live HBG behaviour, no behavioural change to the
engine — only carries `spread = ask - bid` (already computed at L671) through
to TradeRecord so backtest cost accounting matches live.

Touchpoints (all in `include/SweepableEngines.hpp`):

| # | What                              | Pre-fix       | Post-fix          |
|---|-----------------------------------|---------------|-------------------|
| 1 | LivePos struct: add field         | absent        | `double spread_at_entry = 0.0;` |
| 2 | `confirm_fill` signature          | 3-arg         | 4-arg, default 0.0 |
| 3 | `confirm_fill` body: store field  | absent        | `pos.spread_at_entry = spread_at_fill;` |
| 4 | on_tick PENDING block: pass spread| 3-arg call    | 4-arg call, passes `spread` |
| 5 | _close site: read field           | hardcoded 0.0 | `pos.spread_at_entry` |

**Authority:** S51 1A.1.a was authorised live; this is its mirror in HBG_T.
The user explicitly approved D4 in this session before commit.

---

## Decision log

| ID | Item                                          | Status     | Resolution / Notes |
|----|-----------------------------------------------|------------|--------------------|
| D1 | `MIN_TRAIL_ARM_PTS_STRONG` activation rule    | PARKED     | Awaits user auth for live HBG core mod. |
| D2 | `DIR_SL_COOLDOWN_S` consumer gate location    | PARKED     | Awaits user auth for live HBG core mod. |
| D3 | Geometric vs linear grid for new params       | PARKED     | Path A recommended; defer until D1/D2.  |
| D4 | HBG_T spread fidelity port                    | **DONE**   | Shipped this commit. 5-touchpoint mirror of live HBG L142/401-402/420/223-224/567. |
| D5 | Skip 1A.1.b core mods, run baseline sweep     | **NEXT**   | Validates Mac pipeline at this HEAD with HBG_T cost accounting now matching live. |

## D5 run plan (Mac, after this commit lands)

```bash
cd ~/omega_repo
git fetch origin
git checkout main
git pull --ff-only

cd build && rm -rf * && cmake .. && cmake --build . --target OmegaSweepHarness --config Release

# Tick file (canonical, per memory rule):
TICKS=~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv

./OmegaSweepHarness "$TICKS" \
    --engine hbg,asianrange,vwapstretch,emacross \
    --outdir ~/omega_sweep_baseline_$(date +%Y%m%d) \
    --warmup 5000 \
    --verbose
```

Output: 4 CSVs (one per engine), each with 490 rows × {param1_mult,
param2_mult, ...metrics...}. Top-50 ranking by stability×PnL is a separate
post-processing step in `analysis/` (not yet written — track as D5.b).

**D5 outcome decides D1/D2 priority:**
- If baseline shows clean PnL surface across the existing 5 params per
  engine → D1/D2 may be unnecessary; ship the winner.
- If baseline shows flat/noisy surface → D1/D2 likely worth the live-code
  modifications.

---

## Carried-over parked items (not 1A.1.b-specific)

- `STAGE1A1_FIX_SPECS.md` path errors — uses `src/engines/...` throughout;
  real paths are `include/...`. Never actioned.
- Index-tick data sourcing — NAS100/USTEC/SP/DJ30 dukascopy ticks for
  24-month range; blocker for Stages 1A.1.c onwards.
- FIX execution-report path — `order_exec.hpp:445,453` still uses 3-arg
  `confirm_fill`; live trades carry `spread_at_entry=0.0` until updated.
  Not a regression; pre-existing condition acknowledged in S51 1A.1.a
  comment block (HBG L396-400).
- `GoldPos.engine[32]` — populated correctly at fill time? Unverified.
- `drift_window_` capacity in IFlow — spec says 64-tick; unverified
  against `feed_persistence` at L757.

---

## Memory rules to update (action via `memory_user_edits` next session)

The following memory rules are stale-on-disk as of this commit:

1. **Replace** the S51 sweep harness spec line:
   > S51 sweep harness uses pairwise 2-factor (X3 design): 5 params/engine,
   > C(5,2)=10 pairs × 7×7=49 = 490 combos/engine, 1,960 across 4 engines
   > (DXY skipped). 17k full-grid (X2) was rejected — template instantiation
   > OOMs past ~2k.

2. **Add** post-1A.1.b commit reference:
   > Stage 1A.1.b D4 shipped: HBG_T (SweepableEngines.hpp) got spread_at_entry
   > field + 4-arg confirm_fill default-arg + PENDING call-sites pass spread +
   > _close reads field. Mirrors live HBG S51 1A.1.a. No engine behavioural
   > change; only fixes backtest cost accounting drift.

3. **Add** session-persistence rule:
   > Always persist Stage audit + decision logs to `incidents/<date>-<slug>/`
   > in the repo before session end. `/home/claude/...` does not survive
   > session resets and uploaded chat documents do not feed the next session
   > automatically.


---

## Post-incident notes — VPS rebuild OOM (2026-04-28)

### What happened

After D4 commit `ea3e7943` landed on `main`, the VPS rebuild via
`QUICK_RESTART.ps1` failed at compile step 9 of 9
(`OmegaSweepHarness.cpp`) with:

```
C:\Omega\backtest\OmegaSweepHarness.cpp(587,58): error C1060:
compiler is out of heap space
[C:\Omega\build\OmegaSweepHarness.vcxproj]
```

Build duration before failure: 511s. Service recovered correctly via the
`[RECOVERY]` branch — `Start-Service Omega` was invoked, previous binary
(`ed95e27`) resumed live trading. Service stdout log confirmed:
`[OK] Git hash: ed95e27 -- verify against GitHub HEAD`. Mutex guard
`[OMEGA] ALREADY RUNNING` blocked manual `omega.exe --version` invocation,
which itself was indirect proof that the service-controlled instance held
the lock and was alive.

### Root cause

X3 sweep-harness design (locked in commit `dc43ddf9`, predates D4) instantiates
roughly 1,960 templates: `std::tuple<HBG_T<0..489>>` and equivalent for
AsianRange, VWAPStretch, EMACross. Clang on Mac handles this with
`-fbracket-depth=1024`. **MSVC's per-process compiler heap OOMs** on the same
input — failure mode is slow because MSVC backtracks template instantiation
O(depth × width) before erroring out.

The harness was previously X2 and lighter. Tonight's rebuild was the first
attempted MSVC build of the X3 harness on the VPS; the `dc43ddf9` chain was
only ever validated on Mac.

D4 commit `ea3e7943` was **not** the cause. D4 added one field and one
default-arg to HBG_T — template instantiation count is unchanged. D4 only
triggered the rebuild that exposed the latent regression.

### Fix (Option C — belt-and-braces, this commit)

Two independent guards:

1. **CMakeLists.txt:** `add_executable(OmegaSweepHarness ...)` is now wrapped
   in `option(OMEGA_BUILD_SWEEP ... OFF)` / `if(OMEGA_BUILD_SWEEP) ... endif()`.
   The harness is **off by default** and is only built when explicitly
   requested via `cmake .. -DOMEGA_BUILD_SWEEP=ON`. The Mac D5 baseline
   sweep run plan in this memo is updated accordingly.

2. **All three VPS restart scripts** (`QUICK_RESTART.ps1`,
   `REBUILD_AND_START.ps1`, `DEPLOY_OMEGA.ps1`) now invoke
   `cmake --build ... --target Omega ...`. This matches the pattern already
   used in `RESTART_OMEGA.ps1` L313 (which was correct before this commit).
   Even if `OMEGA_BUILD_SWEEP=ON` is ever passed by accident on the VPS,
   the explicit `--target Omega` skips the harness target.

Two guards. Both have to fail for the OOM to recur.

### D5 run plan — UPDATED for OMEGA_BUILD_SWEEP gate

```bash
cd ~/omega_repo
git fetch origin
git checkout main
git pull --ff-only

cd build && rm -rf *
cmake .. -DOMEGA_BUILD_SWEEP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build . --target OmegaSweepHarness --config Release

TICKS=~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv

./OmegaSweepHarness "$TICKS" \
    --engine hbg,asianrange,vwapstretch,emacross \
    --outdir ~/omega_sweep_baseline_$(date +%Y%m%d) \
    --warmup 5000 \
    --verbose
```

Note the new `-DOMEGA_BUILD_SWEEP=ON` flag at configure time. Without it,
even on Mac the harness target won't exist and the `--target` invocation
will fail.

### Lessons (update memory rules)

- New CMake targets that have a known-broken environment (compiler heap,
  toolchain version, OS) must default OFF and require explicit opt-in.
  Never add a heavy template-heavy target as an unconditional
  `add_executable`.
- VPS rebuild scripts must always invoke `cmake --build --target Omega` —
  never bare `cmake --build` — to ensure the production binary is the only
  thing built. `RESTART_OMEGA.ps1` had this pattern; the other three did
  not. Pattern is now consistent across all four.
- First MSVC build of any new template-heavy target should be tested on a
  scratch VPS / non-production Windows host before the unconditional
  `add_executable` lands on `main`, OR the target should ship gated OFF
  from day one.
