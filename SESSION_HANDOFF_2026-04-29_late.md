# Omega Trading System — Session Handoff (Late)
## 2026-04-29 — C1_retuned switched on in C++ live shadow

**This file lives at `/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_late.md`.**
It supersedes `SESSION_HANDOFF_2026-04-29_night.md` (which has the
context that led into this session). Read this file first.

---

## TL;DR for next session

C1_retuned (System B) is **deployed in C++ shadow mode in the live runtime**.
It compiled clean, the service started, and the build is running on commit
`d506b83` as of 2026-04-29 09:19:54 UTC. From this point forward every H1 /
H4 close on XAUUSD ticks the new engine and any trades it would take are
written to `logs/shadow/omega_shadow.csv` tagged with `engine=C1Retuned_*`
and `shadow=true`.

Two pieces are now live in parallel:

1. **Python shadow runner** (`phase2/c1_retuned_shadow.py`) — offline,
   parquet-driven, idempotent. Source of truth for the offline backtest
   against refreshed bars.
2. **C++ live shadow engine** (`include/C1RetunedPortfolio.hpp`) — real
   tick stream from cTrader, paper-only orders, runs alongside the
   existing engines in `Omega.exe`. **This is the canonical "switch
   on".**

Both write to compatible CSV formats so `scripts/shadow_analysis.py`
picks up the C++ trades unchanged.

The next-session agenda is in §"Next session — concrete first step" at
the bottom.

---

## Repo state at end of session

- HEAD: `d506b83` "C1_retuned: add as live-shadow C++ engine alongside
  MinimalH4Breakout"
- Previous commit: `c076a4a` "C1_retuned: switch on live shadow
  paper-trading" (Python runner + verdict trail)
- Both pushed to `origin/main` on `Trendiisales/Omega`. Working tree
  clean on both Mac + Windows runtime box.
- Branch: `main`

### Commits this session (in order)

| sha       | summary                                                      |
|-----------|--------------------------------------------------------------|
| c076a4a   | Python C1_retuned shadow + halt monitor + verdict trail      |
| d506b83   | C1_retuned C++ engine + wiring into runtime + config block   |

---

## What landed this session

### 1. Python C1_retuned shadow runner (commit `c076a4a`)

`phase2/c1_retuned_shadow.py` — single-file, idempotent, cron-friendly.
Reuses `phase2/portfolio_C1_C2.py.simulate()` unchanged so the shadow
ledger is byte-comparable to backtest. Filters trades to entry_time >=
SHADOW_START. Writes `phase2/live_shadow/c1_retuned_shadow.csv` and
per-run summaries.

`phase2/c1_retuned_halt_monitor.py` — read-only audit. Drawdown,
cluster days, cell concentration, daily distribution, silent cells.
Cron-friendly with `--exit-on-breach`.

`phase2/C1_RETUNED_SHADOW.md` — operational README.

This commit also brought in the supporting verdict trail (CHOSEN.md,
sweep_results.json, walkforward outputs, the four cell ledgers, the
build script for the retuned ledger, sim_lib.py, etc.) so the repo
contains everything needed to reproduce C1_retuned from source.

### 2. C++ live shadow engine (commit `d506b83`)

`include/C1RetunedPortfolio.hpp` — 790 lines, single header,
self-contained. Four cells:

| cell                          | TF | bars source              | logic                                |
|-------------------------------|----|--------------------------|--------------------------------------|
| `C1Retuned_donchian_H1_long`  | H1 | `g_bars_gold.h1`         | period=20, sl=3.0 ATR, tp=5.0 ATR    |
| `C1Retuned_bollinger_H2_long` | H2 | synthesised from H1 ×2   | touch lower BB, exit on midline      |
| `C1Retuned_bollinger_H4_long` | H4 | `g_bars_gold.h4`         | touch lower BB, exit on midline      |
| `C1Retuned_bollinger_H6_long` | H6 | synthesised from H1 ×6   | touch lower BB, exit on midline      |

