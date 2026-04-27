# C6 #1C — Final Report

**Status:** Closed, no deploy.
**Type:** Methodology investigation, plus parked predicate findings.
**Branches squash-merged into this commit:** `c6-1c-step3-regime-diagnostic` (3b383074), `c6-1c-step3a-regrade-v3` (91e8d1d3), `c6-1c-step3c-deploy` (bd0f2c0c).
**Live HBG modifications:** none.
**Live VPS modifications:** none.
**Next deployment-relevant work:** S51 parameter sweep against `~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`. After S51 completes, Edge A may be revisited (see §5.1).

---

## 1. Executive summary

C6 #1C set out to take the surviving STRONG-cohort predicates from C6 #1A and #1B, pass them through a regime diagnostic, regrade them under v3 cost models, validate them against the OOS sentinel, apply a vol_bar60 gate, and ship the survivors as live-engine fire-time gates on `GoldHybridBracketEngine` (HBG).

The plan reached the end of step 3c.3 with two surviving predicates and a fully-specified integration plan. It then failed in step 3c.4 — not because the predicates were marginal, not because the integration was infeasible, but because of a structural defect in the edge-finder's scoring methodology that had been latent since the start of the C-cycle and that nothing in steps 1 through 3c.3 was set up to detect.

The defect, in one sentence: **the edge-finder scores predicates against synthetic forward-return scenarios that do not match the SL/TP/horizon profile of any live engine, so the resulting "edges" are predicates with non-zero signal *against the synthetic scoring lens*, not predicates with non-zero signal against actual live trading PnL.**

The two surviving predicates from C6 #1C — pid 57226 (SHORT, vwap_z ≤ −4.27 in ASIAN/VOL_HI/TREND_UP) and pid 51274 (LONG, session_range_pts ≤ 16.45 in ASIAN/VOL_HI/TREND_UP) — were scored against `BRACKETS[5]`, a synthetic 240-minute / 100-pt SL / 300-pt TP forward-return bracket. HBG runs a state machine with ~8–13 pt SL, RR=2 TP, MFE-proportional trail from 1.5 pt MFE / 15 s held, 60 s cooldown. Slot 5 and HBG are not the same thing. A predicate that produces sharpe 0.528 against b5 has unknown sharpe against HBG.

OOS-only event analysis run during step 3c.4 (Feb–Apr 2026) produced a more nuanced finding than the original 3c.2 portfolio metrics suggested:

- **Edge A** has non-trivial OOS signal in non-tariff months. Excluding April 2026, Edge A still produces sharpe 0.387 over 234 trades across Feb+Mar with a positive mean (+15.19) and a positive median (+1.64). The April bonus is real (sharpe 1.078, win rate 87%) but Edge A is not solely a tariff detector.
- **Edge B** is essentially a tariff-burst detector. Excluding April, Edge B's sharpe collapses from 0.315 to 0.078, mean drops to +1.81 (transaction-cost noise floor), and Mar 2026 alone produces sharpe ≈ −0.001 — an entire month with no signal. Edge B is not deployable.

C6 #1C is closing without deploying either predicate. Edge B is filed as not-real (tariff-detector, not steady-state edge). Edge A is filed as **interesting-but-unvalidated** — the b5 sharpe is still not predictive of live HBG performance (synthetic-bracket scoring defect), but the non-tariff OOS signal is non-trivial enough that Edge A is worth re-evaluating against real HBG after S51 completes. The previously-parked edge cell from pid 70823 (b3/b4, REGIME_LOW_VOL, TARIFF_DEPENDENT) is in the same scoring-lens situation as Edge A and Edge B and is flagged accordingly.

The work of steps 1–3c.3 is preserved in the squash-merge for traceability. The next deployment-relevant work is S51 — the parameter sweep harness already built at `dc43ddf9` and ready to run.

---

## 2. What was done

