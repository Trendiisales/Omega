# Session Handoff — 2026-05-13 (NZST), part I

Direct follow-up to `SESSION_HANDOFF_2026-05-13c.md` (part H). This
session executed the in-flight cut design captured in part H §"Proposed
in-flight cut" after the operator confirmed thresholds, edit method,
and Tsmom fix selection.

## TL;DR

1. **Three operator decisions taken from part-H carry-over:**
   - VWAPReversion thresholds: defaults accepted
     (`LOSS_CUT_PCT=0.08 / BE_ARM_PCT=0.05 / BE_BUFFER_PCT=0.02` for
     indices; tighter FX overrides `0.05 / 0.03 / 0.015` for EURUSD).
   - Edit method: **targeted edits**, working-tree only, NO commit
     until explicit go-ahead. (Operator preference recorded for the
     session and now codified in repo CLAUDE.md.)
   - Tsmom_H1_long fix: option (a) — in-flight MAE stacking gate.

2. **Code applied as three targeted edits across two engine files
   plus one config file.** Mid-session syntax-check via sandbox
   `g++ -fsyntax-only` clean across all three files plus a synthetic
   TU that instantiates `VWAPReversionEngine` and `TsmomCell` and
   exercises the new fields.

3. **Two commits landed and pushed to `origin/main`:**
   - `c216db8` — S37-H-followup engine + config changes
   - `0e95ecd` — `CLAUDE.md` project-level operating rules

4. **`CLAUDE.md` created at repo root** to codify the rules that
   surfaced this session: no-commit-without-go-ahead, targeted-edits
   override, core-files list, build verification, standing audits,
   cost-gate semantics, session-handoff convention, stale-lock
   recovery procedure.

5. **All standing audits re-ran clean.** Ungated-engine audit still
   shows the same 4 INERT files. GoldEngineStack chokepoint still has
   exactly one `pos_mgr_.open()` code call site (L4182). Part-H
   handoff's "exactly two hits" count for the chokepoint grep is
   refined below — there are 2 doc-comment mentions plus 1 actual call.

6. **No core code touched.** All edits are in engine files
   (`CrossAssetEngines.hpp`, `TsmomEngine.hpp`) and a config file
   (`engine_init.hpp`), per the operator preference reaffirmed at
   session start.

7. **VPS DEPLOY COMPLETED THIS SESSION.** Operator ran
   `.\OMEGA.ps1 deploy` on the Windows VPS at session end. Service
   restarted with the new binary. Account 8077780 now runs the
   post-`0e95ecd` build. This closes the deploy-pending lineage that
   had carried from parts D / E / F / G / H. New observation window
   begins now.

## Decisions confirmed (operator answers to part-H §"Decision still pending")

| # | Question                          | Operator answer                                                                 |
|---|-----------------------------------|---------------------------------------------------------------------------------|
| 1 | Threshold values                  | Accept defaults                                                                 |
| 2 | Edit method (full file vs target) | Targeted edits, in-memory only (no commit until review)                         |
| 3 | Tsmom_H1_long stacking fix        | Option (a): in-flight MAE check at pre-fire gates                              |

Decision (2) carries forward as a standing preference. The operator
made it explicit that "always full file" (their global pref) is
overridden when they say "targeted edits" in a given session, and
that the no-commit-without-go-ahead rule is now standing — the
part-G unauthorized commit is the precedent that triggered it.

## Edits applied

### `include/CrossAssetEngines.hpp` (+125, -10)

Three sub-edits:

**Sub-edit a: include line (+1)**
```
+#include <limits>
```
Placed alongside the existing C++ standard-library includes (after
`<algorithm>`). Needed for `std::numeric_limits<int>::max()` used in
the rewritten manage block.

**Sub-edit b: three new public fields on `VWAPReversionEngine` (+34)**
Placed after `MIN_SESSION_MIN` / `enabled` and before the existing
confluence thresholds. Each field has documented semantics, a
zero-value disable mode, and rationale referencing the 19-trade live
slice.
```
double  LOSS_CUT_PCT   = 0.08;   // cold-loss cut threshold (% of entry)
double  BE_ARM_PCT     = 0.05;   // mfe % of entry that arms BE ratchet
double  BE_BUFFER_PCT  = 0.02;   // BE_CUT triggers when move <= entry*pct/100 after arm
```
The long preamble comment explains the entry-only nature of
`OmegaCostGuard::is_viable()` and why an in-flight check is needed.
This is the canonical docstring for the in-flight cut pattern; other
engines that adopt it should reference it.

