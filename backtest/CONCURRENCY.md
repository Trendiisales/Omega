# Omega Sweep Harness Concurrency Model

**Authority:** S51 1A.1.b D7+G1+G2 lineage. Written as part of G4
(`incidents/2026-04-28-s51-1a1b-prep/DETERMINISM_GUARDS.md`) immediately
after G2 shipped. The rules here are the post-G1 contract for any author
touching `backtest/OmegaTimeShim.hpp` or `include/SweepableEngines.hpp`.

**Scope:** This doc covers the multi-threaded sweep harness only. The live
`Omega.exe` binary is single-threaded and uses the real wall clock; nothing
in this document constrains live engine code.

**Companion lint:** `scripts/check_no_globals_in_hotpath.py`. Run it before
every commit that modifies the two files listed above. The lint enforces
the allow-list at the bottom of this document.

---

## 1. Threading model

`OmegaSweepHarness` runs **4 worker threads** concurrently, one per engine:

- HBG (`run_hbg_sweep`)
- EMACross (`run_ema_sweep`)
- AsianRange (`run_asian_sweep`)
- VWAPStretch (`run_vwap_sweep`)

Each thread:

1. Owns its own engine tuple (`std::tuple<EngineT<I>...>` of 490 instances)
   allocated via `std::make_unique` in the worker function.
2. Iterates the **same** read-only `std::vector<TickRow> ticks` shared from
   `main()`.
3. Calls `omega::bt::set_sim_time(r.ts_ms)` at the top of each per-tick
   iteration to advance its **own thread-local** simulated clock.
4. Fans the tick into all 490 tuple instances via fold expression.
5. Aggregates per-combo results into a per-thread `std::vector<ComboResult>`
   that is returned to `main()` after thread join.

`main()` joins all worker threads, then writes per-engine CSVs and the
combined summary serially. There is **no inter-thread communication during
the run** — each worker is fully independent until join.

---

## 2. State classification

Every piece of state in the sweep hot path falls into exactly one of the
five buckets below. Adding new state means picking the right bucket and
following the rule for that bucket.

### 2a. Per-instance state (safe — preferred)

Member variables of templated engine classes (`HBG_T<...>`, `EMACrossT<...>`,
`AsianRangeT<...>`, `VWAPStretchT<...>`). Each tuple has its own instance
of each member, and each thread has its own tuple, so members are
automatically per-thread.

**Examples:**
- `HBG_T::m_ticks_received`, `HBG_T::n_position_opens_`, etc.
- `AsianRangeT::asian_hi_`, `AsianRangeT::last_signal_s_`, etc.
- `EMACrossT::_ema_fast`, `EMACrossT::_atr`, etc.

**Rule:** All new mutable engine state goes here. No exceptions.

### 2b. Per-thread loop-local state (safe)

Local variables inside `run_*_sweep`, including stack-allocated and
`std::make_unique`-allocated containers. The `std::tuple<...>` is
unique-ptr-allocated to keep the 1.3 MB tuple off the thread's 512 KB
default stack on macOS, but it is still per-thread because each
`run_*_sweep` call constructs its own.

**Examples:**
- `engines_p` / `engines` (the engine tuple)
- `sinks_p` / `sinks` (the sink array)
- `results` (the per-combo `ComboResult` vector)
- `positions_p` / `positions` (managed-position array for AsianRange/VWAP)
- `sigs_p` / `sigs` (signal array for AsianRange/VWAP)
- `SnapshotBuilder sb` (builds GoldSnapshot per thread)

**Rule:** These are local; no special discipline needed. Just don't make
them `static` or move them to namespace scope.

### 2c. Thread-local namespace state (safe — explicitly listed below)

State that **must** appear at namespace scope for header-include reasons
(the time shim is `inline` and force-included into multiple TUs) but
**must** be per-thread because it's read inside the engine hot path.

**The complete current list:**

| File                           | Symbol                       | Type      |
|--------------------------------|------------------------------|-----------|
| `backtest/OmegaTimeShim.hpp`   | `omega::bt::g_sim_now_ms`    | `int64_t` |
| `backtest/OmegaTimeShim.hpp`   | `omega::bt::g_sim_start_ms`  | `int64_t` |
| `backtest/OmegaTimeShim.hpp`   | `omega::bt::g_sim_started`   | `bool`    |

**Rule:** All three MUST remain `inline thread_local`. Removing
`thread_local` from any of them re-introduces the cross-thread time-shim
race that destroyed deterministic AsianRange results in S51 1A.1.b D6+E1.
The lint script enforces this; the G2 self-test catches it at runtime.

If a future change requires adding a new thread-local namespace global,
add it to **both** the table above and the allow-list in
`scripts/check_no_globals_in_hotpath.py` in the same commit.

### 2d. Atomic shared state (safe — none currently)

`std::atomic<T>` with explicit memory order, used for inter-thread
coordination.

**The complete current list:** *(none — no atomics in the sweep hot path)*

**Rule:** If you add an atomic, document the memory order and the readers/
writers in this section. Atomics in the hot path are a perf hazard;
prefer per-instance state in 2a wherever possible.

### 2e. Read-only shared state (safe — by construction)