### 2.1 Step 1 (pre-flight)
The starting state at the head of C6 #1C was the v2 STRONG cohort from #1B: `(pid, side, bracket_id)` triples that had survived cost-overlay grading at `cost_04_bar`, `cost_04_wall`, `cost_08_bar`, `cost_08_wall`. The cohort included Edge A and Edge B at b5, plus edges 81481, 64201, 70823 across various bracket slots.

### 2.2 Step 3 (pre-flight regime diagnostic)
Branch `c6-1c-step3-regime-diagnostic`, commit `3b383074`. New file `backtest/edgefinder/analytics/regime_diagnostic.py` (33,297 bytes). Per-edge breakdown of the v2 STRONG cohort, computing mean PnL by vol decile, ATR decile, and session for each `(pid, side, bracket_id)`. Findings:
- pid 81481 and pid 64201 dead bleeders across all regime cells (CULL).
- pid 70823 b3/b4 profitable only in REGIME_LOW_VOL with `v3_post_dominates_atr=Y`, distribution heavily weighted by April 2026 — tariff-burst detector, marked TARIFF_DEPENDENT, parked.
- Edge A and Edge B profitable in ASIAN/VOL_HI/TREND_UP, with comparable per-decile behaviour.

### 2.3 Step 3a (v3 regrade)
Branch `c6-1c-step3a-regrade-v3`, commit `91e8d1d3`. New file `backtest/edgefinder/analytics/regrade_strong_v3.py` (40,596 bytes). Re-ran cost grading with `cost_04_bar`, `cost_04_wall`, `cost_08_bar`, `cost_08_wall` overlays plus `v3_post_dominates_atr` post-OOS dominance check. Output `regrade_v3_per_edge.csv` (7 edges × 50 columns).

Final v3 STRONG cohort:
- Edge A: pid 57226 SHORT XAUUSD b5, n=678 ungated.
- Edge B: pid 51274 LONG XAUUSD b5, n=648 ungated.
- pid 70823 b3 and b4 retained as TARIFF_DEPENDENT.
- pid 81481 and pid 64201 CULL.

### 2.4 Step 3c.1 (gate apply)
Branch `c6-1c-step3c-deploy`, commit `1b01e47f`. New file `backtest/edgefinder/analytics/gate_apply.py` (17,387 bytes). Applied vol_bar60 ≥ 0.000375 gate. Reconciled exactly against step 3a's `regrade_v3_per_edge.csv` (7 metrics × 2 edges, all within 1e-3 tolerance). Post-gate, post-dedup stats over the OOS window (Feb–Apr 2026):
- Edge A: n=678 → 302 (deduped from 57226/57230 dual-side rows), sharpe=0.245 → 0.528, sum=5389 → 6751, maxDD=−1560 → −842.
- Edge B: n=648 → 220, sharpe=0.194 → 0.315, sum=2141 → 1734, maxDD=−896 → −663. **Note**: gate is sum-negative for Edge B even though sharpe and maxDD improve.

### 2.5 Step 3c.2 (deduped portfolio metrics)
Commit `cdc69538`. New file `backtest/edgefinder/analytics/portfolio_metrics.py` (29,122 bytes). Joint portfolio (Edge A + Edge B, equal-weight per trade, gated, OOS-only):
- n_trades = 522, n_days = 27, sum = 8485.18 pts.
- sharpe per trade = 0.4424 (vs ungated 0.2170).
- sharpe per day = 0.3777.
- maxDD per trade = −1002.47 (vs ungated −1683).
- Correlation Edge-A ↔ Edge-B (outer-zerofill): −0.072 gated, −0.022 ungated. Near-zero diversification.

### 2.6 Step 3c.3 (integration spec)
Commits `bec5d07b` (initial) and `bd0f2c0c` (revision after risk audit). New file `backtest/edgefinder/docs/step3c3_integration_spec.md` (17,393 bytes, 414 lines). Specified mechanical wiring of the vol_bar60 gate into HBG between ATR_GATE_FAIL (L304-333) and bracket fire (L336+) inside `Phase::ARMED` of `GoldHybridBracketEngine::on_tick`. Risk audit closed R1, R2, R3, R7; left R4, R5, R6 flagged.

