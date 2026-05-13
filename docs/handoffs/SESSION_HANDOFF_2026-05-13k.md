# Session Handoff — 2026-05-13 (NZST), part K

Read this first next session. Direct follow-up to part-J
(`SESSION_HANDOFF_2026-05-13j.md`). Most of this session was diagnostic
rather than productive — a planned MinimalH4Gold promotion + fresh
sweep was interrupted by a live-shape shadow bleed surfacing in the
GUI, which redirected the entire session into a S63 audit. Net code
output: one stop-bleed commit (S68). Net diagnostic output: corrected
mental model of S63 across the engine universe + a state-classification
framework for the broader S63 rollout.

## TL;DR

1. **S68 landed**. `engine_init.hpp` lines 608 + 932: two `.enabled = true`
   flipped to `false` on `g_vwap_rev_nq` (VWAPReversion on USTEC.F) and
   `g_ustec_tf_5m` (UstecTrendFollow5m). Commit message captures
   rationale + the part-K/L override evidence. Stops the bleed
   pattern shown in today's GUI ledger.

2. **Critical correction to mental model — the S63 protection on SP/NQ/EURUSD
   VWAPReversion was deliberately disabled by parts K and L of this same date,
   based on 830 days of backtest data + 4943-trade sweeps.** Re-enabling
   `LOSS_CUT_PCT` / `BE_ARM_PCT` / `BE_BUFFER_PCT` from 0.0 to the
   class defaults would undo that work and is empirically documented
   to make those instruments worse. Comments at engine_init.hpp:597-672
   are the evidence. Next session: read those comments before touching.

3. **Gold promotion stashed**. `g_minimal_h4_gold.shadow_mode = false`
   was prepared, built green, but never committed. Stashed at
   `stash@{0}` with message *"S67: g_minimal_h4_gold shadow_mode=false,
   parked until S63 rollout complete"*. Fresh-tape sweep workflow was
   prepared but never executed (converter created, harness situation
   diagnosed). See "Gold promotion: status" below.

4. **MinimalH4Gold fresh-sweep workflow is ~80% scaffolded** —
   `scripts/duka_to_legacy.py` exists in the working tree (untracked,
   not in S68 commit). Converter is ready; `htf_bt_minimal.cpp` standalone
   compile + run prescription is documented below. Total estimated runtime
   to validate gold edge on fresh tape: ~30 min wall-clock when resumed.

## What did NOT land this session

- S66-followup-2 (the original session focus per part-J) — not started.
  GUI position sources for Bracket/GoldEngineStack/IndexFlow/IndexMacroCrash/
  CandleFlow/MinimalH4 portfolio/C1Retuned still pending.
- S63 LOSS_CUT/BE_RATCHET tuning for the 6 part-J carryover engines.
- PDHL S63 tuning decision (memo at outputs/S63_PDHL_TUNING_RECOMMENDATION_2026-05-13.md).
  Note: depending on the part-J/K state of the PDHL engine, this memo
  may now be partially redundant with the broader S63 audit below.
- Simplest gold engine direction (recommendation memo unchanged, awaiting
  decision in principle but blocked on the fresh-sweep validation that
  was scaffolded this session but not run).

## Commits this session

| commit | message | files |
|--------|---------|-------|
| `<S68_HASH>` | S68: stop-bleed disable on g_ustec_tf_5m + g_vwap_rev_nq | engine_init.hpp (2 line changes) |

origin/main now sits at `<S68_HASH>` (head was `5238a9a` at session
start — part-J handoff doc, which was already landed by the operator
before this session began).

## The S63 audit framework (key output of this session)

Across the engine universe, every engine sits in one of 5 states with
respect to S63 LOSS_CUT_PCT / BE_RATCHET protection:

| State | Description | Action |
|---|---|---|
| **A** | S63 hooks declared + management-path implemented + activated in init | Leave alone. (Confirmed: g_vwap_rev_ger40.) |
| **B** | Hooks + management-path implemented, but deliberately disabled in init with documented backtest evidence | Leave alone, do NOT revert. (Confirmed: g_vwap_rev_sp, g_vwap_rev_nq, g_vwap_rev_eurusd — see engine_init.hpp:597-672 comments for the part-K and part-L sweep evidence.) |
| **C** | Hooks + management-path implemented, disabled by oversight | The legitimate target for "turn on S63" — flip 0.0 to class defaults. **Confirmed for: NONE so far. All 0.0 overrides we found this session were state B.** |
| **D** | Member fields declared but management-path check never written (false safety net) | Add the management-path check, then handle the call-site config. |
| **E** | No S63 anywhere | Add hooks (member fields + management-path + on_fire ratchet update), then activate. |

VWAPReversionEngine is the canonical reference for what state A and B
look like. The full implementation is at `CrossAssetEngines.hpp:1304-1383`
— that's the template to mirror when adding S63 to a state-E engine.

