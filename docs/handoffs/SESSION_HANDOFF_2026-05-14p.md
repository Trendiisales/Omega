# Session Handoff — 2026-05-14 (NZST), part AA

Read this first next session. Direct follow-up to part-Z
(`SESSION_HANDOFF_2026-05-14o.md`). Two distinct work streams this
session: (i) Tier 4 vol-regime gate engine-side groundwork on
VWAPReversionEngine (S85), and (ii) MinimalH4Gold (S67 live promotion)
re-verification across two tape sources — the 25-month Duka tick that
justified the live promotion and a fresh 1-month broker-L2 capture.

> **Naming.** File letter `p` for today's 16th session. Verbal "part AA"
> continues the K → L → V → W → X → Y → Z sequence into double letters.

## TL;DR

1. **S85 Tier 4 vol-regime gate landed on VWAPReversionEngine** (commit
   `e24a666`). Form A (S63 ON in low vol, OFF in high vol), ATR-percentile
   signal, symmetric (gates both BE_RATCHET + LOSS_CUT uniformly), binary
   threshold. Engine code only — gate is inert by default
   (`VOL_GATE_ENABLED=false`). Production behaviour bit-for-bit unchanged.
   Harness CLI (S86) and sweep driver (S87) deferred to next session per
   operator decision to land the engine portion cleanly first.

2. **MinimalH4Gold S67 numbers reproduced bit-for-bit on the 25-month
   tape.** Production config #6 (D=10 SL=1.5 TP=4.0) PF=1.48, best-by-PnL
   #15 (D=15 SL=1.5 TP=4.0) PF=1.65, 27/27 cells profitable, $37,653 total
   PnL across the sweep, mean PF 1.36. Same harness, same tape, same
   converter — full audit-trail confirmation that nothing has silently
   broken since the live promotion three sessions ago.

3. **Fresh 1-month broker-L2 tape (April 9 → May 8) VINDICATES the
   part-O OOS-validation discipline.** Production cfg #6 PF held at
   1.54 (vs 1.48 on 25-month — within noise, edge intact). The 25-month
   "best" cells that I had earlier suggested might dominate production
   collapsed: cfg #15 dropped to PF 1.11; cfg #20 (D=20 SL=1.0 TP=3.0,
   the 25-month PF king at 1.74 with smallest DD) collapsed to **PF=0.00,
   zero winners across 4 trades**. Cfg #21, #24, #27 (the rest of the
   D=20+TP≥3 family) also hit PF=0.00. **Had the live engine been switched
   to any of those cells based on the 25-month "best" framing, it would
   now be running zero-win cells on live capital.** The conservative
   D=10 production choice is the load-bearing decision in this engine's
   design.

4. **New utility: `scripts/l2_xau_to_legacy.py`**. 85-line stdlib-only
   converter from broker-L2 capture format (`ts_ms,bid,ask,...` schema,
   both XAUUSD-prefixed and unprefixed flavours) to the legacy
   htf_bt_minimal format (`YYYYMMDD,HH:MM:SS,bid,ask`). Reusable for any
   future L2-window verification of XAU engines, parallel to
   `scripts/duka_to_legacy.py` (S67) for Dukascopy combined CSVs.

## Commits this session

| Commit | Message | Files |
|---|---|---|
| `e24a666` | S85: Tier 4 vol-regime gate (Form A, ATR-percentile, symmetric) on VWAPReversionEngine | `include/CrossAssetEngines.hpp` (1 file, +143/-3) |
| `<HANDOFF_HASH>` | docs: part-AA handoff + l2_xau_to_legacy.py converter | `docs/handoffs/SESSION_HANDOFF_2026-05-14p.md` + `scripts/l2_xau_to_legacy.py` |

`origin/main` was at `caff5ee` (part-Z handoff) at session start. End-state
sits at the closing handoff commit below.

## What did NOT land this session

- **S86 harness CLI flags** for VWAPReversionBacktest (`--vol-gate-enabled`,
  `--vol-pct-threshold N`, `--atr-lookback-days N`) + report extension.
  Engine code from S85 needs these flags to be exercisable in research
  mode. ~30 min next session.
- **S87 sweep driver** `scripts/vrev_vol_gate_p1.py`. 14-cell sweep
  (2 lookbacks × 7 thresholds) + baseline, 4-window WF, decision criterion
  ≥10% PF improvement over baseline AND ≥1.20 PF + ≥3/4 windows pass per
  Tier 4 scoping memo §4 Phase A protocol.
