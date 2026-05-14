# Session Handoff — 2026-05-14 (NZST), part S

Read this first next session. Direct follow-up to part-R
(`SESSION_HANDOFF_2026-05-14g.md`). This session closed the VWR
USTEC.F retune track entirely (Phases 1 → 2 → 3 walk-forward FAIL,
Tier 4 recommended) and started a scoping pass on the second
stop-bleed engine, UstecTrendFollow5m, where a major correction to
the part-R mental model surfaced.

> **Naming.** Same convention as parts L → R: filename letter is
> per-session. Part-S session = filename letter `h` (part-R used `g`).

## TL;DR

1. **VWR USTEC.F retune track is CLOSED.** Phase 1 → Phase 2 → Phase 3
   walk-forward done end-to-end this session. Phase 3 WF FAILED on
   both decision criteria (2 of 4 windows pass vs ≥3 needed,
   aggregate PF=1.0358 vs ≥1.20 needed). The Phase 2 +0.004876
   expectancy was a weighted average of a bimodal regime: 2024
   strongly negative, 2025-2026 marginally positive. WF correctly
   surfaced what Phase 2 averaging masked. Recommendation per
   scoping memo §4: Tier 4 (signal-shape redesign). Engine stays
   disabled at `engine_init.hpp:608` (`g_vwap_rev_nq.enabled =
   false`, S68 stop-bleed).

2. **MAJOR CORRECTION to part-R's mental model on UstecTrendFollow5m.**
   Part-R classified UTF5m as state E (no S63 hooks). Reading the
   actual code at HEAD shows it is **state A** — S63 LOSS_CUT_PCT /
   BE_ARM_PCT / BE_BUFFER_PCT are fully wired with USTEC-scaled
   defaults (`UstecTrendFollow5mEngine.hpp:323-357` for class fields,
   `:656-705` for management-path with per-cell semantics) AND the
   activation is explicitly re-affirmed in `engine_init.hpp:968-970`.
   The in-code comment says "Completes the state-E → state-A
   transition called out in part-K handoff" — done in part-L.
   Part-R was written before this landed.

3. **The actual promotion gate** is documented at
   `engine_init.hpp:964-967`:
   > Promotion gate: g_ustec_tf_5m.enabled stays FALSE until a fresh-
   > tape backtest sweep confirms S63 + S37 widened SL/TP profile is
   > net-positive on USTEC.

   So the task is not "wire S63" (done) but "run the fresh-tape sweep
   that proves the promotion is viable". This is materially simpler
   than the part-R scoping implied.

4. **Existing UTF5m harnesses exist but are S63-unaware.** Four
   standalone backtest files at `backtest/ustec_trend_follow_5m_*` +
   `backtest/ustec_tf_htf_S35P6_backtest.cpp`. All re-implement the
   engine logic standalone (Donchian, Keltner, prove-it, mutex, ATR
   floor, etc.) for sweep purposes — none have LOSS_CUT_PCT /
   BE_ARM_PCT / BE_BUFFER_PCT awareness. They pre-date S63. Salvaging
   one by adding S63 risks logic drift from production.

5. **The clean path** is to build a dedicated
   `UstecTrendFollow5mBacktest.cpp` that mirrors the
   `VWAPReversionBacktest.cpp` pattern: instantiate the actual
   `omega::UstecTrendFollow5mEngine` (no re-implementation), drive it
   tick-by-tick + 5m-bar from a CSV tape, expose S63 + S37 fields as
   CLI flags, emit `report.csv` / `trades.csv`. Estimated ~600 lines,
   one fresh session. See §"Recommended next-session focus" for the
   shape.

## Commits this session

| Commit | Message | Files |
|---|---|---|
| (S71 Phase 1) | _via operator-side commit_ | Phase 1 memo + p1 driver |
| (S71 Phase 2) | _via operator-side commit_ | Phase 2 memo + p2 driver |
| `bf6db45` | S71 Phase 3: VWR USTEC.F Tier 1 walk-forward FAIL | Phase 3 memo + WF driver |

