# VWR USTEC.F — Structural Rework Scoping (2026-05-14f)

> **Status:** scoping memo only. No code changes this session. Produced
> as part-Q's deliverable to inform a future implementation session.
> Reads `outputs/VWR_USTEC_PHASE{1,2}_RESULTS_2026-05-14e.md` (parameter
> retune closure) and `outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md` §8
> (deferred decisions) as inputs.

> **Naming.** Part-Q session = filename letter `f` (per the L→M→N→O→P→Q
> convention; part-P used `e`).

---

## 0. TL;DR

The parameter retune executed in part-P established that no entry-side
parameter axis (EXTENSION_THRESH_PCT, MAX_EXTENSION_PCT, MAX_HOLD_SEC,
COOLDOWN_SEC) produces a stable positive-expectancy surface on USTEC.F.
The TP-rate diagnostic from Phase 2 — 1-3% across the whole sweep —
is the dominant signal: trades are not closing at profit targets;
positive cells win by avoiding bigger losses, not by capturing
reversions. That is risk shaping, not edge.

This memo categorises the remaining signal-side levers by how costly
they are to test, and proposes a 4-tier ordering so the cheapest
disqualifying tests run first.

The bottom line: only Tier 1 (session/time gates + SL geometry) can be
tested with the existing harness + tick tape. Tier 2 (VIX confluence)
needs a VIX-history side-channel. Tier 3 (L2 confluence) needs synthetic
L2 because real L2 history is generally unavailable on retail tick
tapes. Tier 4 (signal-shape changes) is genuine engine work, not a
parameter sweep, and is the highest-effort fallback.

**Recommendation:** run Tier 1 in a single follow-up session (~1-1.5 hr
incl. harness extension). If Tier 1 produces a robust positive surface,
stop and proceed to walk-forward. If Tier 1 fails, escalate to a design
review for Tier 4 before sinking effort into Tier 2/3 data-pipeline work.

---

## 1. Why parameter retune closed (one-paragraph summary)

Part-P ran 21 univariate Phase-1 cells + 15 Phase-2 cells on the 4.4GB
404-day NSXUSD HistData tape. Exactly one Phase-1 cell technically
passed the +0.001/trade threshold (`ext=0.80, avg_pnl=+0.00282`), but
Phase 2A's fine sweep around 0.80 returned 2 positive cells out of 6
(non-monotonic surface, positives sandwiched between negatives), and
Phase 2B's 2D grid did not improve over Phase 2A's best. TP rates across
the entire 36-cell sweep stayed in the 1-3% band: the engine is not
closing trades at VWAP-reversion targets; it is closing them via timeout
or MAE_EXIT. Engine_init.hpp:625-640 carries the closure comment block.

The implication for the present memo: any structural rework must improve
the TP rate (i.e., produce more actual reversions that touch VWAP within
MAX_HOLD), not just shape the loss distribution. If TP rate stays at
1-3% under a candidate change, the change has not fixed the signal.

---

## 2. The diagnostic — what the TP rate is telling us

The VWR signal fires when:
1. Price is more than `EXTENSION_THRESH_PCT` from EWM-VWAP (and less than `MAX_EXTENSION_PCT`)
2. Current tick moves back toward VWAP (the "reversal tick")
3. Momentum window over the last N ticks isn't still trending away
4. Cost gate passes

The trade closes profitably (TP_HIT) only when price subsequently touches
VWAP within `MAX_HOLD_SEC`. On USTEC.F the observed TP rate sits at
1-3% across every parameter combination tested. The other ~97% of
trades close via:
- `T/O` (timeout, position non-trending)
- `MAE_EXIT` (adverse move > 50% of TP distance)
- `BE_CUT` / `LOSS_CUT` (only when S63 enabled; disabled for this engine)

**Two competing hypotheses for the low TP rate:**

(H1) **Signal is mis-pointed.** USTEC.F at session-overlap-relevant
extensions does not actually mean-revert to its EWM-VWAP within the
chosen MAX_HOLD window. The "reversal tick" entry is catching noise
and the position never makes it back to VWAP. Reversion happens on a
longer timescale, or to a different anchor, or at different session
times than the engine is firing.

(H2) **Signal is correctly pointed but the TP geometry is wrong.**
The reversion does happen but doesn't reach the full VWAP line within
the timeout; price returns partway and then resumes the original trend.
TP = VWAP is too aggressive a target.

H1 favours session/time gating + signal-shape redesign (Tier 1, Tier 4).
H2 favours TP relocation (Tier 1 — `MAE_EXIT_RATIO`, partial-fill TP).