### 2.7 Step 3c.4 (this report and the OOS event analysis behind it)
Two things happened in 3c.4 that turned a planned deployment report into this methodology investigation document:

**First, R4 was reframed during a resolution attempt.** The original R4 question — "confirm bracket_id=5 maps to GoldHybridBracketEngine" — assumed `bracket_id` was an engine-routing field. Reading `PanelSchema.hpp` and `ForwardTracker.hpp` showed that `bracket_id` indexes a fixed array of synthetic forward-return scenarios with no relationship to live engines. See §3.

**Second, a per-month OOS analysis** of the gated trade journal (Feb–Apr 2026) was run to determine whether the predicates' edge was steady-state or event-driven. See §5.

---

## 3. The methodology defect

R4 in the 3c.3 spec was originally framed as: "confirm that bracket_id=5 in the edge-finder panel maps to GoldHybridBracketEngine and not BracketEngine or IndexHybridBracketEngine." The implicit assumption was that bracket_id is an engine-routing field. That assumption was wrong.

### 3.1 What `bracket_id` actually is

From `backtest/edgefinder/extractor/PanelSchema.hpp` (`96cb951d`, sha `3c427591`, L185-192):

```
constexpr BracketSpec BRACKETS[N_BRACKETS] = {
    {  5LL*60  * 1000LL,  10.0,  20.0 },  // b0: scalp 5m / 10sl / 20tp
    { 15LL*60  * 1000LL,  20.0,  50.0 },  // b1: short 15m / 20sl / 50tp
    { 15LL*60  * 1000LL,  30.0,  60.0 },  // b2: medium 15m / 30sl / 60tp
    { 60LL*60  * 1000LL,  50.0, 100.0 },  // b3: swing 60m / 50sl / 100tp
    { 60LL*60  * 1000LL, 100.0, 200.0 },  // b4: wide 60m / 100sl / 200tp
    {240LL*60  * 1000LL, 100.0, 300.0 },  // b5: session 240m / 100sl / 300tp
};
```

Six fixed `(horizon_ms, sl_pts, tp_pts)` tuples — parameters of six **synthetic** forward-return scenarios. Each scenario is a fictional bracketed trade: at every bar close, hypothetically arm a TP-pts bracket and an SL-pts bracket on tick price; resolve whichever hits first; if neither hits within `horizon_ms`, mark-to-market at horizon. Six independent simulations per bar close.

`backtest/edgefinder/extractor/ForwardTracker.hpp` (`96cb951d`, L80, L96-110, L161-184) is the simulator. For each bar close it arms `BRACKETS[i]` for `i ∈ 0..5`, then resolves all six against subsequent ticks, writing results into `panel_row.fwd_bracket_pts[i]` and `panel_row.fwd_bracket_outcome[i]`.

`backtest/edgefinder/extractor/EventExtractor.cpp` (`96cb951d`, L335-343) confirms the chain: tick → BarState → on bar close → ForwardTracker → BRACKETS armed → subsequent ticks resolve TP/SL/MtM. There is no live engine in this pipeline.

### 3.2 What HBG actually is

`include/GoldHybridBracketEngine.hpp` (`96cb951d`, sha `59a8c3ca`) is an entirely different mechanism. From its parameter block (L80-113):

- `MIN_RANGE = 6.0`, `MAX_RANGE = 25.0` — compression detector.
- `SL_FRAC = 0.5`, `SL_BUFFER = 0.5` — SL distance is `range × 0.5 + 0.5 pts`. For a typical 12-pt compression, SL ≈ 6.5 pts. For a maximum 25-pt compression, SL ≈ 13 pts.
- `TP_RR = 2.0` — TP at 2× SL distance. Roughly 13–26 pts.
- `TRAIL_FRAC = 0.25` — MFE-proportional trail.
- `MIN_TRAIL_ARM_PTS = 1.5`, `MIN_TRAIL_ARM_SECS = 15` — trail starts arming at 1.5 pt MFE / 15 s held.
- `COOLDOWN_S = 60`, `DIR_SL_COOLDOWN_S = 120` — post-fire cooldown.
- No fixed horizon. Position exits when SL hits, TP hits, or trail is hit.