`origin/main` ended at `bf6db45` (S71 Phase 3 closure). The intermediate
Phase 1 + Phase 2 commits landed between `56471fb` (part-R doc commit)
and `9ab444e` (HEAD-before-Phase-3-push); their exact hashes are in
`git log` Mac-side. They're standard commits per the suggested-commit
blocks in the Phase 1 and Phase 2 memos.

## Files modified this session — final state

```
A scripts/vrev_sweep_t1_p1.sh                                  (committed S71 P1)
A scripts/vrev_sweep_t1_p2.sh                                  (committed S71 P2)
A scripts/vrev_wf_t1.py                                        (committed S71 P3)
A outputs/VWR_USTEC_TIER1_PHASE1_RESULTS_2026-05-14.md         (committed S71 P1, force-add)
A outputs/VWR_USTEC_TIER1_PHASE2_RESULTS_2026-05-14.md         (committed S71 P2, force-add)
A outputs/VWR_USTEC_TIER1_PHASE3_RESULTS_2026-05-14.md         (committed S71 P3, force-add)
A docs/handoffs/SESSION_HANDOFF_2026-05-14h.md                 (this file, not committed)
```

## VWR USTEC.F closure — quick reference

Per-window WF results (window-id / period / trades / avg_pnl / PF / verdict):

```
w0  2024-01 → 2024-07   1010  −0.00653  0.932  FAIL
w1  2024-07 → 2025-02    730  −0.00204  0.985  FAIL
w2  2025-02 → 2025-09   1302  +0.00965  1.067  PASS
w3  2025-09 → 2026-04   1238  +0.01278  1.087  PASS
```

Aggregate: 4280 trades, avg=+0.004746, PF=1.0358. Threshold ≥1.20 → FAIL.

Side-findings worth carrying forward if UTF5m work uncovers anything
similar:

- **Regime concentration is real.** Half the tape contradicting the
  other half is exactly what walk-forward is designed to catch. Phase
  2's full-tape average can be a misleading signal in the presence of
  regime structure.
- **Even passing windows were marginal** (PF 1.067 / 1.087). A robust
  edge should produce PFs in the 1.2+ range in its good periods.
- **w1 trade count anomaly** (730 vs ~1100-1300 elsewhere) suggests
  volatility-regime dependence — fewer extension setups in low-vol
  windows. A regime gate may be the most actionable Tier 4 candidate
  if VWR USTEC.F is ever revisited.

Two follow-ups from Phase 1 §6 (lower priority now, but worth fixing
for any future VWR work on any instrument):

1. **`ext-sl-ratio` plateau above 0.60.** Cells 0.60, 0.80, 1.00, 1.50
   produce byte-identical metrics. `sig.sl` is computed correctly from
   the field but a downstream SL handler probably ignores it. Quick
   `Grep` pass for `sig.sl` consumers would localise.
2. **`tp_rate ≥ 5%` gate in scoping memo §8** is structurally
   unreachable for VWR USTEC.F — max observed `tp_rate` across 60+
   cells was 3.81%. Strategy edge flows through MAE/timeout-controlled
   losses, not TP hits. Recalibrate to ~3% or remove.

## UstecTrendFollow5m current state (corrected)

Source of record: `include/UstecTrendFollow5mEngine.hpp` at HEAD.

**Class structure (post part-L):**
- Cells: 2 (Donchian N=20 sl3.0/tp7.0, Keltner K=2.0 sl3.0/tp7.0).
  S37 widened sl_mult and tp_mult from 2.0/4.0 to 3.0/7.0.
- Guards: S34-B prove-it exit (150s/2pt), cell mutex (same-direction
  block), MIN_ATR_PTS=20 floor, MIN_SL_PTS_FLOOR=15.
