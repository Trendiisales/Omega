# Session Handoff #2 — S51 1A.1.b D6+E1 complete

**Date:** 2026-04-28 (NZST evening / UTC late afternoon)
**Predecessor:** `incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF.md` (handoff #1)
**Current HEAD:** d1d09db62d4f4389c903e812ffab8d0b67d8a1d8

## TL;DR

D6+E1 ran successfully on the 24-month tick stream. All 5 output files present, all marker checks pass. Read `D6E1_RESULTS.md` (sibling file in this folder) for the full data.

**HBG D6 partial success**: best score 0.4032 → 0.4377, but `min_range` still ceiling-clipped, `max_range` migrated to grid floor, and `trail_frac` produces byte-identical results across 7 different values (logic never engages).

**E1 clean**: 74/490 degenerate combos flagged, all with n_trades=0.

**AsianRange has TWO bugs now**: original 100× frequency mismatch (unchanged from D5), PLUS newly visible cross-instance non-determinism. 10 combos with byte-identical IEEE-754 template params produce different P&L (2 lose, 8 win).

**VWAPStretch & EMACross**: confirmed no edge in any quadrant.

## Recovery from prior session's lost output

Prior session reported a "1096s sweep complete" but no CSVs existed on disk. Investigation this session confirmed: no files were lost. The 23:05 NZST D6+E1 binary had never produced output. Three pre-existing CSV directories were located, all D5 baseline or earlier:

| Path | Mtime | Source |
|---|---|---|
| `~/omega_repo/sweep_results_archive_20260428_115235/` | 27 Apr 17:47 | Pre-D5 |
| `~/omega_repo/sweep_results/` | 28 Apr 12:09 | D5 baseline |
| `~/omega_sweep_baseline_20260428/` | 28 Apr 21:55 | Copy of D5 (cp before D6 work) |

This session's run wrote to `~/omega_repo/sweep_D6E1_20260428_231956/` (timestamped, absolute path). Future sweeps should follow the same pattern — see "Bulletproof run protocol" below.

## Bulletproof run protocol (used this session, recommend keeping)

The script that produced this run is at `/tmp/run_d6e1.sh` on the Mac. Key invariants worth preserving for any future sweep:

1. `OUTDIR` is timestamped: `$HOME/omega_repo/sweep_<TAG>_<TS>/`. Cannot collide with prior runs.
2. `OUTDIR` is **absolute**, never relative. Removes any cwd-mid-run risk.
3. Sentinel file written into outdir before binary launch. Confirms outdir is writable from the launch shell.
4. `2>&1 | tee` captures stderr (catches `write_csv` `ERROR: could not open` lines).
5. `${PIPESTATUS[0]}` captures the binary exit code (not tee's).
6. Post-run `stat`-based file presence check, not trust of binary's stdout claims.
7. Marker validation in-line: header `,degenerate` column, HBG max_range range, EMACross degenerate count.

Whoever runs the next sweep should adapt this script with new `TAG` and re-use the rest verbatim.

## State

### Live trading
- **VPS**: ed95e27c. Untouched. Service running. **No urgency to deploy** any newer commit.

### Repo
- HEAD on origin/main: `d1d09db62d4f4389c903e812ffab8d0b67d8a1d8`
- Local Mac HEAD: same
- Mac binary: `~/omega_repo/build/OmegaSweepHarness`, 23:05 NZST 2026-04-28, contains D6+E1 (verified by post-run marker checks).

### Authorised but not started (carrying forward from handoff #1)
- **D7**: AsianRangeT diagnostic counters (8-9 gate-rejection counters)
- **E2**: EMACross hardcoded RSI dead-band review at SweepableEngines.hpp:991-998
- **D8**: VWAPStretch structural fix

### Newly recommended this session (NOT authorised)
- **D7 (revised scope)**: original counters + per-instance state dump for combos 24, 73, 122 to pinpoint cross-instance non-determinism
- **D7.1**: cross-engine determinism check (verify HBG/VWAPStretch identical-mult combos also vs each other)
- **D6.1**: HBG max_range rebase 32→16 (geometric grid 8..32) to chase the floor-side optimum
- **D6.2**: HBG min_range rebase 3→6 (geometric grid 3..12) to expose above-current-base territory
- **E1.1**: extend E1 to also flag rsi_lo ≥ rsi_hi as degenerate
- **HBG-DIAG**: trail_armed_count + trail_fired_count counters in HBG_T to confirm trail is dead
- **write_csv fail-loud commit**: propagate fopen failure to non-zero exit; cross-cutting infra fix

## Recommended next-session priority

The data suggests a different order than handoff #1's plan:

1. **D7 + D7.1 first.** The non-determinism finding means *every other engine's results may be silently contaminated*. Until this is resolved we cannot trust HBG's score=0.4377 either. The AsianRange counter dump will tell us which gate eats the missing 380 trades AND the per-instance state dump will pinpoint which state leaks.
2. **HBG-DIAG second.** Tiny instrumentation. Tells us whether trail_frac param can be dropped.
3. **write_csv fail-loud.** 10-line infra fix. Removes the silent-failure class.
4. **D6.1 / D6.2** only after D7 confirms harness path is sound.
5. **E2 / D8 / EMACross redesign**: deferred. EMACross has no edge in any quadrant (worst combo loses ~$237k scaled to 5.0 lot over 24 months). VWAPStretch similar.

## Pre-delivery rule additions earned this session

- Always verify GitHub HEAD via contents API at start of session, before any code reading.
- When verifying a claim across files, do not trust regex narrowness — read with `grep -B/-A` first, plain text second, regex last.
- Self-correct in writing the moment a regex returns nothing for a claim that should be present.
- For cwd-sensitive output paths, always use absolute paths in scripts and validate with a sentinel pre-run.
- A "wrote sweep_xxx.csv (N combos)" stdout line proves nothing unless paired with a post-run `stat` of the file.

## Next-session opener (paste verbatim)

```
/ultrathink

Continuing Omega S51 1A.1.b. HEAD pinned at
d1d09db62d4f4389c903e812ffab8d0b67d8a1d8 on main.

D6+E1 complete. Read these in order before touching code:
  1. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF.md  (handoff #1)
  2. incidents/2026-04-28-s51-1a1b-prep/D5_RESULTS.md
  3. incidents/2026-04-28-s51-1a1b-prep/D6E1_RESULTS.md
  4. incidents/2026-04-28-s51-1a1b-prep/SESSION_HANDOFF_2.md  (this memo)

Top of mind: AsianRange has cross-instance non-determinism (10 byte-
identical-param combos produce different P&L). This may contaminate
all engines' results until resolved. D7 must include per-instance
state dump for combos 24, 73, 122.

VPS still on ed95e27c (live, fine, no urgency).

Authorised: D7 (revised), E2, D8 from handoff #1.
Newly recommended (need authorisation): D7.1, D6.1, D6.2, E1.1,
HBG-DIAG, write_csv fail-loud.
```