### Per-engine classification as of session end

The 5 engines flagged in part-J as "S63 wired" need verification each.
We confirmed only the VWAPReversion shape this session. The other 4
are uncategorised:

| Engine | Class location | State | Notes |
|---|---|---|---|
| VWAPReversionEngine (SP/NQ/GER40/EURUSD) | CrossAssetEngines.hpp:1202 | A (GER40), B (SP/NQ/EURUSD) | Done. Don't touch. |
| IndexFlowEngine x4 (SP/NQ/US30/NAS100) | include/IndexFlowEngine.hpp | **?** | Confirmed `if (loss_cut_pct > 0.0)` check at line 419 exists. Confirmed engine_init.hpp has NO call site passing non-zero. Probably state C or D — needs audit. |
| XauusdFvgEngine | include/XauusdFvgEngine.hpp | **?** | S63 comment references at lines 129, 1063, 1067. Mgmt-path presence unverified. |
| PDHLReversionEngine | include/PDHLReversionEngine.hpp | **?** | S63 comment references at lines 59, 221. Mgmt-path presence unverified. PDHL tuning memo (part-J carryover) probably wants this completed first. |
| RSIReversalEngine | include/RSIReversalEngine.hpp | **?** | S63 references at lines 87, 571. Cold-loss-only flavor per comment. Mgmt-path unverified. |
| XauThreeBar30mEngine | include/XauThreeBar30mEngine.hpp | **?** | S63 references at lines 234, 479. Mgmt-path unverified. |

The two stop-bleed targets from S68 are:
| UstecTrendFollow5mEngine | include/UstecTrendFollow5mEngine.hpp | **E** | No S63 hooks anywhere. S37 widened the SL but did not add S63. |

And the remaining ~25 engines (FX Opens, Breakout x5, GoldStack, Bracket x13,
CandleFlow, MinimalH4 portfolio, C1Retuned, TrendPullback x2, MidScalper,
MicroScalper, MinimalH4Breakout, EMACross, H4Regime, MacroCrash,
XauTrendFollow x3, H1Swing, NBM, IndexMacroCrash x4, UstecTrendFollowHtf,
H1SwingGold...) all default to state E unless audited otherwise.

## Recommended next-session focus

In priority order:

1. **State classification of the 4 unverified S63-wired engines** (IndexFlow,
   XauusdFvg, PDHL, RSIReversal, XauThreeBar30m). 1 hour. Output: each
   classified A/B/C/D. For any state-C finds, apply settings-only flip
   (CAREFULLY — read the surrounding comments first to rule out an
   undocumented intentional disable). For any state-D finds, queue
   them for management-path addition.

2. **Resume MinimalH4Gold promotion + fresh sweep** (pop the stash,
   run the prepared workflow). See "Gold promotion: status" below.
   Independent of S63 work — the gold edit blocks nothing else.

3. **Parameter retune for VWAPReversion USTEC.F** — the documented part-L
   queued follow-up. Required before `g_vwap_rev_nq.enabled` can flip back
   to `true`. Out of scope for that 1-hour audit window; probably its own
   session given backtest sweep + WF validation.

4. **S63 for UstecTrendFollow5mEngine** — full state-E → A transition. Mirror
   the VWAPReversionEngine.hpp pattern from CrossAssetEngines.hpp:1304-1383.
   Required before `g_ustec_tf_5m.enabled` can flip back to `true`.

5. **S66-followup-2** (the original part-J focus) — still pending. Lower
   priority than S63 audit but cleanly scoped (GUI position sources only,
   no engine logic).

6. **S63 sweep across the remaining ~25 engines** — multi-session. Batch
   ~5-10 engines per session.

## Gold promotion: status

- `g_minimal_h4_gold.shadow_mode = false` edit (engine_init.hpp:777)
  is parked at `git stash@{0}`. Pop with `git stash pop` to restore.
  Build was green when stashed. **No commit ever happened.**