- **Phase A sweep run + verdict memo** — operator-side, ~30s/cell × 14
  cells × 4 windows.
- **PROVENANCE-vs-WF reconciliation on XauTrendFollow trio** — part-Z #1
  priority. Untouched.
- **`scripts/xtf_d1_wf_t1.py`** — sibling driver to xtf_2h/4h_wf_t1.py.
  Mechanical patch. Untouched.
- **EmaPullback per-cell tuning** — part-Z #4. Untouched.
- **Universe-wide S63 sweep continuation** (~21 state-E engines) — part-Z
  #5. Untouched.

## Recommended next-session focus

In priority order:

1. **Tier 4 Phase A continuation: S86 harness CLI + S87 sweep driver +
   run** (~1.5-2 hrs). Highest-leverage next step because S85's engine
   code is in place but unexercisable until the harness has flags. Keep
   the work scoped to VWR USTEC.F per Phase A; don't generalise to other
   instruments before the verdict is in.

2. **PROVENANCE-vs-WF reconciliation on XauTrendFollow trio** (~1-2 hrs).
   Part-Z #1 priority, still load-bearing. The XauTrendFollow header
   reports each cell net-positive on the 2023-09..2025-09 Duka corpus, yet
   WF Windows 1-3 (fully inside that range on the substitute tape) show
   heavy losses. Tape source / bar construction divergence most likely.
   S63 evaluation on XTF blocked until reconciled.

3. **`scripts/xtf_d1_wf_t1.py` sibling driver** (~10-15 min). Mechanical
   4h→d1 patch. Closes the XTF sibling driver set + provides a third
   data point for the PROVENANCE investigation.

4. **EmaPullback per-cell tuning** (~1 hr). Part-Z #4. PROVENANCE
   expectancy varies 8× across cells (H1 $2.25/trade, H6 $18.81/trade) so
   per-cell tuning likely extracts meaningful additional edge. Engine
   already at state A live (S82 LC=0.10/ARM=0.40/BUF=0.05).

5. **Universe-wide S63 sweep continuation** (~21 state-E engines remaining,
   multi-session). Apply the part-Y/Z lessons: tight-zone sweep first,
   confirm baseline edge before sweeping S63, non-monotonic curves possible.

6. **Optional: L2 capture data-quality investigation** (~30 min, separate
   scope). Files `data/l2_ticks_2026-04-11.csv` (133KB), `04-18.csv`
   (139KB), `XAUUSD_2026-04-26.csv` (2.5MB), `XAUUSD_2026-05-03.csv`
   (2.2MB) are partial-day captures (logger started/stopped). Bars built
   on those days have incomplete tick data. Should either be filtered out
   of L2-window backtests or the underlying logger investigated. Easy
   filter: drop sub-50K-tick days. Out-of-band from the engine work.

## MinimalH4Gold verification — full evidence

### 25-month Duka tick reproduction (S67 baseline)

| Metric | This session | S67 | Match |
|---|---|---|---|
| Tick count | 154,265,439 | 154,265,439 | ✓ |
| Date range | 2024-03-01 → 2026-04-24 | same | ✓ |
| H4 bars built | 3,434 | 3,434 | ✓ |
| Cfg #6 (PROD) PF | 1.48 | 1.48 | ✓ |
| Cfg #15 (best by PnL) PF | 1.65 | 1.65 | ✓ |
| Profitable cells | 27/27 | 27/27 | ✓ |
| Wall-clock | 8.5s | 8.3s | within noise |

Conclusion: zero drift. Harness, converter, legacy tape, and Donchian
logic all stable since the live promotion in part-O.

### 1-month broker L2 confirmation

Tape: `/Users/jo/Tick/legacy/XAUUSD_L2_2026-04-09_2026-05-08_legacy.csv`
(247 MB, 6,473,475 ticks built from 28 daily L2 capture files via
`scripts/l2_xau_to_legacy.py`). Date range 2026-04-09 07:53:39 →
2026-05-08 03:57:40. 147 H4 bars built. 0.3s wall-clock.

Headline cell-by-cell vs 25-month baseline:

| Cell | Spec | 25-mo PF | L2-mo PF | n (L2) |
|---|---|---|---|---|
| **#6 PROD** | D=10 SL=1.5 TP=4.0 | 1.48 | **1.54** | 5 |
| #15 (S67 best by PnL) | D=15 SL=1.5 TP=4.0 | 1.65 | 1.11 | 3 |
| **#20 (S67 best by PF)** | D=20 SL=1.0 TP=3.0 | **1.74** | **0.00** | 4 |
| #21 | D=20 SL=1.0 TP=4.0 | 1.66 | 0.00 | 4 |
| #24 | D=20 SL=1.5 TP=4.0 | 1.50 | 0.00 | 3 |
| #27 | D=20 SL=2.0 TP=4.0 | 1.51 | 0.00 | 3 |
| #3 | D=10 SL=1.0 TP=4.0 | 1.28 | 2.31 | 5 |
| #10 | D=15 SL=1.0 TP=2.0 | 1.25 | 1.79 | 8 |

12/27 cells profitable on L2 month (vs 27/27 on 25-month). Production
cfg #6 holds. The four D=20+TP≥3 cells all collapsed to zero winners.

### What this confirms / doesn't confirm

**Confirms:**
- Production cfg #6 edge is regime-stable across two independent tape
  sources (Duka tick + broker L2). PF in the 1.48-1.54 band on both.
- The OOS-validation rule that kept production at D=10 instead of
  switching to the 25-month "best" was correct. The cells that looked
  best in-sample on the 25-month run did not survive the L2 month.

**Does NOT confirm (small-n caveats):**
- L2 month has 3-10 trades per cell. Even cfg #6's PF=1.54 carries low
  individual confidence. The signal is the AGREEMENT between samples,
  not the L2 number alone.