The 36-cell sweep didn't include `EXTENSION_SL_RATIO` or `MAE_EXIT_RATIO`
(deferred per retune plan §8), so H2 has not yet been falsified. Tier 1
includes these axes specifically to test it.

---

## 3. Available levers, categorised by data dependency

### 3.1 Class fields not yet exercised

From `include/CrossAssetEngines.hpp:1202-1257`:

| Field | Current value | Currently CLI-overridable? | Touched by Phase-1/2 sweep? |
|---|---|---|---|
| `EXTENSION_SL_RATIO` | 0.60 | No | No |
| `MAE_EXIT_RATIO` | 0.50 | No | No |
| `MAE_COOLDOWN_SEC` | 600 | No | No |
| `CONSEC_FC_BLOCK_SEC` | 1800 | No | No |
| `TP_FLIP_COOLDOWN_SEC` | 1200 | No | No |
| `MIN_SESSION_MIN` | 120 | No | No |
| `CONF_VIX_THRESH` | 18.0 | No | No |
| `CONF_L2_THRESH` | 0.12 | No | No |
| `EWM_VWAP_HALF_LIFE_SEC` | 7200 (static constexpr) | No (static) | No |

Plus the **hard-coded** session gates in the on_tick body
(`CrossAssetEngines.hpp:1439-1446`):

| Gate | Current rule | Hard-coded location |
|---|---|---|
| Session hour gate | `h < 8 || h >= 22` (08:00-22:00 UTC entries) | L1442 |
| NY/London overlap bonus | `5*60+30 <= elapsed < 9*60` (13:30-17:00 UTC) adds +1 score | L1529 |
| Reversal tick | `is_long ? prev_mid > mid : prev_mid < mid` | L1498 |
| Momentum block | `trend_move` over `TREND_WINDOW` ticks vs `block_thresh` | L1503-1510 |

Some of these are score factors that lift size but never gate entry;
those could be promoted to hard gates without engine-class changes
(just `engine_init.hpp` overrides if the field is exposed, or a small
class addition for the score-to-gate conversion).

### 3.2 Confluence layer is inert in the existing harness

`include/CrossAssetEngines.hpp:1274-1276`:

```
CrossSignal on_tick(const std::string& sym, double bid, double ask,
                    double vwap_seed, CloseCb on_close,
                    double vix = 0.0, double l2_imb = 0.5) noexcept
```