- Fresh-sweep workflow scaffolded:
  - `scripts/duka_to_legacy.py` — Dukascopy `timestamp_ms,askPrice,bidPrice`
    → htf_bt_minimal-format `YYYYMMDD,HH:MM:SS,bid,ask` converter. In
    working tree, untracked. Should be `git add` + committed separately
    next session (independent utility, not tied to gold promotion).
  - Input tape located: `/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`
    (4.93 GB, 154.3M rows, March 2024 → April 2026 — fresher than the
    original Sept 2023 → Sept 2025 sweep tape that's now gone).
  - Harness: `backtest/htf_bt_minimal.cpp` (standalone compile, NOT a
    CMake target). Build: `clang++ -O3 -std=c++17 -o backtest/htf_bt_minimal backtest/htf_bt_minimal.cpp`
  - Weekend handling verified: harness has hardcoded `is_weekend_gated()`
    at lines 217-228 (Fri 20:00+ through end of Sun blocked). Bar
    construction is anchored 4hr windows; weekend gaps handled correctly.
    Same logic as original 27/27 baseline run.

To resume:
```bash
cd ~/omega_repo
git stash pop                                          # restores S67 edit
git status                                              # confirm engine_init.hpp dirty + scripts/duka_to_legacy.py
# Validate untouched build:
cmake --build build --target OmegaBacktest -j 2>&1 | tail -5
# Run the fresh-sweep workflow per part-K message log (Stages 2-5).
```

Decision criteria for committing the promotion: PF ≥ 1.20 cost-pessimistic
and ≥24/27 configs profitable on the fresh tape. If fail → `git stash drop`,
no commit, and queue parameter retune for MinimalH4Gold.

Also note: there's a minor printf-literal bug in engine_init.hpp:779
that should be folded into the gold commit when it eventually lands —
the format string says `"shadow=true"` as a literal but `shadow_mode`
will be `false`. Fix: change to `"shadow=%s"` + add
`g_minimal_h4_gold.shadow_mode ? "true" : "false"` to the args list.

## Important lessons / don't-repeat

1. **Always read the comment block above an engine_init.hpp setting before
   "fixing" what looks like a 0.0 default.** Today I almost prescribed
   reverting parts K and L based on partial information. The
   30-second cost of reading the comment block above each setting saves
   undoing prior careful work.

2. **"Code written but never implemented" requires verification.** The user's
   frustration was justified in shape but wrong in target — the code WAS
   implemented (CrossAssetEngines.hpp:1304-1383), just disabled by config
   for documented reasons. Always grep for management-path usage of a
   member field before concluding it's dead code.

3. **Shadow-mode trades that "look live" in the GUI are still shadow.**
   `g_ustec_tf_5m.shadow_mode = true` means no real fills. The GUI is
   showing what would have happened. The bleed is informational, not
   capital. Action urgency calibration: minutes, not seconds.

4. **The S63 protection has *instrument-specific* validity.** It helps on
   instruments with fat-tailed losses (GER40 baseline justifies the cuts)
   and hurts on instruments with tight tails + winner-amputation risk
   (US500 baseline p95 -0.13 is too tight for the protection to add value).
   Blanket "enable everywhere" is wrong. Per-instrument backtest evidence
   is the gating criterion.

## Files modified this session — final state

```
M include/engine_init.hpp                              (S68 committed)
?? scripts/duka_to_legacy.py                            (working tree, not committed)
?? docs/handoffs/SESSION_HANDOFF_2026-05-13k.md         (working tree, not committed - this file)
```

Suggested final commits:
```bash
cd ~/omega_repo
git add scripts/duka_to_legacy.py docs/handoffs/SESSION_HANDOFF_2026-05-13k.md
git commit -m "docs+tools: part-K handoff + duka_to_legacy.py converter

Converter created for Dukascopy combined CSV -> htf_bt_minimal format
for the queued MinimalH4Gold fresh-tape sweep (parked at stash@{0}).
Standalone utility, no engine_init.hpp wiring."
git push origin main
```

(Or split the handoff doc and the converter into separate commits if
preferred.)

## Stash state at session end

```
stash@{0}: On main: S67: g_minimal_h4_gold shadow_mode=false, parked until S63 rollout complete
```

This stash contains the one-line edit to `include/engine_init.hpp:777`
(`shadow_mode = true` → `false`). It will conflict with itself if the
gold engine init block changes; if so, the resolution is to keep the
`false` value.

## Standing audit at session end

Per part-J — no engine-class additions this session. The S68 commit
modified only `enabled` boolean flags in `engine_init.hpp`. No engine
logic, no fire path, no manage block, no cost gate semantics modified.
The ungated-engine sweep expectations from part-J are unchanged:
`LatencyEdgeEngines` (S13 culled), `RSIExtremeTurnEngine` (S52 disabled),
`SweepableEngines`/`SweepableEnginesCRTP` (research-only). All other
production engines remain cost-gated. No regression this session.

`GoldEngineStack` chokepoint audit: this session did not modify
`include/GoldEngineStack.hpp`. The two-hit expectation (L50 include
comment + the gated `pos_mgr_.open()` call site) holds.

## Pre-commit checklist for next session

Before any commit:
1. `cmake --build build --target OmegaBacktest -j` is green on Mac.
2. `git diff` shows ONLY the intended changes (no whitespace drift,
   no accidental other-file edits).
3. For any engine_init.hpp settings change touching `LOSS_CUT_PCT` /
   `BE_ARM_PCT` / `BE_BUFFER_PCT` / `enabled`, the COMMENT BLOCK
   directly above the line has been read and the change is consistent
   with it (or knowingly overrides it with explicit new evidence).
4. For any engine S63 management-path addition, the change is
   accompanied by a call-site activation in the same commit. No
   more "fields exist, check never runs" commits.
5. Build → diff-review → commit → push.
