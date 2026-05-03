# HANDOFF — USDJPY Asian Open: Walk-Forward Validation (next session)

This doc is the entry point for the *next* session. It contains everything
needed to start the walk-forward work without re-reading the two prior
session docs.

If you do want the full context, the lineage is:
- `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_TRAIL_FIX.md` — original
  trail-fix sweep that found the chosen winner
- `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_PHASE2_FINE.md` — fine grid
  that confirmed the plateau and rejected the corner-peak overfit
- this doc — what to do next

## Where things stand

Branch: `feature/usdjpy-asian-open` (off `omega-terminal`).
Prior commits are local; this and the previous session are uncommitted.
Working-tree changes at the moment of this handoff:

```
M  scripts/usdjpy_asian_trail_sweep.py        (added run_phase2_fine)
?? docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_PHASE2_FINE.md
?? docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD_HANDOFF.md
?? phase0_ema_scalp_backtest.py    (pre-existing, not from these sessions)
```

Production engine `include/UsdjpyAsianOpenEngine.hpp`:
md5 `d514fce983cf10914c77d001311bd4be`. Untouched. Off-limits to sweep
tooling per project policy.

Backtest copy `backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp`: resynced to
the chosen winner. Not git-tracked. Sweep tooling regenerates it per
cell.

Harness binary `build/usdjpy_asian_bt`: rebuilt at chosen-winner state.

Sweep results CSV `build/usdjpy_trail_sweep_results.csv`: 89 rows
(39 trail-fix sweep + 45 fine grid + 4 OOS validation + 1 verify cell).

## Chosen winner — current production candidate

```cpp
// Override on top of the production engine defaults.
static constexpr double MIN_RANGE       = 0.20;
static constexpr double SL_FRAC         = 0.80;
static constexpr double MFE_TRAIL_FRAC  = 0.15;
```

Performance, fixed train/test split:

| Period                  | Trades | WR     | PF   | Net PnL  | DD      | Months |
|-------------------------|-------:|-------:|-----:|---------:|--------:|--------|
| TRAIN  2025-03..09      |    176 | 81.8 % | 1.34 | +$288.94 | $118.01 | 5/7    |
| TEST   2025-10..2026-04 |    103 | 77.7 % | 1.19 | +$113.38 | $141.04 | 3/7    |
| FULL   14 months        |    278 | 79.5 % | 1.21 | +$317.10 | $202.51 | 8/14   |

The fine grid showed this cell sits inside a 2-D plateau (15/15 MR=0.20
cells profitable, PF 1.14–1.22). It is OOS-best of the three plateau
candidates I checked.

## Next task — walk-forward validation

Currently the OOS check is one fixed 7-month split (train 2025-03..09,
test 2025-10..2026-04). That can hide regime-shift fragility — if Q4
2025 happens to suit the strategy and Q1 2026 doesn't, a single fixed
split conceals that. Walk-forward is the appropriate test.

### Spec

Rolling window across the full 14 months:

- Train window: 6 months
- Test window: 1 month
- Step: 1 month
- Total folds: 14 − 6 = 8 folds

| fold | train period       | test month |
|------|--------------------|------------|
| 1    | 2025-03 .. 2025-08 | 2025-09    |
| 2    | 2025-04 .. 2025-09 | 2025-10    |
| 3    | 2025-05 .. 2025-10 | 2025-11    |
| 4    | 2025-06 .. 2025-11 | 2025-12    |
| 5    | 2025-07 .. 2025-12 | 2026-01    |
| 6    | 2025-08 .. 2026-01 | 2026-02    |
| 7    | 2025-09 .. 2026-02 | 2026-03    |
| 8    | 2025-10 .. 2026-03 | 2026-04    |

For each fold:
- Fix params at the chosen winner (no per-fold re-fit yet — that's a
  later question; first establish whether the *fixed* config holds up).
- Run train period: collect WR / PF / PnL / months. (Sanity check the
  train still looks like train.)
- Run test period: collect WR / PF / PnL.
- Stack the 8 test-month results into an "out-of-sample equity curve"
  and the 8 train PnLs into a "rolling train PnL."

### What "passes" looks like

Soft pass:
- Total walk-forward PnL across the 8 test months ≥ 50 % of the fixed
  test-fold $113.38 (i.e. ≥ ~$56). Lower bound because each month is
  noisier than a 7-month aggregate.
- No more than 2 of the 8 test months are deeply negative (deeply
  negative ≈ worse than −$80).
- Walk-forward PF (sum of test-month wins / sum of test-month losses)
  ≥ 1.05.

Hard pass (would actually be promotable to live):
- Walk-forward PF ≥ 1.15.
- ≥ 5 of 8 test months profitable.
- Worst test month DD < $200.

Hard fail:
- Walk-forward PF < 1.0.
- Any single test month with PnL < −$200 (worse than the full-period
  fixed-test DD).

### How to run it