HBG is a state machine. It detects compression. It arms bracket orders both sides. It fills whichever side breaks. It manages the live position with an MFE-proportional trail that starts locking after 1.5 pt MFE has accumulated for 15 s. It exits aggressively when MFE retreats.

### 3.3 The mismatch

`BRACKETS[5]` is a 240-minute / 100 pt SL / 300 pt TP bracket. HBG runs ~8–13 pt SL with RR=2 TP and an MFE trail that fires within minutes of any meaningful move. SL distance differs by an order of magnitude. TP distance differs by an order of magnitude. Time horizon differs by two orders of magnitude. Exit logic is structurally different (state-machine trail vs static MtM-at-horizon).

A predicate that produces a high sharpe in `BRACKETS[5]` simulations is a predicate where, conditional on it firing, the next 240 minutes of price action favoured one side strongly enough that a 100-pt-SL bracket survived and frequently hit a 300-pt TP — i.e. **price persistence over four hours with high tolerance for mid-flight noise**. HBG, gated by the same predicate at fire-time, would experience that future price action through its own state machine. The 4-hour persistence is irrelevant — HBG either takes profit at TP within minutes, or trails out within minutes, or stops out within minutes. The synthetic-bracket sharpe says nothing calibrated about how HBG would handle the same moments.

For deployment-decision purposes, the b5 sharpe is uninformative about live HBG performance. It is a screening signal: "this predicate appears to have non-zero forward-return information under one specific simulation profile." Whether that translates to non-zero PnL through HBG's actual state machine is a separate empirical question that nothing in C6 #1A/#1B/#1C answers.

### 3.4 Every C6 edge inherits this defect

The defect is not specific to Edge A or Edge B. It is a property of the edge-finder methodology. Every predicate ranked by C6 #1A and #1B was scored against `BRACKETS[i]` for some `i`. None of those rankings has any guaranteed relationship to live-engine PnL.

The C6 #1C STRONG cohort therefore contains predicates that scored well in synthetic-bracket terms but whose live-engine PnL is unknown. Without real-engine validation, no member of the cohort is deployable.

---

## 4. Why panel-derived gates against HBG are a difficult target (theoretical)

A separate concern surfaced in 3c.4 around whether to rebuild the methodology by discovering gates against real HBG fires. The argument is that even a clean methodology that scores against real engine PnL would face structural difficulty for HBG specifically.

### 4.1 The redundancy argument

HBG's internal logic already reads price-action features — implicitly. Its compression detector reads the rolling 20-bar high-low range. Its expansion gate reads the median of recent ranges. Its cooldown gates filter on recent fires. Its spread gate reads tick-level bid-ask. Its DOM filter (when L2 is real) reads order-book slope and walls. Its trail logic reads MFE.

These internal reads are computed on the same tick stream that the panel writer uses to build PanelRow features (vwap_z, rsi_14, ema_9_minus_50, vol_60bar_stddev, range_20bar_position, bb_position, ret_Nbar_pts, etc.). Same input stream, different transforms. The transforms are not statistically independent.

A gate built from PanelRow features and applied to HBG can therefore improve HBG only by either (a) being a slightly better version of a filter HBG already approximates, or (b) capturing a price-action structure that HBG's compression+expansion+cooldown logic is genuinely blind to. (a) is a parameter tweak in disguise — S51 will do that better and more directly. (b) exists in principle; whether it exists in practice for any given predicate is an empirical question.

### 4.2 Empirical caveat to §4.1

