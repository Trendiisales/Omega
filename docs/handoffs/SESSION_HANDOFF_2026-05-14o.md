# Session Handoff — 2026-05-14 (NZST), part Z

Read this first next session. Direct follow-up to part-Y
(`SESSION_HANDOFF_2026-05-14n.md`). Part-Z closed out the XTF 4h
harness fill-in + WF execution carry-over from part-X/Y and ran the
2h sibling for cross-reference. Both verdicts came in decisively FAIL,
but the actual research question opened by this session is **not** the
S63 generalisation framing the drivers' boilerplate suggests — it's a
PROVENANCE-vs-WF contradiction on the substitute tape.

> **Naming.** Same convention as parts K → L → V → W → X → Y → Z.
> Part-Y = `n`. Part-Z = `o`.

## TL;DR

1. **S83 landed (commit `b54f60b`)**. `XauTrendFollowBacktest.cpp`
   4h+d1 dispatch fill-in (`run_4h_engine` + `run_d1_engine` mirror
   `run_2h_engine` with the relevant bar-type / bucket / signature
   substitutions) + `scripts/xtf_4h_wf_t1.py` driver + `.gitignore`
   patterns for `/epb_*.csv` `/epb_*.txt` `/xtf_trades.csv`
   `/xtf_report.csv`. Build green Mac-side; smoke tests on both
   `--engine 4h` and `--engine d1` produced valid reports with
   correct engine_name + S63 fields at the 30000-tick limit.

2. **XTF 2h + 4h Phase 3 walk-forward both FAIL decisively.** Not a
   near-miss. Tape: the part-K substitute tape
   `/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`,
   784.8 days, 4-window WF, `--mode baseline` (S63 = 0/0/0):

   | timeframe | windows pass | agg PF | agg avg_pnl | trades |
   |---|---|---|---|---|
   | 2h | 1/4 | 0.724 | -0.147 | 846 |
   | 4h | 0/4 | 0.407 | -0.506 | 945 |

   Decision threshold: aggregate PF ≥ 1.20 AND ≥ 3/4 windows
   `avg_pnl ≥ +0.001`. Engines remain at class-default state-B
   values (S63 OFF) — and should stay there.

3. **The drivers' verdict-text boilerplate is misleading.** Both
   ran `--mode baseline` (S63 trio all 0.0), so the FAIL cannot
   be evidence about S63 specifically — S63 was OFF throughout.
   The boilerplate claiming "S63-adverse pattern from VWR USTEC
   / UTF5m USTEC generalises to XAU trend-follow" is incorrect
   for any baseline run. Part-X carry-over #6 had already flagged
   this. Patched both drivers this session; the new text frames
   the FAIL as baseline-engine underperformance and lists the
   investigation order (reconcile PROVENANCE first, defer S63
   sweep until baseline edge restored). Patches pending the
   S84 commit (see "Files modified this session" below).

4. **The PROVENANCE-vs-WF contradiction is the load-bearing
   research finding.** `XauTrendFollow4hEngine.hpp:7-25` reports
   each cell net positive on the 3-year Duka corpus 2023-09 ..
   2025-09. WF Windows 1-3 (2024-03 .. 2025-10) are **fully
   inside** that range yet show heavy losses (4h W1 PF=0.220,
   W2 PF=0.189, W3 PF=0.893 — all negative avg_pnl). Either
   the original PROVENANCE corpus diverges from the substitute
   tape in source / bar construction / cost model, or the
   harness / engine has drifted since PROVENANCE was measured.
   This needs answering before any further XTF S63 work
   proceeds — S63 evaluation on a losing baseline is
   structurally moot.

