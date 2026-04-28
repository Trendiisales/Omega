# S51 1A.1.b — DETERMINISM_GUARDS — Design

**Status:** DESIGN DOC. No code changed. Each section ends with explicit per-piece authorisation prompts.

**HEAD at write:** `0227281534cf7b4c46b487530df7bb7aea03df0a`
**Author context:** Written immediately after D7_RESULTS.md, before D7-FIX-2 has run. The mechanism in section 1 below was found by reading `backtest/OmegaTimeShim.hpp` and `backtest/OmegaSweepHarness.cpp` line-by-line after the diag analysis. **It supersedes the threading hypothesis in D7_RESULTS.md** which was correct in spirit but imprecise about the mechanism.

---

## 1. Root cause — what the bug actually is

### The shared mutable global

`backtest/OmegaTimeShim.hpp:73`:

```cpp
inline int64_t g_sim_now_ms = 0;
```

A single, non-atomic, non-thread-local `int64_t` in the `omega::bt` namespace, force-included into every TU of `OmegaSweepHarness.cpp` via CMake `-include` / `/FI`.

### Four threads racing on this single global

`OmegaSweepHarness.cpp` lines 854, 942, 1049, 1172 — each engine's run loop has the same pattern:

```cpp
for (int64_t k = 0; k < N; ++k) {
    const TickRow& r = ticks[k];
    omega::bt::set_sim_time(r.ts_ms);    // <- writes the shared global
    // ... process tick across all 490 combos in this engine's tuple ...
}
```

`set_sim_time` is just `g_sim_now_ms = epoch_ms`. Plain assignment. No atomic, no fence.

**Four threads (HBG, EMA, AsianRange, VWAPStretch) each run this loop concurrently over the same 154M-tick stream.** Each thread iterates over its own copy of the tick vector and writes its own `r.ts_ms` to the shared global before processing.

### Why it manifests as different `h`/`yday` to different engines on "the same" tick

Inside the AsianRange thread:

1. Thread sets `g_sim_now_ms = ticks[k].ts_ms`. Call this T_k.
2. Thread iterates over its 490-engine tuple via fold expression.
3. Engine instance `i=0` calls `process(snap)`, which calls `sweep_utc_hms()`, which reads `g_sim_now_ms`. **It might read T_k. It might also read T_j for some j ≠ k from the HBG/EMA/VWAP thread's most recent write.**
4. Engine instance `i=24` does the same. Reads `g_sim_now_ms`. Different scheduling moment → different value.
5. Engine instance `i=73`, `i=122`, etc. — each reads `g_sim_now_ms` at its own moment in the iteration. The OS scheduler interleaves the four threads' writes between the reads.

Within a single tick `k` in one thread, the **engine tuple iteration is fast** — but it's not zero. With 490 engines per tuple and four threads ratelimited only by memory bandwidth, the read-window for each engine instance overlaps with thousands of writes from the other three threads at the AsianRange-tick rate.