- S63 (part-L 2026-05-14): LOSS_CUT_PCT=0.08, BE_ARM_PCT=0.05,
  BE_BUFFER_PCT=0.02. USTEC-scaled (@28000): ~22pt cold cut, ~14pt
  ratchet arm, ~5.6pt buffer. Per-cell in `_manage_open` at
  `UstecTrendFollow5mEngine.hpp:656-705`.
- S37-P2 RiskMonitor wiring: on_fire_hook + three auto-pin callbacks
  (engine_init.hpp:1005+).
- `g_ustec_tf_5m.enabled = false` (S68 stop-bleed,
  `engine_init.hpp:950`). `shadow_mode = true` (HARD shadow).

**Promotion gate (documented in engine_init.hpp:964-967):**
> g_ustec_tf_5m.enabled stays FALSE until a fresh-tape backtest sweep
> confirms S63 + S37 widened SL/TP profile is net-positive on USTEC.

**Existing UTF5m harnesses (all S63-unaware):**

| File | Purpose | S63-aware |
|---|---|---|
| `backtest/ustec_trend_follow_5m_entry_sweep.cpp` | entry-axis sweep (donchian_N, keltner_K, session_hr, cell_enable) | NO |
| `backtest/ustec_trend_follow_5m_planA_sweep.cpp` | Plan A grid (sl_mult / tp_mult / prove-it) | NO |
| `backtest/ustec_trend_follow_5m_planB_sweep.cpp` | Plan B grid (ATR floor) | NO |
| `backtest/ustec_tf_htf_S35P6_backtest.cpp` | HTF S35-P6 backtest | NO |

All four re-implement the engine logic standalone — they have their
own `class UstecTfEngine { ... }` and don't include
`include/UstecTrendFollow5mEngine.hpp`. Adapting one would require
porting the S63 management-path into the standalone copy and
maintaining drift between standalone and production logic.

None are in `CMakeLists.txt` — they're built standalone with
`clang++ -std=c++17 -O3 ...` per the standard pattern.

The S37 promotion evidence (PLAN_A_B_REPORT.md cited in the engine's
header comment, 25-month tape sweep, IS +$7,586 / OOS +$5,207) came
from these harnesses but **without S63**. The current promotion gate
asks specifically whether **S63 + S37** is net-positive — which has
never been measured.

## Recommended next-session focus

In priority order:

1. **Build `backtest/UstecTrendFollow5mBacktest.cpp`** — dedicated
   harness mirroring `backtest/VWAPReversionBacktest.cpp`'s pattern:
   - Instantiate `omega::UstecTrendFollow5mEngine` directly (no
     re-implementation). Drives by `on_5m_bar(bar, bid, ask,
     atr14_external=0.0, now_ms, on_close)` (engine computes ATR
     internally) + `on_tick(bid, ask, now_ms, on_close)`.
   - Tick → 5m-bar aggregation in the harness (mirror the tape-driving
     pattern from `OmegaBacktest.cpp` / `IndexBacktest.cpp`).
   - CLI flags for S63 trio (`--loss-cut`, `--be-arm`, `--be-buffer`)
     and for the S34-B guards (PROVE_IT_SECS, PROVE_IT_MIN_FAVOURABLE_PTS,
     MIN_ATR_PTS — note these are `static constexpr` in the engine, so
     either promote them to non-static members per S70 precedent OR
     pin them at engine class defaults and only sweep S63).
   - `--mode baseline` (S63 trio zeroed) / `--mode tuned` (engine defaults).
   - Emit `report.csv` (metric,value) + `trades.csv` mirroring VWR's
     format so the WF driver (`vrev_wf_t1.py`) pattern can be reused.
   - Add to `CMakeLists.txt` with appropriate target.
   - Est. ~600 lines, ~1 session.