The existing `cell` subcommand handles arbitrary `--from / --to` ranges,
so the walk-forward driver can be a simple bash loop. No script change
needed.

```bash
cd ~/omega_repo
git checkout feature/usdjpy-asian-open
export USDJPY_TICKS=$HOME/Tick/USDJPY        # adjust per machine

python3 scripts/usdjpy_asian_walkforward.py    # this is the new file you write
```

Suggested new file `scripts/usdjpy_asian_walkforward.py` (small driver,
~80 lines, can reuse `run_one` / `parse_summary` / `_print_row` from the
existing trail sweep script via `importlib`). Pseudocode:

```python
FOLDS = [
    ("2025-03", "2025-08", "2025-09"),
    ("2025-04", "2025-09", "2025-10"),
    ("2025-05", "2025-10", "2025-11"),
    ("2025-06", "2025-11", "2025-12"),
    ("2025-07", "2025-12", "2026-01"),
    ("2025-08", "2026-01", "2026-02"),
    ("2025-09", "2026-02", "2026-03"),
    ("2025-10", "2026-03", "2026-04"),
]
PARAMS = {"MIN_RANGE": 0.20, "SL_FRAC": 0.80, "MFE_TRAIL_FRAC": 0.15}

# import write_engine_copy / compile_bt / run_bt from the trail-sweep script
# write_engine_copy(PARAMS); compile_bt() ONCE up front
# for each fold: run_bt(label_train, frm=tr_start, to=tr_end)
#                run_bt(label_test,  frm=test_month, to=test_month)
# write a summary CSV: build/walkforward_results.csv
# print fold-by-fold table + walk-forward aggregate stats
```

Critical optimisation: compile *once* up front, not per-fold. The
chosen-winner config doesn't change between folds, so the binary is
constant. That's 16 runs (8 train + 8 test) at ~7 s of run time each
(no compile per call) ≈ 2 min total vs ~3 min if you compile each time.
Inside the 45-second sandbox bash limit you can do 5–6 fold runs per
call without help.

Alternative: extend the existing trail-sweep script with a
`walkforward` subcommand. Cleaner long-term but a bigger change.
First-pass standalone driver is fine.

### Gotchas (carried over from this session)

1. **Tick path.** The script's default tick path resolution starts at
   prior-session paths that no longer exist; always set
   `USDJPY_TICKS=$HOME/Tick/USDJPY` (or whatever it's called in the new
   session's mount layout).

2. **Bash timeout.** Sandbox bash calls hard-limit at 45 s. With the
   per-cell compile, ~3 cells fit per call. Without compile (single
   compile up front), ~5–6 cells fit per call.

3. **Backtest determinism confirmed.** The chosen-winner cell
   reproduces $317.10 / DD $202.51 / 8/14 months exactly across reruns.
   If a re-run produces different numbers, something is wrong with the
   tick data alignment, not the code.

4. **Don't touch `include/UsdjpyAsianOpenEngine.hpp`.** All
   parameterisation goes through the sweep tooling, which rewrites the
   `backtest/usdjpy_bt/` copy only.

5. **CSV resumability.** The trail sweep CSV de-dupes by label string,
   not by (overrides) tuple. Pick distinct labels for walk-forward
   rows so they don't collide with prior-session labels. Suggested:
   `WF_fold1_train`, `WF_fold1_test`, etc.

## Lower-priority queue (for after walk-forward)

These were items 2–6 in the prior queue and are still in the same order.

2. **USDJPY London hours (06–09 UTC).** Cheapest test of "is this engine
   architecture sound or only working on Asian range data." Override
   `SESSION_START_HOUR_UTC=6, SESSION_END_HOUR_UTC=9` on the chosen
   winner. One full-period cell.
3. **Tokyo-fix exclusion (00:50–01:00 UTC).** With MR=0.20 most
   fix-ranges should already be filtered out, but worth one cell to
   confirm.
4. **Restricted session windows {01–04, 02–04, 00–02} UTC.** First hour
   after Tokyo open is documented chop.
5. **Kelly sizing analysis.** WR=79.5 %, avg_win=$8.22, avg_loss=$26.32,
   Kelly fraction ≈ 0.139, half-Kelly = 7 %. Current LOT_MAX=0.20
   already aggressive vs OOS PF=1.19 — do not raise without a 2-week
   shadow gate.
6. **Mean-reversion variant.** Lower priority; breakout direction is
   right.

## Reminders

- GitHub PAT in `CLAUDE.md` (`ghp_9M2I…24dJPV4`) — please rotate. Has
  been called out in two prior session docs and not yet acted on.
- Promotion of the chosen winner to live still requires a 2-week shadow
  paper run per the engine header's safety gate. Walk-forward is a
  validation step, not a substitute for the shadow gate.
- Phase 2 fine grid took ~10 min wall-clock at ~12.9 s/cell. Walk-forward
  with single up-front compile should be faster (~2 min total).
