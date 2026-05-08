# HANDOFF — S18 (continues S17 P1-2 / P1-3 / P1-8 / VWAP re-enable)

Prepared: 2026-05-08, end of S17
Branch / HEAD at handoff: `main` @ `06b989d` **plus four working-tree changes** (S17's edits to `CrossAssetEngines.hpp`, `config.hpp`, `quote_loop.hpp`, `engine_init.hpp`) that the user is committing/pushing locally as I write this. Confirm with `git log --oneline -3` after their commit lands. The first commit on top of `06b989d` is the S17 omnibus.
Mode: `mode=SHADOW` (no live orders firing). Re-enabled VWAPReversion engines are paper-only until LIVE flip.
Standing user preferences carried over: full code (no snippets/diffs), warn at 70% chat with summary, never modify core code without clear instruction, paste-friendly bash commands (no leading `#` comments — zsh errors on those), warn before any session-usage block.

---

## TL;DR for the fresh Claude reading this

S17 closed P1-2 (label fidelity for `CrossPosition::force_close`), closed P1-2-a (the missing-supervisor hunt — there is no missing supervisor; rows 15/16 in the historical tape are explained by the SIGINT/SIGTERM signal handler at `omega_main.hpp:36`, which runs in SHADOW too), fixed P1-3 (FxCascade cooldown moved from entry to close), fixed P1-8 (EsNqDivergence confirm-logic rewrite — old version had two compounding bugs that made the engine effectively unfireable), and re-enabled all 4 `g_vwap_rev_*` instances. All in one commit, ready for `.\OMEGA.ps1 deploy`.

Your job for S18 is the user's pick from a short menu (see "Outstanding decisions" below). Two are post-deploy data tasks (P1-1 close-out, MAE_EXIT_RATIO retune) that need a fresh tape before they're actionable. The rest are static-analysis-friendly P1 backlog items (P1-4 through P1-12). Don't dive in — ask first.

**Fastest way to come up to speed — read these in this order:**

1. This file. Self-contained.
2. [`HANDOFF_S17_AFTER_S16_P1_TRACES.md`](computer:///Users/jo/omega_repo/HANDOFF_S17_AFTER_S16_P1_TRACES.md) — prior handoff. Backlog state and S17 pre-state.
3. [`AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md) — P1-2 trace. P1-2-a section is now stale (closed in S17 — see Step 4 below for the corrected reading).
4. [`AUDIT_S16_P1_1_LEDGER_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_1_LEDGER_TRACE.md) — P1-1 trace. Awaiting fresh tape.
5. [`AUDIT_S15_ENGINES.md`](computer:///Users/jo/omega_repo/AUDIT_S15_ENGINES.md) — full engine audit. Don't read end-to-end; Grep for what you need.
6. [`docs/SESSION_2026-05-03_ENGINE_AUDIT_INSTALLMENT_2.md`](computer:///Users/jo/omega_repo/docs/SESSION_2026-05-03_ENGINE_AUDIT_INSTALLMENT_2.md) — H1 explains why VWAPReversion was disabled (conservative pre-deploy gating, not a quality cull) and confirms the engine was healthy and well-tuned at the time of the disable.

Don't re-read engine source files unless you're picking up a P1 item that demands it.

---

## STEP 1 — Get repo access

The sandbox does NOT have a folder mounted by default. First action:

```
mcp__cowork__request_cowork_directory(path="~/omega_repo")
```

Mount path for host-side tools (Read / Write / Edit / Glob / Grep): `/Users/jo/omega_repo/`. The bash sandbox path differs (`/sessions/<id>/mnt/omega_repo/`) but bash is currently broken — see Step 2.

---

## STEP 2 — Sandbox + repo + Mac environment

### Sandbox VM

`mcp__workspace__bash` is **still broken** as of end-S17. Same symptom as S13/S15/S16:

```
bash failed on resume, create, and re-resume.
useradd: /etc/passwd.NNNNNN: No space left on device
```

S17 did not retest mid-session. Implications unchanged from the S17 handoff: cannot run git, python, awk, or compile from the sandbox. All git operations happen on the user's Mac. CSV inspection uses chunked `Read` (500-1000 lines per call) or delegates to a `general-purpose` Agent. Build verification is `.\OMEGA.ps1 deploy` on the VPS.

### Mac local environment

```
host:    jo@Jos-MacBook-Pro
shell:   zsh   (NOT bash — `#` comment lines in pasted scripts cause errors)
repo:    ~/omega_repo
```

When pasting multi-line shell scripts to the user, **strip leading `#` comments** before sending. Zsh interprets each as a command and emits `zsh: command not found: #` for every one.

### GitHub remote

```
URL:     https://github.com/Trendiisales/Omega
branch:  main
```

There is a PAT in the global `CLAUDE.md` (`ghp_...`). It's there for **read context only**. Do NOT use it to push. The user pushes manually with their own credentials.

### Build / deploy

Build cycle is on the VPS. The user runs `.\OMEGA.ps1 deploy` (PowerShell on the VPS) after each push.

### Trading mode

`mode=SHADOW` per `omega_config.ini`. No live orders. The S17 changes are all paper-safe in SHADOW. The VWAPReversion re-enable produces shadow trades only until the LIVE flip.

---

## STEP 3 — Verify what S17 left in the tree

### Already in `06b989d` (S17 starting commit)

S16 P1-2 trace + S17 handoff. Confirm with `Read /Users/jo/omega_repo/.git/refs/heads/main`.

### S17 working-tree edits (one omnibus commit at end of S17)

| File                              | What                                                      |
| --------------------------------- | --------------------------------------------------------- |
| `include/CrossAssetEngines.hpp`   | P1-2 label fidelity (14 sites) + P1-3 FxCascade cooldown (L777-825) + P1-8 EsNqDiv rewrite (L350-440) + private field rename `bool confirm_is_long_` → `int confirm_dir_` |
| `include/config.hpp`              | P1-2 midnight labels at L179, L230, L242                  |
| `include/quote_loop.hpp`          | P1-2 stale labels (5 sites L605-680), shutdown labels (21 sites L802-818 + 22 sites L901-917), reconnect labels (17 sites L1047-1081) |
| `include/engine_init.hpp`         | VWAP re-enable (4 instances: L447, 450, 453, 457 flipped to `enabled = true`) |

The omnibus commit message is in the S17 final summary. If the user used the script verbatim it covers all four files.

### Untracked files left intentionally out of git (from S16 — still untracked at end-S17)

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/
```

Decision still deferred to the user. S17 did not touch these.

### S17-generated docs (this file is the only one)

`HANDOFF_S18_AFTER_S17_FIXES.md` — pending commit. Bundle with the S17 omnibus or commit separately, your call.

---

## STEP 4 — What S17 deliberately did NOT do

Don't redo any of these — they're handled or out of scope:

- **Update `AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`.** The doc has a "What needs follow-up (P1-2-a)" section that is now stale. The user deferred this update. If you want to update it, here's what's now known:
  - P1-2-a closed: project-wide grep across `include/` confirms no missing supervisor. The 5 sources the trace named are exhaustive for `g_vwap_rev_*.force_close` callers.
  - Mystery rows 6/7/10 (profitable LONGs, intraday, in SHADOW) are explained by **engine MAE early-exit at `CrossAssetEngines.hpp:1252`**, not by hidden supervisors. The trace's claim "MAE doesn't fire on profit" was wrong — `adverse` is current adverse from entry, and a position can be MFE-positive then turn slightly adverse before closing on a small absolute profit.
  - Mystery rows 15/16 (7.78/7.79h holds, in SHADOW) are explained by the **SIGINT/SIGTERM/SetConsoleCtrlHandler graceful-shutdown path at `omega_main.hpp:36-38`**, which runs even in SHADOW. The trace's "LIVE-only" framing applied only to the `quote_loop` reconnect path at L691-697, not to the signal handler. Date of those trades: **2026-03-27 entries ~11:45 UTC, exits ~19:32 UTC** — same UTC day, mid-NY-session, before midnight. The user confirmed this corresponds to a software-update restart on their end.

- **Re-deploy.** S17 only edited source. The user runs `.\OMEGA.ps1 deploy` on the VPS after pushing the omnibus commit.

- **Validate VWAPReversion settings against current data.** S17 only flipped `enabled = false` → `enabled = true`. Settings (`EXTENSION_THRESH_PCT`, `MAX_EXTENSION_PCT`, `MAX_HOLD_SEC`, `COOLDOWN_SEC`) are unchanged from the May-3 audit values. EURUSD instance is missing explicit `MAX_EXTENSION_PCT` and `MAX_HOLD_SEC` lines (falls back to class defaults) — worth a future tune if EURUSD VWAP-rev shows poor performance, but not a blocker.

- **Touch the older S14 / S15 handoff files.** Left untracked per Step 3.

- **Reconcile the S15 + S17_AUDIT.md catalogs.** Still deferred.

- **Process Tier-1 vs backtest WR comparison.** Still requires a fresh post-Tier-1 tape (>=200 closed trades since 2026-04-29 ship). The historical tape ends 2026-04-08 and is entirely pre-Tier-1.

- **Apply any P1-4 through P1-12 fix.** All open. Listed in Step 6 below.

- **`HANDOFF_S17_AFTER_S16_P1_TRACES.md`.** Stale on items 1-2 of its "Outstanding decisions" list (P1-1 deploy status, P1-2 fix go-ahead). Both are now resolved. Don't update unless the user asks.

---

## STEP 5 — Outstanding decisions to ask the user (in priority order)

Use `AskUserQuestion` for the first one if multiple options apply. Don't start work without explicit pick.

1. **P1-1 verification close-out.** Has `.\OMEGA.ps1 deploy` been run on the VPS since the S17 omnibus commit? If yes, run `grep "[LEDGER-CORRUPT-TS]"` against the binary's stderr/stdout log. If empty after >=100 closed shadow trades or >=1 week, P1-1 closes resolved. If any line appears, the engine routing table at the end of `AUDIT_S16_P1_1_LEDGER_TRACE.md` maps `engine=` to a file:line.

2. **MAE_EXIT_RATIO retune cycle.** After 2-4 weeks of fresh post-deploy tape (with VWAPReversion now firing and label fidelity in place), run the cohort analysis:
   - Filter shadow CSV rows where `engine == "VWAPReversion"` AND `exit_reason == "MAE_EARLY_EXIT"`.
   - For each row, look at `mfe` column. If `mfe > 0` significantly, that's a candidate "we cut a winner" event.
   - Compute `cut-winners / total-MAE-trades`. If > 30%, MAE_EXIT_RATIO is too tight; if < 10%, it's about right; if = 0, you're missing the signal entirely.
   - Same analysis for `MIDNIGHT_ROLLOVER`, `STALE_PRIOR_DAY`, `RECONNECT_CLOSE` cohorts.
   - `MAE_EXIT_RATIO` is a member constant in `CrossAssetEngines.hpp` — grep for it when you want to retune.

3. **P1 backlog: P1-4 onward.** Items P1-4 through P1-12 are untouched. Recommended next: **P1-4 (`IndexFlow` 29.4% WR vs 35% threshold)** if the user has fresh post-Tier-1 data, otherwise **P1-5 (`TrendPullback` consec-SL knobs to INI)** as a static-analysis-friendly hop.

4. **VWAPReversion EURUSD tune.** Currently `g_vwap_rev_eurusd` only has `EXTENSION_THRESH_PCT = 0.12` and `COOLDOWN_SEC = 120` set explicitly — `MAX_EXTENSION_PCT` and `MAX_HOLD_SEC` fall back to class defaults (1.20 and 600 seconds respectively, per the SP/NQ examples). For an FX pair vs an index, those defaults may be too loose. Consider asking the user whether to set explicit values once shadow data accumulates.

5. **Documentation cleanup.** Update `AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md` with the closure note + SIGINT shutdown finding. Update `ENGINE_AUDIT_CHECKLIST.md` to reflect VWAPReversion as live-shadow rather than dormant. Both deferred.

---

## STEP 6 — P1 backlog state (carry-forward + S17 closures)

Status as of end-S17:

| Item   | Status                  | Notes                                                       |
| ------ | ----------------------- | ----------------------------------------------------------- |
| P1-1   | Static trace done       | Awaiting post-deploy tape. See `AUDIT_S16_P1_1_LEDGER_TRACE.md`. |
| P1-2   | **CLOSED in S17**       | Label fidelity fix in `CrossAssetEngines.hpp` + `config.hpp` + `quote_loop.hpp`. 70+ call sites updated. |
| P1-2-a | **CLOSED in S17**       | No missing supervisor. Rows 6/7/10 = MAE early-exit. Rows 15/16 = SIGINT/Console-Ctrl shutdown handler at `omega_main.hpp:36`. |
| P1-3   | **CLOSED in S17**       | FxCascade cooldown moved from entry to close. `CrossAssetEngines.hpp:777-825`. |
| P1-4   | Open                    | `IndexFlow` 29.4% WR vs 35% threshold. Replay-required.     |
| P1-5   | Open                    | `TrendPullback` consec-SL knobs to INI.                     |
| P1-6   | Open                    | `TrendPullback` index instances missing `m5_trend_state_` wiring. |
| P1-7   | Open                    | `BracketEngine CONFIRM_SECS` formalisation per fill latency. |
| P1-8   | **CLOSED in S17**       | EsNqDivergence confirm-logic rewrite. Engine remains `enabled=false` at `engine_init.hpp:1585` — fix is prep-work for future enablement. `CrossAssetEngines.hpp:350-440`. |
| P1-9   | Open                    | `IndexSwingEngine` TP% asymmetry comprehension.             |
| P1-10  | Open                    | `SweepProEngine` naming/comment doc.                        |
| P1-11  | Open                    | `MinimalH4Breakout` (gold) cold-start CSV warm-load.        |
| P1-12  | Open                    | `C1RetunedPortfolio` cluster-day boundary UTC vs session.   |

P2 backlog (~25 items) untouched. Listed in S16 handoff Step 7.

---

## STEP 7 — Files state at end of S17

### Modified, working tree at end of S17 (one omnibus commit pending or just-pushed)

```
M  include/CrossAssetEngines.hpp     (P1-2 + P1-3 + P1-8)
M  include/config.hpp                (P1-2)
M  include/engine_init.hpp           (VWAP re-enable)
M  include/quote_loop.hpp            (P1-2)
A  HANDOFF_S18_AFTER_S17_FIXES.md    (this file)
```

### Untracked, deliberately not staged (carry-over)

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/                                (CSV + zip + python helper)
```

The audit reports + handoffs all live at repo root by convention.

---

## STEP 8 — Repo / git recon shortcuts (bash-free) — carry-over from S17

| What           | How                                                      |
| -------------- | -------------------------------------------------------- |
| Current branch | `Read /Users/jo/omega_repo/.git/HEAD`                    |
| HEAD SHA       | `Read /Users/jo/omega_repo/.git/refs/heads/main`         |
| Recent commits | `Read /Users/jo/omega_repo/.git/logs/HEAD` (large; offset to tail) |
| Untracked?     | Ask the user to paste `git status --short`.              |
| Diff a file    | `Read` the working-tree file and compare to memory of the commit anchors. |

For tape inspection (`audit_input/omega_shadow.csv`):

| What                              | How                                                   |
| --------------------------------- | ----------------------------------------------------- |
| Header                            | `Read offset=1 limit=2`                               |
| Engine rows                       | `Grep ",ENGINE_NAME,"` — anchor commas to avoid partial-name hits |
| Counts                            | `Grep -c` (count mode)                                |
| Full row reads                    | Chunked `Read offset=N limit=500`, accumulate         |
| Stats across thousands of rows    | Spawn a `general-purpose` Agent with the chunked-Read pattern |

CSV columns:

```
ts_unix, symbol, side, engine, entry_px, exit_px, pnl, mfe, mae,
hold_sec, exit_reason, spread_at_entry, latency_ms, regime
```

`ts_unix` is the entry timestamp, NOT exit. `pnl` is post-cost. `hold_sec = exit_ts - entry_ts` in seconds.

Date-from-ts_unix conversion (no bash needed):
- `2026-01-01 00:00:00 UTC = 1767225600`
- `(ts - 1767225600) / 86400 = days into 2026`
- Jan = 31 days, Feb 2026 (not leap) = 28 days, etc.

---

## STEP 9 — Verification checklist before claiming any P1 work is done

Mirror what S15, S16, S17 did:

1. Read each file you modified end-to-end after the edit. Don't trust your own diff.
2. Confirm anchor strings around your changes are unique and intact.
3. For any new field, grep for callers and confirm they compile against the new shape.
4. For any new include, confirm it's already present (header-only project).
5. For any change touching a code path that produces `TradeRecord`, hand-verify against existing `TradeRecord` assignments (entryTs, exitTs, engine label, side, exitReason).
6. Per the standing user pref, leave changes uncommitted unless the user explicitly says to commit.
7. When delivering shell commands, strip `#` comment lines (zsh, not bash).

Specific to S17's changes:
- Project-wide grep for `force_close` calls confirmed all CrossPosition wrappers updated. No 3-arg `pos_.force_close(bid, ask, on_close)` remain inside CrossAssetEngines.hpp; legacy 4-arg `g_gold_stack.force_close(b, a, rtt, cb)` calls are different signature and left intact.
- FxCascade cooldown semantic verified by reading the new tick_pair body — `cooldown` is set in the `if (!pos.active)` post-manage branch, not in the open() branch.
- EsNqDiv confirm logic verified — the new `confirm_dir_` field is initialized in the private section, used in on_tick, and the old `confirm_is_long_` is fully removed (only one comment reference remains in the historical context).

---

## STEP 10 — Explicit "do not" list

- Do NOT auto-commit. Do NOT auto-push. Always hand the user the exact zsh-friendly command sequence and let them run it.
- Do NOT use the user's GitHub PAT. Read context only.
- Do NOT modify core code without an explicit per-task go-ahead.
- Do NOT add documentation files (markdown, README) unless the user asks. Audit reports, trace docs, and handoff docs are the exception by convention.
- Do NOT modify the S15 / S16 audit / trace docs without asking.
- Do NOT propose backtests as P1 fixes. Backtests are out of scope.
- Do NOT touch anything in `omega-terminal/node_modules/` or `build/`.
- Do NOT widen audit scope to non-handoff-catalog engines without asking.
- Do NOT spend context re-reading large audit docs end-to-end. Use Grep.
- Do NOT include leading `#` comment lines in shell command blocks delivered to the user.
- Do NOT flip `mode=LIVE` in `omega_config.ini` without an explicit, separate user instruction. The S17 VWAP re-enable is paper-only in SHADOW.

---

## STEP 11 — One-line summary for next user message

After repo access is mounted and you've Read this handoff, tell the user:

> Repo mounted. S17 closed P1-2, P1-2-a, P1-3, P1-8 and re-enabled all 4 VWAPReversion instances; SIGINT shutdown handler at omega_main.hpp:36 explained the SHADOW-mode 7.79h trades on 2026-03-27. Confirm scope: P1-1 close-out (post-deploy LEDGER-CORRUPT-TS grep), MAE_EXIT_RATIO retune (needs 2-4 weeks of fresh tape), P1-4 IndexFlow WR investigation, P1-5 TrendPullback INI knobs, or other.

Then wait for the user to choose. Don't dive in.

---

## Appendix — S17 final commit message (for reference / diff reading)

The S17 omnibus commit message is reproduced here so you can read it without `git log` access:

```
S17: P1-2/2a label fidelity + P1-3 FxCascade cooldown + P1-8 EsNqDiv + VWAP re-enable

P1-2 + P1-2-a: extend CrossPosition::force_close at CrossAssetEngines.hpp:264
with optional reason parameter (defaults to FORCE_CLOSE for backward compat).
Update every CrossPosition wrapper to forward reason. Update each supervisor
call site to pass the meaningful label. P1-2-a closed: project-wide grep
confirms no missing supervisor; rows 6/7/10 in the historical tape are
explained by engine MAE early-exit, and rows 15/16 are explained by the
SIGINT/SIGTERM/Console-CtrlHandler graceful-shutdown path at omega_main.hpp:36
which runs in SHADOW too (the S16 trace's "LIVE-only" framing applied only to
the quote_loop reconnect path, not the signal handler).

P1-3 (FxCascade): cooldown reset moved from entry to close. tick_pair() now
sets cooldown = ca_now_sec() + COOLDOWN_SEC only when manage() transitions
pos.active to false. This makes COOLDOWN_SEC a true minimum gap between
trades rather than overlapping the held duration. May reduce trade count
slightly but produces clean cooldown semantics. CrossAssetEngines.hpp:777-825.

P1-8 (EsNqDivergence): rewrite of confirm logic. Old version had two bugs:
(1) is_long was hardcoded true in both qualifying branches so the
"confirm_is_long_ != is_long" flip-detection was dead code; (2) per-symbol
on_tick dispatch caused every non-laggard tick to reset confirm_count_=0,
making the engine effectively unfireable. New approach: track signed
direction (confirm_dir_) from the sign of div, only count up on
laggard-symbol ticks of matching direction, reset only on direction flip
or below-threshold. Replaced bool confirm_is_long_ with int confirm_dir_.
Engine remains enabled=false at engine_init.hpp:1585 -- the fix is
prep-work for future enablement. CrossAssetEngines.hpp:350-440.

VWAP re-enable: flipped enabled=false to enabled=true for all four
g_vwap_rev_* instances at engine_init.hpp:447,450,453,457. The engines
were architecturally healthy and well-tuned per the May-3 audit; disable
was conservative gating pending Phase 3 backtest, not a quality cull.
With the new label fidelity in place, MAE_EARLY_EXIT vs other reasons
will be distinguishable in the tape, enabling a proper retune cycle.
Trading mode is SHADOW so this re-enable is paper-only until kShadowDefault
or [section] live-mode is changed.

Refs: AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md
      HANDOFF_S17_AFTER_S16_P1_TRACES.md
```

Good luck.