2. **Decide on the static-constexpr-vs-member question.** The S34-B
   guards (PROVE_IT_SECS, PROVE_IT_MIN_FAVOURABLE_PTS, MIN_SL_PTS_FLOOR,
   MIN_ATR_PTS) are currently `static constexpr` in the engine. The
   S37 promotion changed them via source edit, not via per-instance
   override. If the sweep wants to test variants of these (which the
   existing entry_sweep.cpp does), they need to be non-static
   members. Two options:
   - (a) Promote them to members per the S70 VWR precedent (additive,
     defaults preserve current behaviour).
   - (b) Pin them at engine defaults and only sweep S63 trio. Smaller
     scope but limits what the harness can test.
   Recommendation: (a) — same precedent S70 set for VWR. The promotion
   is small (4 fields), additive, and lets the harness explore the
   full Plan A/B grid that the existing standalone sweeps covered.

3. **Run the fresh-tape S63 + S37 sweep** on the same 4.4 GB NSXUSD
   tape (`/Users/jo/Tick/NSXUSD_merged.csv`) used for VWR. Mirror the
   3-phase structure: Phase 1 univariate across S63 axes (off/default,
   3-5 cells each) + S37 widened-vs-class-default toggle; Phase 2 fine
   sweep around any winning axis; Phase 3 walk-forward at the Phase 2
   winner using `scripts/vrev_wf_t1.py` (the WF driver is engine-
   agnostic at the harness CLI level — would just need a few flag
   substitutions).
   - Decision rule per `engine_init.hpp:964-967`: PASS = net-positive
     PnL on the full tape AND WF survives at ≥3 of 4 windows positive
     with aggregate PF ≥ 1.20.

4. **If sweep PASSES** → flip `g_ustec_tf_5m.enabled = true` at
   `engine_init.hpp:950`. Keep `shadow_mode = true` for 6 months of
   live confirmation per the engine's own caveat at
   `UstecTrendFollow5mEngine.hpp:23-25`.

5. **If sweep FAILS** → closure memo mirroring the VWR Tier 4
   recommendation. Engine stays disabled. Document which dimension
   failed (S63 too aggressive? S37 SL/TP geometry wrong on this tape
   vs the original 25-month evidence? regime concentration like VWR?).

6. **Other carried-over work from part-R recommended-next-focus
   (lower priority than UTF5m):**
   - XauTrendFollow trio S63 sweep (similar structure to UTF5m).
   - Wrapper engine S63 design pass + audit (multi-session, read-only).
   - GoldEngineStack chokepoint audit (standing, before any
     GoldEngineStack edit).

## Important lessons / don't-repeat

1. **Always read the actual engine code before trusting a handoff
   classification.** Part-R said UTF5m was state E (no S63). The
   actual code at HEAD shows state A (S63 wired + active). Part-R
   was written before part-L's S63 wiring landed. The cost of this
   would have been planning a "build S63" session that's already
   done — wasted scope. Same lesson as part-K observed for the
   VWR engines and the part-K audit framework remains the right
   tool: classify by code, not by handoff text.

2. **Walk-forward is the only honest test in the presence of regime
   structure.** Phase 2's +0.004876 looked PASS-worthy. Phase 3
   showed it was a bimodal regime average. If we'd skipped to
   "flip enabled = true" after Phase 2, the live deploy would have
   spent 14 months losing money before the regime that produced the
   positive half showed up. The 3-phase protocol earned its keep.
   Carry this discipline into UTF5m: WF, not just full-tape.

3. **Standalone re-implementation harnesses have drift risk.** The
   four existing UTF5m harnesses re-implement engine logic. They
   were correct when written but the engine has had three updates
   since (S34, S34-B, S37 widening, S63). The promotion evidence
   they produced no longer reflects the production engine. Future
   harnesses should instantiate the actual engine (VWR pattern), not
   re-implement.