Portfolio: `max_concurrent=4`, `risk_pct=0.005`, `start_equity=10000`,
`margin_call=1000`, `max_lot_cap=0.05` (tighter than backtest 0.10
while shadow-validating).

Wiring (4 surgical adds, no logic changes elsewhere):

- `globals.hpp`: `static omega::C1RetunedPortfolio g_c1_retuned;`
- `engine_init.hpp`: init block inside the gold-init conditional, right
  after `MinimalH4Breakout` init.
- `tick_gold.hpp`:
  - H1 close handler → `g_c1_retuned.on_h1_bar(...)` (drives Donchian
    H1 + synth H2 + synth H6)
  - H4 close handler → `g_c1_retuned.on_h4_bar(...)` (drives Bollinger
    H4)
  - on_tick → `g_c1_retuned.on_tick(...)` alongside
    `g_minimal_h4_gold.on_tick`
- `omega_config.ini`: new `[c1_retuned]` section, hot-reloadable.

H2 and H6 bars **do not exist in `g_bars_gold`** — the engine
synthesises them internally from completed H1 bars (stride 2 / stride 6)
and computes its own Bollinger bands + Wilder ATR14 over the
synthesised series. This kept the integration narrow: the runtime only
has to feed H1-close and H4-close events.

Compile-verified standalone with `g++ -O2 -std=c++17 -Wall -Wextra
-Wno-unused-parameter` clean (zero warnings) plus a runtime smoke test
that exercised init / on_h1_bar / on_h4_bar / on_tick / halt_status.

### 3. Build picked up (Windows runtime)

User pulled `d506b83` on the Windows runtime, ran `QUICK_RESTART.ps1`,
build succeeded in 214s, 4 .cpp files compiled, `Omega.exe` linked,
service started, EXE timestamp matches binary, git hash `d506b83`
confirmed in log. Mode: **SHADOW**. GUI:
`http://185.167.119.59:7779`.

`VERIFY_STARTUP.ps1` reported 2 warnings (VIX no tick yet, RSI Reversal
in shadow) — both pre-existing and unrelated to C1Retuned. No FAILs.
Bar state loaded from disk warm: `m1=1 m5=1 m15=1 h4=1 sp=1 nq=1
m1_ready=1`. ATR / EMA / RSI valid on disk.

The C1Retuned engine is now ticking live in the shadow runtime. First
trades will appear in `logs/shadow/omega_shadow.csv` as cells fire. At
backtest cadence the H1 cell fires ~1 trade every 30 H1 bars on
average; the Bollinger cells fire 1–3 trades per week each. Expect
first activity within hours / first day of London or NY hours.

---

## Halt criteria (locked in `omega_config.ini`)

| check                | threshold                                          |
|----------------------|----------------------------------------------------|
| max DD               | <= -7.5% (1.5x backtest -5.85%)                    |
| cluster days         | >= 4 cells losing same UTC session                 |
| cluster rate (proxy) | >= 1 cluster day in shadow window                  |

The portfolio surfaces `halt_status()` which returns `{ok, equity,
peak, max_dd_pct, cluster_days, open_count, blocked_max_concurrent,
reason}`. **It does NOT auto-disable cells** — a human decides what to
do with the flag. Same philosophy as `MinimalH4Breakout`.

The Python halt monitor (`phase2/c1_retuned_halt_monitor.py`) does
deeper analysis on the offline shadow ledger (cell concentration,
daily distribution, silent cells, full cluster table). Run it twice
daily as a periodic audit.

---

## Open decisions (pending Jo's input)

1. **GUI surface for `g_c1_retuned.halt_status()`.** Right now the only
   way to read live halt state is to grep the stdout log for
   `[C1RETUNED-CLOSE]` lines. A dashboard tile (mirror the
   `minimal_h4_*` GUI panels) would surface equity / DD / cluster days
   / open count at a glance. ~30 lines in
   `src/gui/` + telemetry. Not wired this session — Jo's call when /
   if to add.

