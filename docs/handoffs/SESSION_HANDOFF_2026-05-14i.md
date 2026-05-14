# Session Handoff — 2026-05-14 (NZST), part T

Read this first next session. Direct follow-up to part-S
(`SESSION_HANDOFF_2026-05-14h.md`). This session built the UTF5m
dedicated harness, ran the Phase 1 sweep against the S63 trio, and
closed Phase 1 with a decisive result: S63 is empirically adverse on
`UstecTrendFollow5mEngine`, mirroring the VWR USTEC.F pattern.

> **Naming.** Same convention as parts L → S: filename letter is
> per-session. Part-T session = filename letter `i` (part-S used `h`).

## TL;DR

1. **S72 P0 landed.** Dedicated harness
   `backtest/UstecTrendFollow5mBacktest.cpp` + CMakeLists target wired
   (commit `51487fa`). Mirrors `VWAPReversionBacktest.cpp` pattern —
   instantiates the production `omega::UstecTrendFollow5mEngine`
   directly, drives ticks + 5m bars from CSV, exposes the S63 trio
   (`--loss-cut` / `--be-arm` / `--be-buffer`) at the CLI. S34-B guards
   and cell sl/tp pinned at engine class defaults per the phased
   decision; promotion to non-static members deferred unless needed.

2. **S72 P1 landed.** Phase 1 univariate sweep + closure memo
   committed. 19 cells over the full 4.4 GB NSXUSD tape, 344s
   wall-clock (much faster than the pre-run 3-5 hr estimate). Self-
   consistency check passed. **Decisive result: baseline gross=+928.88
   vs tuned gross=-979.16.** S63 protection is winner-amputating on
   this engine — same instrument-specific pattern as the VWR USTEC.F
   reverts (parts K/L).

3. **Phase 2 SKIPPED.** Signal decisive enough that no axis refinement
   would add information. No axis cell crossed positive territory; the
   best single-axis-off cell (be-arm=0.00) was still -150 vs baseline
   +929.

4. **Phase 3 walk-forward is the gating step for the
   `g_ustec_tf_5m.enabled = true` flip.** WF on the baseline
   configuration (S63 zeroed). Per the VWR S71 lesson, full-tape +929
   could still be regime concentration. Required: 3+ of 4 windows
   positive AND aggregate PF ≥ 1.20.

5. **If WF passes:** add per-instance S63 reverts to `engine_init.hpp`
   (LOSS_CUT_PCT/BE_ARM_PCT/BE_BUFFER_PCT = 0.0) mirroring the part-K/L
   VWR precedent, then flip `enabled = true` at line 950. Keep
   `shadow_mode = true` for 6 months. If WF fails: closure memo,
   engine stays disabled.

## What did NOT land this session

- Phase 2 fine sweep — intentionally skipped per the decisive Phase 1
  result.
- Phase 3 walk-forward — needs its own session (`scripts/vrev_wf_t1.py`
  adaptation + 4-window split + decision memo).
- S34-B-as-members engine promotion — deferred per part-T phased
  decision option (b), and likely permanently unnecessary now that
  baseline is the right configuration.
- The `engine_init.hpp` S63 reverts for `g_ustec_tf_5m` — gated on
  Phase 3 WF passing, not done speculatively.
- Other part-S carryover items: XauTrendFollow trio sweep, wrapper
  engine audit, GoldEngineStack chokepoint audit — all still pending.

## Commits this session

| Commit | Message | Files |
|---|---|---|
| `51487fa` | S72 P0: UstecTrendFollow5mBacktest harness for the S63+S37 promotion-gate sweep | `backtest/UstecTrendFollow5mBacktest.cpp` (new), `CMakeLists.txt` (one add_executable block) |
| `<S72_P1_HASH>` | S72 P1: UTF5m Phase 1 sweep -- S63 empirically adverse, mirrors VWR pattern | `scripts/utf5m_sweep_p1.sh` (new), `outputs/UTF5M_PHASE1_RESULTS_2026-05-14.md` (new, force-add) |
| `<HANDOFF_HASH>` | docs: part-T handoff | `docs/handoffs/SESSION_HANDOFF_2026-05-14i.md` (this file) |

`origin/main` should end at the handoff commit. Exact hashes Mac-side.

## Files modified this session — final state

```
A backtest/UstecTrendFollow5mBacktest.cpp                  (S72 P0 committed)
M CMakeLists.txt                                            (S72 P0 committed)
A scripts/utf5m_sweep_p1.sh                                 (S72 P1 committed)
A outputs/UTF5M_PHASE1_RESULTS_2026-05-14.md                (S72 P1 committed, force-add)
A docs/handoffs/SESSION_HANDOFF_2026-05-14i.md              (this commit)
```

Engine code untouched. Core code untouched. No engine_init.hpp changes
(the S63 reverts are gated on Phase 3 WF).

## Phase 1 closure — quick reference