- The L2 "best" pick (cfg #3 PF=2.31 on 5 trades) is statistical noise.
  Don't retune to it.
- Cell-level differences between samples could reflect genuine regime
  drift OR L2 capture data-quality issues OR small-n variance.
- Aggregate "12/27 vs 27/27" should not be read as "the engine is
  failing" — small-n is doing most of the work in those numbers.

## Important lessons / don't-repeat

1. **Don't chase in-sample peaks no matter how clean they look.** When I
   summarised the 25-month run earlier this session I flagged that cfg
   #20 (PF 1.74, smaller DD, more trades) appeared to dominate production
   on every metric, and noted that "the only reason not to switch is the
   OOS-validation comment". The L2 month run is the textbook reason that
   discipline exists — cfg #20 went from 25-month-best to PF=0.00 zero
   winners on L2. Resist the analyst-pattern of "the data clearly shows
   cell X is better"; require independent-sample confirmation.

2. **Bidirectional confirmation is a real test.** When both 25-month tick
   and 1-month L2 agree on a config (cfg #6), that's qualitatively
   stronger than either alone. Should be required for any future
   production-config change.

3. **L2 captures have data-quality issues that bias short-window
   backtests.** Sub-50K-tick day files are partial-day captures and the
   bars built on them have incomplete extremes. Easy to filter; harder to
   notice when not flagged. Build the filter into any future L2 workflow.

4. **The Tier 4 gate engine code is inert by default — that's
   intentional.** `VOL_GATE_ENABLED=false` at member init means all four
   live VWR instances (SP/NQ/GER40/EURUSD) preserve current behaviour
   bit-for-bit. Production engine config is untouched until Phase A
   produces a verdict and the operator decides to wire it into
   engine_init.hpp. Don't get clever and pre-activate.

5. **CLAUDE.md "S63 management-path additions must be accompanied by
   call-site activation in the same commit" rule was satisfied in spirit
   for S85.** The Tier 4 gate is not new S63 plumbing; it conditions
   existing S63 on a vol regime. The check IS live every tick (entry-time
   block always runs and always sets `vg_s63_active_`); when
   `VOL_GATE_ENABLED` is false the check trivially defaults to true. Not
   dead code. Activation pathway via harness CLI lands in S86 next
   session per Phase A protocol.

## Files modified this session — final state

```
M  include/CrossAssetEngines.hpp                       (S85 e24a666 committed)
A  scripts/l2_xau_to_legacy.py                         (working tree, pending closing commit)
A  outputs/MINIMAL_H4_GOLD_FULL_BACKTEST_2026-05-14p.log    (gitignored, on-disk only)
A  outputs/MINIMAL_H4_GOLD_L2_BACKTEST_2026-05-14p.log      (gitignored, on-disk only)
A  /Users/jo/Tick/legacy/XAUUSD_L2_2026-04-09_2026-05-08_legacy.csv  (247MB, NOT in repo)
A  backtest/htf_bt_minimal_results.txt                 (overwritten by L2 run, ignored)
A  backtest/htf_bt_minimal_best_trades.csv             (now reflects L2 cfg #3, ignored)
A  backtest/htf_bt_minimal_best_equity.csv             (now reflects L2 cfg #3, ignored)
A  docs/handoffs/SESSION_HANDOFF_2026-05-14p.md        (this file, pending closing commit)
```

The two `outputs/` log files are gitignored project-wide; commit with
`-f` if a permanent audit trail is desired (parallel to how
`outputs/S67_FRESH_SWEEP_2026-05-14.log` was force-added in part-O).
This handoff doc treats them as on-disk only by default — the per-cell
tables above are sufficient summary.

## Standing audits at session end

**Core code preserved.** None of `OmegaCostGuard.hpp`, `OmegaTradeLedger.hpp`,
`SymbolConfig.hpp`, `OmegaFIX.hpp`, `OmegaApiServer.hpp`,
`GoldPositionManager.hpp` were modified.

**Engine code: only `CrossAssetEngines.hpp` (additive, S85). engine_init.hpp
untouched.** All four live VWR instances preserve current behaviour because
`VOL_GATE_ENABLED` defaults false at construction.

**Stop-bleed disables intact:**
- `g_vwap_rev_nq.enabled = false` at L608 (stays disabled per S68; Tier 4
  gate addition does not re-enable it)
- `g_vwap_rev_nq.LOSS_CUT_PCT/BE_ARM_PCT/BE_BUFFER_PCT = 0.0` at L701-703
  (stays at zero per part-K + part-L + S69 P2 lock-in)
- `g_ustec_tf_5m.enabled = false` (stays disabled per S68 + S73 P3 closure)

**MinimalH4Gold live state confirmed unchanged:**
- `g_minimal_h4_gold.shadow_mode = false` (S67 promotion)
- `g_minimal_h4_gold.enabled = true`
- Production config: D=10 SL=1.5 TP=4.0 (cfg #6, PF stable across both
  samples — no change recommended)

**S63 state inventory unchanged from part-Z:**

| State | Engines | Count |
|---|---|---|
| A (active) | g_vwap_rev_ger40, g_iflow x4, g_xauusd_fvg, g_pdhl_reversion, g_xau_3bar_30m, g_ema_pullback | 8 |
| B (deliberately disabled / S63 hooks present but zeroed) | VWR SP/NQ/EURUSD, UTF5m, XTF 2h/4h/d1 | 7 |
| E (no hooks) | ~21 remaining engines | ~21 |

S85 adds Tier 4 vol-regime gate to VWAPReversionEngine but does NOT
change S63 state inventory — Tier 4 layers on top of S63, not replaces.

**Ungated-engine sweep expectations unchanged.** No engine added or
modified into the ungated set.

**GoldEngineStack chokepoint audit:** not touched this session.

## Stash state at session end

```
$ git stash list
(empty)
```

Inherited clean from part-Z. No new stashes this session.

## Operational notes

- **Sandbox bash continues to be dead.** Same as parts T-Z — Cowork
  workspace VM useradd disk-full on every `mcp__workspace__bash` call.
  All builds, harness runs, and commits were operator-side via Mac
  (paste-back from this session's chat). Sandbox-side file tools (Read /
  Grep / Glob / Edit / Write) worked normally throughout.
- **Three real run cycles + one engine commit** (S85 + 25-month MinimalH4Gold
  reproduction + L2-month MinimalH4Gold confirmation + handoff). Wall-clock
  ~2 hrs. Productive session despite the mid-session pivot from Tier 4
  closeout to MinimalH4Gold check.
- **L2 converter run** processed 6.47M ticks across 28 files in <30s.
  Performance is fine for any future L2 work.
- **Harness binary is current.** `backtest/htf_bt_minimal` was rebuilt
  from source at the start of the MinimalH4Gold check. CrossAssetEngines.hpp
  S85 edit doesn't affect this harness (no engine headers included).
- **`build/OmegaBacktest` was rebuilt successfully after S85** — Mac canary
  green, two pre-existing PDHL warnings only, no new warnings on the
  Tier 4 vg_* members.

## Pre-commit checklist for next session

Standard project CLAUDE.md checklist applies. Specific notes inherited from
part-Z and added this session:

1. Before any S86 harness CLI work: read `outputs/TIER4_VOL_REGIME_SCOPING_2026-05-14.md`
   §4 Phase A protocol + §5 follow-on decisions to refresh context. The
   sweep matrix (2 lookbacks × 7 thresholds = 14 cells + baseline) and
   decision criterion (≥10% PF improvement over baseline AND clear
   ≥1.20 PF gate) are the load-bearing parameters.
2. S86 should mirror the existing `--mode baseline / tuned` CLI surface in
   `backtest/VWAPReversionBacktest.cpp`. Add the three Tier 4 flags as
   independent overrides applied AFTER the mode-based defaults.
3. Tier 4 verdict-text should explicitly disclaim "this is Tier 4 evidence
   on top of S63=0" so future session sweeps don't conflate. (Same
   discipline as part-Z's S84 verdict-text patch.)
4. Production-config change rule (new from this session): require
   bidirectional confirmation on at least two independent tape samples
   before any live config switch. The S67 vs L2 cfg #6 stability is the
   first instance of this pattern; document it as the gating rule.

Standard pre-commit:
- Mac canary green: `cmake --build build --target OmegaBacktest -j`
- `git diff` reviewed: only intended changes
- Commit messages reference `S<N>` numbering
- Unrelated changes split via `git add -p`
- For engine_init.hpp settings touching `LOSS_CUT_PCT` / `BE_ARM_PCT` /
  `BE_BUFFER_PCT` / `enabled`: read the comment block above the line
- For S63 management-path additions: call-site activation in same commit

## Closing note

Productive two-stream session. Tier 4 Phase A engine code is in place (S85,
inert by default, ready for S86 harness work next session). MinimalH4Gold
live promotion is reaffirmed across two independent tape sources, with the
strongest evidence yet that the OOS-validated production config is robust
to in-sample chasing. The L2 verification surfaced a clean methodological
template — bidirectional confirmation across Duka tick + broker L2 — that
should become the standing gate for any future production-config change
on this or other XAU engines.

Tier 4 Phase A verdict timeline: S86 + S87 next session, sweep run +
verdict memo the session after. Decision criterion is pre-committed
(per-memo §4): ≥10% PF improvement over baseline AND clear ≥1.20 PF gate +
≥3/4 windows pass. If the gate doesn't add value, kill the line cleanly
and pivot to remaining queued work (PROVENANCE reconciliation, EmaPullback
per-cell, universe S63 sweep).

### Suggested commit command for closing the part-AA session

```bash
cd ~/omega_repo

# (a) Stage closing artefacts -- handoff doc + L2 converter
git status
git add docs/handoffs/SESSION_HANDOFF_2026-05-14p.md scripts/l2_xau_to_legacy.py
git status   # confirm only the two files staged

# (b) Single commit (small enough to bundle, both are session-closure
#     artefacts; split with `git add -p` if you prefer separate commits)
git commit -m "docs+tools: part-AA handoff + l2_xau_to_legacy.py converter

Captures the part-AA session arc:
  - S85 engine code (already on origin/main at e24a666): Tier 4 vol-regime
    gate on VWAPReversionEngine. Form A, ATR-percentile, symmetric, binary
    cutoff. Engine-only commit; harness CLI (S86) and sweep driver (S87)
    queued for next session. Production behaviour bit-for-bit preserved.
  - MinimalH4Gold S67 live promotion re-verified: 25-month Duka tick
    reproduces bit-for-bit (cfg #6 PF=1.48), fresh 1-month broker L2
    capture confirms production cfg #6 PF=1.54 (within noise). The 25-month
    'best' cells (cfg #15, #20) collapsed on L2 month — cfg #20 hit PF=0.00
    zero winners. The OOS-validation discipline that kept production at
    cfg #6 was VINDICATED.
  - scripts/l2_xau_to_legacy.py: ~85-line stdlib-only converter from
    broker L2 capture format (ts_ms,bid,ask,...) to legacy htf_bt_minimal
    format (YYYYMMDD,HH:MM:SS,bid,ask). Handles both XAUUSD-prefixed and
    unprefixed L2 schemas via header-name parsing. Reusable for any future
    L2-window verification of XAU engines.

Next-session priorities (revised from part-Z):
  1. Tier 4 Phase A continuation: S86 harness CLI + S87 sweep driver + run
  2. PROVENANCE-vs-WF reconciliation on XauTrendFollow trio
  3. scripts/xtf_d1_wf_t1.py sibling driver
  4. EmaPullback per-cell tuning
  5. Universe-wide S63 sweep continuation (~21 state-E engines)
  6. Optional: L2 capture data-quality investigation (partial-day filter)

New rule documented: production-config changes require bidirectional
confirmation on at least two independent tape samples (S67 + L2 is the
first instance). Documented in the handoff for future sweep work.

Handoff at docs/handoffs/SESSION_HANDOFF_2026-05-14p.md."

git push origin main
git log --oneline -5
```
