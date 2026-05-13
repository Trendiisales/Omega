# Session Handoff — 2026-05-13 (NZST), part M

Direct follow-up to `SESSION_HANDOFF_2026-05-13e.md` (part L).

## TL;DR

1. **Phase 2a parity contract is fully restored.** V1 vs V2 ledgers are
   byte-identical at both `--max-pos 1` AND `--max-pos 10` after porting
   the V1 MAE_EXIT + S12 cooldown gate + in-flight stacking gate into
   `CellEngine.hpp`. Stronger than what part L scoped.
2. **VPS is live on today's HEAD `17ccb45`.** The 19-engine
   winner-exemption rollout (`0e37efc`) plus all subsequent P2/IFG/cleanup
   commits are deployed and the trading service is running clean.
3. **GitHub auth migrated to SSH.** PAT rotated, remote swapped, working
   end-to-end. The old PAT is dead.
4. **One new issue surfaced at session end and is queued as Priority 1
   for the next session: GBPUSD LONG trade at 07:02:41, SL_HIT in
   7m41s, -$17.20. Not yet investigated** (logs not yet read in detail;
   session budget exhausted before the trade lifecycle could be parsed).

## Commits this session (all on origin/main)

```
4007009  S37-L-P2: port V1 MAE_EXIT + S12 cooldown to CellEngine for V2 max-pos=1 parity
a9608ca  S37-L-P2-followup: port V1 in-flight MAE stacking gate to CellEngine for V2 max-pos=10 parity
70aa540  repo cleanup: archive untracked session handoffs + rm part-H/I/K staging files
ebcaca7  auth: confirm SSH-based push works (no code change)
17ccb45  S37-L-build-fix: rename inner mid -> bar_mid in 3 rollout sites to satisfy MSVC /W4 /WX
```

`70aa540` had one false start (`9a404ec`) that GitHub push protection
blocked for containing the full literal PAT in
`SESSION_HANDOFF_2026-05-13a.md:280`. The line was scrubbed to the
partial-redaction pattern (`ghp_9M2I…24dJPV4`) before the successful
push. Process documented in this session's commit history.

## Phase 2a parity achievement

Smoke test post-deploy (run with `./smoke_test_part_L.sh`):

```
                  V1                V2
max-pos=1
  trades          (same)            (same)
  equity          17456.81          17456.81
  max_dd          -4.28%            -4.28%
  parity          PARITY OK byte-identical

max-pos=10
  trades          100               100
  equity          40230.91          40230.91
  max_dd          -24.81%           -24.81%
  parity          ledger CSVs byte-identical (manual diff)
```

The smoke test script's max-pos=10 verdict logic still hardcodes
"parity check skipped" because the original handoff expected divergence
there. Cosmetic cleanup; the actual data is parity-clean.

## VPS deploy