**Sub-edit c: manage-block replacement (+90, -53)**
Replaces L1265-1318 of the pre-edit file (the
"progressive timeout + MAE-exit + pos_.manage(MAX_HOLD_SEC)" block)
with the two-phase logic from part-H design:

- **Phase 1 — BE_RATCHET (runs first):** arms when `pos_.mfe >=
  entry*BE_ARM_PCT/100`. Once armed, retrace to `entry +
  entry*BE_BUFFER_PCT/100` triggers `BE_CUT`. Short cooldown
  (`COOLDOWN_SEC`), no FC-counter penalty (giveback isn't a thesis
  failure).
- **Phase 2 — LOSS_CUT:** triggers when `adverse >=
  entry*LOSS_CUT_PCT/100`. Full `MAE_COOLDOWN_SEC` cooldown and FC
  counter accounting (still escalates to 30-min directional block on
  2 consecutive same-direction LOSS_CUT/MAE).
- **Progressive timeout:** retained but only effective on the losing
  side (winners get `eff_max_hold = INT_MAX` and never hit T/O).
- **Legacy `MAE_EXIT_RATIO`:** retained behind LOSS_CUT as a backstop.
  When `LOSS_CUT_PCT > 0` this rarely fires (LOSS_CUT is tighter).
  When `LOSS_CUT_PCT == 0` the legacy path is the only adverse-cut
  mechanism — matches pre-S37-H behaviour.
- **Winners ride the trail (no timeout):** `pos_.manage(bid, ask,
  INT_MAX, on_close)` makes the `CrossPosition::manage()` T/O branch
  inert when in profit; existing BE / mid-lock / trail logic at
  CrossPosition L171-202 handles natural exit via TP_HIT or trailed
  SL_HIT. Losers still see `MAX_HOLD_SEC` and get T/O'd at the
  original deadline.

`pos_.mfe` is updated in the engine BEFORE the BE-ratchet gate fires
(to ensure fresh-tick mfe is visible) and again by
`CrossPosition::manage()` at end of the block. Both writers are
monotonic-up, so the double-write is safe.

### `include/engine_init.hpp` (+22, -0)

Per-instance defaults for each of the four VWAPReversion instances:

- `g_vwap_rev_sp` (US500.F): `0.08 / 0.05 / 0.02`
- `g_vwap_rev_nq` (USTEC.F): `0.08 / 0.05 / 0.02`
- `g_vwap_rev_ger40` (GER40): `0.08 / 0.05 / 0.02`
- `g_vwap_rev_eurusd` (EURUSD): `0.05 / 0.03 / 0.015`

FX uses tighter values because FX moves smaller in percentage terms
than indices — a 0.08% adverse move on EURUSD is ~9 pips which is
larger than a typical full move at the session range, defeating the
purpose.

### `include/TsmomEngine.hpp` (+36, -0)

In-flight MAE stacking gate inserted in the pre-fire gate block
(after the existing S12 post-MAE_EXIT cooldown gate, before
"5. tsmom signal"). Scans `positions_` and returns 0 (blocking the
new entry) when any open position has
`p.mae <= -0.5 * mae_exit_atr * p.atr`.

Auto-disabled when `mae_exit_atr == 0.0` (the gate has no meaning
when MAE_EXIT itself is off — same condition the S12 cooldown
effectively requires).

The gate logs the offending position id, mae value, and threshold so
post-mortem analysis can identify which sub-position was already
bleeding when a new entry was attempted.

## Sandbox build verification