The redundancy argument is theoretical. The actual OOS data on Edge A (see §5.1 below) shows non-trivial signal at sharpe 0.387 across 234 trades excluding April. That is at least consistent with Edge A capturing something HBG's internal logic does not — though the b5 scoring lens means we cannot quantify the live-HBG impact from these numbers.

The honest read: panel-feature gate discovery against HBG faces a higher noise floor than gate discovery against engines whose internal logic is less correlated with the panel. It is not impossible. Edge A's non-tariff signal is suggestive enough that the case for real-engine validation is stronger than the §4.1 theoretical argument alone would suggest.

### 4.3 Recommendation for future methodology

If gate-discovery work continues against HBG, target signals genuinely orthogonal to HBG's internal logic where possible: cross-asset (DXY direction, equity-index correlation), calendar effects (FOMC proximity, NFP day, day-of-week), news events. Panel features should be treated as a higher-noise discovery axis for HBG specifically, with mandatory real-engine validation before any deployment talk.

---

## 5. The OOS event analysis

The journal at `backtest/edgefinder/output/paper_trade/regime_diagnostic_journal.csv` contains 3,468 rows spanning Feb–Apr 2026 only — the OOS partition. Per-month breakdown of the gated trade distribution for each surviving edge:

### 5.1 Edge A — pid 57226 SHORT XAUUSD b5

Predicate: `vwap_z ≤ -4.2690483935941375 | session=ASIAN, vol=VOL_HI, trend=TREND_UP`.

| Month | n | sum | mean | median | sharpe | wr | maxDD |
|---|---|---|---|---|---|---|---|
| Feb 2026 | 132 | +1827 | +13.84 | +3.02 | +0.337 | 54.5% | −556 |
| Mar 2026 | 102 | +1727 | +16.93 | +0.42 | +0.459 | 51.0% | −275 |
| Apr 2026 | 68 | +3198 | +47.02 | +32.86 | +1.078 | 86.8% | −275 |
| Full OOS | 302 | +6751 | +22.36 | +12.01 | +0.528 | 60.6% | −842 |

Excluding April: n=234, sum=+3554, mean=+15.19, median=+1.64, sharpe=+0.387, wr=53.0%.
Excluding Mar+Apr: n=132 (Feb only), sharpe=+0.337.

**Reading:**
- April 2026 is exceptional (sharpe more than doubles, win rate jumps to 87%, median +33 vs near-zero in other months) — the tariff regime expressing itself.
- Feb and Mar are not collapses. Feb sharpe 0.337 with 132 trades and Mar sharpe 0.459 with 102 trades are both non-trivial signal.
- The 234-trade ex-April slice produces sharpe 0.387 — well above noise.
- Mar's mean (+16.93) is much higher than its median (+0.42), indicating Mar PnL is heavily skewed by a few large winners. This is consistent with a predicate that occasionally catches large persistent moves but is otherwise close to zero per trade.

**Synthetic-bracket caveat applies.** All numbers above are b5 outcomes (240m / 100sl / 300tp MtM). They describe Edge A's behaviour against a 4-hour bracketed scoring lens, not against HBG. Live HBG performance gated by the same predicate is unknown.

**Status:** Not deployed. **Re-evaluate after S51 completes** — Edge A is the strongest case in the C6 #1C cohort for real-engine validation if such validation work is undertaken in a future session.

### 5.2 Edge B — pid 51274 LONG XAUUSD b5

Predicate: `session_range_pts ≤ 16.450000000000728 | session=ASIAN, vol=VOL_HI, trend=TREND_UP`.

| Month | n | sum | mean | median | sharpe | wr | maxDD |
|---|---|---|---|---|---|---|---|
| Feb 2026 | 76 | +302 | +3.98 | +1.21 | +0.148 | 52.6% | −663 |
| Mar 2026 | 90 | −1 | −0.01 | +1.95 | −0.001 | 53.3% | −464 |
| Apr 2026 | 54 | +1433 | +26.53 | +37.45 | +1.274 | 88.9% | −65 |
| Full OOS | 220 | +1734 | +7.88 | +5.54 | +0.315 | 61.8% | −663 |