```
ref baseline (S63 OFF, S37 ON)   1403 trades   gross=+928.879  avg=+0.662   ✓ NET POSITIVE
ref tuned    (S63 ON,  S37 ON)   5533 trades   gross=-979.165  avg=-0.177   ✗ NET NEGATIVE
```

Best in each axis (other two S63 fields at engine defaults):

```
loss-cut=0.00    4656 trades   gross=-688.62   (BE_ARM/BUFFER still on)
be-arm=0.00      3325 trades   gross=-150.33   (LOSS_CUT/BUFFER still on -- best non-baseline cell)
be-buffer=0.00   4745 trades   gross=-837.12   (LOSS_CUT/ARM still on)
```

**No S63-active cell beats baseline.** The closest (be-arm=0.00,
LOSS_CUT and BE_BUFFER still on at engine defaults) loses ~$1,079 of
edge vs baseline.

Sweep artifacts (gitignored; only the memo is committed):
- `outputs/utf5m_p1_20260514_142448/phase1_summary.csv`
- `outputs/utf5m_p1_20260514_142448/cells/*_report.csv`
- `outputs/utf5m_p1_20260514_142448/cells/*_trades.csv`
- `outputs/utf5m_p1_20260514_142448/cells/*_stderr.log`

## Recommended next-session focus

In priority order:

1. **Adapt `scripts/vrev_wf_t1.py` for UTF5m + run Phase 3 WF on
   baseline.** Estimated 1 session — the WF driver is mostly engine-
   agnostic; main work is flag substitution (drop `--symbol` since
   the harness is USTEC-only, swap VWR-specific flags for the S63
   trio or omit them since `--mode baseline` zeroes the S63 fields).
   Same 4-window WF split as VWR S71 P3. Same decision criteria:
   ≥3 of 4 windows positive AND aggregate PF ≥ 1.20.
   Per part-S, design for sequential window-stream-then-delete to
   avoid the disk-pressure incident.

2. **If WF passes → engine_init.hpp S63 reverts + enabled flip.**
   Three lines for the reverts (LOSS_CUT_PCT, BE_ARM_PCT,
   BE_BUFFER_PCT all = 0.0 for `g_ustec_tf_5m`) mirroring the part-K/L
   VWR precedent at `engine_init.hpp:597-672`. One line for the
   `enabled = true` flip at `engine_init.hpp:950`. Keep `shadow_mode
   = true`. Same commit per the part-K/L precedent — the gate text
   evidence + WF results justify the per-instance overrides as a
   documented intentional disable of S63 on this instrument.

3. **If WF fails → closure memo + leave disabled.** Document which
   dimension failed (regime concentration / trade-count drop in
   low-vol windows / other). Mirror the VWR S71 Phase 3 closure shape.

4. **Other part-S carryover (lower priority than UTF5m closure):**
   - XauTrendFollow trio S63 sweep (similar structure to UTF5m; same
     instrument-specific question — does S63 help or hurt XAUUSD?).
     If the UTF5m result generalises, XauTrendFollow may also have
     S63 set adversely, but this needs evidence not analogy.
   - Wrapper engine S63 design pass + audit.
   - GoldEngineStack chokepoint audit (standing, before any
     GoldEngineStack edit).

## Important lessons / don't-repeat

1. **The "S63 is instrument-specific" lesson from part-K is now
   twice-confirmed.** VWR USTEC.F (parts K/L revert) and UTF5m (this
   session) both trade USTEC, both have tight-tail / winner-amputation
   risk, and both show S63 as empirically adverse. The part-K
   framework's state classification (A/B/C/D/E with B = "documented
   intentional disable") was the right model. Any future S63 work on
   any engine should:
   - Treat the question per-instrument, not per-strategy.
   - Run baseline-vs-tuned-vs-axis-sweep on real tape before flipping
     `enabled = true`.
   - Document the result in engine_init.hpp comments so future
     sessions don't try to "fix" the documented disable.

2. **Estimates for harness runtime were wildly off.** Pre-run
   prediction: 3-5 hours for 19 cells. Actual: 344s (5.7 min). The
   harness-instantiates-real-engine pattern is materially faster than
   the standalone-re-implementation pattern (which the old UTF5m
   harnesses used) AND faster than the VWR harness on similar tape.
   Calibration data point for future sweep planning: at ~18s/cell,
   a 100-cell sweep is feasible in ~30 min. This unlocks more
   thorough Phase 1 / Phase 2 grids than previously assumed.

3. **The "winner-amputation + churn" mechanism is identifiable in
   the data.** Tuned 4x'd the trade count vs baseline (5533 vs 1403)
   because S63 cuts trades early and the per-cell `cooldown_bars=1`
   immediately frees the cell for re-fire. Each S63-cut trade is
   replaced by a worse-EV trade. This is a structural property of
   any engine with (a) high cell-fire frequency, (b) short
   cooldowns, and (c) edge that depends on letting winners run.
   `UstecTrendFollow5mEngine` has all three; so do the four
   VWAPReversionEngine instances. This is now a reliable predictor
   of S63 adversity, not just a post-hoc observation.