State allocated and initialised in `main()` before threads launch, then
never written by any thread. Read-only sharing is automatically safe.

**Examples:**
- `const std::vector<TickRow> ticks` (parsed once from CSV; passed by
  const-reference to every worker)
- `const Args& args` (parsed once from argv; passed by const-reference)
- `static constexpr` arrays in `OmegaSweepHarness.cpp` (`GRID_MULT[]`,
  `pair_indices()` lookup, etc.)
- `static const char*` in `print_*` helpers (string literals)

**Rule:** As long as no thread writes, no synchronisation is needed.
Adding a writer to anything currently in this category requires
re-classifying it (most likely as 2a or 2c).

---

## 3. Forbidden patterns

The following are **not allowed** in `backtest/OmegaTimeShim.hpp` or
`include/SweepableEngines.hpp` without an explicit entry in the allow-list
of `scripts/check_no_globals_in_hotpath.py` and a corresponding row in
section 2c above:

1. **Non-atomic, non-thread-local, non-const namespace-scope variables.**
   This is exactly the bug-class that destroyed S51 1A.1.b D6+E1. Example
   of a forbidden line:

   ```cpp
   inline int64_t g_some_clock_helper = 0;   // FORBIDDEN: not thread_local
   ```

2. **Mutable static locals in functions called from `process()` /
   `on_tick()` / `manage()`.** Static locals are global state with a
   different syntax. Forbidden example:

   ```cpp
   inline int sweep_utc_hour() noexcept {
       static int last_hour = -1;          // FORBIDDEN: hidden global
       int h = ...; if (h != last_hour) { last_hour = h; ... }
       return h;
   }
   ```

3. **Singletons or registries in the hot path.** The `Meyers singleton`
   pattern (`static T& instance() { static T t; return t; }`) is also a
   hidden global and is forbidden in the hot path.

4. **Wall-clock reads.** Any function called inside an engine's
   `process()`/`on_tick()`/`manage()` that calls `std::time(nullptr)`,
   `std::chrono::system_clock::now()`, or `std::chrono::steady_clock::now()`
   directly bypasses the time shim and breaks both backtest correctness
   and harness determinism. Use `omega::bt::sim_now_sec()` /
   `omega::bt::g_sim_now_ms` instead.

5. **Direct file I/O inside `process()`.** Diagnostic dumps go through the
   `OMEGA_SWEEP_DIAG`-gated `dump_diag()` methods called *after* the sweep
   loop completes. Writing to disk from inside the per-tick path is both a
   determinism hazard (filesystem ordering varies between runs) and a perf
   hazard.

---

## 4. How to add new state

The decision tree:

1. **Does the state describe a single engine instance's behaviour?**
   → 2a (per-instance class member). Almost always the right answer.

2. **Is the state needed across all combos of a single engine, but only
   one thread reads/writes it?**
   → 2b (per-thread loop local in `run_*_sweep`).

3. **Does the state need to be visible at namespace scope from a header
   that gets included into multiple TUs?**
   → 2c (thread-local namespace global). Add to both the table in §2c
   and the lint allow-list in the same commit.

4. **Do multiple threads genuinely need to coordinate via this state?**
   → 2d (`std::atomic<T>`). Document memory order in §2d. Strongly
   reconsider whether you really need this — the harness is designed to
   avoid inter-thread coordination during the run.

5. **Is the state computed once and never written again?**
   → 2e (read-only shared). Set it up in `main()` before threads launch.

If none of these fit, post in the audit memo (`incidents/<date>-<slug>/`)
and ask before writing any code.

---

## 5. Lint enforcement

`scripts/check_no_globals_in_hotpath.py` is the mechanical enforcement of
this document. It scans `backtest/OmegaTimeShim.hpp` and
`include/SweepableEngines.hpp` for namespace-scope mutable variable
declarations that are not in its allow-list. The allow-list mirrors §2c
above exactly.

**To run:**

```
cd ~/omega_repo
python3 scripts/check_no_globals_in_hotpath.py
```

Exit code 0 = clean. Exit code 1 = at least one disallowed global. The
script prints `file:line` for each offence with the offending declaration
quoted.

**To add a new entry to the allow-list:**

1. Update §2c above with the new symbol.
2. Update the `ALLOWED_GLOBALS` set at the top of the lint script.
3. Both must be in the same commit.

**Limitations:** The lint is a text scanner, not a full C++ parser. It
catches the common case of `inline T name = ...;` at namespace scope but
will not detect adversarial bypasses (aliases, indirection through
pointers, etc.). It is a guard against accidental regression, not a
security boundary.

---

## 6. References

- `incidents/2026-04-28-s51-1a1b-prep/DETERMINISM_GUARDS.md` — full design
  of G1..G4 and the root-cause analysis of the original bug.
- `incidents/2026-04-28-s51-1a1b-prep/D7_RESULTS.md` — the diagnostic run
  that proved the threading hypothesis.
- `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_3.md` — G1+G2
  status and the authorisation menu that put G4 in the queue.
- `backtest/OmegaTimeShim.hpp` (post-G1) — the canonical example of a
  correctly thread-local namespace global.
- `backtest/OmegaSweepHarness.cpp` (post-G2) — the runtime self-test that
  catches violations of this contract dynamically.