2. **`max_lot_cap` relaxation timing.** Currently 0.05 (half backtest
   0.10) until first 100 closed shadow trades match WR/PF distribution
   of the backtest. Defined in code (`engine_init.hpp` line ~427), not
   ini-driven. Trivial to flip when ready.

3. **2026-03-18 cluster post-mortem.** Still pending from the night
   handoff. The data is in `cluster_postmortem_2026_03_18_v2.py`
   (untracked). CHOSEN.md flagged this as required-in-parallel before
   live capital. Doing it earlier than later means we're not racing
   the post-mortem against accumulating shadow data.

4. **System A (HBG/CFE/MCE) suspension.** The night handoff
   recommended NOT committing fresh capital to those engines. They are
   still running in the live build. SpreadRegimeGate v2 was NOT
   committed this session (still uncommitted). Two options:
     a. Commit + push v2, leave HBG/CFE/MCE running with the gate as
        damage limitation.
     b. Set `enabled=false` on HBG/CFE/MCE (or remove their dispatch
        sites), let C1_retuned be the only Gold engine in shadow.
   Decision left to next session.

5. **Bar-refresh wiring for the Python shadow runner.** The C++ engine
   is now self-driven from the live tick stream, so bar refresh isn't
   blocking. But the Python `phase2/c1_retuned_shadow.py` still needs
   a refresh pipeline if you want it to accumulate trades alongside.
   Probably not worth wiring — the C++ engine is now the canonical
   shadow source.

---

## Known gotchas & ops notes

### Stale `.git/index.lock` / `.git/HEAD.lock`

Multiple times this session a stale lock file blocked git operations
on the Mac. The fix:

```
cd /Users/jo/omega_repo
find .git -name '*.lock' -print -delete
```

If you hit "untracked working tree files would be overwritten by
merge", the right move is:

```
git stash push -u -m "pre-pull-stash"
git pull origin main
git stash drop
```

If both compound (stash drops while pull failed mid-fast-forward):

```
find .git -name '*.lock' -delete
git fetch origin main
git reset --hard origin/main
```

### Bindfs sandbox limitation

The Cowork sandbox cannot `unlink` files on the bindfs-mounted host
folder. Git operations from the sandbox path therefore can't clean
their own lock files or rewrite indexes. **Workaround used this
session:** clone fresh into `/dev/shm/omega_clone` (tmpfs), copy
modified files in, commit + push from there using the PAT at
`.github_token`. The sandbox CAN read and write to the host folder
normally — it just can't unlink, so Edit / Write / Read tools work
fine for content changes but git operations need the tmpfs detour.

### `omega_config.ini` is gitignored at root

The file is ignored by `.gitignore` (line `omega_config.ini`) AND
listed under "Runtime-written files." But **THIS SESSION'S
`d506b83` commit DID push the file** because it was never previously
tracked and `git add omega_config.ini` was issued explicitly from the
sandbox clone. Going forward the file may diverge between the Mac
working copy and what's on GitHub. The runtime reads the local copy;
GitHub's copy is canonical for new clones only.

If config diverges, the canonical source of `[c1_retuned]` defaults is
the constant initialisers in `C1RetunedPortfolio.hpp` itself — the ini
section is for hot-reload tuning only.

### EXE warm restart

`QUICK_RESTART.ps1` wipes `.obj/.pch` but preserves `CMakeCache`. After
this the bar-state files load warm from disk
(`bars_gold_m1/m5/m15/h1/h4.dat`), so the C1Retuned engine starts with
**`g_bars_gold.h1.ind.atr14`** already populated. The Donchian H1 cell
needs 20 H1 closes before its channel is "ready" — that's 20 hours from
cold start, near-instant from warm. The synthesised H2/H6 indicators
need 20 of their respective bars (40 H1 closes for H2, 120 H1 closes
for H6 — so H6 effectively warms up over ~5 days of live data).

---

## Next session — concrete first step

**Step 1.** Pull the log and confirm `[C1RETUNED-INIT]` printed:

```
ssh / RDP to the runtime box
grep "\[C1RETUNED" C:\Omega\logs\latest.log | head -10
```

Expected:

```
[C1RETUNED-INIT] portfolio shadow=true max_conc=4 risk_pct=0.0050 ...
```

If absent, either the build cache lied (re-run `QUICK_RESTART.ps1`)
or the engine_init.hpp dispatch never ran (check the gold-init
conditional gate).

**Step 2.** Wait for first H1 close after market open and verify
on_h1_bar dispatch is actually firing. Look for:

```
[C1RETUNED-DONCHIAN_H1] ENTRY LONG @ ...
```

(if a signal fired) or check that 20+ H1 closes have happened
(`grep "g_bars_gold.h1.add_bar" logs/latest.log` — won't appear in
print form; instead grep for `[C1RETUNED-CLOSE]` to confirm cells
emit close records).

**Step 3.** Once first 5–10 closed shadow trades have accumulated,
spot-check them against the backtest cadence:

- WR roughly 50–55% (backtest 55.2%)
- avg trade $ value not absurd (backtest $48 win / -$24 loss in $10k
  account at 0.5% risk)
- mfe / mae populated (these come from on_tick path)
- exit reason distribution: TP_HIT / SL_HIT / INDICATOR / TIMEOUT
  (Donchian uses TP_HIT/SL_HIT/TIMEOUT; Bollinger uses INDICATOR/SL_HIT/TIMEOUT)

**Step 4.** Pick one of the open decisions above (GUI surface,
`max_lot_cap` plan, cluster post-mortem, System A suspension). My
recommendation order:

1. Cluster post-mortem first (it's the only known hidden risk).
2. System A suspension second (every day they bleed is real cost).
3. GUI surface third (nice-to-have).
4. `max_lot_cap` last (gated on accumulating shadow trade count).

### Next-session opener line for Jo to paste

> **State for next session — C1_retuned live shadow monitoring + cleanup tracks.**
>
> SHIPPED 2026-04-29: C1_retuned (System B) deployed in C++ live shadow
> in `Omega.exe`. Build at `d506b83`, pushed to GitHub. Engine wired
> alongside MinimalH4Breakout. Four cells (Donchian H1 retuned +
> Bollinger H2/H4/H6 long), max_concurrent=4, 0.5% risk, shadow_mode=true.
> Verdict baseline: +74.12% / -5.85% DD / Sharpe 2.651 backtest. See
> `phase2/donchian_postregime/CHOSEN.md` and
> `SESSION_HANDOFF_2026-04-29_late.md`.
>
> First action: pull log on runtime box and confirm `[C1RETUNED-INIT]`
> printed plus first H1-close fires once market activity resumes.
>
> Open tracks (pick one): (a) 2026-03-18 cluster post-mortem,
> (b) System A (HBG/CFE/MCE) suspension or commit
> SpreadRegimeGate v2 as damage layer, (c) GUI tile for
> `g_c1_retuned.halt_status()`, (d) max_lot_cap relaxation gating.
>
> Read `SESSION_HANDOFF_2026-04-29_late.md` first.

---

## User preferences (carry forward)

- Always provide full code files, not snippets / diffs.
- Warn at 70% chat context with summary. **This handoff written at
  ~80–85% context after the C++ engine landed; next session starts
  fresh.**
- Warn before time/session blocks.
- Never modify core code without explicit instruction. The C1Retuned
  C++ wiring (4 surgical adds in globals.hpp / engine_init.hpp /
  tick_gold.hpp / omega_config.ini) was done with explicit instruction
  ("i want this added in c to my exisiting shadow trading system").
- Use the PAT without arguments when committing — stored at
  `/Users/jo/omega_repo/.github_token`. Do NOT mention rotation.
- Email: kiwi18@gmail.com
- Name: jo

---

## Where to find this doc on session start

```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_late.md
```

Predecessor (still useful for the night-session context):

```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_night.md
```
