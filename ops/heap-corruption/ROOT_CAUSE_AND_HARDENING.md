# Why Tsmom most likely triggered the heap corruption — and what to do about it
**2026-04-30 PM session**

This document covers two things:

1. **Why Tsmom triggered it** — ranked hypotheses for the mechanism by which adding `g_tsmom` exposed a heap-corruption bug. Static review of `TsmomEngine.hpp` itself found no smoking-gun memory error, so the most likely story is "Tsmom is structurally correct, but its addition tickles a latent bug elsewhere in the binary." This section ranks the candidate mechanisms by likelihood and tells you how to falsify each.
2. **What else to do** — short / medium / long-term hardening to stop this class of incident from recurring, organised so each item is independently doable.

## Part 1 — Why Tsmom triggered it

### Candidate mechanism A — Allocator-pressure shift exposes a latent corruption (HIGHEST LIKELIHOOD)

**Mechanism.** Adding Tsmom adds significant new heap-allocation traffic per tick:

- `wrap()` creates a `std::function<void(TradeRecord)>` capturing `[this, runtime_cb]` once per cell-tick. 5 cells × 50 ticks/sec on XAUUSD ≈ **250 small heap allocs/sec just for callbacks**. The closure size (`8 + sizeof(std::function)` ≈ 40-56 bytes) exceeds `std::function`'s SBO threshold so each one heap-allocates.
- `std::vector<Position>` push/erase, `std::deque<double>` chunk allocs, `OmegaTradeLedger::m_trades` vector grows on every close (Tsmom alone projects ~7,120 closes/yr ≈ ~30/day; each goes into the ledger).
- `warmup_from_csv` reads ~6,156 H1 bars at startup → ~6,156 strings parsed, multiple containers built.

**Why this triggers a pre-existing bug.** A latent off-by-one buffer overrun, use-after-free, or unsynchronised concurrent heap access can sit dormant for months when the allocator's free-list state happens to put nothing valuable behind the corrupted region. Adding ~250 alloc/sec to the steady-state load **changes which freed regions get reused, on what schedule, by what type of object**. The bug doesn't change — what changes is what gets corrupted, and how soon something tries to use it.

**Evidence supporting this hypothesis.**
- The fault offsets vary across crashes (`Omega.exe+0x56add`, `0x59317`, `0x6c250`, `0xb025e`, `0xb08f3`, `0xaddf3`) — that's the segment heap detecting corruption at *whatever happens to be the next allocation/free attempt*, not at the line that did the corrupting write. Classic "downstream detection of upstream poison."
- Static review of Tsmom's own code found no obvious memory error — multi-position `erase`-while-iterating is correct, `Position` is POD, callbacks are synchronous, `closes_` deque is bounded.
- Tsmom is single-threaded (only the quote thread reaches it), so no internal race.

**How to falsify.** Run Variant A (Tsmom-only disabled). If crashes stop, this hypothesis becomes operative — and the **next** step is finding *which other component* is the actual buggy one. That requires AppVerif + `+ust` (the script bundle) on a Tsmom-active build to capture an allocation backtrace at the moment of corruption.

### Candidate mechanism B — Latent SIOF / static-construction interaction (MEDIUM LIKELIHOOD)

**Mechanism.** Adding `g_tsmom`, `g_donchian`, `g_ema_pullback`, `g_trend_rider` to `globals.hpp` (or to `omega_types.hpp`) shifts the static-initialisation order of every global declared after them. C++ static init order *within a single TU* is top-to-bottom in declaration order, but the constructors themselves can reach *across* the TU's other globals. If any pre-existing global's constructor was relying on another global being constructed first — and the new globals shifted that ordering — you'd see correct-looking code start mis-initialising, with the symptoms surfacing later as heap corruption when a pointer or container header has the wrong contents.

**Evidence supporting.**
- Multiple new globals were added in audit-fixes-22 onwards.
- The handoff explicitly flagged "Static initialisation order fiasco after main.cpp split" as Suspect #5.
- All affected globals live in `omega_types.hpp` or `globals.hpp` (same TU as `main.cpp`).

**Evidence against.**
- All file-scope statics in `omega_types.hpp` are in the same TU; init order is deterministic top-to-bottom.
- The new Tsmom/Donchian/EpbPullback/TrendRider declarations appear *after* the older globals (additive at the bottom of the header), so older statics still construct in the same relative order.

**How to falsify.** Build Omega.exe with the four new engines fully constructed but never *called* (e.g., comment out the `g_tsmom.init()` call but leave the global declared). If crashes still happen, init-order-of-construction is the culprit. If crashes stop, it's the *runtime activity* of the engine that triggers, not its mere existence — pointing back to mechanism A.

### Candidate mechanism C — std::function/lambda chain exhausts a custom allocator or arena (LOWER LIKELIHOOD)

**Mechanism.** If Omega has a custom allocator (overloaded `operator new`/`operator delete`, or a thread-local arena) somewhere, the new Tsmom allocation rate could be hitting a sized-pool limit or fragmenting a fixed-size arena. Some arenas silently return wrong-sized blocks under exhaustion.