- **Pre-deploy HEAD on VPS:** `0e95ecd` (part-D era).
- **Post-deploy HEAD on VPS:** `17ccb45` (today's tip).
- **Services:** `Omega` and `OmegaWatchdog` both `Running`.
- **Startup verifier:** 2 yellow warnings, both benign:
  - `VIX Level` — no VIX.F tick in first 10s; self-resolves on first tick.
  - `RSI Reversal Active` — engine is in `shadow_mode=true` (intentional).
- **Live behaviour delta to watch:** the 0e37efc rollout means 19
  engines now exempt winners from their TIME_EXIT / MAX_HOLD paths.
  Expect a lower TIME_EXIT count and a higher TP_HIT / TRAIL_HIT /
  SL_HIT count over the next live tape session. The VWAPReversion
  smoke test on the canonical engine (US500/USTEC/GER40/EURUSD)
  showed this pattern -- GER40 specifically gained +9.25 gross_pnl
  from the cuts; the other three were either neutral (revert kept the
  cuts off) or pending per-symbol validation.

## Security migration

| item                | before                                        | after                                          |
|---------------------|-----------------------------------------------|------------------------------------------------|
| GitHub PAT          | `ghp_9M2I…dJPV4` (in CLAUDE.md, 10 .md files) | rotated; new PAT not stored in any repo file   |
| omega_repo remote   | `https://github.com/Trendiisales/Omega.git`   | `git@github.com:Trendiisales/Omega.git` (SSH) |
| Auth                | HTTPS + PAT (manual paste / Keychain)         | SSH (ed25519 in `~/.ssh`)                      |
| `ssh -T git@github` | n/a (untested)                                | `Hi Trendiisales! ...` (verified)              |
| Push to origin      | required PAT entry                            | no prompt, no PAT needed                       |

Outstanding security loose ends (LOW priority -- rotation makes them inert):

- **Global CLAUDE.md** still contains the dead PAT and the HTTPS remote
  reference. Replace with the SSH-documented text from this session's
  Phase 4 walk-through. Path is somewhere under `~/.claude/CLAUDE.md`
  or `~/Library/Application Support/Claude/CLAUDE.md` -- find with
  `find ~ -name CLAUDE.md -not -path "*/node_modules/*" 2>/dev/null`.
- **VPS startup verifier** at `C:\Omega\VERIFY_STARTUP.ps1` still
  authenticates GitHub API with the dead PAT (visible in deploy output
  as `GitHub API unreachable: 401 Unauthorized`). The check is
  informational -- binary hash is the source of truth either way --
  but should be updated to either a fresh fine-scoped PAT or `gh`
  CLI auth.
- **9 older session docs** with partial `ghp_9M2I…24dJPV4` references
  (`docs/SESSION_2026-05-02_*.md`, `docs/SESSION_2026-04-30b_HANDOFF.md`,
  `SESSION_HANDOFF_2026-04-29.md`). Partial refs aren't exploitable;
  the rotation has made them meaningless trivia. Optional follow-up
  via `git filter-repo` if cleaner history is desired.

## Files changed this session

```
include/CellEngine.hpp          edited (P2 + IFG gate)
include/TsmomStrategy.hpp       edited (build_default_tsmom_topology MAE config)
include/HTFSwingEngines.hpp     edited (build fix: mid -> bar_mid at L487)
include/MacroCrashEngine.hpp    edited (build fix: mid -> bar_mid at L755)
include/BracketEngine.hpp       edited (build fix: mid -> bar_mid at L475)
docs/handoffs/SESSION_HANDOFF_2026-05-12c.md  newly committed (was untracked)
docs/handoffs/SESSION_HANDOFF_2026-05-12d.md  newly committed (was untracked)
docs/handoffs/SESSION_HANDOFF_2026-05-12e.md  newly committed (was untracked)
docs/handoffs/SESSION_HANDOFF_2026-05-13a.md  newly committed + scrubbed (PAT->partial)
docs/handoffs/SESSION_HANDOFF_2026-05-13b.md  newly committed (was untracked)
docs/handoffs/SESSION_HANDOFF_2026-05-13c.md  newly committed (was untracked)
docs/handoffs/SESSION_HANDOFF_2026-05-13d.md  newly committed (was untracked)
```

Removed in `70aa540` (untracked staging files from parts H/I/K):
`VWAPReversionBacktest.cpp` (root copy),
`backtest/VWAPReversionBacktest.cpp.pre-p4`, `apply_p4_precision_fix.sh`,
`run_p1_us500_sweep.sh`, `run_p1b_eurusd_sweep.sh`,
`run_p2_ustec_widen.sh`, `run_p3_ustec_gap_filter.sh`.

## Priorities for next session

### Priority 1 — GBPUSD trade investigation (NEW)

User flagged this trade at session end as "must be fixed":

```
07:02:41   GBPUSD   LONG   1.35   1.35   7m41s   ✗SL   GbpusdLondonOpenSL   -$15.30   -$1.30   -$17.20
```

Context gathered before stop:

- Engine source: `include/GbpusdLondonOpenEngine.hpp` (1024 lines).
  Session window 07:00-10:00 UTC, MIN_RANGE=12 pips, MAX_RANGE=75 pips,
  SL_FRAC=0.80, RR=2.0, BE-lock at 6 pips MFE, same-level block 20
  min post-SL / 10 min post-TP.
- Trade fits the engine's design risk envelope (`RISK_DOLLARS=$30`,
  loss `$17.20` is well inside the budget). No obvious code bug
  produced this specific trade; it's a strategy doing what it was
  designed to do and losing on a particular compression.
- The trade was generated by the **pre-deploy binary** (`0e95ecd`),
  not by today's HEAD. Today's deploy at 07:33 means the engine is
  now running on `17ccb45` -- but the change between `0e95ecd` and
  `17ccb45` is the 19-engine winner-exemption rollout, which
  GbpusdLondonOpenEngine was NOT part of (the rollout touched
  Bracket/Breakout/EMACross/etc., not the FX London-Open trio).
  So the same engine code that produced this loss is still live.

**Logs not yet pulled.** The investigation stopped at the point of
fetching the trade's state-change lifecycle. The next session should
start by running this on the VPS:

```powershell
Select-String -Path C:\Omega\logs\omega_2026-05-13.log -Pattern 'GBP-LDN-OPEN' |
    Where-Object { $_.Line -match 'ARMED|FIRE|FILL|EXIT|COST_FAIL|ATR_GATE|COLD_START|DOM_BLOCK|FILL_SPREAD|PENDING TIMEOUT|SANITY' } |
    ForEach-Object { $_.Line }
```

That returns the ARMED → FIRE → FILL → EXIT lines for every GBPUSD
trade today, including the 07:02:41 one. Specifically pull: the
`range`, `sl_dist`, `spread` at entry, the MFE the trade reached, and
whether the engine emitted any DIAG warnings around that timestamp.