4. **Sandbox disk-full was a session-long blocker.** Could not run
   sandbox-side `git status` / `git diff` / `g++ -fsyntax-only` at
   any point during this session. The Mac canary build remained the
   sole verification. This is recoverable — the operator's Mac was
   the authoritative environment anyway per CLAUDE.md — but it cost
   me one self-caught warmup-gate bug that would have been caught by
   even a syntax check (eng.enabled toggling silenced bar history
   accumulation). For future sandbox-blocked sessions: do a more
   careful code review pass before committing, particularly on
   anything that touches engine-state-toggle logic.

## Standing audit at session end

**Core code preserved.** None of `include/OmegaCostGuard.hpp`,
`include/OmegaTradeLedger.hpp`, `include/SymbolConfig.hpp`,
`include/OmegaFIX.hpp`, `src/api/OmegaApiServer.hpp`, or
`include/GoldPositionManager.hpp` were modified this session.

**Engine code untouched.** No changes to
`include/UstecTrendFollow5mEngine.hpp` or any other `*Engine.hpp`
file. The S34-B-as-members promotion was deferred per the phased
decision; the Phase 1 result has now made that promotion likely
unnecessary.

**Stop-bleed disables intact:**
- `g_vwap_rev_nq.enabled = false` at `engine_init.hpp:608` — still
  closed-off per S71 Phase 3 WF FAIL.
- `g_ustec_tf_5m.enabled = false` at `engine_init.hpp:950` — still
  disabled. Phase 1 sweep confirms the **right** configuration is
  baseline (S63 zeroed). Enabled flip blocked on Phase 3 WF.

**Ungated-engine sweep expectations unchanged.** No engine code
modified.

**GoldEngineStack chokepoint audit:** not touched this session.
Two-hit expectation should still hold; verify before any
GoldEngineStack edit.

## Pre-commit checklist for next session

Standard project CLAUDE.md checklist applies. Specific notes for the
Phase 3 WF + (if pass) engine_init.hpp S63 revert:

1. The S63 revert in `engine_init.hpp` for `g_ustec_tf_5m` follows
   exactly the part-K/L VWR precedent at `engine_init.hpp:597-672`.
   Per CLAUDE.md the comment block above the setting MUST be read
   before any change; this means writing a NEW comment block
   documenting the Phase 1 + Phase 3 evidence at the new site
   BEFORE editing the values. Future sessions will read that comment
   when wondering why `g_ustec_tf_5m` has S63 zeroed.

2. The `enabled = true` flip and the S63 revert lines should be in
   the same commit — they are interdependent. Per the CLAUDE.md
   precondition: "no more 'fields exist, check never runs' commits"
   the inverse applies here — no more "instrument flipped on without
   its documented S63 disable" commits.

3. Mac canary build for `OmegaBacktest` AND
   `UstecTrendFollow5mBacktest` must be green before commit. Per
   CLAUDE.md §Build Verification — bare `cmake --build build -j`
   always fails on macOS due to winsock2.h. Use the specific
   targets.

## Stash state at session end

```
$ git stash list
(empty)
```

Inherited clean from part-S. No new stashes this session.

## Notes for whoever picks up part-U

The path is well-defined:

1. Read this handoff first, then `outputs/UTF5M_PHASE1_RESULTS_2026-05-14.md`
   for the full Phase 1 reasoning.
2. Read `scripts/vrev_wf_t1.py` end-to-end (the engine-agnostic
   skeleton + per-window invocation logic). It's the template for
   the UTF5m WF driver.
3. Adapt: drop `--symbol` (UTF5m harness is USTEC-only). Drop any
   VWR-specific axis flags (TP_FRACTION, EXTENSION_SL_RATIO, etc.).
   Add the `--mode baseline` flag for the WF run.
4. Run Phase 3 WF on the same 4.4 GB NSXUSD tape (sequential
   window-stream-then-delete to avoid the part-S P3 disk incident).
5. Decision per the embedded rule: ≥3 of 4 windows positive AND
   aggregate PF ≥ 1.20.
6. If PASS → engine_init.hpp S63 reverts + enabled flip (one commit,
   with comment block documenting the Phase 1 + Phase 3 evidence).
7. If FAIL → closure memo + leave disabled.

Either way, this is a 1-session task. The harness is fast (~18s/cell);
WF over 4 windows is ~80s wall-clock for the per-window runs plus
post-processing time.

## Operational notes

- Disk pressure was the dominant constraint in parts S + early-T.
  Operator cleaned disk this session; sandbox-side disk-full
  prevented all sandbox commands but did not block Mac-side work.
- Harness commit blast radius was minimal: 1 new file + 1 CMakeLists
  block. Re-verified buildable + smoke-tested before sweep launch.
- Phase 1 sweep ran cleanly end-to-end on first try. Self-consistency
  check passed. No follow-up debugging needed.