The values read aren't random — they're always *some* tick from *some* thread's recent write. Sometimes they're correct (the AsianRange thread's own most recent write). Sometimes they're stale (from the HBG thread's tick stream, which is at a different point in time because HBG is processing ticks at a slightly different rate due to its different per-tick cost).

### Why pre-time-gate counters survive

`n_called`, `n_rej_invalid`, `n_rej_spread`, `n_rej_session` are computed from the **snapshot `s`** which is passed by const reference into `process()`. The snapshot's `ts_ms` lives in the `TickRow` of the AsianRange thread's loop variable. **It does not pass through the shared global.** All 490 engines in the AsianRange tuple see the same `s` for the same `k`, so these counters are byte-identical.

The first counter that diverges is the one that reads `g_sim_now_ms` indirectly via `sweep_utc_hms()` — which is exactly what we observe in `asian_diag.tsv`.

### Why HBG's centre combos look identical

HBG's hot path uses `g_sim_now_ms` only in `manage()` for trail-arm hold-time checks (`held_s >= MIN_TRAIL_ARM_SECS`). Hold time is robust to small jitter — a 50ms read from a different thread doesn't change whether `held_s >= 30` for a position that's been open for 5 minutes. So HBG's results survive the race even though the race is happening to its threads too.

AsianRange's hot path uses `g_sim_now_ms` for **hour-of-day gating** (`h >= 7 && h < 11`). A 50ms read from a different thread can't change `h`. But a **5-hour** read from a different thread (entirely possible if the threads are at different progress points in 154M ticks) absolutely can — and that's what the diag shows: `n_in_build_window` differs by 4 million ticks across centre combos. That's not microsecond jitter. That's the AsianRange thread reading time values from threads that are several hours of simulated time ahead or behind.

### Verifiable prediction this implies

Run the harness with HBG and AsianRange on the same input but **only one engine per run** (single-threaded). Then:

- AsianRange centre-combo counters should be byte-identical.
- HBG centre-combo counters should be byte-identical (probably already are; we'd just confirm).
- VWAPStretch results may shift: thousands of trades, so the hour-of-day component of any of its gates will become deterministic. WR may go from 0.5% to a different number.

This is what **D7-FIX-2** tests for AsianRange. The design below assumes the prediction holds — but every guard works regardless of the precise mechanism, so even if D7-FIX-2 surprises us, this spec stands.

---

## 2. Four guards, ranked by leverage

The principle: don't rely on careful authors. Make the harness **fail loudly** when its determinism assumptions break. Each guard is independently shippable and independently authorisable.

```
Tier  Guard                                Cost     Catches
────  ──────────────────────────────────  ───────  ─────────────────────────
G1    thread_local g_sim_now_ms            ~1 hr    Today's bug; future races
G2    Determinism self-test in the         ~2 hr    Any new utility race;
      harness (twice-run, byte-equal)               regressions on shim/harness
G3    Per-engine input-stream checksum     ~1 hr    Input drift between engines
      reported in sweep_summary                     (unrelated determinism bugs)
G4    Concurrency model documentation +    ~1 hr    Future authors writing new
      static-analysis lint rule                     globals into the time path
```

Total spec: ~5 hours of focused work. Each piece can be its own commit; none requires touching live engine code; none requires a VPS rebuild.

---

## 3. G1 — Make `g_sim_now_ms` per-thread

### What changes

`backtest/OmegaTimeShim.hpp:73` becomes:

```cpp
// Per-thread simulated epoch time. Each sweep thread maintains its own
// simulated clock to prevent cross-thread reads when threads progress
// through the tick stream at different rates.
//
// Live (single-threaded) usage is unaffected: there's only one thread,
// so thread_local is equivalent to global.
//
// MUST remain inline (header-included into multiple TUs).
inline thread_local int64_t g_sim_now_ms = 0;
```

`set_sim_time` requires no change — it still does `g_sim_now_ms = epoch_ms`, but now writes the calling thread's local copy. `sweep_now_sec`, `sweep_now_ms`, `sweep_utc_hms` all read the calling thread's local copy.

### Why this is safe

- **Live VPS:** The live `Omega.exe` does not include `OmegaTimeShim.hpp` (`OMEGA_BT_SHIM_ACTIVE` is undefined). All time access in live code goes via `std::time(nullptr)` and `system_clock::now()`. No production path touches this global. **Zero live impact.**
- **Single-threaded backtest:** `OmegaBacktest.cpp` (the single-threaded backtest tool) runs one thread that writes and reads the global. `thread_local` is equivalent to `global` for one thread. **No behavioural change.**
- **Multi-threaded sweep:** The whole point. Each sweep thread now reads only its own writes.

### What this fixes

- AsianRange centre-combo determinism returns immediately.
- VWAPStretch hour-of-day gates become deterministic.
- All four engines see only their own thread's tick stream's time. The race is gone.

### What it doesn't fix

- Any *other* shared mutable global in the harness/engines (G2 catches these).
- Engine drift between runs of the binary (G3 catches this).
- Future authors adding new globals to the time path (G4 catches this).

### Failure modes

**False sense of security.** `thread_local` fixes *this* global. If there are other shared globals on the time path (there aren't currently, but future engines might add them), they remain a hazard. **G2 is the answer to this** — a determinism self-test catches new races regardless of mechanism.

### Risk

**Low.** One-line change. Thread-local storage is well-supported on macOS / MSVC / Linux. No ABI change, no calling-convention change, no header layout change. The `inline` keyword combines correctly with `thread_local` in C++17.

### Per-piece authorisation

> **G1 — "Make g_sim_now_ms per-thread":** authorise / hold for D7-FIX-2 / reject?

---

## 4. G2 — Determinism self-test in the harness

### What changes

Add a new `--selftest-determinism` flag to `OmegaSweepHarness`. When passed (or always, in a default-on mode), the harness runs a **5,000-tick smoke test** at startup before the real sweep:

1. Read first 5,000 ticks from the input file.
2. Run the smoke test sweep with the **full 4-engine, 490-combo configuration**.
3. Capture the centre-combo counter rows for each engine (i.e., the 6 IEEE-identical-param combos, just like the diag dump).
4. Run the same smoke test **again**.
5. Compare counter rows between run 1 and run 2.
6. If any counter differs across the two runs, **fail loud** with exit code 2 and a clear message:

```
DETERMINISM FAULT: AsianRange combo 24 produced different counter row in run 1 vs run 2.
  run 1: n_called=5000  n_in_build_window=1234  ...
  run 2: n_called=5000  n_in_build_window=1567  ...
This is a harness bug. Real sweep results cannot be trusted. Aborting.
```

### Why two runs and not just one?

A single run could pass the centre-combo determinism check (all 6 instances see the same time) and still be wrong if those 6 instances happen to be co-scheduled. Running **twice** with a fresh thread spawn between the two runs guarantees that the OS scheduler interleaves differently. If results are byte-identical across two re-spawns, threading is genuinely insulated.

### Why 5,000 ticks?

- Long enough to exercise the engine state machines (build window → fire window for AsianRange, multiple positions for HBG).
- Short enough to add <1s to harness startup.
- 5,000 matches the existing `--warmup` default, so the same tick subset can be reused.

### Cost vs benefit

- **Cost:** ~1 second per harness invocation. Negligible compared to 18-min sweep.
- **Benefit:** Every future code change to engine, shim, or harness gets tested for determinism, automatically, every time. No human discipline required.

### What this catches

- **The bug we just found**, even if G1 weren't shipped (the self-test would fail and stop the run).
- **Future races.** Any author who adds a new global without `thread_local` will see the self-test fail on first run.
- **Compiler regressions.** A compiler bug or optimisation that introduces non-determinism (rare but real) is caught.
- **Library regressions.** If we update libc and `gmtime_r` becomes non-thread-safe in a new way, caught.

### What it doesn't catch

- Slow-burn drift over millions of ticks that cancels itself in 5,000-tick samples (very unlikely but theoretically possible). G3 catches this.

### Failure modes

**False positives** — if the self-test occasionally fails on a healthy harness due to (e.g.) thread-startup ordering. Mitigation: run the test until it passes 3 times in a row, or fail. The cost is still <5 seconds.

**Long-term flakiness erodes trust** — if the self-test fails once a month and people learn to ignore it. Mitigation: clear, loud, exit-code-2 failure with a diagnostic dump. Make the failure painful enough that ignoring it isn't viable.

### Risk

**Low.** New code path, completely isolated from sweep logic. Worst case: the self-test itself has a bug and falsely fails — at which point we disable the flag and investigate. The real sweep is not blocked.

### Per-piece authorisation

> **G2 — "Determinism self-test in the harness":** authorise / hold for D7-FIX-2 / reject?

---

## 5. G3 — Per-engine input-stream checksum

### What changes

Add a per-engine 64-bit FNV-1a hash that updates on every tick the engine processes:

```cpp
struct EngineRunStats {
    uint64_t input_checksum = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
    uint64_t n_ticks_seen   = 0;
};
```

Update on each tick:

```cpp
stats.input_checksum ^= static_cast<uint64_t>(snapshot.ts_ms);
stats.input_checksum *= 0x100000001b3ULL;
stats.n_ticks_seen++;
```

At sweep end, the harness prints a summary:

```
Engine       n_ticks_seen   input_checksum
HBG          154,265,439    0xa3f9...e21c
EMACross     154,265,439    0xa3f9...e21c
AsianRange   154,265,439    0xa3f9...e21c
VWAPStretch  154,265,439    0xa3f9...e21c
[OK] All engines saw identical input streams.
```

If any of the four hashes differ, fail loud:

```
[FAULT] HBG saw a different input stream than AsianRange.
        HBG          input_checksum=0xa3f9...e21c  n_ticks=154,265,439
        AsianRange   input_checksum=0x7c2b...d5a4  n_ticks=154,265,439
        Real sweep results cannot be compared across engines.
```

### What this catches

- **The original mystery.** Today's bug is *not* an input drift — engines see identical inputs but different *time*. G3 wouldn't catch today's bug. But it *would* catch a future bug where different threads read from different positions in the tick file, or where a tick is dropped on one thread but not another.
- **Tick file corruption mid-run.** If the OS swaps out and brings back a different version of the file, caught.
- **Off-by-one bugs in tick iteration.** If one engine accidentally skips a tick, caught.

### What it doesn't catch

- Today's bug. Inputs were identical. Time-of-decision was different. G1 + G2 together cover today's bug; G3 covers a different bug class.

### Cost

- **One XOR + one multiply per engine per tick.** ~3.6B operations across the full sweep. Negligible (tens of ms total).
- **One additional column in `sweep_summary.txt`.** Negligible.

### Risk

**Negligible.** Pure observation, no decision logic affected. Could not perturb sweep behaviour even if the hash function had a bug.

### Per-piece authorisation

> **G3 — "Per-engine input-stream checksum":** authorise / hold / reject?

---

## 6. G4 — Concurrency model doc + lint rule

### What changes

#### 6a. Document the concurrency model

Add `backtest/CONCURRENCY.md`:

```
# Omega Sweep Harness Concurrency Model

OmegaSweepHarness runs 4 sweep threads concurrently:
  - HBG, EMACross, AsianRange, VWAPStretch

Each thread iterates the entire tick stream over its own engine tuple.

PER-THREAD STATE (safe):
  - The std::tuple<EngineT<I>...> instance is per-thread (stack-allocated
    per launch lambda).
  - Engine member variables are per-instance and per-thread.
  - The TickRow& reference passed into each engine's process() is from
    the calling thread's local loop variable.

THREAD-LOCAL STATE (safe via thread_local):
  - omega::bt::g_sim_now_ms (after G1)

SHARED STATE (HAZARD — must be reviewed before any new addition):
  - <none currently — see lint rule below>

DO NOT ADD any of the following to the engine hot path without thread-
safety review:
  - Non-atomic global variables read inside an engine's process().
  - Mutable static locals in functions called from an engine's process().
  - Singletons or registries.
  - Any function that reads the wall clock without the time shim.

If a new utility function is needed in the engine hot path, it MUST be:
  - Pure (no state), OR
  - thread_local (preferred), OR
  - std::atomic with explicit memory order documented, OR
  - Reviewed and explicitly added to this document.
```

#### 6b. Add a lint rule

A small Python script `scripts/check_no_globals_in_hotpath.py`:

```python
# Scans SweepableEngines.hpp and OmegaTimeShim.hpp for new non-atomic,
# non-thread_local globals declared at namespace scope. Fails CI if any
# are added without a corresponding entry in CONCURRENCY.md's allow-list.
```

Run as a pre-commit hook or part of the bulletproof launch script — whatever the team has. (Today: just run manually before committing changes to those two files.)

### Why this matters

The current bug existed because someone added `inline int64_t g_sim_now_ms = 0;` and nobody flagged it. The lint rule makes that flag automatic. Without it, the next bug of this shape will take another 6 months to find.

### Cost

**~1 hour** to write the doc and a 50-line Python lint script. Negligible ongoing cost.

### Per-piece authorisation

> **G4 — "Concurrency model doc + lint rule":** authorise / hold / reject?

---

## 7. Recommended sequencing

Once D7-FIX-2 confirms the threading hypothesis (which I strongly expect):

1. **Ship G1 first.** Single-line fix. Restores determinism today. One commit.
2. **Ship G2 second.** Catches anything G1 missed and prevents regression. One commit.
3. **Re-run D6+E1 deterministically.** All HBG numbers from the prior session need to be re-verified before any further parameter decisions. One run, ~18 min.
4. **Ship G3** after the deterministic D6+E1 baseline is locked in. One commit.
5. **Ship G4** as a separate commit. Doc + lint, no code logic.

All five steps are gated behind `OMEGA_BUILD_SWEEP=ON` and have **zero live impact**. The VPS does not need to be touched at any point.

---

## 8. What this does NOT cover (and why)

- **The X3 pairwise design's centre-combo redundancy.** It's wasteful (10 instances of the same params) but it was the **only thing** that made today's bug visible. **Keep the redundancy until G1+G2+G3 are all live.** After that, the redundancy can be removed for efficiency. Until then, it's our canary.
- **Engine code refactor to CRTP + SoA.** Mentioned in the prior session handoff as a future S52 priority. Out of scope here.
- **Live-engine determinism.** The live `Omega.exe` is single-threaded and uses wall-clock time. Different concerns; not this doc.

---

## 9. Authorisation prompt

To proceed cleanly, please answer per-piece:

1. **G1** (thread_local on g_sim_now_ms): authorise now, authorise after D7-FIX-2 confirms, or reject?
2. **G2** (determinism self-test in harness): authorise now, authorise after G1, or reject?
3. **G3** (per-engine input checksum): authorise now or after G1+G2?
4. **G4** (concurrency doc + lint): authorise now (independent of D7-FIX-2)?

Default position if no per-piece answer: hold all four until D7-FIX-2 results are in, then reconvene.