4. **Tape disk discipline matters.** The Mac hit 100% disk during
   Phase 3 (2.84 GB free of 460 GB). The original `vrev_wf_t1.py`
   v1 needed 4.4 GB peak temp (all 4 splits concurrent) and died.
   The rewrite to sequential-with-delete capped at ~1.1 GB and
   succeeded. Any future sweep on this tape should design for
   sequential temp use, not concurrent. The operator cleaned disk
   between Phase 3 and session end, but the constraint will recur.

## Standing audit at session end

**Core code preserved.** None of `include/OmegaCostGuard.hpp`,
`include/OmegaTradeLedger.hpp`, `include/SymbolConfig.hpp`,
`include/OmegaFIX.hpp`, `src/api/OmegaApiServer.hpp`, or
`include/GoldPositionManager.hpp` were modified this session.
Only files touched: `outputs/VWR_USTEC_TIER1_PHASE*.md` (3 memos,
this handoff), `scripts/vrev_sweep_t1_p1.sh`,
`scripts/vrev_sweep_t1_p2.sh`, `scripts/vrev_wf_t1.py`. All
non-core.

**Stop-bleed disables intact:**
- `g_vwap_rev_nq.enabled = false` at `engine_init.hpp:608` —
  confirmed CLOSED-OFF by S71 Phase 3 walk-forward. Tier 4 recommended.
- `g_ustec_tf_5m.enabled = false` at `engine_init.hpp:950` — still
  pending fresh-tape sweep per scoping above.

**Ungated-engine sweep expectations unchanged.** No engine code
modified.

**GoldEngineStack chokepoint audit:** not touched this session.
Two-hit expectation should still hold; verify before any
GoldEngineStack edit.

## Pre-commit checklist for next session

Standard project CLAUDE.md checklist applies. Specific notes for the
UTF5m harness build:

1. The harness includes `include/UstecTrendFollow5mEngine.hpp` directly
   (no re-implementation). If that header changes, the harness
   recompiles — that's the right coupling.
2. Mac canary build for both `OmegaBacktest` and the new
   `UstecTrendFollow5mBacktest` target must be green before any
   commit. Per project CLAUDE.md §Build Verification — bare
   `cmake --build build -j` always fails on macOS due to winsock2.h.
   Use the specific targets.
3. If promoting S34-B guards (PROVE_IT_SECS etc.) from `static
   constexpr` to non-static members, mirror the S70 VWR precedent
   for additive changes that preserve default behaviour. Same
   commit ordering (engine change first, harness uses second).
4. Per CLAUDE.md, never bundle unrelated changes. The harness build
   commit is one unit; any subsequent sweep results memo is a
   separate commit; any S34-B-promotion-to-members is yet another.

## Stash state at session end

```
$ git stash list
(empty)
```

Inherited clean from part-R. No new stashes this session.

## Operational note

Mac was at 100% disk utilisation (2.84 GB free of 460 GB) during
Phase 3. Operator cleaned disk between Phase 3 closeout and session
end. The constraint will recur on any tape-heavy sweep. Future
harnesses should design for sequential temp use. The `vrev_wf_t1.py`
v2 rewrite (sequential window-stream-then-delete) is the working
pattern to inherit.

## Notes for whoever picks up part-T

If you continue with operator-side execution:
- Build the UstecTrendFollow5mBacktest harness per §"Recommended
  next-session focus" item 1.
- Decide on the S34-B-guards-as-members question (§ item 2) before
  scoping the sweep.
- Run the sweep per § item 3.
- Treat the WF as gating per the VWR precedent — full-tape average
  alone is not enough.

If you continue with in-chat work:
- Read this handoff first, then read `UstecTrendFollow5mEngine.hpp`
  end-to-end (it's ~800 lines and the comments contain a lot of
  S33 → S37 → S63 history that's load-bearing for any decision).
- Then read one of the existing standalone harnesses (entry_sweep is
  the most general) to understand the tick-driving pattern.
- Then either build the new harness or write a more detailed scoping
  memo that decides the (a) vs (b) tradeoff in § item 2.