Possible fix angles (decide AFTER reading the log lines, not before):

- **Tighten entry** — raise MIN_RANGE, add a slope filter, require
  more pre-breakout consolidation (longer MIN_BREAK_TICKS).
- **Widen SL** — SL_FRAC=0.80 → 0.95 or 1.00; or switch to ATR-based.
- **Disable engine to shadow** — flip `shadow_mode = true` in
  engine_init.hpp until the strategy is re-validated. Lowest-risk
  immediate response.
- **Narrow session window** — currently 07:00-10:00 UTC; if loss-heavy
  hour is one of the three, narrow.
- **Same-level block already exists** (SAME_LEVEL_POST_SL_BLOCK_S=1200,
  20 min); so the engine should not have re-fired in the same area
  within 20 min of an earlier SL -- worth verifying it didn't.

User's reaction was brisk ("this bad trade must be fixed"); read that
as "investigate and propose a real fix", not "panic-disable".

### Priority 2 — Global CLAUDE.md security-cleanup

Replace the four lines that contain the dead PAT and HTTPS remote
with the SSH-documented version. See "Security migration" section
above. Find the file with:

```bash
find ~ -name CLAUDE.md -not -path "*/node_modules/*" 2>/dev/null
```

Then edit. ~5 minutes.

### Priority 3 — VPS verifier PAT update

`C:\Omega\VERIFY_STARTUP.ps1` GitHub API call fails 401 (dead PAT).
Update the verifier's token (or switch to `gh` CLI auth if `gh` is
available on the VPS). Cosmetic — doesn't block trading.

### Priorities carried from part L (unchanged)

- **P1** — VWAPReversionBacktest `#include engine_init.hpp` refactor.
- **P3** — USTEC.F parameter retune (EXTENSION_THRESH_PCT,
  MAX_HOLD_SEC, session window) to move baseline from
  `-0.00147/trade` toward viability.
- **P4** — Smoke-test verdict-logic refinement (GER40 false-positive
  REVIEW; max-pos=10 hardcoded "skipped" verdict).
- **P5** — EURUSD live cost analysis (baseline `+0.000854` gross over
  3892 trades; barely above realistic spread + commission).
- **P6** — Done this session (`70aa540`).
- **P7** — Tick-clock weekend-gap fix in `OmegaTimeShim`.

## Bookkeeping

- **HEAD on origin/main:** `17ccb45` (build fix).
- **HEAD on VPS:** `17ccb45` (deployed at 07:33 UTC).
- **Working tree:** clean.
- **Open branches with edits:** none unique to this session (all on
  `main`).
- **Sandbox bash:** not used this session (operator-side workflow
  throughout, as in part L).
- **VPS log files of interest** (for the GBPUSD investigation):
  - `C:\Omega\logs\latest.log` (post-deploy, from 07:33:45 onward)
  - `C:\Omega\logs\omega_2026-05-13.log` (the full day, includes the
    07:02 trade)
  - `C:\Omega\logs\omega_service_stdout.log` (stdout mirror)

## Validation actions next session

```bash
cd ~/omega_repo

# Preflight (the mandatory gate per PREFLIGHT.md).
bash .claude-preflight.sh
echo "preflight_exit=$?"

# Confirm working tree state.
git log --oneline -10
git status --short
# Expect: HEAD = 17ccb45 or this handoff's commit if it landed,
# working tree empty (or just this handoff if not yet committed).

# Confirm VPS binary matches.
# (Operator-side, Windows PowerShell.)
cd C:\Omega
git log -1 --pretty=%H
# Should equal Mac HEAD.

# If matches, begin Priority 1 -- pull the GBP-LDN-OPEN state lines
# from the log per the command above.
```

## Quick-reference files

| file                                          | purpose                                    | state           |
|-----------------------------------------------|--------------------------------------------|-----------------|
| `include/CellEngine.hpp`                      | V2 base; MAE_EXIT + S12 + IFG live         | committed       |
| `include/TsmomStrategy.hpp`                   | V2 Tsmom; MAE topology configured          | committed       |
| `include/HTFSwingEngines.hpp`                 | build fix landed                           | committed       |
| `include/MacroCrashEngine.hpp`                | build fix landed                           | committed       |
| `include/BracketEngine.hpp`                   | build fix landed                           | committed       |
| `include/GbpusdLondonOpenEngine.hpp`          | **Priority 1 investigation target**         | unchanged       |
| `docs/handoffs/SESSION_HANDOFF_2026-05-13e.md`| part L (reference)                         | committed       |
| `docs/handoffs/SESSION_HANDOFF_2026-05-13f.md`| this document — part M                     | current         |
| `smoke_test_part_L.sh`                        | parity validation harness                  | committed       |