Excluding April: n=166, sum=+301, mean=+1.81, median=+1.37, sharpe=+0.078, wr=53.0%.
Excluding Mar+Apr: n=76 (Feb only), sharpe=+0.148.

**Reading:**
- April 2026 is exceptional (sharpe 1.274, win rate 89%) — same tariff bonus as Edge A.
- Mar 2026 is **flat**: sharpe ≈ −0.001 over 90 trades. An entire month with no signal at all. This is the killer.
- Feb is barely positive (sharpe 0.148, n=76).
- Excluding April collapses the edge: sharpe 0.078, mean +1.81 per trade — at or below transaction-cost noise floor for a 240m bracketed scenario.

**Edge B is essentially a tariff-burst detector.** The Mar 2026 zero-signal month combined with the ex-April collapse means whatever Edge B captured in April is an event-specific phenomenon, not a steady-state regime edge. Even ignoring the synthetic-bracket-vs-HBG mismatch, Edge B's apparent edge is one event large enough to dominate two months of noise.

**Status:** Not deployed. Filed as not-real-edge. No re-evaluation recommended.

### 5.3 Joint portfolio (Edge A + Edge B union, equal-weight per trade)

| Slice | n | sum | mean | sharpe | wr |
|---|---|---|---|---|---|
| Full OOS | 522 | +8485 | +16.26 | +0.442 | 61% |
| Excl Mar | 330 | +6760 | +20.48 | +0.525 | 66% |
| Excl Apr | 400 | +3855 | +9.64 | +0.282 | 53% |
| Excl Mar+Apr | 208 | +2130 | +10.24 | +0.279 | 54% |

**Reading:**
- Excl-Apr joint sharpe 0.282 over 400 trades is real signal — but it's almost entirely Edge A. (Edge A excl-Apr sharpe 0.387; Edge B excl-Apr sharpe 0.078.)
- The portfolio's apparent sharpe of 0.442 over all OOS is driven by April (55% of total points from 23% of trades) plus Edge A's non-April signal. Edge B contributes mostly noise outside April.

**Status:** Joint portfolio deployment was the original C6 #1C target. Not deployed. If a future session validates Edge A against real HBG and finds positive results, Edge A *alone* is the candidate, not the joint portfolio.

---

## 6. The parked pid 70823 cell

Step 3 (regime_diagnostic) flagged pid 70823 b3/b4 as TARIFF_DEPENDENT — profitable only in REGIME_LOW_VOL with `v3_post_dominates_atr=Y`, distribution heavily weighted by tariff months.

