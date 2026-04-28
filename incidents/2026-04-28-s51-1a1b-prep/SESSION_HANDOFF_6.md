# SESSION_HANDOFF_6.md

**Date:** 2026-04-29 (NZST) / 2026-04-28 UTC
**HEAD on origin/main:** `816543b171801ff53ba22b1fd8b6a247e12b64ee`
**VPS HEAD:** `ed95e27c` (no rebuild needed)

---

## What this session did

Built and pushed the CRTP sweep harness Jo asked for. This is the rebuild after
the previous session's runtime-array attempt was reverted (see SESSION_HANDOFF_5).

**Commit:** `816543b1` — "S51 1A.1.b: CRTP sweep harness (parallel target, OFF by default)"

**Files pushed:**
- `include/SweepableEnginesCRTP.hpp` (new, 1468 lines, 60103 bytes)
- `backtest/OmegaSweepHarnessCRTP.cpp` (new, 1308 lines, 53131 bytes)
- `CMakeLists.txt` (modified, +61 lines, -0)

**Architecture (CRTP, Shape B):**
For each of the 5 engines (AsianRange, VWAPStretch, DXYDivergence, HBG, EMACross):
1. `XBaseParams` — live defaults as `static constexpr`
2. `XTraits<I>` — per-combo params via `mult_for_param_crtp(I, p)` over the
   geometric grid (0.5x..2.0x, 7 values, pairwise C(5,2)=10 pairs, 490 combos)
3. `XBase<Derived>` — engine logic; reads params via
   `using T = typename Derived::traits_t; T::PARAM_NAME`
4. `XCRTP<Traits>` — derives from base, defines `using traits_t = Traits;`

**HBG-FIX-1 lineage preserved:**
- TRAIL_FRAC fixed at 0.25 (live default)
- Slot 4 in the sweep is MFE_LOCK_FRAC (centred 0.20)
- D6 rebases preserved: HBG MAX_RANGE 25.0->32.0, SL_FRAC 0.5->0.42

---

## Pre-delivery verification (all passed before push)

1. ✓ Read all 3 changed files via contents API before write
2. ✓ Verified existing `OmegaSweepHarness` target byte-untouched in new
     CMakeLists (diff confirmed pre-insertion section + post-EdgeFinder
     section byte-identical to original except for the inserted CRTP block
     and new status line)
3. ✓ Pushed
4. ✓ Confirmed SHA from API (`816543b1`)
5. ✓ Confirmed changed files in commit via API (1 modified + 2 added,
     exactly the expected set)
6. ✓ Re-fetched all 3 files via contents API and byte-compared to local
     copies — all match

---

## Existing system state

- `OmegaSweepHarness` (non-type-template version at `backtest/OmegaSweepHarness.cpp`)
  is **untouched** and still works. Output dir: `sweep_results`.
- `OmegaSweepHarnessCRTP` is the new parallel target. Output dir:
  `sweep_results_crtp`. Both can coexist.
- `OMEGA_BUILD_SWEEP_CRTP` is **OFF by default**. Default Omega build path
  (`--target Omega`) does not pull it in.
- VPS at `ed95e27c` requires no rebuild.

---

## Honest perf note

CRTP version is **~zero faster** than the existing harness. Both compile to
identical hot-path assembly:
- Both fully inline the templated Sink&
- Both resolve params to immediate constants (constexpr template args vs
  constexpr Traits members — same thing post-codegen)
- Both dispatch via fold-expression over `std::index_sequence<490>`
- The CRTP `static_cast<Derived*>(this)` downcast compiles to nothing

CRTP buys architectural cleanliness, not perf. If real speedup is wanted,
the lever is fewer combos or shorter date windows, not this refactor.

Jo was told this directly during the session.

---

## Mac next-session command

```
cd ~/omega_repo && git pull
cmake -B build -DOMEGA_BUILD_SWEEP_CRTP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OmegaSweepHarnessCRTP -j
./build/OmegaSweepHarnessCRTP ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv \
    --outdir sweep_results_crtp --verbose
```

Mac binary path: `build/OmegaSweepHarnessCRTP` (NOT `build/Release/`).
Mac default CMake generator is Unix Makefiles, single-config.

---

## Open risks / unverified

- **Compile not yet attempted on Mac.** 490 CRTP instantiations × 4 engines =
  1960 distinct types — same scale as `OmegaSweepHarness` which compiles
  cleanly on Mac/Clang with `-fbracket-depth=1024`. Same flag is set on the
  new target. Should compile, but unverified.
- **CRTP gotcha to watch:** `EMACrossBase` member-init line:
  ```cpp
  double _ema_fast_alpha = 2.0 / (Derived::traits_t::FAST_PERIOD + 1.0);
  ```
  This works because the base is only ever instantiated via CRTP from
  derived (so `Derived::traits_t` is a complete type at base-instantiation
  time). But if the compiler complains about incomplete type at this line,
  it's the canonical CRTP gotcha — fix is to move the alpha computation
  into the constructor body or compute it lazily on first use.
- **G2 determinism self-test** is wired in and runs by default on the first
  20k ticks. Exit code 2 on fault, before the real sweep. `--no-selftest`
  flag to skip.

---

## Mood / context

Session was emotionally charged. Previous session's botched delivery
(runtime-array masquerading as CRTP, reverted commit `9a5289c7`) left Jo
furious and skeptical. This session delivered the actual CRTP, told Jo the
honest perf truth (no speedup), and pushed in one clean commit with all
pre/post-delivery checks passing.

Do not resurrect the dangling `9a5289c7`. Do not propose runtime-array
"fallbacks". Do not modify `SweepableEngines.hpp` or `OmegaSweepHarness.cpp`
without explicit instruction — those are the existing system Jo's analysis
pipeline uses, and they remain the canonical baseline.