**Evidence against.** Quick search across `include/` found no `operator new` overload, no custom `allocator<>` template specialisation, no obvious arena. The build uses standard MSVC `new`/`delete` paths into the segment heap.

**How to falsify.** Run capture_status.ps1 right after Omega.exe starts and again after 30 minutes of normal load. If `WorkingSet` is climbing without bound but `Threads` and `Handles` stay flat, an allocator is leaking proportional to engine activity — that's a strong signal of mechanism C. If memory is stable at e.g. ~50 MB, mechanism C is unlikely.

### Candidate mechanism D — TradeRecord ledger or telemetry race (MEDIUM-LOW LIKELIHOOD)

**Mechanism.** `OmegaTradeLedger::m_trades` is a `std::vector<TradeRecord>` with a `std::mutex` guarding every public method. The locking discipline is correct. BUT: `OmegaTelemetryServer` runs on its own thread and reads `g_telemetry` (a different object) which is updated by the quote thread. If telemetry has an unsynchronised path that reads the trade ledger or some derived counter during a vector grow, that's a corruption window.

Tsmom adds many more closes per unit time (multi-position, 5 cells, sub-day exits) so the ledger grows faster and reallocs are more frequent.

**Evidence against.** Audit didn't surface unsynchronised access in OmegaTradeLedger itself. The mutex is acquired everywhere I looked.

**How to falsify.** AppVerif's `Locks` check (which is in our scripts) catches mismatched/missing critical-section acquisitions. If an AppVerif `Locks` stop fires, this is the cause.

### Candidate mechanism E — Compiler-/optimiser-induced UB activation (LOW LIKELIHOOD)