In `backtest/VWAPReversionBacktest.cpp` (the harness used for both
Phase 1 and Phase 2), `on_tick` is called without the `vix` / `l2_imb`
arguments, so they fall through to the defaults: VIX=0.0 ("unknown,
skip"), L2=0.5 ("neutral"). The confluence score therefore reaches at
most 2 (base + session-overlap bonus) in backtest, never 3 or 4.

**Implication:** the Phase 1/2 sweep was operating on a 1- or 2-factor
score. Any rework that raises `CONF_VIX_THRESH` or `CONF_L2_THRESH` —
or that converts those score factors into hard gates — cannot be
back-tested on the existing tape without first wiring VIX and L2 into
the harness's tick stream. That is the Tier 2/3 work below.

### 3.3 The four tiers

**Tier 1 — Tick-only testable, harness extension < 1 hr.**

Axes that depend only on bid/ask tick data + wall-clock:

- `MIN_SESSION_MIN` (currently 120; sweep e.g. {120, 180, 240, 330, 540})
- Hard session window (currently 08-22 UTC; sweep e.g. tighter
  windows {10-21, 13-21, 13:30-17:00 NY/Lon overlap only})
- `EXTENSION_SL_RATIO` (currently 0.60; sweep e.g. {0.40, 0.50, 0.60,
  0.80, 1.00, 1.50})
- `MAE_EXIT_RATIO` (currently 0.50; sweep e.g. {0.30, 0.40, 0.50, 0.65,
  0.80})
- Partial-fill TP (NEW): close at e.g. 50% / 75% of VWAP distance
  instead of full VWAP. Requires a class field (`TP_FRACTION = 1.0`
  default; ratio in (0, 1]).
- `EWM_VWAP_HALF_LIFE_SEC` (currently static constexpr 7200; promote to
  non-static field and sweep {1800, 3600, 7200, 14400})

These are roughly 3 axes (`MIN_SESSION_MIN`, `EXTENSION_SL_RATIO`,
`MAE_EXIT_RATIO`) with existing field hooks + 3 axes that need a
small class change (session window override, `TP_FRACTION` field,
EWM half-life promotion).

**Tier 2 — Needs VIX side-channel.**

Tests `CONF_VIX_THRESH` as a score factor or as a hard gate. Requires
VIX history aligned to the USTEC.F tick tape's UTC timestamps.

- Data source options:
  - VIX daily close from FRED / Yahoo (public, free; coarse — daily
    resolution only)
  - VIX 1m bars (need a paid source or scrape; gives intraday detail
    relevant to the 08-22 UTC window)
- Harness work: read a VIX side-CSV indexed by UTC date or
  date+time, forward-fill, pass into `on_tick` as the `vix` argument.

Estimated: 2-3 hr including data fetch + harness extension.

**Tier 3 — Needs L2 (synthetic or recorded).**

Tests `CONF_L2_THRESH` as a score factor or hard gate. Retail tick
tapes (HistData, Dukascopy) generally do not include depth-of-book.
Two paths:

- **Synthetic L2 from spread.** Compute a proxy for `l2_imb` from
  recent spread + tick direction asymmetry. Cheap but loose; risks
  capturing nothing the existing momentum filter doesn't already.
- **Recorded L2.** Capture L2 going forward on the live feed, then
  build a sample set from 30-90 days of recorded data. High effort,
  delayed.

Estimated: 3-5 hr for synthetic; multi-week for recorded.

**Tier 4 — Signal-shape redesign.**

If Tiers 1-3 don't move the TP rate above ~10%, the entry condition
itself is wrong on USTEC.F. Candidate redesigns:

- **Different VWAP anchor.** Daily-open seed + 2hr half-life EWM is
  one design choice. Alternatives: session-anchored VWAP (resets on
  London open / NY open), volume-weighted instead of time-decayed,
  multi-timeframe (4hr VWAP + 1hr position filter).
- **Different trigger.** Replace "extension + reversal tick" with
  "extension + N-bar consolidation + breakout against extension
  direction" or similar. Significantly different strategy; arguably
  a new engine, not a tune of VWR.
- **Index-conscious overlay.** USTEC is index futures; momentum
  persists much longer than on EURUSD or even GER40. The mean-
  reversion thesis may simply not hold on USTEC at the timeframes
  the engine considers. Could try a much shorter `MAX_HOLD_SEC`
  (e.g. 60-120s scalping window) on the strongest extensions.

Estimated: design 4-8 hr; implementation + test 1-2 sessions.

---

## 4. Recommended phased approach

Run Tier 1 in its entirety before considering Tier 2 / 3 / 4. Reasoning:

1. **Cheapest disqualifying tests first.** If Tier 1 reveals nothing,
   we know the signal needs structural redesign and the VIX/L2
   data-pipeline work would be premature — those gates only filter
   trades the signal generates, they don't change which trades the
   signal generates. A bad signal × confluence filter is still a bad
   signal.

2. **Most likely to move TP rate.** Tier 1 includes `MAE_EXIT_RATIO`
   and `EXTENSION_SL_RATIO` (geometry) and `TP_FRACTION` (partial-fill
   TP). These directly affect whether a partial reversion counts as a
   win — which is the cleanest test of H2 vs H1 in §2.

3. **Reuses existing infrastructure.** The harness already supports
   tick replay with arbitrary parameter overrides. Adding 3-6 more CLI
   flags is mechanical (≤ 1 hour for the harness work + a class field
   or two for `TP_FRACTION` and the session window override).

4. **Decisive in one session.** A focused Tier 1 sweep (~25 cells across
   the new axes) plus a Tier 1 2D refinement on whichever axis pops
   should fit in one session at the cadence Phase 1+2 ran (~30s/cell).

### Suggested Tier 1 session schedule

| Step | Effort | Outcome |
|---|---|---|
| 1. Read this memo + part-P PHASE_RESULTS memos | 10 min | Context loaded |
| 2. Add CLI flags for `--ext-sl-ratio`, `--mae-exit-ratio`, `--min-session-min`, `--tp-fraction`, `--session-window` | 45 min | Harness ready (analogous to part-P Phase 0 work) |
| 3. Add `TP_FRACTION` and `MIN_SESSION_MIN` overridability + session-window override path in the class (engine-touch; small) | 30 min | Class supports the sweep axes |
| 4. Run Phase-1 univariate (4-5 axes × 5-6 levels each = 20-30 cells) | 30 min wall-clock + 10 min agent time | Surface map |
| 5. Analysis + decision: any axis showing monotonic positive surface? TP rate > 5%? | 15 min | Pass/fail vs Tier 4 escalation |
| 6. If pass: Phase-2 refinement (~10 cells) | 15 min wall-clock + 10 min agent time | Tighter result |
| 7. If pass: walk-forward validation (per retune plan §5 Phase 3) | 45 min | Re-enable decision |
| 8. Write up + commit | 20 min | Closure or escalation memo |

Total: ~3-3.5 hr operator session. Within feasibility budget for a
focused chat.

---

## 5. Out-of-scope for the Tier 1 session

- Re-enabling `g_vwap_rev_nq.enabled` without walk-forward. Re-enable
  criteria from retune plan §7 still apply: 3-fold OOS, all positive,
  std < 50% of mean.
- Adding VIX/L2 to the harness. Tier 2/3 work — defer until Tier 1
  results justify the data-pipeline investment.
- Touching SP / EURUSD / GER40 VWR instances. Their state-A/B status
  is settled and per-instrument backtest-evidenced. This memo's scope
  is USTEC.F only.
- Re-evaluating the S63 trio on USTEC.F. The retune plan §6 framework
  for S63 evaluation post-positive-baseline still applies, but the
  positive baseline doesn't exist yet, so S63 work remains downstream.
- Touching `g_ustec_tf_5m` (the trend-follow engine). Different engine
  entirely; tracked as part-P recommended item P1.

---

## 6. Engine-class changes the Tier 1 session would need

Three small additions, all additive, none touching the existing entry
or exit logic:

### 6.1 Promote `EWM_VWAP_HALF_LIFE_SEC` from static constexpr to a member field

Currently at `include/CrossAssetEngines.hpp:1256`:

```
static constexpr double EWM_VWAP_HALF_LIFE_SEC = 7200.0;
```

Change to a non-static member, so it's settable per-instance from
`engine_init.hpp` and from the harness. No other change needed —
the existing use site at L1298 already references the name and would
resolve to the member.

This needs careful review per CLAUDE.md §"Edit Discipline" because
the field name is referenced in user-facing comments at L1252-1256
that describe the half-life mechanism. Update the comments at the
same time to clarify it's now per-instance.

### 6.2 Add `TP_FRACTION` field

New member with default 1.0 (preserves current behaviour):

```
double TP_FRACTION = 1.0;  // 1.0 = TP at VWAP; <1.0 = partial reversion target
```

Use site at L1539: replace `tp = vwap;` with
`tp = above_vwap ? mid - tp_dist_full * TP_FRACTION : mid + tp_dist_full * TP_FRACTION;`
(or equivalent formulation that respects direction). One careful edit.

### 6.3 Add session-window override fields

Two members with defaults matching current behaviour:

```
int SESSION_OPEN_HOUR  = 8;   // hard gate: don't enter before this UTC hour
int SESSION_CLOSE_HOUR = 22;  // hard gate: don't enter at or after this UTC hour
```

Use site at L1442: replace `if (h < 8 || h >= 22) return {};` with
`if (h < SESSION_OPEN_HOUR || h >= SESSION_CLOSE_HOUR) return {};`.
One edit.

**Discipline note (CLAUDE.md):** all three changes are in
`CrossAssetEngines.hpp` (engine code, not core). They are additive
and per-instance, so default behaviour is preserved across all four
VWR instances. Sweep gets the override knob; production gets a no-op
unless engine_init.hpp opts in.

---

## 7. Pre-flight checklist (before starting the Tier 1 session)

Run at the very start of the Tier 1 session, in order:

1. `cmake --build build --target VWAPReversionBacktest --config Release -j`
   on Mac — confirm the harness currently builds clean.
2. Re-read `include/CrossAssetEngines.hpp:1202-1300` — confirm the
   class shape hasn't drifted since part-P. The three small additions
   in §6 assume those line numbers and that shape.
3. Re-read `outputs/VWR_USTEC_PHASE2_RESULTS_2026-05-14e.md` §3 (the
   TP-rate diagnostic) — orient on the metric the Tier 1 sweep needs
   to move.
4. Confirm `g_vwap_rev_nq.enabled = false` and the comment block at
   `engine_init.hpp:625-640` is still load-bearing.
5. Pull `git log --oneline -10` to confirm no in-flight changes from
   another session conflict with the harness/class edits planned.

---

## 8. Risk register for the Tier 1 work

| Risk | Likelihood | Mitigation |
|---|---|---|
| Class field additions break compile on Windows-only paths | Low | The fields are pure C++; no platform-specific code. Sandbox `g++ -fsyntax-only` is the necessary check; Mac canary is the sufficient check. |
| Default values not preserved across all four instances | Low-medium | Add explicit defaults in the class header; rely on engine_init.hpp's per-instance overrides not changing for SP/EURUSD/GER40. Audit those three after the class change. |
| TP_FRACTION = 1.0 calculation differs subtly from current `tp = vwap` | Medium | Calculated `mid + tp_dist * 1.0 = mid + (vwap - mid) = vwap` when correctly directional. Add an assert(TP == vwap) when TP_FRACTION == 1.0 during development; remove for release. |
| Tier 1 sweep produces another single-cell artifact and we waste another session | Medium | Apply the part-P lesson: any "passing" cell must include a trade-count floor (≥ 60% of baseline 4943) AND ≥ 4 of 6 fine-sweep cells positive AND monotonic, not the {0.80, 1.00} sandwich pattern. Decision rule defined upfront in the Tier 1 session's preamble. |
| Tier 1 passes but walk-forward fails | Low-medium | This is the expected stop condition. Per retune plan §7, no re-enable without 3-fold OOS. |
| Operator decides to skip Tier 1 and jump straight to Tier 4 | N/A | This memo's recommendation is Tier 1 first; if the operator prefers Tier 4, that's a valid choice and Tier 1 becomes unnecessary. |

---

## 9. What success looks like

A Tier 1 session "succeeds" if any of these is true at the end:

- **Tier 1 PASS → Phase 3 WF queued.** Tier 1 surfaces a robust
  positive-expectancy parameter set (avg_pnl ≥ +0.001/trade, TP rate
  ≥ 5%, monotonic in at least 2 of 4 axes near the local maximum,
  trade count ≥ 60% of baseline). Next session runs walk-forward.

- **Tier 1 CLEAN FAIL → escalate to Tier 4 design session.** All Tier 1
  axes show the same noise pattern (non-monotonic, low TP rate, no
  trade-count-respecting positive cells). Closure memo recommends
  Tier 4 (signal-shape redesign) without sinking the VIX/L2 data work
  effort first.

- **Tier 1 PARTIAL → Tier 2/3 informed by which axis helped.** If TP
  rate moves above 5% on a session-window restriction (e.g. NY-overlap
  only), that suggests time-of-day filtering has leverage — VIX (which
  varies by session) becomes a plausible Tier 2 addition. If
  `EXTENSION_SL_RATIO` is what helps, Tier 2/3 are still pointless.

The closure memo from the Tier 1 session should explicitly state which
of these three outcomes obtained, mirroring the structure of part-P's
PHASE2_RESULTS memo §5-7.

---

## 10. Open questions for the operator

None blocking — this scoping memo can stand without operator input. The
following are flagged as items the Tier 1 session could ask if they
become relevant:

1. **Tier 4 stomach.** If Tier 1 fails cleanly, is the operator
   prepared to invest a multi-session signal-shape redesign on
   USTEC.F, or is "engine stays disabled indefinitely" acceptable?
   USTEC.F's contribution to portfolio P&L was already marginal (the
   engine wasn't producing positive expectancy before S68 disabled it),
   so the cost of "permanent disable" is low. Worth confirming before
   sinking another session.

2. **Symbol set scope.** If a Tier 1 lever (e.g. `MAE_EXIT_RATIO = 0.40`)
   improves USTEC.F, the same lever should be tested on US500/EURUSD
   for consistency. Out of scope for the Tier 1 USTEC.F session
   itself, but worth flagging as a follow-up.

3. **Walk-forward folding choice.** Retune plan §5 Phase 3 specifies
   3-fold contiguous OOS. With a fresh Dukascopy tape (Mar 2024 → Apr
   2026 = 25 months) that's about 8 months per fold. Confirm that's
   the right granularity for USTEC.F regime variation before running.

---

## 11. Companion documents

This memo plus the following form the canonical USTEC.F VWR archive:

- `outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md` — CLOSED parameter
  retune plan
- `outputs/VWR_USTEC_PHASE1_RESULTS_2026-05-14e.md` — Phase 1 univariate
  sweep results
- `outputs/VWR_USTEC_PHASE2_RESULTS_2026-05-14e.md` — Phase 2
  refinement results + structural-rework recommendation
- `include/engine_init.hpp:625-640` — closure comment block in code
- this memo — structural rework scoping

---

*End of scoping memo. No code committed; this memo is the carry-over
artifact for the eventual VWR-USTEC.F Tier 1 structural rework session.*
