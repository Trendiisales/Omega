# HANDOFF — S17 (continues S16 P1 trace work)

Prepared: 2026-05-08, end of S16
Branch / HEAD at handoff: `main` @ `4db4750` **plus one pending commit** (the S16 P1-2 trace report) that the user is committing/pushing locally as I write this. Confirm with `git log --oneline -3` after their commit lands. If the new SHA is `<X>`, that's the real HEAD for S17.
Mode: `mode=SHADOW` (no live orders firing).
Standing user preferences carried over: full code (no snippets/diffs), warn at 70% chat with summary, never modify core code without clear instruction, paste-friendly bash commands (no leading `#` comments — zsh errors on those).

---

## TL;DR for the fresh Claude reading this

S16 produced two trace reports (P1-1 ledger sanitizer trace, P1-2 VWAPReversion FORCE_CLOSE trace) plus committed and pushed the S15 P0 fixes that had been sitting in the working tree at S16 start. The actual binary fix work for P1-1 and P1-2 is **deferred** and waiting on (a) the user deploying the P0s on the VPS to produce a fresh shadow tape, and (b) explicit go-ahead before any source changes for P1-2. Your job for S17 is the user's choice from a short menu (see "Outstanding decisions" below). Don't dive in.

**Fastest way to come up to speed — read these in this order:**