Sandbox cannot run `cmake` (no `cmake` binary; project pulls in
`winsock2.h` which Linux doesn't ship). Used three sandbox-side
checks:

1. `g++ -fsyntax-only -Iinclude -x c++ include/CrossAssetEngines.hpp`
   → clean (one harmless `#pragma once in main file` warning).
2. `g++ -fsyntax-only -Iinclude -x c++ include/TsmomEngine.hpp`
   → clean (same harmless warning).
3. Synthetic TU at `outputs/syntax_check2.cpp` that instantiates
   `omega::cross::VWAPReversionEngine` and `omega::TsmomCell` and
   assigns the three new fields → exit 0, no errors.

`engine_init.hpp` cannot be checked standalone (it depends on the
full main.cpp include chain including Windows-only headers). The
synthetic TU covers the field-existence check that engine_init.hpp
relies on.

The sufficient check is the Mac canary build (operator-side, still
pending). See "Carry-over" below.

## Commits

```
0e95ecd Add CLAUDE.md operating rules
c216db8 S37-H-followup: VWAPReversion in-flight cut + Tsmom stacking gate
1c2306b Cost-gate rollout: MinimalH4 + C1Retuned + GoldEngineStack    (part-G)
```

Both new commits pushed to `https://github.com/Trendiisales/Omega.git`
main. Remote HEAD = `0e95ecd8c5a8198b0394871213eb1af70e0c8b62`.
Working tree clean.

## CLAUDE.md (new, repo root)

Project-level operating rules that supplement the operator's global
user-preferences. Covers:

- Edit discipline (no commits without go-ahead; targeted-edits
  override; core-files list).
- Build verification (sandbox `g++` necessary-not-sufficient; Mac
  canary as sufficient check).
- Standing audit checks (ungated-engine, GoldEngineStack chokepoint).
- Cost-gate semantics (entry filter only; in-flight protection
  belongs in the engine's manage block; canonical pattern is the
  VWAPReversion LOSS_CUT + BE_RATCHET fields).
- Session-handoff convention.
- Stale `.git/index.lock` recovery procedure.

The "canonical in-flight pattern" reference in CLAUDE.md points to
this session's `VWAPReversionEngine` work as the reference
implementation. Engines that share the "mean-reversion + fixed
timeout" profile are candidates for the same pattern — see "Future
work" below.

## Standing audit results (post-edits)

### Ungated-engine audit
```bash
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard" "$f"; then echo "UNGATED: $f"; fi
  fi
done
```
Output (unchanged from part-H):
```
UNGATED: include/LatencyEdgeEngines.hpp        # S13 culled
UNGATED: include/RSIExtremeTurnEngine.hpp      # S52 disabled (0/153 combos)
UNGATED: include/SweepableEngines.hpp          # research-only sweep harness
UNGATED: include/SweepableEnginesCRTP.hpp      # research-only sweep harness
```
Exactly the four INERT files expected. No drift.

### GoldEngineStack chokepoint audit
```bash
grep -nE "\.open\(" include/GoldEngineStack.hpp
```
Output:
```
50:#include "OmegaCostGuard.hpp"  // see GoldEngineStack::on_tick() pos_mgr_.open() gate
4163:            // single pos_mgr_.open() call below. TP distance derived from
4182:            pos_mgr_.open(gs, spread, latency_ms, current_regime_name());
```

**Note on count**: part-H handoff said "exactly two hits" but the
correct count is three: two doc-comment mentions (L50 include comment
+ L4163 architecture comment) plus one actual code call (L4182).
Inspected L4163 this session: it is a comment ("// single
pos_mgr_.open() call below. TP distance derived from ...") inside
the cost-gate block, not a call site. The chokepoint structure is
intact — there is still exactly ONE `pos_mgr_.open(` code call site
in the 4763-line file, at L4182, directly gated by the
`ExecutionCostGuard::is_viable("XAUUSD", ...)` check at L4176.

Refined expectation for future audits: **2 doc-comment mentions +
1 actual call**. A fourth `.open(` match anywhere in the file would
warrant investigation. CLAUDE.md updates may want this refinement
next time it's edited.

### TsmomEngine pre-fire gates inventory
The new in-flight MAE stacking gate sits at the end of section 4
(pre-fire gates). Reading the `return 0;` inventory in
`on_bar()` confirms the gate is in the correct position (after the
S12 MAE_EXIT cooldown gate at L423, before the tsmom signal eval at
L471):
```
398:        if (!enabled)
399:        if (n_open() >= max_positions_per_cell)
400:        if (cooldown_left_ > 0)
401:        if ((int)closes_.size() < lookback + 1)
402:        if (!std::isfinite(atr14_at_signal) ...)
404:        if (!std::isfinite(spread_pt) ...)
405:        if (spread_pt > max_spread_pt)
406:        if (size_lot <= 0.0)
423:        S12 MAE_EXIT cooldown gate
458:        IN_FLIGHT_MAE gate (NEW THIS SESSION)
471:        sig_dir == 0
472:        sig_dir != direction
```

## Next session — fix all remaining issues

This is the priority list the operator asked for at session end:
"ensure we fix all remaining issues in next session." Items are
ordered by what the next session can actually act on, and by
dependency.

### Priority 1 — Observation of today's deploy (NEW; replaces VPS-deploy carryover)

The deploy at the end of this session is now LIVE on account 8077780.
First observation pass should happen in the next session:

```powershell
# On the Windows VPS:
cd C:\Omega
.\VERIFY_STARTUP.ps1
# Review C:\Omega\logs\startup_report.txt
```

Check for:
- `[VWAP-REV]` startup banners for `_sp / _nq / _ger40 / _eurusd`
  (4 banners expected; confirms the new fields loaded with the
  per-instance configs from `engine_init.hpp`).
- `[Tsmom_H1_long]` cell init line.
- No `LOSS_CUT_PCT` warning / parse errors in the first 5 minutes
  of log.
- Service status `Running`, watchdog status `Running` (if
  installed).

Then pull a fresh 24-48hr trade slice from the live tape after a
full session has elapsed:

```bash
# On Mac (operator-side OR Claude can do if granted log access):
ssh vps "Get-Content C:\Omega\logs\latest.log | Select-String 'VWAP-REV|Tsmom|LOSS_CUT|BE_CUT' | Out-File slice.log"
# Then analyse: exit-reason distribution, LOSS_CUT/BE_CUT vs T/O ratio.
```

Expected effect (target signal for "edit worked"):
- Exit-reason distribution shifts: `T/O` count down, `LOSS_CUT` and
  `BE_CUT` counts up.
- Outlier-loss tail (worst-case per-trade loss) shrinks from ~$80 to
  ~$40-45.
- Tsmom_H1_long stops producing back-to-back same-direction MAE_EXIT
  stacks.

If after 24-48hr the distribution has NOT shifted, the thresholds
need tuning. Most likely cause: index instances chose `LOSS_CUT_PCT
= 0.08` which is wider than the typical adverse run; consider 0.05
for a tighter cut.

### Priority 2 — Mac canary build (still pending operator-side)

```bash
cd ~/omega_repo
cmake --build build --target OmegaBacktest -j
```
**Do NOT** use the bare `cmake --build build -j` — that target needs
Windows-only headers and always fails on macOS even though the
surrounding green "Built target X" lines make it look like a pass.

A clean build of `OmegaBacktest` confirms the cross-platform
sub-set of the edits compiles clean (the production build on the
Windows VPS already succeeded — this is the Mac backtest harness).

### Priority 3 — Backtest validation of the new VWAPReversion thresholds

The thresholds (`0.08 / 0.05 / 0.02` indices; `0.05 / 0.03 / 0.015`
FX) are live-tape-design-validated but NOT backtest-validated. Run
the VWAPReversion backtest harness over the post-2025-04 corpus
with the new fields populated and compare:

| metric                          | baseline (no new fields) | with new fields |
|---------------------------------|--------------------------|-----------------|
| Net PnL                         | (current)                | (?)             |
| Win rate                        | (current)                | (?)             |
| Largest-loss percentile (worst) | (current)                | (?)             |
| TP_HIT count                    | (current)                | (?)             |
| T/O count                       | (current)                | (?)             |
| LOSS_CUT count                  | 0                        | (?)             |
| BE_CUT count                    | 0                        | (?)             |

Tune any threshold that materially HURTS the central tendency.
Tighten any threshold that doesn't catch the outlier tail.

Harness invocation (to be confirmed next session via repo search):
likely `cmake --build build --target IndexBacktest -j` then run with
appropriate ledger files.

### Priority 4 — Roll the in-flight cut pattern to candidate engines

The VWAPReversion `LOSS_CUT_PCT + BE_ARM_PCT + BE_BUFFER_PCT` is now
the canonical "in-flight cost-cover protection" reference (codified
in CLAUDE.md). Three candidate engines share the same profile
(mean-reversion or fixed-timeout + no TP_HIT in live tape):

a. **`TrendPullbackEngine`** (4 LIVE instances) — has `MAX_HOLD_SEC`,
   no in-flight cut. Highest priority among the three because it's
   live, not shadow.

b. **`NoiseBandMomentumEngine`** gold_london — session timeouts, no
   in-flight cut. Live on XAUUSD London session.

c. **`OpeningRangeEngine`** (5 instances, only US500/USTEC enabled) —
   range-breakout with fixed hold. Similar exit profile.

Apply pattern per-engine, NOT bulk. Each has different TP semantics
and the default thresholds need tuning. Operator approval per-engine
before applying. **Recommended sequence:**

1. Wait ~1 week of post-deploy live tape on VWAPReversion to confirm
   the pattern produces the expected distribution shift.
2. If confirmed, apply to `TrendPullbackEngine` first (live, highest
   exposure).
3. Then `NoiseBandMomentumEngine` gold_london.
4. Then `OpeningRangeEngine`.

### Priority 5 — `GoldPositionManager::TICK_SIZE` accessibility (core code)

Still private constexpr. `GoldEngineStack.hpp` gate at L4173 uses a
local `GS_TICK_SIZE = 0.10` literal as a workaround. If the broker
ever changes XAUUSD tick size both copies must change in lock-step.

This is a **core code change** — `GoldPositionManager` is on the
core-files list in CLAUDE.md. Operator must explicitly instruct
before this is touched.

Proposed minimal change (for operator review next session):
- Promote `TICK_SIZE` from `private constexpr` to `public static
  constexpr`, OR
- Add a public `get_tick_size() noexcept` accessor.

Either is a 1-line cleanup. The second option is more conservative
(no header layout change visible to callers).

### Priority 6 — USTEC TrendFollow5m promotion (waiting period)

Still in the 2-month shadow window (started at commit `176c746`,
2026-04-something). Promotion-to-live is the future "S37-P4" commit,
after the window closes. Nothing to do this session or the next —
listed for completeness.

### Priority 7 — Sandbox / Mac cleanup (housekeeping)

Run on Mac side:
```bash
rm -f ~/omega_repo/.git/*.lock.bak* ~/omega_repo/.git/*.lock.bak.*
rm -f ~/omega_repo/outputs/syntax_check*.cpp
```
Both are sandbox-leftover scratch files. Not in the repo (gitignored
by .git/ scope and outputs/ is not part of the build).

---

## Reference — original carry-over items (for context)

These items were on the part-H handoff list and are now resolved or
re-stated in the priority list above:

- **VPS deploy** — DONE this session (Priority 1 above is now
  observation of the live deploy).
- **GoldEngineStack chokepoint monitoring** — ongoing; refined grep
  count (3 hits expected, not 2; see audit section above).
- **In-flight cut pattern propagation** — captured in Priority 4
  above.
- **Replay validation** — captured in Priority 1 above.

## Bookkeeping

- HEAD = `0e95ecd Add CLAUDE.md operating rules`
- Remote HEAD = `0e95ecd8c5a8198b0394871213eb1af70e0c8b62` (verified)
- Working tree clean (modulo sandbox `.git/index.lock` cleanup noise
  — see CLAUDE.md §"Stale `.git/index.lock`" for recovery, applied
  this session via `mv` rename trick since the sandbox cannot `rm`
  inside `.git/`).
- Mac-side cleanup recommended:
  ```
  rm -f ~/omega_repo/.git/*.lock.bak* ~/omega_repo/.git/*.lock.bak.*
  rm -f ~/omega_repo/outputs/syntax_check*.cpp
  ```
  Both are sandbox-leftover scratch files. Not in the repo.

## Validation actions next session

```bash
cd ~/omega_repo
# 1. Confirm today's HEAD survived the deploy
git log --oneline -5
# Expect HEAD: 0e95ecd CLAUDE.md (or a later commit if next session adds more)

# 2. Re-run the ungated audit
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard" "$f"; then echo "UNGATED: $f"; fi
  fi
done
# Expect: only LatencyEdgeEngines, RSIExtremeTurnEngine,
# SweepableEngines, SweepableEnginesCRTP.

# 3. Re-run the GoldEngineStack chokepoint audit
grep -nE "\.open\(" include/GoldEngineStack.hpp
# Expect: exactly 3 hits (L50 + L4163 doc comments + L4182 gated call).

# 4. On VPS -- check service health after a full session
# (Windows PowerShell)
Get-Service Omega, OmegaWatchdog
Get-Content C:\Omega\logs\latest.log -Tail 100 | Select-String 'VWAP-REV|Tsmom|LOSS_CUT|BE_CUT|ERROR'

# 5. Mac canary (operator-side, optional now since VPS deploy succeeded)
cmake --build build --target OmegaBacktest -j
```

## Quick-reference files

| file                                       | purpose                          | size      |
|--------------------------------------------|----------------------------------|-----------|
| `include/CrossAssetEngines.hpp`            | edited this session (VWAPReversion in-flight cut) | ~108 KB |
| `include/TsmomEngine.hpp`                  | edited this session (in-flight MAE gate) | ~45 KB  |
| `include/engine_init.hpp`                  | edited this session (4 VWAPReversion instance configs) | ~210 KB |
| `CLAUDE.md`                                | NEW this session (project rules) | ~5 KB     |
| `outputs/SESSION_HANDOFF_2026-05-13c.md`   | part H (reference)               | reference |
| `outputs/SESSION_HANDOFF_2026-05-13d.md`   | this document                    | current   |