**Mechanism.** Adding new code adds new translation units (well, new code in `main.cpp`'s TU). MSVC's optimiser may hoist, vectorise, or coalesce loads/stores differently. Pre-existing undefined behaviour (uninitialised reads, signed overflow, strict-aliasing violations) that previously compiled to "harmless" instructions could compile to actually-corrupting instructions under the new code-gen.

**Evidence against.** Build flags use `/W4 /WX` (warnings as errors). UB warnings would have failed the build. Older builds (before audit-fixes-22) didn't crash.

**How to falsify.** Build the same source with `/Od` (no optimisation). If crashes stop, it's optimiser-activated UB. If they continue, it's a real heap bug.

### Bottom line on causation

The combination of evidence points strongly to **mechanism A** — Tsmom's code is correct, but its addition shifts the heap allocator into a state where a pre-existing bug elsewhere starts triggering. The diagnostic path that confirms this and pins the *actual* buggy component is:

1. Variant A disable confirms Tsmom presence is the trigger.
2. Re-enable Tsmom on a non-prod replica with AppVerif + `+ust` (the script bundle).
3. Wait for next crash, analyse dump.
4. The allocation backtrace from `+ust` names the data structure being corrupted.
5. Code review of that component's allocator interactions yields the bug.

Total clock time from "starting the diagnostic" to "patch ready": probably 1-2 days.

## Part 2 — What else to do (hardening menu)

Items grouped by horizon. Each is independently doable.

### Short-term (this week)

**S1. Mandatory non-prod build host with ≥8 GB RAM.** Today's session was hampered because `cl.exe` OOMs on the 3 GB VPS. This is a structural problem, not an MSVC quirk. Fix: stand up any spare host (your laptop, a small AWS/Azure VM, a Mac with parallels) with the matching MSVC toolchain. Document it as the canonical build host. Future deploys: build there, copy `Omega.exe` to the VPS. Never build on the VPS again.

**S2. Pre-deploy AppVerif soak.** Before flipping any new engine to LIVE, run it for 24 hours under AppVerif (the scripts in this bundle) on the non-prod replica. If Locks/Memory/Handles stops fire, the engine has a defect that would surface in production within hours.

**S3. Watchdog memory-pressure guard.** The current watchdog restarts Omega.exe blindly on crash. Add a pre-flight check: if `Get-Counter '\Memory\Available MBytes'` < 500 MB, log a warning and *delay* restart by 60s instead of restarting immediately into the same OOM. This buys Windows a window to reclaim non-pageable.

**S4. Crash-dump capture ON by default.** The WER LocalDumps configuration in `enable_appverif_ust.ps1` should be the baseline state for production, not a diagnostic-only setting. Set DumpType=2 (full), DumpCount=10, folder C:\CrashDumps. ~50-200 MB per dump; budget 2-3 GB of disk for retention.

**S5. Shadow-mode SLA.** Any new engine stays in `shadow_mode = true` for at least 7 trading days regardless of backtest verdict. Tsmom went from "shadow ship" to "load-bearing in the binary" in the same session. The shadow window is your free runway to detect issues — use it.

### Medium-term (next 1-3 sessions)

**M1. Migrate to AddressSanitizer-instrumented CI build.** MSVC's `/fsanitize=address` is the highest-leverage hardening you can do. Run the same source through ASan once a week (or before each release): the resulting binary detects heap corruption at the corrupting instruction with zero false positives. Caveat: ~2-3× memory overhead; can't be deployed to production but is the gold standard for pre-prod soak.

**M2. Reduce `wrap()` allocation churn in TsmomEngine and the other multi-position engines.** Each `wrap()` heap-allocates a `std::function`. Two ways to fix:

- Replace `std::function<void(TradeRecord)>` with a `template <typename F>` parameter — the lambda goes on the stack, no heap alloc.
- Or, store `runtime_cb` once on the portfolio and refer to it via an immutable pointer — no per-tick wrap needed.

This reduces per-tick alloc rate by ~250/sec and shrinks the surface area for mechanism A.

**M3. `static` → `inline` in `omega_types.hpp` / `globals.hpp`.** The handoff already flagged this. C++17 `inline` variables give external linkage with single-instance-across-TUs semantics, which is what you actually want. Today's `static` (internal linkage) only works because `omega_types.hpp` is only included by `main.cpp` — verified in audit but fragile. Do this *after* the heap bug is fixed; bundle as a single dedicated commit.

**M4. Allocation telemetry.** Add a per-engine bookkeeping counter that logs `allocs_since_init` and `current_resident_bytes` every N minutes. Pure diagnostic; helps confirm or refute mechanism C and gives forward visibility on engine memory behaviour.

**M5. Engine kill-switches as config flags, not source rebuilds.** Today, disabling Tsmom requires editing `engine_init.hpp` and rebuilding. Hot-reload from `omega_config.ini` already exists for the SP/NQ/CL/etc. engines. Wire the four new portfolio engines (`g_tsmom`, `g_donchian`, `g_ema_pullback`, `g_trend_rider`) into the same `OmegaHotReload` callback so `[tsmom] enabled=false` in the config file actually disables it without a rebuild. This makes future Variant-A-style experiments trivial — flip a flag, restart service, watch.

**M6. Eliminate raw pointers in CTraderDepthClient.** `std::mutex* l2_mtx = nullptr;` and `std::unordered_map<std::string,L2Book>* l2_books = nullptr;` (lines 701-702) are raw pointers to globals owned by main. They work, but they're a permanent landmine: any code path that passes the depth client around without those pointers being valid will UAF. Replace with references-to-globals or wrap the depth client to take a setter that asserts non-null.

### Long-term (architectural, next 5+ sessions)

**L1. Single-engine-per-TU.** Today every engine header is included in `main.cpp`'s TU. The result: one giant TU, slow rebuilds, brittle ordering, every engine sharing the same anonymous-namespace state with every other. A modest refactor moves each engine's *implementation* (not just declarations) into its own .cpp. CMake compiles them in parallel. PCH amortises STL/Win32 includes across TUs. Each engine becomes individually unit-testable.

**L2. RAII / unique_ptr / shared_ptr where currently raw.** Beyond the depth client (M6), there are smaller raw-pointer pockets across the codebase. None of them are necessarily bugs, but each is a place where future code changes could introduce one. Mechanical sweep with `clang-tidy` modernise pass.

**L3. Integration test harness with recorded feeds.** Capture 1 hour of live FIX + cTrader feed once. Build a harness that replays it deterministically against an under-test Omega.exe binary. Run that harness in CI on every PR. ASan + harness = pre-merge bug discovery.

**L4. Crash-recovery state log.** A small append-only file that Omega.exe flushes on every position open/close. On startup, replay the log to reconstruct what was open. This makes watchdog-restart-during-active-position a non-event for trade reconciliation.

**L5. Dedicated engine threads with explicit message passing.** Today's "single thread does everything" model means a 1ms hiccup in Tsmom delays L2 ingestion by 1ms. A queue-per-engine model with workers that pull from a shared tick stream isolates engine work and naturally bounds the heap-pressure-per-engine signal we're chasing. Expensive to retrofit but eliminates the entire class of "engine X added → engine Y's allocator behaviour shifts" scenarios.

## Decision matrix

| If you have... | Do this first |
|---|---|
| 1 hour | Run `enable_appverif_ust.ps1` on a non-prod replica + Variant A disable |
| 1 day | Above + S1 (build host) + S4 (dump capture as default) |
| 1 week | Above + S2 + S3 + S5, plus M5 (config-driven engine disable) |
| 1 sprint | Everything above + M1 (ASan CI) + M2 (wrap() rewrite) + M3 (inline globals) |
| 1 quarter | Everything above + L1 (single-engine-per-TU) + L3 (replay harness) |

The one item that pays for itself within 24 hours regardless of horizon is **S1 (non-prod build host with adequate RAM)**. Without it, every other item — including this morning's two clean fixes — is bottlenecked by build OOMs.

## Files in this update

- `ROOT_CAUSE_AND_HARDENING.md` — this document
- `scripts/enable_appverif_ust.ps1` — turn on AppVerif + gflags +ust + WER dumps
- `scripts/disable_appverif_ust.ps1` — clean rollback
- `scripts/capture_status.ps1` — read-only health check (run any time)
- `scripts/analyse_dump.windbg` — WinDbg command script for dump analysis
- `scripts/analyse_dump.ps1` — wrapper that runs cdb against the latest dump

Plus the existing engine_init.hpp variants for the disable experiment.