1. This file. Self-contained.
2. [`AUDIT_S16_P1_1_LEDGER_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_1_LEDGER_TRACE.md) — P1-1 static trace; concludes the historical 48 corrupt rows are likely already resolved in current main.
3. [`AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md) — P1-2 trace; concludes the all-FORCE_CLOSE pattern is a label-fidelity defect in `CrossPosition::force_close` (5 supervisors all hardcode the same literal). Engine `manage()` path is intact.
4. [`HANDOFF_S16_AFTER_S15_P0_FIXES.md`](computer:///Users/jo/omega_repo/HANDOFF_S16_AFTER_S15_P0_FIXES.md) — prior handoff. Most P1 backlog items are still open and listed there.
5. [`AUDIT_S15_ENGINES.md`](computer:///Users/jo/omega_repo/AUDIT_S15_ENGINES.md) — full engine audit. Don't read end-to-end; Grep for what you need.
6. [`AUDIT_S15_P0_FINDINGS.md`](computer:///Users/jo/omega_repo/AUDIT_S15_P0_FINDINGS.md) — P0 triage one-pager.

Don't re-read engine source files unless you're picking up a P1 item that demands it. The S16 reports already cite the relevant `file:line` anchors.

---

## STEP 1 — Get repo access

The sandbox does NOT have a folder mounted by default. First action:

```
mcp__cowork__request_cowork_directory(path="~/omega_repo")
```

Mount path for host-side tools (Read / Write / Edit / Glob / Grep): `/Users/jo/omega_repo/`. The bash sandbox path differs (`/sessions/<id>/mnt/omega_repo/`) but bash is currently broken — see Step 2. Do not proceed past Step 1 without confirmed mount.

---

## STEP 2 — Sandbox + repo + Mac environment

### Sandbox VM

`mcp__workspace__bash` is **still broken** as of end-S16. Same symptom as S13/S15:

```
bash failed on resume, create, and re-resume.
useradd: /etc/passwd.NNNNNN: No space left on device
useradd: cannot lock /etc/passwd; try again later.
```

Tested at end-S16 with a single `git status` — fails immediately. Implications:

- Cannot run git from this sandbox. All git operations (status, diff, log, add, commit, push) happen on the user's Mac in their terminal.
- Cannot run python/awk/cut over the shadow CSV directly. Use chunked `Read` (500-1000 lines per call) and accumulate state, or delegate to a `general-purpose` Agent. S15 and S16 both used the chunked-Read pattern successfully.
- Cannot compile. Every change is build-verified only when the user runs `.\OMEGA.ps1 deploy` on the VPS.
- Skill scripts that shell out (xlsx/pdf/docx via subprocess) may fail. Stick to direct file IO.

If bash starts working mid-session, great — but don't depend on it.

### Mac local environment (where the user runs git)

```
host:    jo@Jos-MacBook-Pro
shell:   zsh   (NOT bash — `#` comment lines in pasted scripts cause errors)
repo:    ~/omega_repo
```

When pasting multi-line shell scripts to the user, **strip leading `#` comments** before sending. Zsh interprets each as a command and emits `zsh: command not found: #` for every one. Functionally harmless but noisy. The S16 first commit had this problem; subsequent ones did not.

### GitHub remote

```
URL:     https://github.com/Trendiisales/Omega
branch:  main
```

There is a PAT in the global `CLAUDE.md` (`ghp_...`). It's there for **read context only**. Do NOT use it to push. The user pushes manually with their own credentials.

### Build / deploy

Build cycle is on the VPS, NOT local Mac, NOT this sandbox. The user runs `.\OMEGA.ps1 deploy` (PowerShell on the VPS) after each push.

### Trading mode

`mode=SHADOW` per `omega_config.ini`. No live orders. The S15 audit and S16 fixes were all clearly authorised; S17 fixes need explicit per-task go-ahead before any source edits — the user pref is "never modify core code without clear instruction" and that has been respected throughout S16.

---

## STEP 3 — Verify what S16 left in the tree

### Already committed and pushed in S16 (commit `4db4750`)

| File                                          | What it is                                         |
| --------------------------------------------- | -------------------------------------------------- |
| `omega_config.ini`                            | S15 P0-3 (TrendRider `max_lot_cap=0.20`)           |
| `include/CrossAssetEngines.hpp`               | S15 P0-2 (TrendPullback `MAX_TRADE_LOSS_USD` cap)  |
| `include/engine_config.hpp`                   | S15 P0-4 (`[minimal_h4_us30]` parser)              |
| `include/engine_init.hpp`                     | S15 P0-5 (ORB per-instance `OPEN_HOUR/MIN`)        |
| `include/trade_lifecycle.hpp`                 | S15 P0-1 (`[LEDGER-CORRUPT-TS]` sanitizer)         |
| `AUDIT_S15_ENGINES.md`                        | S15 full audit                                     |
| `AUDIT_S15_P0_FINDINGS.md`                    | S15 P0 triage                                      |
| `HANDOFF_S16_AFTER_S15_P0_FIXES.md`           | S15→S16 handoff                                    |
| `AUDIT_S16_P1_1_LEDGER_TRACE.md`              | S16 P1-1 trace report                              |

That commit is on `origin/main`. Confirm with `Read /Users/jo/omega_repo/.git/refs/heads/main` (should be `4db4750...` or whatever the user's S16 P1-2 commit advanced it to).

### Pending / freshly added at end of S16

The following file was added by S16 but its commit-and-push command sequence was handed to the user as the final step. Confirm via `git log --oneline -3` on their side:

| File                                          | Status                                             |
| --------------------------------------------- | -------------------------------------------------- |
| `AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`       | Pending commit (user runs `git add && git commit && git push` per the script delivered at end-S16) |
| `HANDOFF_S17_AFTER_S16_P1_TRACES.md`          | This file — also pending commit (recommend bundling with the P1-2 report into one commit, OR a separate handoff commit; ask user) |

### Untracked files left intentionally out of git (still untracked at end-S16)

These showed up in `git status` at the time of the S16 commit but were not staged:

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/
```

Decision deferred to the user:
- The two older handoff docs (S14, S15) are paper-trail and could be added in a follow-up commit. Low risk. Ask before doing it.
- `audit_input/` contains the shadow CSV + export zip + a `process_trades.py` helper. Should probably be `.gitignore`'d to silence the `??` noise. Don't commit it (CSVs + zips are bulky and re-derivable). Don't add a `.gitignore` line without asking.

---

## STEP 4 — What S16 deliberately did NOT do

Don't redo any of these — they're handled or out of scope:

- **Apply any P1-1 source fix.** P1-1 is gated on a fresh post-deploy shadow tape with the new sanitizer running. If the grep for `[LEDGER-CORRUPT-TS]` lines is empty after >=100 trades or >=1 week, the bug closes as historical-only. If it has lines, the routing table at the end of `AUDIT_S16_P1_1_LEDGER_TRACE.md` maps `engine=` to a file:line.
- **Apply any P1-2 source fix.** The label-fidelity fix is described in the report (extend `CrossPosition::force_close` to take a reason parameter, default to `"FORCE_CLOSE"`, then pass the right label at each of the 5 supervisor call sites). NOT applied because (a) the user's pref is "never modify core code without clear instruction" and (b) the user chose "concise report + commit, stop" scope at end-S16.
- **Apply P1-2-a (the missing-supervisor hunt for trade 6/7/10).** That's a follow-up grep pass; report flags it for next session.
- **Touch the older S14 / S15 handoff files.** Left untracked per Step 3.
- **Write a new audit doc.** The two new reports (`AUDIT_S16_P1_1_LEDGER_TRACE.md` and `AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`) are the only docs S16 added.
- **Touch the S15 audit docs.** Two minor corrections for them are listed in `HANDOFF_S16_AFTER_S15_P0_FIXES.md` Step 5 and remain unaddressed.
- **Reconcile the S15 + S17_AUDIT.md catalogs.** Still deferred.
- **Process the live-vs-backtest WR comparison for Tier-1 portfolios.** The shadow CSV still pre-dates Tier-1 ship (2026-04-08 vs 2026-04-29+). Until a fresh post-Tier-1 CSV arrives, nothing useful can be said.

---

## STEP 5 — Outstanding decisions to ask the user (in priority order)

Use `AskUserQuestion` for the first one if multiple options apply. Don't start work without explicit pick.

1. **P1-1 status check.** Has `.\OMEGA.ps1 deploy` been run on the VPS since end-S16? If yes, send me the post-deploy `[LEDGER-CORRUPT-TS]` grep (or the new `omega_shadow.csv` once it has >=100 closed trades). If no, P1-1 is parked until deploy lands.

2. **P1-2 fix go-ahead.** The recommended fix is a 1-line signature extension on `CrossPosition::force_close` (default keeps existing behaviour) plus ~5-10 caller updates to pass meaningful labels (`MIDNIGHT_ROLLOVER`, `STALE_PRIOR_DAY`, `RECONNECT_CLOSE`, `SHUTDOWN`, `MAE_EARLY_EXIT`). Pure label change, no behavioural risk. Proposal in section "Recommended fix shape" of the P1-2 report. If user says yes, this is a clean 1-2 hour task with full-file edits to 4 files: `CrossAssetEngines.hpp`, `config.hpp`, `quote_loop.hpp`, plus the verification grep.

3. **P1-2-a missing-supervisor hunt.** 30-minute follow-up grep for any other `g_vwap_rev_*.force_close` call site that explains the profitable LONG trades (rows 6, 7, 10 in the tape) closing via `force_close`. The five supervisors I found don't fit those rows. Hypotheses listed at the end of the P1-2 report.

4. **P1-3 onward.** The full P1 backlog is in `HANDOFF_S16_AFTER_S15_P0_FIXES.md` Step 6. Items P1-3 through P1-12 are untouched. Recommended next: P1-3 (`FxCascade` per-pair cooldowns reset on entry, not on close) — concrete, static-analysis-friendly.

---

## STEP 6 — P1 backlog state (carry-forward from S16 handoff)

Status as of end-S16:

| Item   | Status                  | Notes                                                       |
| ------ | ----------------------- | ----------------------------------------------------------- |
| P1-1   | Static trace done       | Awaiting post-deploy tape. See `AUDIT_S16_P1_1_LEDGER_TRACE.md`. |
| P1-2   | Trace done              | Awaiting fix-decision. See `AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`. |
| P1-2-a | New, follow-up to P1-2  | 30-min grep for missing `g_vwap_rev_*.force_close` call site. |
| P1-3   | Open                    | `FxCascade` cooldown resets on entry not close. Static-analysis-friendly. |
| P1-4   | Open                    | `IndexFlow` 29.4% WR vs 35% threshold. Replay-required.     |
| P1-5   | Open                    | `TrendPullback` consec-SL knobs to INI.                     |
| P1-6   | Open                    | `TrendPullback` index instances missing `m5_trend_state_` wiring. |
| P1-7   | Open                    | `BracketEngine CONFIRM_SECS` formalisation per fill latency. |
| P1-8   | Open                    | `EsNqDivergence confirm_count_` direction-flip reset bug.   |
| P1-9   | Open                    | `IndexSwingEngine` TP% asymmetry comprehension.             |
| P1-10  | Open                    | `SweepProEngine` naming/comment doc.                        |
| P1-11  | Open                    | `MinimalH4Breakout` (gold) cold-start CSV warm-load.        |
| P1-12  | Open                    | `C1RetunedPortfolio` cluster-day boundary UTC vs session.   |

P2 backlog (~25 items) untouched. Listed in S16 handoff Step 7.

---

## STEP 7 — Files state at end of S16

### Modified, committed in S16 commit `4db4750`

```
M  include/CrossAssetEngines.hpp     (P0-2)
M  include/engine_config.hpp         (P0-4)
M  include/engine_init.hpp           (P0-5)
M  include/trade_lifecycle.hpp       (P0-1)
M  omega_config.ini                  (P0-3)
A  AUDIT_S15_ENGINES.md
A  AUDIT_S15_P0_FINDINGS.md
A  HANDOFF_S16_AFTER_S15_P0_FIXES.md
A  AUDIT_S16_P1_1_LEDGER_TRACE.md
```

### Pending commit at end of S16 (user runs locally)

```
A  AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md      (P1-2 trace)
A  HANDOFF_S17_AFTER_S16_P1_TRACES.md         (this file)
```

### Untracked, deliberately not staged

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/                                (CSV + zip + python helper)
```

The audit reports + handoffs all live at repo root by convention. Nothing under `include/` was touched by S16 except via the already-committed P0 fixes.

---

## STEP 8 — Repo / git recon shortcuts (bash-free)

When you need git facts and bash is dead:

| What           | How                                                      |
| -------------- | -------------------------------------------------------- |
| Current branch | `Read /Users/jo/omega_repo/.git/HEAD`                    |
| HEAD SHA       | `Read /Users/jo/omega_repo/.git/refs/heads/main`         |
| Recent commits | `Read /Users/jo/omega_repo/.git/logs/HEAD` (large; offset to tail) |
| Untracked?     | Ask the user to paste `git status --short`. No clean way from sandbox. |
| Diff a file    | `Read` the working-tree file and compare to memory of the commit anchors. Or ask the user for `git diff <file>` paste. |

For tape inspection (`audit_input/omega_shadow.csv`):

| What                              | How                                                   |
| --------------------------------- | ----------------------------------------------------- |
| Header                            | `Read offset=1 limit=2`                               |
| Engine rows                       | `Grep ",ENGINE_NAME,"` — anchor commas to avoid partial-name hits (`,VWAPReversion,` not just `VWAPReversion`) |
| Counts                            | `Grep -c` (count mode)                                |
| Full row reads                    | Chunked `Read offset=N limit=500`, accumulate         |
| Stats across thousands of rows    | Spawn a `general-purpose` Agent with the chunked-Read pattern |

CSV columns (memorize):

```
ts_unix, symbol, side, engine, entry_px, exit_px, pnl, mfe, mae,
hold_sec, exit_reason, spread_at_entry, latency_ms, regime
```

`ts_unix` is the entry timestamp, NOT exit. `pnl` is post-cost (apply_realistic_costs has already run). `hold_sec = exit_ts - entry_ts` in seconds.

---

## STEP 9 — Verification checklist before claiming any P1 work is done

Mirror what S15 and S16 did:

1. Read each file you modified end-to-end after the edit. Don't trust your own diff.
2. Confirm anchor strings around your changes are unique and intact.
3. For any new field, grep for callers and confirm they compile against the new shape.
4. For any new include, confirm it's already present (header-only project; everything is `#include`'d via `omega_pch.hpp` or directly from `main.cpp`).
5. For any change touching a code path that produces `TradeRecord`, hand-verify against existing `TradeRecord` assignments (entryTs, exitTs, engine label, side, exitReason).
6. Per the standing user pref, leave changes uncommitted unless the user explicitly says to commit.
7. When delivering shell commands, strip `#` comment lines (zsh, not bash).

---

## STEP 10 — Explicit "do not" list

- Do NOT auto-commit. Do NOT auto-push. Always hand the user the exact zsh-friendly command sequence and let them run it.
- Do NOT use the user's GitHub PAT. Read context only.
- Do NOT modify core code without an explicit per-task go-ahead.
- Do NOT add documentation files (markdown, README) unless the user asks. Audit reports and trace docs are the exception by convention.
- Do NOT modify the S15 / S16 audit / trace docs without asking.
- Do NOT propose backtests as P1 fixes. Backtests are out of scope.
- Do NOT touch anything in `omega-terminal/node_modules/` or `build/`.
- Do NOT widen audit scope to non-handoff-catalog engines without asking.
- Do NOT spend context re-reading large audit docs end-to-end. Use Grep.
- Do NOT include leading `#` comment lines in shell command blocks delivered to the user.

---

## STEP 11 — One-line summary for next user message

After repo access is mounted and you've Read this handoff plus the two S16 trace docs, tell the user:

> Repo mounted. S16 left two trace reports (P1-1 ledger sanitizer, P1-2 VWAPReversion FORCE_CLOSE). P0s already deployed to VPS (commit 4db4750)? Confirm scope: P1-1 close-out (need the post-deploy `[LEDGER-CORRUPT-TS]` grep), P1-2 label-fidelity fix (1-line core change + ~5-10 caller updates, needs go-ahead), P1-2-a missing-supervisor hunt, or move on to P1-3 (FxCascade cooldown).

Then wait for the user to choose. Don't dive in.

Good luck.
