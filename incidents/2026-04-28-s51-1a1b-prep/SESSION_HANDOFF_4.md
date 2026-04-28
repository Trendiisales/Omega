# Session Handoff #4 — 2026-04-29 — G2 + G4 shipped, HBG-FIX-1 queued

**HEAD at handoff:** `f01024a9eaa7548b0afb7058cd3da8f0815dc50f`
**Date:** 2026-04-29 NZST mid-session
**Predecessors:**
- `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF.md`   (#1, end of D5)
- `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_2.md` (#2, end of D6+E1)
- `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_3.md` (#3, post-G1, post-G1CLEAN baseline)
- This file (#4, post-G2, post-G4)

**Reason for handoff:** Context budget approaching 70%. G2 and G4 both shipped clean and verified. HBG-FIX-1 is the next item in the priority order from #3, but it's the first item that modifies engine code (`include/SweepableEngines.hpp`), so it's higher-stakes and benefits from a fresh-context session for the read + propose + edit + push cycle.

---

## TL;DR

Three commits this session, all pre-delivery-verified. The G1 fix from prior session is now belt-and-braces protected: G2 catches non-determinism at runtime; G4 catches it via lint at edit time.

| Commit | Subject | Verification |
|---|---|---|
| `b11b0853` | G2: determinism self-test (initial) | Pushed; **failed Mac compile** — forward-ref to `Args` |
| `6af0afbe` | G2-FIX-1: move `Args/parse_args/engine_in_list` above G2 to fix forward-ref | Pushed + Mac compile clean |
| `f01024a9` | G4: `CONCURRENCY.md` + `scripts/check_no_globals_in_hotpath.py` | Pushed + Mac lint clean |

The G2 smoke test on Mac was never confirmed run-to-completion — Jo's Mac built it (after the path correction `./build/Release/...` → `./build/...`) but the actual smoke run output was not captured. **First action next session:** confirm G2 PASS on the Mac before relying on the harness.

---

## What was shipped

### G2 — determinism self-test in `OmegaSweepHarness`

**Commits:** `b11b0853` (initial), `6af0afbe` (forward-ref fix)
**File:** `backtest/OmegaSweepHarness.cpp` (1492 → 1746 lines, +254)
**Spec:** `incidents/2026-04-28-s51-1a1b-prep/DETERMINISM_GUARDS.md` §4

Added on-startup self-test that runs each requested engine's sweep TWICE on the first 20,000 ticks (~1.85h simulated time on XAUUSD), byte-compares `ComboResult` fingerprints across the two runs, and exits 2 with a diagnostic on any divergence.

**Spec deviations from DETERMINISM_GUARDS.md §4 (all approved by Jo this session):**
- Comparison surface is the always-present `ComboResult` (n_trades/wins/total_pnl/q_pnl[]/q_trades[]/stddev_q/score), not the `OMEGA_SWEEP_DIAG`-gated `dump_diag()` counters. Lets G2 work in production builds, not only DIAG builds.
- Compares **all 490 combos × 4 engines**, not just the 6 byte-identical centre combos.
- **Default-on**, with `--no-selftest` flag to disable.
- Smoke honours `--engine` filter.
- Tick count: **20,000** (not the spec's 5,000) — chosen to cross several hour-of-day gate boundaries on XAUUSD's ~180 ticks/min, exercising the AsianRange race that the original bug exposed.
- Diagnostic includes actual run1/run2 mismatched values, not just "field X diverged".

**Self-correction this session:** initial commit `b11b0853` defined `run_selftest_determinism(const Args&)` at line 1441 but `struct Args` was at line 1554. Mac compile failed with `unknown type name 'Args'`. Fixed in `6af0afbe` by moving the entire `Args + usage + parse_args + engine_in_list` block as one unit to immediately before the G2 banner. Pure rearrangement; same byte content for all moved functions.

**Lesson written into pre-delivery discipline:** before defining a function that takes `const T&`, grep for `struct T`/`class T` in the same TU and confirm the definition is at a lower line number than the use site.

### G4 — `CONCURRENCY.md` + lint script

**Commit:** `f01024a9`
**Files (both new):**
- `backtest/CONCURRENCY.md` (10,240 bytes, 256 lines, mode 100644)
- `scripts/check_no_globals_in_hotpath.py` (17,085 bytes, 483 lines, mode 100755)
**Spec:** `incidents/2026-04-28-s51-1a1b-prep/DETERMINISM_GUARDS.md` §6

`CONCURRENCY.md` documents the post-G1 contract: 5-bucket state classification (per-instance / per-thread loop-local / thread-local namespace / atomic shared / read-only shared), forbidden patterns, decision tree for adding new state, lint usage, references.

The lint scans `backtest/OmegaTimeShim.hpp` and `include/SweepableEngines.hpp` for namespace-scope mutable globals not in the allow-list. `thread_local`/`constexpr`/`const`/`std::atomic<>` are auto-safe. Allow-list mirrors `CONCURRENCY.md` §2c (the three `g_sim_*` thread-locals in OmegaTimeShim.hpp).

**Pre-push verification:** clean dry-run on real codebase (exit 0); 3 deliberately-injected violations all caught (exit 1); each violation reported with file:line + actionable fix message.

**Self-correction during dev:** first version of `classify_brace` mis-classified the `namespace omega {` brace in `SweepableEngines.hpp` because the brace was preceded by `#include "..."` lines and the brace-classifier picked up stripped-string `"` quotes as the leading token. Fixed by changing the heuristic from "first token of decl" to "rightmost keyword in decl". Verified against both files before push.

**Push mechanism:** two new files in one commit via the git data API (blob → tree → commit → ref update), since the contents API allows only one file per PUT call.

**Mac confirmation (this session):**
```
scanning backtest/OmegaTimeShim.hpp ...
  clean (backtest/OmegaTimeShim.hpp)
scanning include/SweepableEngines.hpp ...
  clean (include/SweepableEngines.hpp)
OK: 2 file(s) clean.
```

---

## State at handoff

### Live trading
- **VPS HEAD:** `ed95e27c`. Untouched all session. Service running, mutex held. **No urgency to deploy** any newer commit. None of G2/G4 affects the live binary.

### Repo
- **`origin/main`:** `f01024a9eaa7548b0afb7058cd3da8f0815dc50f`
- **Mac local:** confirmed at this commit after `git pull` (Jo ran the lint successfully against this HEAD).

### Mac binary
- **Path:** `~/omega_repo/build/OmegaSweepHarness` (NOT `build/Release/` — default CMake generator on Mac is Unix Makefiles, single-config; `Release/` only exists with multi-config generators like Xcode or MSVC).
- **Built:** 2026-04-29 ~09:59 NZST after the `6af0afbe` fix landed. 3,318,528 bytes.
- **Contains:** G1 (thread_local g_sim_now_ms) + G2 (self-test on startup with 20k-tick smoke).
- **G2 smoke status:** binary built and runnable, but the 20k-tick smoke run was not confirmed end-to-end this session. **Open thread.**

### Memory rule deltas applied this session
- **Rule 21 replaced** (was: stale Omega HEAD `dc43ddf9` description) with: Mac binary path correction. PAT preserved.
- **Rule 30 (pre-delivery for all deliverables)** continues to apply. This session it caught the parent-SHA mistake (assumed `678addbb`, was `6c3e6820`) and the forward-ref compile error (only after Mac surfaced it; should have caught earlier — see "Pre-delivery additions" below).

### Pre-delivery additions earned this session
1. **Type-visibility check.** Before defining a function that takes `const T&` or returns `T`, grep for `struct T` / `class T` in the same TU and confirm definition line < use line. (Born from G2 forward-ref bug.)
2. **Multi-pattern parser test.** When writing parsing code that targets multiple anchor patterns (e.g., `namespace omega { namespace sweep {` vs `namespace omega::bt {`), test against representative samples of *each* pattern before pushing. (Born from G4 lint `classify_brace` bug.)
3. **Parent-SHA freshness.** When resuming a session, the parent SHA in a memo (e.g., `678addbb` in `SESSION_HANDOFF_3.md`) is the SHA at *the time the memo was written*; subsequent commits in the same session may have advanced HEAD past it (e.g., `6c3e6820` for the SESSION_HANDOFF_3 commit itself). Always `GET /git/refs/heads/main` to find current HEAD before pushing.

---

## Open threads

### Thread A — G2 smoke confirmation on Mac
The G2 self-test has been built into the Mac binary at `~/omega_repo/build/OmegaSweepHarness` but the actual run-to-completion of the smoke was not captured in chat. The expected output is:

```
G2 self-test: running each requested engine TWICE on 20000 ticks (warmup=5000) to verify determinism
G2 self-test: PASS in ~1-2s (engines=HBG EMA ASN VWP)
```

If `PASS` appears, G1 + G2 are jointly validated end-to-end. If `DETERMINISM FAULT:` appears with a divergence diagnostic, we have a second non-determinism source beyond G1 and need to investigate before HBG-FIX-1's re-sweep can be trusted.

**Run command (~1-2s for the smoke; full sweep is ~21min — Ctrl-C is fine right after the PASS/FAULT line):**
```
cd ~/omega_repo
./build/OmegaSweepHarness ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv \
  --outdir sweep_g2_smoke --verbose
```

### Thread B — HBG-FIX-1 (next-session priority 1)

**Goal:** expose the hardcoded `mfe * 0.20` trail-lock-fraction at `include/SweepableEngines.hpp:954` (per memory rule 21 / SESSION_HANDOFF_3.md authorisation menu) as a sweep parameter `mfe_lock_frac`, so the trail aggressiveness lever is inside the sweep grid rather than fixed.

**Why this matters:** D6+E1 results showed `trail_frac` produces byte-identical PnL across 7 different values for HBG combos 308–314 (per HBG-DIAG dump). The current sweep grid varies `trail_frac` but the trail-lock distance is `min(trail_frac * mfe, mfe * 0.20)` — i.e., clamped at 20% of MFE for almost the entire grid. The 20% constant is the real lever, currently outside the grid.

**Risk classification:** This is the **first commit this session that modifies engine code**, not just harness or infrastructure. Jo's `userPreferences` rule: "Never modify core code unless instructed clearly." Ship this only after written go/no-go on a proposal memo.

**Recommended sequence next session:**
1. **First action:** confirm G2 smoke result on Mac (Thread A).
2. Read the relevant span of `include/SweepableEngines.hpp` carefully — HBG_T class declaration, `manage()` function, trail-arming and trail-update path. The `mfe * 0.20` site is at L954 per the prior session's audit.
3. Write `incidents/2026-04-28-s51-1a1b-prep/HBG_FIX_1_PROPOSAL.md` containing:
   - The exact line(s) to change
   - The new template parameter (`MFE_LOCK_FRAC` as `double`)
   - Default value matching current behaviour (0.20) so unchanged combos are byte-identical
   - The corresponding sweep grid change in `OmegaSweepHarness.cpp` (probably: drop one of the existing 5 HBG params and replace with `mfe_lock_frac`, or extend to 6 params and update C(N,2) accordingly — to be discussed)
   - What the existing `trail_frac` becomes (likely fixed at base value; demoted from sweep grid)
4. Get explicit go/no-go from Jo.
5. On go: edit, verify, push.
6. Re-build on Mac. Re-run G2 smoke (catches any race introduced by the change). Re-run full sweep.

**Defer to a session after HBG-FIX-1:**
- D6.1 (HBG `max_range` rebase 32→16, geometric grid 8..32)
- D6.2 (HBG `min_range` rebase 3→6, geometric grid 3..12)
- E1.1 (extend E1 filter to flag `rsi_lo ≥ rsi_hi` as degenerate)
- write_csv fail-loud (propagate `fopen` failure to non-zero exit)

These are best bundled as one combined commit *after* HBG-FIX-1 lands and the sweep grid restructure is settled.

### Thread C — ASN-AUDIT (separate dedicated session)

Reading exercise comparing `AsianRangeT` (harness) vs `AsianRangeEngine` (live) line-by-line to determine whether the 6pp WR gap (live 49.7%, harness best 43.5% under G1CLEAN) is a port-drift issue or a genuine "no edge on this grid" pathology. Required before declaring AsianRange dead. Don't bundle with HBG-FIX-1; keep the audit pure.

### Thread D — G3 (deferred)

Per-engine input-stream FNV-1a checksum. Lower-priority complement to G2; catches a different bug class (input drift vs decision-time non-determinism). Authorisation pending. Not in the critical path; can come after HBG-FIX-1 + D6.1/D6.2/E1.1 + ASN-AUDIT.

---

## Authorisation menu for next session

### Already authorised, ready to ship
| ID | Description | Risk | Cost |
|---|---|---|---|
| **HBG-FIX-1** | Expose `mfe*0.20` (`SweepableEngines.hpp:954`) as sweep param `mfe_lock_frac`; demote `trail_frac` to fixed | Medium (engine code) | ~1 hr (proposal + edit + push) |
| **D6.1** | HBG `max_range` rebase 32→16 (geometric grid 8..32) | None | Bundled with HBG-FIX-1 follow-up |
| **D6.2** | HBG `min_range` rebase 3→6 (geometric grid 3..12) | None | Bundled |
| **E1.1** | Extend E1 filter to flag `rsi_lo ≥ rsi_hi` as degenerate | Low | Bundled |
| **write_csv fail-loud** | Propagate fopen failure to non-zero exit code | None | Bundled |
| **E2** | EMACross hardcoded RSI dead-band review at SweepableEngines.hpp:991-998 | Low | ~30 min |
| **D8** | VWAPStretch structural fix (SL/TP/sigma redesign) | None | ~1 hr + sweep |

### Newly recommended (need authorisation)
| ID | Description | Risk | Why |
|---|---|---|---|
| **G3** | Per-engine FNV-1a input checksum reported at sweep end | Negligible | Different bug class than G1+G2; complementary |
| **ASN-AUDIT** | Read AsianRangeT vs live AsianRangeEngine line-by-line; produce diff memo | None (audit only) | Required before declaring AsianRange "no edge" |

### Rejected (do not pursue)
- **D6.3** (drop trail_frac from sweep) — counter-evidence in G1CLEAN data. trail_frac=0.125 differs in stddev_q from 0.158-0.500. Param is clamped, not dead. HBG-FIX-1 supersedes.

---

## Recommended priority order for next session

1. **Confirm G2 smoke on Mac** (~30s of effort: run command, paste output). Resolves Thread A. If FAULT, divert to investigation; if PASS, proceed.
2. **HBG-FIX-1 proposal phase** (~30 min): read SweepableEngines.hpp HBG_T section, write `HBG_FIX_1_PROPOSAL.md`, commit it.
3. **HBG-FIX-1 implementation** (~1 hr): on Jo's go-ahead, edit + push + verify.
4. **Mac rebuild + G2 smoke** (catches any new race) + **full sweep re-run** (~21 min).
5. **D6.1 + D6.2 + E1.1 + write_csv fail-loud combined commit** (~30 min).
6. **Re-run sweep with new grid** (~21 min); lock new baseline as "S51 1A.1.b D9" or similar.

Items 1–6 fit comfortably in one fresh-context session. ASN-AUDIT, G3, E2, D8 belong to subsequent sessions.

---

## What CANNOT be assumed at session start

- **Memory rule 21 was rewritten this session** to capture the Mac binary path (`./build/`, not `./build/Release/`). Earlier hints in SESSION_HANDOFF_3.md and elsewhere are wrong for Mac.
- **HEAD is `f01024a9`**, not `678addbb` (SESSION_HANDOFF_3 quote) or `6c3e6820` (SESSION_HANDOFF_3 commit).
- **G2 smoke PASS is not yet confirmed end-to-end on Mac** (Thread A). Treat the harness as deterministic *until proven otherwise this run*. The lint passing clean does not substitute for the runtime check.
- **HBG-FIX-1 is engine-code-modifying.** It is the first such change since the start of this G1+G2+G4 sequence. The pre-delivery rule "Never modify core code unless instructed clearly" applies — the proposal memo is mandatory before the edit.

---

## Pre-delivery rule reminder (carried forward)

Per memory rules 2 and 30, plus this session's additions:

1. State assumptions before writing logic against them.
2. Read referenced files via the GitHub contents API before writing against them — never assume.
3. Check whether the existing system already solves the problem before proposing new code.
4. Flag any number/claim whose provenance was not verified this session.
5. After every push: confirm new SHA via API, confirm files in commit via API, byte-equivalence md5(disk) == md5(api).
6. **(NEW this session)** Type-visibility check: grep for `struct T` / `class T` definition before writing code that uses `T` by reference/value, and confirm definition is at a lower line number than the use.
7. **(NEW this session)** Multi-pattern parser test: when writing text-parsing code targeting multiple syntactic patterns, test against samples of each before pushing.
8. **(NEW this session)** Parent-SHA freshness: always `GET /git/refs/heads/main` immediately before push to find the *actual* current HEAD; never trust a SHA quoted from a memo as the live parent.
9. For zsh-on-Mac: write multi-line scripts to `/tmp/<name>.sh` via `<< 'OMEGAEOF' ... OMEGAEOF` heredoc, then execute. Avoid pasting multi-line code blocks at the shell prompt.

---

## Next-session opener (paste verbatim)

```
/ultrathink

Continuing Omega S51 1A.1.b. HEAD pinned at
f01024a9eaa7548b0afb7058cd3da8f0815dc50f on main.

G1 + G2 + G4 all shipped. Read these in order before touching code:
  1. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF.md   (#1)
  2. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_2.md (#2)
  3. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_3.md (#3 - G1CLEAN baseline)
  4. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_4.md (#4 - this memo, read first)
  5. incidents/2026-04-28-s51-1a1b-prep/DETERMINISM_GUARDS.md (G2/G4 specs)
  6. backtest/CONCURRENCY.md  (post-G4 concurrency contract)

Top of mind:
  - HBG numbers are hard-verified clean (G1CLEAN baseline locked).
  - G2 self-test in OmegaSweepHarness; default-on, 20k-tick smoke.
  - G4 lint at scripts/check_no_globals_in_hotpath.py; clean on this HEAD.
  - G2 smoke PASS not yet confirmed end-to-end on Mac. Open thread.

Recommended priority:
  1. Confirm G2 smoke PASS on Mac (run ./build/OmegaSweepHarness ...).
  2. HBG-FIX-1 proposal phase: read HBG_T section of SweepableEngines.hpp,
     write HBG_FIX_1_PROPOSAL.md, get Jo's go/no-go.
  3. On go: implement HBG-FIX-1, push, verify, rebuild Mac, re-run G2 smoke,
     re-run full sweep.
  4. D6.1 + D6.2 + E1.1 + write_csv fail-loud combined commit.
  5. Re-run sweep, lock new baseline.

VPS still on ed95e27c. No urgency.

Authorised, queued: HBG-FIX-1, D6.1, D6.2, E1.1, write_csv fail-loud, E2, D8.
Newly recommended (need auth): G3, ASN-AUDIT.

First action: confirm G2 smoke PASS on Mac before any code change.
```

---

## One thing I owe future-Jo a memo for

The G2 forward-ref bug (`Args` defined below its use) is a class of mistake that would have been caught instantly by a 5-second pre-push compile on Mac. We don't have that loop in this session topology — Jo's Mac is the only build environment, and waiting on a compile per push round-trips ~2 minutes through the chat interface.

Two ways to mitigate going forward:

1. **Pre-push static checks I can run myself.** A minimal C++ syntax check via `clang -fsyntax-only -fno-color-diagnostics` on a stub that includes the modified header against mock dependencies. Not free to set up, but worth it for engine-code changes (HBG-FIX-1 onward).

2. **Tighter type-visibility discipline.** Already added above as pre-delivery rule #6. Cheaper. Catches this specific bug-class with no infrastructure.

For now, going with #2. If HBG-FIX-1 surfaces another shape of compile error that #2 wouldn't have caught, escalate to #1.