5. **Window 4 2h-vs-4h divergence is regime intelligence.**
   Window 4 (2025-10-10 .. 2026-04-24, the recent XAU rally)
   shows 2h passing (PF 2.088, 201 trades, avg_pnl +0.531)
   while 4h fails harder than any other 4h window (PF 0.454,
   227 trades, avg_pnl -0.735). At similar trade counts the
   sign inversion is not small-n. Useful supporting evidence
   for the Tier 4 vol-regime gate work (part-Y next-session
   item #3).

## Commits this session

| Commit | Message | Files |
|---|---|---|
| `b54f60b` | S83: XTF 4h+d1 harness dispatch fill-in + xtf_4h_wf_t1 driver | backtest/XauTrendFollowBacktest.cpp + scripts/xtf_4h_wf_t1.py + .gitignore |
| `<S84_HASH>` | S84: XTF 2h+4h Tier 1 Phase 3 WF verdicts (both FAIL) + driver verdict-text fix | outputs/XTF_2H_4H_TIER1_PHASE3_RESULTS_2026-05-14.md (force-add) + scripts/xtf_2h_wf_t1.py + scripts/xtf_4h_wf_t1.py |
| `<HANDOFF_HASH>` | docs: part-Z handoff | docs/handoffs/SESSION_HANDOFF_2026-05-14o.md |

`origin/main` end-state at handoff commit. (S83 already on `origin/main`
at session-write time; S84 + handoff pending the closing commit
sequence below.)

## What did NOT land this session

- **`scripts/xtf_d1_wf_t1.py`** — mechanical "4h" → "d1" patch of the
  4h driver. The harness side is ready (`run_d1_engine` shipped in
  S83). Even with D1 expected to FAIL given W1-W3 cross-timeframe
  consistency, completing the sibling driver set closes out the
  part-Y item cluster cleanly. ~5-line patch.
- **PROVENANCE-vs-WF reconciliation** — surfaced this session, not
  investigated. Now the top XTF priority before any further S63 work
  on the trio.
- **Tier 4 Phase A on VWR USTEC.F** — part-Y next-session item #3.
  Untouched. Still the highest-leverage research track independent
  of XTF.
- **EmaPullback per-cell tuning** — part-Y next-session item #4.
  Untouched.
- **Universe-wide S63 sweep continuation** (~21 state-E engines)
  — part-Y next-session items #5/#7. Untouched.

## Recommended next-session focus

In priority order:

1. **PROVENANCE-vs-WF reconciliation on XauTrendFollow4hEngine**
   (~1-2 hours, possibly longer). Find or reconstruct the original
   PROVENANCE corpus (3-year Duka 2023-09-27 .. 2025-09-26 per
   `XauTrendFollow4hEngine.hpp:9-11`), re-run
   `XauTrendFollowBacktest --engine 4h --mode baseline` on it, and
   compare aggregate to PROVENANCE's expected cell-level net values
   (Donchian +$1332, InsideBar +$1124, ER0.20 +$840 over 3 years).
   Hypotheses to test, in prior-probability order:
     a. **Different tape source.** Original Duka corpus is gone per
        part-K; substitute tape may have different broker / spread /
        gap characteristics. Easiest to confirm: spread distribution
        comparison if any sample of the original tape survives.
     b. **Bar-construction divergence.** Harness uses UTC-aligned
        4-hour buckets (`h4_bucket_ms` at
        `XauTrendFollowBacktest.cpp:367-369`); PROVENANCE may have
        used true H4 candles from a market-data feed. Session-
        boundary and weekend-gap handling could differ enough to
        invert edges.
     c. **Cost / fill divergence.** PROVENANCE: "$0.06/RT cost
        subtracted, 0.01 lot." Harness reports gross_pnl, no cost
        subtraction. $0.06 × 945 trades ≈ $57 — cannot account for
        the ~$478 PnL swing. Cost model is unlikely to be the
        explanation but worth confirming.
     d. **Engine drift since PROVENANCE.** S33g/h/i (2026-05-11)
        retuned cell R:R from the original PROVENANCE values.
        New sl/tp were Pass-4..6 deep_dive optimised on what was
        presumably the same tape source — should not explain a
        tape-vs-tape regression but does mean the *current* cells
        are S33-tuned descendants, not strict PROVENANCE cells.

2. **Tier 4 Phase A on VWR USTEC.F** — part-Y next-session item #3.
   Read `outputs/TIER4_VOL_REGIME_SCOPING_2026-05-14.md` end-to-end,
   work §7 pre-implementation checklist with operator, start the
   `VWAPReversionEngine` ATR-percentile gate edit. Independent of
   XTF; remains highest-leverage research track. The XTF Window 4
   2h-vs-4h divergence adds small supporting evidence that per-
   timeframe regime sensitivity is real on XAU trend-follow — a
   uniform LOSS_CUT / BE_RATCHET overlay would smear over it,
   which is part of the Tier 4 motivation.

3. **Create `scripts/xtf_d1_wf_t1.py`** (~10-15 min). Mechanical
   "4h" → "d1" patch of `scripts/xtf_4h_wf_t1.py`. Add a per-window
   trade-count floor for the small-n D1 cadence (see
   `XauTrendFollowD1Engine.hpp:174-179` — ~2 trades/month combined
   across 3 cells over 30 months). Run it; expected FAIL given the
   W1-W3 cross-timeframe consistency, but completes the sibling
   driver set and provides a third data point for the PROVENANCE-
   vs-WF investigation.

4. **EmaPullback per-cell tuning** (part-Y next-session item #4).
   Lower priority than the XTF investigation but cleanly scoped.
   ~1 hour of harness work to add per-cell override CLI flags +
   sweep, then another sweep run. PROVENANCE expectancy varies 8×
   across cells (H1 $2.25/trade, H6 $18.81/trade) so per-cell
   tuning likely extracts meaningful additional edge.

5. **Universe-wide S63 sweep continuation** (~21 state-E engines
   remaining). Multi-session. Batch ~5-10 per session, applying
   the part-Y lessons: tight-zone sweep first, operational-band
   testing, non-monotonic curves possible. The XTF result this
   session is a cautionary data point — running an S63 sweep on
   an engine whose baseline has no edge on the test tape would
   produce misleading "S63 hurts" or "S63 helps" conclusions
   that don't generalise. Confirm baseline edge before sweeping.

## Important lessons / don't-repeat

1. **A FAIL on `--mode baseline` is not evidence about S63.** Both
   XTF WF runs hard-pin `--mode baseline` (S63 trio = 0/0/0), so
   the FAIL is intrinsic baseline-engine underperformance. The
   drivers' boilerplate verdict-text claiming "S63-adverse pattern
   generalises" conflates two distinct findings. Patched in
   `scripts/xtf_2h_wf_t1.py` + `scripts/xtf_4h_wf_t1.py` this
   session. **Lesson for next-session sweep work:** for any future
   WF gate that runs baseline-only, the verdict implication must
   frame the FAIL as "baseline engine has no edge on this tape,"
   not "S63 adverse." S63 evidence requires a tuned-mode comparison
   against a baseline that has edge.

2. **PROVENANCE-vs-test-tape contradictions need to be resolved
   before downstream work.** The headline result this session is
   not the WF FAIL itself — it's that the WF FAIL contradicts the
   engine header's PROVENANCE block on what should be an overlapping
   period. Resolving that contradiction is gating: S63 work, Phase
   1 sweep work, and any tuning decisions all depend on knowing
   whether the baseline engine actually has the edge PROVENANCE
   claims, on the tape we're testing on. Generalises beyond XTF —
   any time a backtest disagrees with header-documented PROVENANCE
   on a supposedly-overlapping period, the divergence is the
   research question, not the WF gate.

3. **The drivers' verdict-text shipped in part-W made an unconditional
   claim about S63 from a baseline-only run.** Part-X carry-over #6
   flagged this; part-Y didn't get to it; part-Z patched it. The
   broader lesson: any boilerplate text that interprets a sweep
   result needs to be conditioned on what the sweep actually tested.
   `--mode baseline` (S63 OFF) cannot produce S63 evidence — and
   the verdict shouldn't claim to. Future driver / sweep / harness
   text should follow this rule.

4. **`outputs/` is gitignored project-wide.** Closure memos in
   `outputs/` need `git add -f` to track. Run artefacts in
   `outputs/xtf_*_t1_p3_<ts>/` stay on disk only by default — the
   harness binary and driver script in git are authoritative, not
   the per-run artefacts. Confirmed this session when the
   `outputs/XTF_4H_TIER1_PHASE3_RESULTS_TEMPLATE.md` template
   created in S83's working-tree did NOT make the b54f60b commit
   despite `git add` (silently ignored — caught by `git status`
   warning).

## Files modified this session — final state

```
M  backtest/XauTrendFollowBacktest.cpp      (S83 b54f60b committed)
M  .gitignore                                (S83 b54f60b committed)
A  scripts/xtf_4h_wf_t1.py                   (S83 b54f60b committed)

M  scripts/xtf_2h_wf_t1.py                   (S84 pending -- verdict-text patch)
M  scripts/xtf_4h_wf_t1.py                   (S84 pending -- verdict-text patch)
A  outputs/XTF_2H_4H_TIER1_PHASE3_RESULTS_2026-05-14.md  (S84 pending, needs -f)
A  outputs/XTF_4H_TIER1_PHASE3_RESULTS_TEMPLATE.md       (gitignored, on-disk only)
A  outputs/xtf_2h_t1_p3_20260514_191707/     (WF run artefacts, on-disk only)
A  outputs/xtf_4h_t1_p3_20260514_191218/     (WF run artefacts, on-disk only)
A  outputs/xtf_2h_t1_p3_20260514_192117/     (second 2h run, on-disk only)
A  docs/handoffs/SESSION_HANDOFF_2026-05-14o.md          (this file, pending commit)
```

Untracked debris from prior sessions (part-Y leftovers + earlier):
```
?? .agents/
?? baseline_report.csv
?? baseline_trades.csv
?? full_baseline_report.csv
?? full_baseline_trades.csv
?? full_tuned_report.csv
?? full_tuned_trades.csv
?? skills-lock.json
?? skills-main/
?? utf5m_report.csv
?? utf5m_trades.csv
?? xtf_2h_sanity_report.csv
?? xtf_2h_sanity_trades.csv
```

These are all per-run scratch from earlier sessions; the part-Z
.gitignore patterns for `/xtf_trades.csv` `/xtf_report.csv` `/epb_*`
will catch future scratch but won't retroactively ignore these. Can
be safely `rm` next session if the operator wants a clean working
tree.

## Standing audits at session end

**Core code preserved.** None of `OmegaCostGuard.hpp`,
`OmegaTradeLedger.hpp`, `SymbolConfig.hpp`, `OmegaFIX.hpp`,
`OmegaApiServer.hpp`, `GoldPositionManager.hpp` were modified.

**Engine code: only harness + drivers modified.** No `*Engine.hpp`
touched this session. `engine_init.hpp` untouched.

**Stop-bleed disables intact:**
- `g_vwap_rev_nq.enabled = false` (verified L608 — part-Y note carried over)
- `g_ustec_tf_5m.enabled = false` (verified L1009 — part-Y note carried over)

**Ungated-engine sweep expectations unchanged.** No engine added or
modified into the ungated set.

**GoldEngineStack chokepoint audit:** not touched this session.

**S63 state inventory unchanged from part-Y:**
| State | Engines | Count |
|---|---|---|
| A (active) | g_vwap_rev_ger40, g_iflow x4, g_xauusd_fvg, g_pdhl_reversion, g_xau_3bar_30m, g_ema_pullback | 8 |
| B (deliberately disabled / S63 hooks present but zeroed) | VWR SP/NQ/EURUSD, UTF5m, XTF 2h/4h/d1 | 7 |
| E (no hooks) | ~21 remaining engines | ~21 |

XTF 2h/4h/d1 confirmed-state-B as before; the WF FAIL did NOT shift
any of them, because the FAIL is about baseline-engine performance,
not about S63 evaluation. No promotion or demotion this session.

## Stash state at session end

```
$ git stash list
(empty)
```

Inherited clean from part-Y. No new stashes this session.

## Operational notes

- **Sandbox bash continues to be dead.** Same as parts T-Y — Cowork
  workspace VM useradd disk-full on every `mcp__workspace__bash`
  call. All builds, harness runs, WF executions, and commits were
  operator-side via Mac (paste-back from this session's chat).
  Sandbox-side file tools (Read / Grep / Glob / Edit / Write)
  worked normally throughout.
- **Two real commits + one handoff commit** (S83 done, S84 + handoff
  pending the closing sequence below). Wall-clock ~1.5-2 hours.
  Significantly shorter than part-Y's 3-4 hours because the harness
  fill-in was a clean mechanical mirror of the 2h dispatch path,
  the driver was a clean mechanical adaptation, and the WF results
  were unambiguous (no iteration through wrong shapes like part-Y).
- **`cmake -S . -B build` re-configure was NOT needed this session.**
  The CMake target `XauTrendFollowBacktest` was already in
  `CMakeLists.txt` from part-W; only the source file changed, so
  the incremental build hit the existing target cleanly.
- **WF wall-clock was ~30s per timeframe** (faster than the ~60-70s
  the 2h driver header estimated, presumably because Mac SSD
  throughput on a 5GB tick CSV is excellent). 4h ran first
  (`outputs/xtf_4h_t1_p3_20260514_191218/`), then 2h ran for
  cross-reference (`outputs/xtf_2h_t1_p3_20260514_191707/`), then a
  second 2h re-run (`outputs/xtf_2h_t1_p3_20260514_192117/` — same
  result, the operator re-ran after I asked for cross-reference;
  the patched driver was edited AFTER the second 2h run so the
  printed verdict text reflects the pre-patch boilerplate). All
  three artefact directories live on disk only (gitignored).

## Pre-commit checklist for next session

Standard project CLAUDE.md checklist applies. Specific notes
inherited from part-Y / Z for the recommended item 1 (PROVENANCE-vs-WF
investigation):

1. Before any conclusion about XTF cell viability, identify which
   tape (or tape construction methodology) PROVENANCE was measured
   on. The line `XauTrendFollow4hEngine.hpp:9-11` cites "3-year
   Dukascopy 2023-09-27 -> 2025-09-26 + 1-month XAU L2 2026-04-09
   -> 2026-05-08". The 1-month L2 sample may still exist on disk
   somewhere; if so, that's the simplest cross-check.

2. If PROVENANCE corpus is unrecoverable, the next-best evidence is
   to run the same harness against any other XAU tape source the
   operator has access to (a BlackBull tick dump for a different
   period, a HistData m1 export, etc.) and check whether the cells
   land near PROVENANCE expectancy on something other than the
   substitute tape. If they do, the substitute tape is the
   divergent variable. If they don't, the harness or engine has
   drifted.

3. Resist the temptation to retune the cells to fit the substitute
   tape. The PROVENANCE corpus was validated; retuning would discard
   the prior research. Better: find the divergence first, then
   decide whether to fix the tape, fix the harness, or accept tape-
   specific cells.

4. The driver verdict-text patches landed this session (S84 pending)
   should NOT need further iteration — they explicitly disclaim S63
   evidence on baseline runs. If a future sweep run uses
   `--mode tuned` or explicit S63 overrides, the verdict text will
   need separate handling for that case (currently the boilerplate
   only fires for baseline; tuned-mode adds different text). Out of
   scope until a tuned-mode WF actually runs.

Additionally, inherited from part-Y for any future S63 activation:

5. Read the cross-engine LOSS_CUT cluster first:
   `Grep("LOSS_CUT_PCT\\s*=\\s*[0-9]", "include/*.hpp")`.
6. Grid the engine harness's `--sweep` in the operational band of
   same-family engines (XAU 0.04-0.05%, indices 0.07-0.08%, FX
   0.03%), NOT arbitrary coarse values.
7. If first sweep concludes "LC hurts," re-grid at tighter LC
   before generalising. Curves can be non-monotonic.
8. Wire S63 hooks into ALL management paths the engine has (check
   for both `on_bar` and `on_tick`). S78 caught this for EmaPullback;
   future engines may have the same dual-path shape.
9. Confirm baseline edge on the test tape before any S63 sweep.
   Part-Z lesson: S63 evaluation on a losing baseline is
   structurally moot.

## Closing note

Part-Z was a clean, mechanically-scoped session. The XTF 4h harness
fill-in worked first try (build green, smoke tests valid). The WF
verdicts came in immediately and unambiguously. The drift from the
naive "S63 generalises adversely" interpretation to the actual
"PROVENANCE-vs-tape contradiction" interpretation took one round of
analysis. Both driver verdict-text patches landed alongside the
closure memo without further iteration.

The session did NOT extract S63 evidence for XTF — but it did surface
the prerequisite that needs answering first. That re-prioritisation
is the substantive deliverable.

The Tier 4 vol-regime gate (part-Y next-session item #3) remains the
highest-leverage research track. The XTF Window 4 2h-vs-4h divergence
finding adds a small piece of supporting evidence: per-timeframe
regime sensitivity is real on XAU trend-follow, which is one of the
core Tier 4 motivations. Worth folding into the Tier 4 scoping memo
when the work resumes.

The remaining XTF S63 work (4h Phase 1 sweep, 2h Phase 1 sweep, d1
Phase 3 WF, then potentially d1 Phase 1 sweep) is all blocked behind
the PROVENANCE reconciliation. If reconciliation resolves with a
clear answer (substitute-tape divergence vs harness/engine drift),
the path forward becomes obvious. If reconciliation is inconclusive,
XTF S63 work should be parked and the engineering effort redirected
to Tier 4 + EmaPullback per-cell tuning + the broader universe-wide
S63 sweep on engines whose baselines DO have edge on the substitute
tape.

### Suggested commit command for closing the part-Z session

```bash
cd ~/omega_repo

# (a) Verify diffs are surgical (verdict-text patches only)
git diff scripts/xtf_2h_wf_t1.py scripts/xtf_4h_wf_t1.py | head -120
git status

# (b) Stage S84 -- driver verdict-text fix + force-add the closure memo
git add scripts/xtf_2h_wf_t1.py scripts/xtf_4h_wf_t1.py
git add -f outputs/XTF_2H_4H_TIER1_PHASE3_RESULTS_2026-05-14.md
git status   # confirm only the three files staged

git commit -m "S84: XTF 2h+4h Tier 1 Phase 3 WF verdicts (both FAIL) + driver verdict-text fix

WF results on /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv:
  - XTF 2h: 1/4 windows pass, agg PF 0.724, avg_pnl -0.147 (846 trades)
  - XTF 4h: 0/4 windows pass, agg PF 0.407, avg_pnl -0.506 (945 trades)

Both FAIL the Phase 3 gate decisively. Engines remain at class-default
state-B values (S63 OFF).

Key finding: the FAIL is NOT evidence of S63 adversity. Both runs were
--mode baseline (S63 trio = 0/0/0), so the FAIL is intrinsic baseline-
engine underperformance on this tape, not anything S63-related. The
load-bearing follow-up question is the PROVENANCE-vs-WF contradiction:
XauTrendFollow4hEngine.hpp:7-25 reports each cell net positive on the
2023-09..2025-09 Duka corpus, yet WF Windows 1-3 (fully inside that
range on the substitute tape) show heavy losses. Tape-source or bar-
construction divergence is the most likely explanation. S63 evaluation
on XTF is moot until this is reconciled.

Window 4 (2025-10..2026-04, recent XAU rally) shows 2h vs 4h timeframe
divergence: 2h passes (PF 2.088, 201 trades) while 4h fails harder
(PF 0.454, 227 trades). Regime-sensitivity intelligence relevant to
the queued Tier 4 vol-regime gate work.

Files:
  outputs/XTF_2H_4H_TIER1_PHASE3_RESULTS_2026-05-14.md
    Combined closure memo (force-added; outputs/ is project-gitignored).
    Documents the verdicts, the PROVENANCE contradiction, the Window 4
    divergence, and the recommended next-step priority order.

  scripts/xtf_2h_wf_t1.py + scripts/xtf_4h_wf_t1.py
    Verdict-text patch (part-X carry-over #6). Removes the misleading
    'S63-adverse pattern generalises' boilerplate -- the boilerplate
    was incorrect because --mode baseline runs with S63 OFF cannot
    produce evidence about S63 specifically. The replacement text
    frames the FAIL as baseline-engine underperformance and lists the
    investigation order: reconcile PROVENANCE first, cross-reference
    sibling timeframes, defer S63 sweep until baseline edge restored.

Closes part-Y next-session items #1 (XTF 4h harness + WF) and the
part-X carry-over #6 (verdict-text patch). Items #2 (XTF d1 WF
driver) and #3 (Tier 4 Phase A on VWR USTEC.F) remain queued.

No engine code modified. No engine_init.hpp modified. No core code
touched."

# (c) Stage and commit the handoff doc separately
git add -f docs/handoffs/SESSION_HANDOFF_2026-05-14o.md
git commit -m "docs: part-Z handoff -- XTF 2h+4h Phase 3 WF closure + PROVENANCE reconciliation prioritised

Captures the part-Z session arc:
  - S83 (already on origin/main) XTF 4h+d1 harness dispatch fill-in
    + scripts/xtf_4h_wf_t1.py driver.
  - WF execution on the substitute tape -- both 2h and 4h FAIL the
    Phase 3 gate decisively.
  - S84 driver verdict-text patches (part-X carry-over #6) +
    combined closure memo.
  - Substantive finding: PROVENANCE-vs-WF contradiction is the
    load-bearing research question, not the WF FAIL itself.

Next-session priorities (revised from part-Y):
  1. PROVENANCE-vs-WF reconciliation on XauTrendFollow*Engine (NEW).
  2. Tier 4 Phase A on VWR USTEC.F (unchanged; highest-leverage).
  3. scripts/xtf_d1_wf_t1.py (sibling driver, mechanical patch).
  4. EmaPullback per-cell tuning.
  5. Universe-wide S63 sweep continuation.

Handoff at docs/handoffs/SESSION_HANDOFF_2026-05-14o.md."

git push origin main
git log --oneline -8
```