The same methodology defect applies. pid 70823 was scored against `BRACKETS[3]` (60m / 50sl / 100tp) and `BRACKETS[4]` (60m / 100sl / 200tp). Both synthetic, neither matches any live engine. The TARIFF_DEPENDENT label is correct but understated: it identifies one problem (regime concentration), but not the deeper one (the b3/b4 sharpe doesn't predict live PnL either).

pid 70823 b3/b4 stays parked. Future-review rationale should be both:
1. (existing) TARIFF_DEPENDENT — fires concentrated in tariff regime cells.
2. (added by this report) Synthetic-bracket scoring lens does not predict live-engine PnL; bracket-specific sharpe is screening signal only, not actionable evidence.

If this predicate is ever revisited, both reasons need to be addressed.

---

## 7. What was almost done and is not being done

### 7.1 Almost done: HBG fire-time gate
The 3c.3 spec defined a clean, two-parameter, defaulted-additive modification to `HBG::on_tick` to inject the vol_bar60 gate plus the predicate gate. Wiring at `tick_gold.hpp` L1929 / L1980 was specified. Sidecar logging plan was specified. None is being implemented. The spec is preserved at `backtest/edgefinder/docs/step3c3_integration_spec.md` for the record but is **superseded** by this report.

### 7.2 Almost done: Real-engine validation harness
A plan emerged to add a `--mode decorated-fires` path to `OmegaSweepHarness.cpp` that would run HBG over the tick history, snapshot PanelRow features at every fire-time, and emit a per-trade CSV. This is not being built in C6 #1C. It is **a candidate next step after S51** if the OOS findings on Edge A justify the build.

The harness would need:
- A new CLI mode in the existing sweep harness target.
- One `HBG_T` instance with default live parameters.
- One `BarState` instance driven from the same tick loop, emitting PanelRow on every M1 close.
- A snapshot mechanism: at every HBG fire-callback, attach the most-recent PanelRow to the trade record.
- Per-trade CSV output: trade fields plus all ~70 panel features at fire-time.
- (Optional, for gated runs) Three-line additive `fire_gate` callback parameter on `HBG_T::on_tick` defaulted to no-op.

Build estimate: ~300-500 lines added to the existing harness, no modifications to live engines, no modifications to BarState, no modifications to the panel schema. Additive only.

### 7.3 Not done in C6 #1C: S51 parameter sweep
The S51 sweep harness has been built and ready since `dc43ddf9` (2026-04-27). It was deferred during sessions 2 and 3 of C6 #1C in favour of finishing the regrade, gate, and integration spec. With C6 #1C closing, S51 is unblocked and is the next session's work.

S51 specifications:
- Five parameters per engine: `(min_range, max_range, sl_frac, tp_rr, trail_frac)` for HBG.
- Geometric grid: 0.5× to 2.0× of default, 7 values per parameter.
- 7^5 = 16,807 combos per engine.
- Priority order: HBG → DXY → AsianRange → VWAPStretch → EMACross. DXY produces zero trades without a DXY tick feed and is included only for harness completeness.
- Total: ~67k runs.
- Curve-fit guard: stability × PnL ranking across four chronological quarters.
- Output: ranked top-50 per engine in `sweep_results/`.
- Backtest input: `~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv` (4.94 GB, 2024-03 to 2026-04).
- Build: existing CMake `OmegaSweepHarness` target on Mac.
- VPS: still on `ed95e27c`, no rebuild needed for S51.

---

## 8. Methodology recommendations for C7 and beyond

### 8.1 Score against the real engine, not synthetic brackets
The synthetic `BRACKETS[6]` array is fine for **hypothesis generation** — finding panel features with non-zero predictive power in *some* forward-return regime. The synthetic-bracket sharpe/sum/maxDD numbers should never be presented as "edge metrics" in deployment-decision contexts. They are screening signals only.

The validation step for any deployment-bound predicate must score against the real engine that will fire it. Two routes:
1. Decorate real-engine fires with feature snapshots (the §7.2 harness) and compute predicate-conditional vs predicate-unconditional PnL distributions on the *real* trades.
2. Run the real engine twice — once unconditional, once with the predicate as a fire-time gate — and compare the resulting trade distributions directly. (Requires the `fire_gate` callback hook in `HBG_T`.)

Either route produces a metric with a meaningful relationship to "what this gate would do in production." The b-slot sharpe does not.

### 8.2 Choose orthogonal signals where possible
For HBG specifically, panel features overlap with HBG's internal logic. Discovery against panel features faces a higher noise floor than discovery against orthogonal signals. Where future gate work is undertaken, prioritise: cross-asset, calendar, news, term structure. Panel features are still usable but should be expected to require larger samples and tighter validation.

### 8.3 Treat OOS as fragile after multi-stage selection
The C6 #1C OOS partition (2026-02-01 to 2026-04-30) was peeked at during steps 1, 2, 3a as a selection filter. By step 3c, OOS is no longer fully held-out — it has been used implicitly as a third selection round. The Edge A finding of sharpe 0.387 ex-April is more robust than nothing, but it is in-sample-plus-peeked-OOS, not unbiased held-out. The only fully held-out window is post-2026-04-30 (live-from-now).

For future C-cycles, either the held-out partition needs to be larger (so it can absorb one selection peek), or the selection process needs to be staged so at most one peek happens at OOS, with strict no-further-tuning afterwards.

---

## 9. Open items and disposition

| Item | Status | Disposition |
|---|---|---|
| R1 std ddof | Resolved | ddof=1 confirmed. C++ implementation, if ever built, must use n-1 divisor. |
| R2 input source | Resolved | M1 bar close on mid-price. Live `g_bars_gold.m1.bars_[i].close` matches. |
| R3 lookahead | Resolved | Returns window `[k-60 : k]` exclusive. No lookahead. |
| R4 bracket-engine mapping | **Re-resolved as defect** | bracket_id is not an engine-routing field; it indexes synthetic forward-return scenarios. The defect is structural. See §3. |
| R5 warmup | Not applicable now | No HBG modification, no warmup procedure required. |
| R6 threshold drift | Not applicable now | No vol_bar60 threshold being deployed. |
| R7 bar-count vs wall-clock | Resolved | Spec uses bar-count. Sanity-check during any future implementation that OHLCBarEngine doesn't insert synthetic flat bars across gaps. |
| Edge A deployment | **Cancelled, but parked for re-evaluation** | Not deployed. b5 sharpe not predictive of live HBG. Non-tariff OOS signal sharpe 0.387 makes Edge A the strongest C6 #1C candidate for real-engine validation if future work is undertaken. |
| Edge B deployment | **Cancelled, not parked** | Not deployed. Tariff-burst detector. Mar 2026 sharpe ≈ 0; ex-April sharpe 0.078 (transaction-cost noise). Filed as not-real-edge. |
| pid 70823 b3/b4 | Reaffirmed parked | Stays parked. Rationale extended: TARIFF_DEPENDENT *and* synthetic-bracket-defect. |
| pid 81481, pid 64201 | CULL (unchanged) | Bleeders across all regime cells. |
| 3c.3 integration spec | **Superseded** | Preserved at `backtest/edgefinder/docs/step3c3_integration_spec.md` for record. Not authoritative; this report supersedes it. |
| vol_bar60 sidecar logging | Not deployed | Idea preserved in 3c.3 spec for any future gate that does ship. |
| S51 parameter sweep | Queued | Next session. Harness HEAD `dc43ddf9`. |
| Decorated-fires harness for Edge A real-engine validation | Conditionally queued | Build only after S51 completes if (a) S51 results don't subsume Edge A's expected effect and (b) the Edge A non-tariff signal still looks worth chasing in light of S51 findings. |

---

## 10. Closing posture

C6 #1C closes without a live deployment. The work product is this methodology investigation, the artefacts in `backtest/edgefinder/analytics/` (regime_diagnostic.py, regrade_strong_v3.py, gate_apply.py, portfolio_metrics.py), the artefacts in `backtest/edgefinder/docs/` (step3c3_integration_spec.md, this report), and three institutional-memory findings:

1. Synthetic-bracket scoring does not predict live-engine PnL (§3).
2. Panel-feature gate discovery against HBG faces a higher noise floor than orthogonal-signal discovery (§4) — but Edge A's non-tariff OOS signal of sharpe 0.387 over 234 trades is a counter-example worth real-engine validation (§5.1).
3. Multi-stage selection erodes OOS held-out status; future C-cycles should design around this (§8.3).

Edge B is filed as not-real. Edge A is filed as parked-pending-real-engine-validation, with that validation conditional on S51 results. pid 70823 stays parked with extended rationale. No HBG modifications. No VPS modifications. No live-side changes of any kind.

Next deployment-relevant work: S51 against the real tick history with the real engines. Already-built harness, real engine, real ticks, real PnL, quarter-stratified curve-fit guard. That work has been ready since 2026-04-27 and was deferred only by C6 #1C session occupancy. With C6 #1C closing, S51 is unblocked.

---

*End of report.*
