# HANDOFF — S19 (continues S18 P1 closeouts batch)

Prepared: 2026-05-08, end of S18
Branch / HEAD at handoff: `main` @ `1573b7a` (S17 omnibus) **plus eight S18 commits** that the user is committing/pushing locally as I write this. After their push, `git log --oneline -10` should show, top to bottom: a HANDOFF_S19 commit, an S18 build-fix commit (rename `reason` → `close_reason` to clear MSVC C4458), an S18 docs-final commit, P1-11+EURUSD-tune commit (or split as two), P1-6 commit, P1-5 commit, P1-10 commit, P1-12 commit, S18 docs-initial commit, then `1573b7a` (S17 omnibus). Confirm with `git log --oneline -12` after push lands.

Mode: `mode=SHADOW` (no live orders firing). All S18 changes are paper-only until LIVE flip.

**Build status:** S17's `CrossPosition::force_close` introduced a `reason` parameter that shadowed the class member `reason[32]` at `CrossAssetEngines.hpp:159`. MSVC `/W4 /WX` rejected the build with `C4458`. S18 added a build-fix commit renaming the parameter to `close_reason` in that one site only. After this fix the build should pass on the VPS via `.\OMEGA.ps1 deploy`. If MSVC reports any further shadowing warnings on the rebuild, the fix pattern is the same: rename the parameter, leave the member alone, document why.

Standing user preferences carried over: full code (no snippets/diffs), warn at 70% chat with summary, never modify core code without clear instruction, paste-friendly bash commands (no leading `#` comments — zsh errors on those), warn before any session-usage block.

---

## TL;DR for the fresh Claude reading this

S18 closed nine P1 backlog items in addition to S17's four:

- **P1-5** TrendPullback consec-SL knobs refactored to public member fields (`CONSEC_SL_THRESH=2`, `BLOCK_AFTER_CONSEC_SEC=600`); 8 sites across 4 exit reasons updated; defaults match prior hardcoded behaviour exactly so no functional change unless overridden.
- **P1-6** GER40 TrendPullback wiring closed via 14-line documentation comment in `tick_indices.hpp::on_tick_ger40` — verified that SP and NQ instances ARE properly wired (the agent's earlier read suggested otherwise) and only GER40 is missing wiring intentionally because new entries are disabled.
- **P1-7** BracketEngine `CONFIRM_SECS` formalisation closed via static analysis — feature already asymmetric per-class (XAUUSD 3.0pt/30s, others 0/0=disabled), no bug, future per-instance INI exposure tracked as P3 enhancement.
- **P1-9** IndexSwingEngine TP% asymmetry closed via static analysis — hypothesis was unfounded; engine has no TP at all, exits via BE-locked trail or 8h timeout, fully symmetric.
- **P1-10** SweepProEngine docstring added to `LiquiditySweepProEngine` (`GoldEngineStack.hpp:1943-1959`) explaining detection mechanism, gates, defaults.
- **P1-11** MinimalH4Breakout cold-start CSV warm-load — new public method `seed_channel_from_csv()` on the engine + private `_parse_csv_h4_line()` parser (~165 lines incl. doc block at `MinimalH4Breakout.hpp:163-367`); two fallback call sites wired in `engine_init.hpp:927-974` (state-loaded-but-H4-cold + no-state-at-all). No sidecar collector needed — gold's continuous H4 bar maintenance is handled by `g_bars_gold.h4`.
- **P1-12** C1RetunedPortfolio cluster-day boundary — UTC-midnight semantics confirmed intentional; doc-string drift fixed at `C1RetunedPortfolio.hpp:53,640` ("UTC session" → "UTC day").
- **VWAPReversion EURUSD** explicit tune at `engine_init.hpp:467-469`: `MAX_EXTENSION_PCT=0.40`, `MAX_HOLD_SEC=600` (chosen to preserve indices' threshold-to-max ratio of ~3.25x; ~44 pips at EURUSD ~1.10).
- Doc cleanup: `AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md` closure annotation added; `ENGINE_AUDIT_CHECKLIST.md` VWAPReversion + ESNQ rows updated, KNOWN BUGS rows 7/8/9 added, header date refreshed; new `AUDIT_S18_P1_CLOSEOUTS.md` trace doc.

What's left after S18 are all tape-dependent items: P1-1 (LEDGER-CORRUPT-TS post-deploy grep), P1-4 (IndexFlow WR investigation), MAE_EXIT_RATIO retune, VWAPReversion EURUSD post-shadow re-tune.

**Your job for S19 is the user's pick from a short menu (see "Outstanding decisions" below).** All remaining backlog items need fresh post-deploy tape to act on. Don't dive in — ask first.

**Fastest way to come up to speed — read these in this order:**

1. This file. Self-contained.
2. [`HANDOFF_S18_AFTER_S17_FIXES.md`](computer:///Users/jo/omega_repo/HANDOFF_S18_AFTER_S17_FIXES.md) — prior handoff. Important historical context for the S17 changes (force_close label fidelity, FxCascade cooldown, EsNqDiv rewrite, VWAP re-enable). Do NOT re-execute its instructions.
3. [`AUDIT_S18_P1_CLOSEOUTS.md`](computer:///Users/jo/omega_repo/AUDIT_S18_P1_CLOSEOUTS.md) — S18's main trace doc. Has rationale for P1-7/9/12 static closures and applied-edit details for P1-5/6/10/11 + VWAPReversion EURUSD.
4. [`AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md) — P1-2 trace. Closure annotation prepended explains the SIGINT shutdown handler finding.
5. [`AUDIT_S16_P1_1_LEDGER_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_1_LEDGER_TRACE.md) — P1-1 static trace. Awaiting fresh post-deploy log.
6. [`ENGINE_AUDIT_CHECKLIST.md`](computer:///Users/jo/omega_repo/ENGINE_AUDIT_CHECKLIST.md) — engine status table; VWAPReversion now LIVE-SHADOW with full EURUSD tune, ESNQ row notes the S17 confirm-logic rewrite. KNOWN BUGS rows 7/8/9 are S17-fixes.
7. [`AUDIT_S15_ENGINES.md`](computer:///Users/jo/omega_repo/AUDIT_S15_ENGINES.md) — full engine audit. Don't read end-to-end; Grep for what you need.

Don't re-read engine source files unless you're picking up a backlog item that demands it.

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

`mcp__workspace__bash` was **still broken at start of S18** with the same symptom as S13/S15/S16/S17:

```
bash failed on resume, create, and re-resume.
useradd: /etc/passwd.NNNNNN: No space left on device
```

S18 did not retest mid-session. Implications unchanged: cannot run git, python, awk, or compile from the sandbox. All git operations happen on the user's Mac. CSV inspection uses chunked `Read` (500-1000 lines per call) or delegates to a `general-purpose` Agent. Build verification is `.\OMEGA.ps1 deploy` on the VPS.

If you DO try bash and it works, great — but don't rely on it for the critical path. Plan around the broken-bash assumption first; treat working bash as a bonus.

### Mac local environment

```
host:    jo@Jos-MacBook-Pro
shell:   zsh   (NOT bash -- `#` comment lines in pasted scripts cause errors)
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

Build cycle is on the VPS. The user runs `.\OMEGA.ps1 deploy` (PowerShell on the VPS) after each push. The S18 changes require this for the new wiring to take effect (engine_init.hpp runs at startup; new code paths are dead until next process restart).

### Trading mode

`mode=SHADOW` per `omega_config.ini`. No live orders. The S18 changes are all paper-safe in SHADOW. The VWAP re-enable from S17 + new VWAP EURUSD tune produces shadow trades only until the LIVE flip.

---

## STEP 3 — Verify what S18 left in the tree

### Already in `1573b7a` (S18 starting commit, also the S17 omnibus tip)

S17's full omnibus: P1-2/2a label fidelity, P1-3 FxCascade cooldown, P1-8 EsNqDiv rewrite, VWAP re-enable. Plus the prior intermediate commit `83a4d66`. Confirm with `Read /Users/jo/omega_repo/.git/refs/heads/main`.

### S18 commits stacked on top of `1573b7a`

S18 produced seven or eight commits depending on whether the user used the bundled or split P1-11+EURUSD path. The expected sequence (oldest first, on top of `1573b7a`):

| Commit | Subject (approximate)                                           |
| ------ | --------------------------------------------------------------- |
| 1      | S18 docs (initial): P1-2/2a closure annotation, P1-7/9/12 close-out trace, checklist updates |
| 2      | P1-12: rename 'UTC session' to 'UTC day' in cluster-day boundary |
| 3      | P1-10: add class-level docstring to LiquiditySweepProEngine     |
| 4      | P1-5: TrendPullback consec-SL knobs to public member fields     |
| 5      | P1-6: document GER40 TrendPullback wiring dependency             |
| 6      | P1-11 + VWAP EURUSD tune (bundled), OR P1-11 alone if split     |
| 7a     | (only if split) VWAPReversion EURUSD: explicit MAX_EXTENSION_PCT and MAX_HOLD_SEC |
| 7      | S18 docs (final): P1-11 + EURUSD tune closure annotations       |
| 8      | **S18 build fix: rename CrossPosition::force_close 'reason' param to avoid C4458** (mid-deploy fix; see Appendix) |
| 9      | (this) HANDOFF_S19_AFTER_S18_CLOSEOUTS.md                        |

### Files modified across the S18 commits

| File                                            | What                                                              |
| ----------------------------------------------- | ----------------------------------------------------------------- |
| `AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`         | Closure annotation prepended (S18 commit 1)                       |
| `ENGINE_AUDIT_CHECKLIST.md`                     | VWAPReversion + ESNQ rows + KNOWN BUGS 7/8/9 + header date (S18 commits 1 + final) |
| `AUDIT_S18_P1_CLOSEOUTS.md`                     | NEW trace doc                                                     |
| `include/C1RetunedPortfolio.hpp`                | "UTC session" → "UTC day" at L53,640 (P1-12)                      |
| `include/GoldEngineStack.hpp`                   | LiquiditySweepProEngine docstring at L1940-1959 (P1-10)           |
| `include/CrossAssetEngines.hpp`                 | TrendPullback consec-SL public knobs at L1579-1591; 8 callsite refactors L1755/1764/1811/1820/1872/1881/1951/1960; private member-field comment block at L2337-2343 (P1-5) |
| `include/tick_indices.hpp`                      | GER40 wiring dependency comment at L713-727 (P1-6)                |
| `include/MinimalH4Breakout.hpp`                 | New `seed_channel_from_csv()` + private `_parse_csv_h4_line()` at L163-367; includes added (P1-11) |
| `include/engine_init.hpp`                       | VWAP EURUSD explicit tune at L457-469; CSV warm-load fallback at L927-974 (P1-11 + EURUSD tune) |
| `include/CrossAssetEngines.hpp`                 | (additional, build-fix commit) renamed `force_close` parameter `reason` → `close_reason` at L264-273 to clear MSVC C4458 (param shadowed class member `reason[32]` at L159). Functionally identical. |

### Untracked files left intentionally out of git (carry-over from S15/S16/S17/S18 — no change)

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/
```

Decision still deferred to the user. S18 did not touch these.

### S18-generated docs

- `AUDIT_S18_P1_CLOSEOUTS.md` — new trace, committed in S18 commits 1 + final
- `HANDOFF_S19_AFTER_S18_CLOSEOUTS.md` — this file. Pending commit. Bundle with S18 docs-final or commit separately, your call.

---

## STEP 4 — What S18 deliberately did NOT do

Don't redo any of these — they're handled or out of scope:

- **Run `.\OMEGA.ps1 deploy`.** Source-only edits. The user runs the deploy on the VPS after pushing. New code paths (CSV warm-load wiring, EURUSD tune, consec-SL public knobs, P1-6 comment) are dead until process restart.
- **Drop the `bars_xauusd_h4.csv` warm-load CSV.** If the user wants the P1-11 wiring to actually do something, they need to provide a Dukascopy-style XAUUSD H4 OHLC CSV at `<log_root_dir>/bars_xauusd_h4.csv` BEFORE deploy. Without it, the engine prints "cold start -- drop CSV at..." and proceeds with the existing 40hr warmup. No regression.
- **Apply CSV warm-load to `H4RegimeEngine`.** Out of scope for P1-11 which was specifically scoped to MinimalH4Breakout. The H4RegimeEngine still does its own `seed_channel_from_bars(g_bars_gold.h4.get_bars())` warm path at `engine_init.hpp:923` and remains cold when `g_bars_gold.h4` is empty.
- **Touch M5 bar engine for indices.** P1-6 closed by documenting the GER40 wiring dependency, not by adding new wiring. The SP and NQ instances ARE already wired with `seed_bar_emas` + `seed_m5_trend` at `tick_indices.hpp:122-135` and `:394-404`. GER40 is intentionally dark because new entries are disabled (`engine_init.hpp:824` plus the "GER40 NEW ENTRIES DISABLED" gate at `tick_indices.hpp:713`). If GER40 is ever re-enabled, the L713-727 comment block specifies what wiring to add.
- **Move TrendPullback consec-SL knobs to omega_config.ini.** P1-5 exposed them as public member fields (settable from engine_init.hpp) but did NOT add the INI loader plumbing. That's a future hop documented in the new public-knob block at `CrossAssetEngines.hpp:1587-1588`.
- **Re-tune VWAPReversion EURUSD from real data.** S18's tune was derived analytically from the index threshold-to-max ratio (~3.25x). Re-tune from fresh shadow tape after 2-4 weeks of live-shadow EURUSD data — see Step 5 #4.
- **Process Tier-1 vs backtest WR comparison.** Still requires a fresh post-Tier-1 tape (≥200 closed trades since 2026-04-29 ship). The historical tape ends 2026-04-08 and is entirely pre-Tier-1.
- **Apply any further P1 fix.** All remaining backlog items are tape-dependent.
- **`HANDOFF_S18_AFTER_S17_FIXES.md`.** Stale on its "Outstanding decisions" 1-5 (all closed in S18). Don't update unless the user asks.

---

## STEP 5 — Outstanding decisions to ask the user (in priority order)

Use `AskUserQuestion` for the first one if multiple options apply. Don't start work without explicit pick.

1. **P1-1 verification close-out.** Has `.\OMEGA.ps1 deploy` been run on the VPS since the S18 commits landed? If yes, run `grep "[LEDGER-CORRUPT-TS]"` against the binary's stderr/stdout log. If empty after ≥100 closed shadow trades or ≥1 week, P1-1 closes resolved. If any line appears, the engine routing table at the end of `AUDIT_S16_P1_1_LEDGER_TRACE.md` maps `engine=` to a file:line.

2. **MAE_EXIT_RATIO retune cycle.** After 2-4 weeks of fresh post-deploy tape (with VWAPReversion now firing AND label fidelity in place), run the cohort analysis:
   - Filter shadow CSV rows where `engine == "VWAPReversion"` AND `exit_reason == "MAE_EARLY_EXIT"`.
   - For each row, look at `mfe` column. If `mfe > 0` significantly, that's a candidate "we cut a winner" event.
   - Compute `cut-winners / total-MAE-trades`. If > 30%, MAE_EXIT_RATIO is too tight; if < 10%, it's about right; if = 0, you're missing the signal entirely.
   - Same analysis for `MIDNIGHT_ROLLOVER`, `STALE_PRIOR_DAY`, `RECONNECT_CLOSE`, `SHUTDOWN` cohorts.
   - `MAE_EXIT_RATIO` is a member constant in `CrossAssetEngines.hpp` — grep for it when you want to retune.

3. **P1-4 IndexFlow WR investigation.** Needs ≥200 closed trades since 2026-04-29 ship. If the user has accumulated that, the ask is: filter shadow CSV by `engine == "IndexFlow"`, compute WR, compare against the 35% threshold. The S15 audit doc (`AUDIT_S15_ENGINES.md`) has prior IndexFlow analysis; grep for `IndexFlow` to find the historical context.

4. **VWAPReversion EURUSD post-shadow re-tune.** S18's tune (`MAX_EXTENSION_PCT=0.40`, `MAX_HOLD_SEC=600`) was derived analytically from the index ratio. After 2-4 weeks of live-shadow EURUSD VWAP-rev fills, evaluate:
   - Cohort by `exit_reason`. How many are TP_HIT vs SL_HIT vs MAE_EARLY_EXIT vs TIMEOUT? If TIMEOUT is dominant, `MAX_HOLD_SEC=600` may be too tight (genuine reversions need more time on FX). If SL_HIT is dominant with `mfe < 0.10%`, the engine is firing on noise — tighten `EXTENSION_THRESH_PCT` (currently `0.12`).
   - Check whether `MAX_EXTENSION_PCT=0.40` blocks any genuine fills. If you see warnings about over-extended dislocations being skipped (engine prints with `MAX_EXTENSION_PCT` keyword), consider raising to 0.50.

5. **Documentation cleanup (carry-over).** `HANDOFF_S15_ENGINE_AUDIT.md`, `HANDOFF_S14_POST_CTRADER_CULL.md`, and the `audit_input/` directory are still untracked. The user has been deferring this decision since S15. Worth asking once: include in git or leave dark.

6. **TrendPullback consec-SL INI plumbing (P3 enhancement).** P1-5 exposed the knobs as member fields but did not wire INI loading. If the user wants this wired (e.g., to tune per-symbol without recompile), the pattern is the existing DAILY_LOSS_CAP loader. Find via grep — likely in `engine_config.hpp` or `config.hpp`.

---

## STEP 6 — P1 backlog state (carry-forward + S17 + S18 closures)

Status as of end-S18:

| Item   | Status                  | Notes                                                       |
| ------ | ----------------------- | ----------------------------------------------------------- |
| P1-1   | Static trace done       | Awaiting post-deploy tape. See `AUDIT_S16_P1_1_LEDGER_TRACE.md`. |
| P1-2   | CLOSED in S17           | Label fidelity fix in `CrossAssetEngines.hpp` + `config.hpp` + `quote_loop.hpp`. 70+ call sites updated. |
| P1-2-a | CLOSED in S17           | No missing supervisor. Rows 6/7/10 = MAE early-exit. Rows 15/16 = SIGINT/Console-Ctrl shutdown handler at `omega_main.hpp:36`. |
| P1-3   | CLOSED in S17           | FxCascade cooldown moved from entry to close. `CrossAssetEngines.hpp:777-825`. |
| P1-4   | Open                    | `IndexFlow` 29.4% WR vs 35% threshold. Replay-required (≥200 fresh trades). |
| P1-5   | **CLOSED in S18**       | TrendPullback consec-SL knobs exposed as public member fields. `CrossAssetEngines.hpp:1579-1591`; 8 callsite refactors. |
| P1-6   | **CLOSED in S18**       | GER40 TrendPullback wiring documented in `tick_indices.hpp:713-727`. SP/NQ already correctly wired. |
| P1-7   | **CLOSED in S18**       | Static analysis: BracketEngine `CONFIRM_SECS` already per-class asymmetric, no bug. P3 enhancement only. |
| P1-8   | CLOSED in S17           | EsNqDivergence confirm-logic rewrite. Engine remains `enabled=false`. `CrossAssetEngines.hpp:350-440`. |
| P1-9   | **CLOSED in S18**       | Static analysis: IndexSwingEngine has no TP%; symmetric by design (BE-locked trail + 8h timeout). Hypothesis unfounded. |
| P1-10  | **CLOSED in S18**       | LiquiditySweepProEngine docstring added. `GoldEngineStack.hpp:1943-1959`. |
| P1-11  | **CLOSED in S18**       | `seed_channel_from_csv()` + parser added to MinimalH4Breakout; two fallback callsites in engine_init.hpp. |
| P1-12  | **CLOSED in S18**       | UTC-midnight cluster boundary intentional; doc-string drift fixed at `C1RetunedPortfolio.hpp:53,640`. |
| VWAP-EURUSD-tune | **CLOSED in S18** | Explicit `MAX_EXTENSION_PCT=0.40`, `MAX_HOLD_SEC=600` at `engine_init.hpp:467-469`. |

P2 backlog (~25 items) untouched. Listed in S16 handoff Step 7.

P3 enhancements not previously catalogued:
- BracketEngine `CONFIRM_SECS` per-symbol INI exposure (P1-7 closure noted this).
- TrendPullback consec-SL knobs INI plumbing (P1-5 closure noted this).

---

## STEP 7 — Files state at end of S18

### Modified, working tree at end of S18 (committed and pushed; verify with `git status`)

All S18 commits should be on `main` after the user pushes. `git status` should be clean except possibly:

```
?? HANDOFF_S19_AFTER_S18_CLOSEOUTS.md   (this file -- pending its own commit)
```

### Untracked, deliberately not staged (carry-over)

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/                                (CSV + zip + python helper)
```

The audit reports + handoffs all live at repo root by convention.

---

## STEP 8 — Repo / git recon shortcuts (bash-free) — carry-over from S17/S18

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

## STEP 9 — Verification checklist before claiming any work is done

Mirror what S15-S18 did:

1. Read each file you modified end-to-end after the edit. Don't trust your own diff.
2. Confirm anchor strings around your changes are unique and intact.
3. For any new field, grep for callers and confirm they compile against the new shape.
4. For any new include, confirm it's already present (header-only project) — note S18 added `<cstdlib>`, `<ctime>`, `<fstream>`, `<vector>` to MinimalH4Breakout.hpp.
5. For any change touching a code path that produces `TradeRecord`, hand-verify against existing `TradeRecord` assignments (entryTs, exitTs, engine label, side, exitReason).
6. Per the standing user pref, leave changes uncommitted unless the user explicitly says to commit.
7. When delivering shell commands, strip `#` comment lines (zsh, not bash).

Specific to S18's changes:
- `seed_channel_from_csv()` is callable from the public API. The new private `_parse_csv_h4_line()` is `static` so it doesn't access instance state — it's a pure function.
- All 8 TrendPullback consec-SL sites use `CONSEC_SL_THRESH` and `BLOCK_AFTER_CONSEC_SEC`. Verified by grep — no leftover `>= 2` or `+ 600` literals in the consec-SL pattern (other `>= 2` and `+ 600` literals exist for unrelated purposes; do not refactor those).
- The print messages still hardcode "10min" and "2 consec losses" — this is documented behaviour. Any operator setting non-default knob values must update the prints separately.
- VWAPReversion EURUSD line numbers shifted in `engine_init.hpp` due to the 11-line comment block at L457-466 that explains the tune rationale.

---

## STEP 10 — Explicit "do not" list (carry-over)

- Do NOT auto-commit. Do NOT auto-push. Always hand the user the exact zsh-friendly command sequence and let them run it.
- Do NOT use the user's GitHub PAT. Read context only.
- Do NOT modify core code without an explicit per-task go-ahead.
- Do NOT add documentation files (markdown, README) unless the user asks. Audit reports, trace docs, and handoff docs are the exception by convention.
- Do NOT modify the S15 / S16 / S18 audit / trace docs without asking. Closure annotations to S15/S16 trace docs require explicit go-ahead.
- Do NOT propose backtests as P1 fixes. Backtests are out of scope.
- Do NOT touch anything in `omega-terminal/node_modules/` or `build/`.
- Do NOT widen audit scope to non-handoff-catalog engines without asking.
- Do NOT spend context re-reading large audit docs end-to-end. Use Grep.
- Do NOT include leading `#` comment lines in shell command blocks delivered to the user.
- Do NOT flip `mode=LIVE` in `omega_config.ini` without an explicit, separate user instruction. The S17/S18 changes are paper-only in SHADOW.
- Do NOT add INI plumbing for the new TrendPullback consec-SL knobs (P1-5) without an explicit ask — S18 deliberately exposed them as members only.

---

## STEP 11 — One-line summary for next user message

After repo access is mounted and you've Read this handoff, tell the user:

> Repo mounted. S18 closed P1-5/6/7/9/10/11/12 plus VWAPReversion EURUSD tune, plus a build-fix commit renaming the S17 `force_close` parameter to clear MSVC C4458. Combined with S17 the live backlog is now P1-1, P1-4, MAE_EXIT_RATIO retune, and the EURUSD post-shadow re-tune — all four are tape-dependent. Confirm scope: P1-1 close-out grep (post-deploy), MAE retune cohort analysis (needs 2-4 weeks), P1-4 IndexFlow WR (needs ≥200 trades since ship), VWAP EURUSD post-shadow re-tune (needs 2-4 weeks), P3 enhancements (BracketEngine CONFIRM_SECS INI, TrendPullback consec-SL INI), confirm successful VPS rebuild after the build-fix, or other.

Then wait for the user to choose. Don't dive in.

---

## Appendix — S18 commit message bodies (for reference / diff reading)

The S18 commits' bodies are reproduced here so you can read them without `git log` access. The user's exact final commit messages may differ slightly.

**Commit 1: S18 docs (initial)**

```
S18 docs: P1-2/2a closure annotation, P1-7/9/12 close-out trace, checklist updates

AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md: closure annotation explaining the
SIGINT/SIGTERM/SetConsoleCtrlHandler graceful-shutdown path at
omega_main.hpp:36 (which runs in SHADOW too) accounts for rows 15/16
(7.78h/7.79h holds), and engine MAE early-exit at
CrossAssetEngines.hpp:1252 accounts for rows 6/7/10 (profitable LONGs).
No missing supervisor.

ENGINE_AUDIT_CHECKLIST.md: VWAPReversion row updated to LIVE-SHADOW for
all four instances (SP/NQ/GER40/EURUSD per S17 1573b7a). ESNQ row notes
the S17 P1-8 confirm-logic rewrite. KNOWN BUGS rows 7/8/9 added covering
P1-2/3/8.

AUDIT_S18_P1_CLOSEOUTS.md (new): static closure rationale for P1-7
(CONFIRM_SECS already per-class asymmetric, no bug), P1-9 (no TP% at all,
hypothesis unfounded, design symmetric by construction), P1-12
(UTC-midnight boundary intentional). Plus scoped specs for P1-5/6/10/11.
```

**Commit 2: P1-12 doc drift fix**

```
P1-12: rename 'UTC session' to 'UTC day' in cluster-day boundary

Two word substitutions in C1RetunedPortfolio.hpp at L53 (header docstring)
and L640 (halt_reason_ log string). The boundary code at L628 is
(tr.exitTs)/86400LL which is UTC-midnight aligned, not session aligned.
Existing comment at L551 already said 'UTC day' so this brings the other
two references into agreement. No behaviour change.
```

**Commit 3: P1-10 SweepPro docstring**

```
P1-10: add class-level docstring to LiquiditySweepProEngine

Previously the only documentation was the bare '5. LiquiditySweepProEngine'
banner. Added 16-line block describing the detection mechanism (densest
cluster within 0.35pt band over 120-tick window, requires >=8 prices in
cluster), the entry conditions (price extends >0.80pt from cluster on
momentum spike >0.70pt with decaying momentum and >2.00pt VWAP distance),
direction logic, trend-alignment gate, Asia-session tighter-spread filter,
and defaults. No behaviour change.
```

**Commit 4: P1-5 consec-SL knobs**

```
P1-5: TrendPullback consec-SL knobs to public member fields

Replace hardcoded literals '>= 2' and '+ 600' across all four loss-exit
paths in TrendPullbackEngine (IMM_REVERSAL ~L1755, TIME_STOP ~L1811,
HARD-TIME-STOP ~L1872, SL_HIT ~L1951) with new public member fields:

  int CONSEC_SL_THRESH        = 2;
  int BLOCK_AFTER_CONSEC_SEC  = 600;

Added under 'Improvement 9: Consecutive-SL direction block' in the public
knob section (~L1579). Defaults match the prior hardcoded behaviour exactly
so no functional change unless an operator explicitly overrides the values
per-instance from engine_init.hpp.

The print messages still hardcode the strings -- documented in the new
doc-block. INI plumbing deferred as a future hop.

Private member-field comments at L2337-2343 updated to reference the new
public knobs. 8 sites total updated, verified by grep -- no leftover
'>= 2' or '+ 600' literals in the consec-SL pattern.
```

**Commit 5: P1-6 GER40 wiring comment**

```
P1-6: document GER40 TrendPullback wiring dependency in on_tick_ger40

Verified that g_trend_pb_sp and g_trend_pb_nq ARE properly wired with
seed_bar_emas() and seed_m5_trend() at tick_indices.hpp:122-135 and
:394-404 respectively. Only g_trend_pb_ger40 is missing the wiring,
intentionally (engine_init.hpp:824 sets enabled=false; on_tick_ger40
also gates new entries with 'GER40 NEW ENTRIES DISABLED' at line above).

Added 14-line comment block in on_tick_ger40 documenting why the wiring
is absent, what MUST be added if g_trend_pb_ger40.enabled is ever flipped
to true (mirror SP/NQ pattern), and the behavioural impact of running
without the wiring. No code change beyond the comment. Closes P1-6.
```

**Commit 6 (bundled): P1-11 + VWAP EURUSD tune**

```
P1-11 + VWAP EURUSD tune: CSV warm-load for MinimalH4Breakout, explicit
EURUSD VWAPReversion params

P1-11 (MinimalH4Breakout cold-start CSV warm-load):
  Add seed_channel_from_csv(path) public method to MinimalH4Breakout, plus
  a private _parse_csv_h4_line() helper. Parser mirrors
  backtest/seed_us30_h4.cpp for cross-engine schema consistency: accepts
  comma/tab/semicolon separators, three timestamp formats (epoch sec,
  epoch ms, ISO8601), rejects zero-range bars (Dukascopy closed-market),
  skips header rows, validates OHLC. Returns false gracefully on
  missing-file, unreadable, or insufficient-bars cases.

  Wire two fallback call sites in engine_init.hpp: on the H4-cold branch
  and the no-state-at-all branch, attempt
  seed_channel_from_csv(log_root_dir() + '/bars_xauusd_h4.csv'). On
  success the engine is hot for the next H4 close. On failure the
  existing 40-hour cold-start path proceeds unchanged.

  No sidecar collector needed: g_bars_gold.h4 handles ongoing bar
  maintenance. CSV warm-load is bootstrap-only.

  Includes added: <cstdlib>, <ctime>, <fstream>, <vector>.

VWAP EURUSD tune (deferred from S17):
  Set explicit MAX_EXTENSION_PCT=0.40 and MAX_HOLD_SEC=600 on
  g_vwap_rev_eurusd in engine_init.hpp. Previously these fell back to
  class defaults (0.80 / 900s). New values preserve the threshold-to-max
  ratio of indices (avg ~3.25x) so EURUSD's EXTENSION_THRESH_PCT=0.12
  maps to MAX_EXTENSION_PCT ~= 0.40 (~44 pips at EURUSD ~1.10).
  MAX_HOLD_SEC=600 aligns with indices' 'exit stalled trades faster'
  rationale at engine_init.hpp:446. Re-tune from fresh shadow tape after
  2-4 weeks.
```

**Commit final: S18 docs (final)**

```
S18 docs: P1-11 + EURUSD tune closure annotations

ENGINE_AUDIT_CHECKLIST.md: VWAPReversion row reflects EURUSD's S18
explicit tune; the prior 'fall back to class defaults' note is removed.

AUDIT_S18_P1_CLOSEOUTS.md: P1-11 section rewritten as CLOSED (S18) with
file:line of applied changes; new VWAPReversion EURUSD tune section
added under closures; future-enhancement list updated; sources updated.
Title also revised to reflect the applied state.
```

**Commit build-fix: rename force_close 'reason' to 'close_reason'**

```
S18 build fix: rename CrossPosition::force_close 'reason' param to avoid C4458

MSVC /W4 emits C4458 'declaration of reason hides class member' at
CrossAssetEngines.hpp:265 because the optional 'reason' parameter
introduced by S17's force_close signature change shadows the class
member 'reason[32]' declared at L159. /WX makes this a hard error.

Renamed the parameter only inside CrossPosition::force_close --
'reason' -> 'close_reason'. The body's single usage is updated to
match. The wrapper methods in other engine classes
(OilEventFadeEngine::force_close, BrentWtiSpreadEngine::force_close,
FxCascadeEngine::force_close etc.) keep their 'reason' parameter
because those classes have no 'reason' member, so no shadowing.

Functionally identical to the prior code -- the parameter is forwarded
unchanged to emit(). All call sites were already passing positional
arguments, not named, so no caller updates needed.
```

Good luck.
