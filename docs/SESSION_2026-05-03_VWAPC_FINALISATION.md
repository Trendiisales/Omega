# VWAP-Continuation Finalisation

**Date:** 2026-05-03
**Author:** Jo (Trendiisales/Omega) + Claude session continuation
**Status:** Edge-marginal. Strict spec says shelve; one focused research path is defensible. Decision criterion documented below.
**Sister docs:**
- `SESSION_2026-05-03_NR2_20_HANDOFF.md` — the spec & decision tree we executed against
- `HANDOFF_NR2_20_BACKTEST.md` — strategy spec & §10 acceptance gate

---

## TL;DR

1. **VWAPC narrowly fails the §10 gate at default costs** (PF 1.094, Sharpe 0.498, max DD 14.34R). 3 of 5 gates pass.
2. **VWAPC narrowly fails the §10 gate at tight costs** (PF 1.155, Sharpe 1.348, max DD 12.49R). 4 of 5 gates pass — only PF still fails, and it fails by 0.145.
3. **The PF gap is dominated by one window (W4: 2025-06 → 2025-09 OOS test).** PF=0.69, Sharpe=-4.99 in W4 with 121 trades is most of what pulls the aggregate below the 1.30 bar. W2, W3, W5 average PF ≈ 1.31 at tight costs.
4. **Two windows (W1, W6) had no combo pass the in-sample train gate** even at tight costs and 100 random parameter samples. Either the gate is too strict for those regimes or no edge exists there.
5. **NR2-20 is functionally dead at both cost levels** (3 gates fail including a structural max-DD problem at ~22R vs the 15R cap). Out of scope here; separate postmortem if elected.
6. **Strict spec = shelve.** §10 says PF ≥ 1.30 or shelve. We're at 1.155.  
   **Edge-marginal reading = one focused research session is defensible** — see Path B below for the explicit decision criterion.

---

## The data — 2-year walk-forward, both cost levels

Walk-forward methodology: 6m train / 3m test / 3m step → 6 windows.
CSV: `~/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv` (50,719 bars, 2024-03-01 02:00 UTC → 2026-04-24 20:45 UTC).

Aggregate OOS metrics (VWAPC only):

| Metric                  | Threshold | Default costs (0.5 + 0.3) | Tight costs (0.2 + 0.1) |
|-------------------------|-----------|---------------------------|-------------------------|
| Profit factor           | ≥ 1.30    | 1.0939 FAIL               | **1.1549 FAIL (−0.145)**|
| Sharpe (trade)          | ≥ 1.0     | 0.4979 FAIL               | **1.3484 PASS**         |
| Max DD (R)              | ≤ 15.0    | 14.3392 PASS              | 12.4870 PASS            |
| Trade count             | ≥ 200     | 341 PASS                  | 457 PASS                |
| Win rate                | ≥ 0.38    | 0.4228 PASS               | 0.4399 PASS             |
| Total R                 | —         | +4.12                     | +24.09                  |
| Cross-strategy corr     | < 0.40    | +0.043                    | −0.009                  |

Total R nearly six-fold at tight costs. Sharpe nearly triples. This is the textbook "edge-marginal, broker-dependent" pattern from the spec.

---

## Failure shape — per-window detail (tight costs)

| Window | OOS test span      | Result                                          |
|--------|--------------------|-------------------------------------------------|
| W1     | 2024-09 → 2024-12  | no combo passed train gate                      |
| W2     | 2024-12 → 2025-03  | PF=1.09  Sharpe=1.14  trades=124  win=42.7%     |
| W3     | 2025-03 → 2025-06  | PF=1.53  Sharpe=5.66  trades=96   win=51.0%     |
| W4     | 2025-06 → 2025-09  | **PF=0.69  Sharpe=-4.99  trades=121  win=33.1%**|
| W5     | 2025-09 → 2025-12  | PF=1.31  Sharpe=3.59  trades=116  win=49.1%     |
| W6     | 2025-12 → 2026-03  | no combo passed train gate                      |

Two interacting failure modes:

1. **W4 catastrophic loss.** With PF=0.69 across 121 trades, W4 alone produced a loss in the order of ~20R. That single window is most of what holds aggregate PF below 1.30.
2. **W1 and W6 train-gate failure.** With 100 random parameterizations none cleared the in-sample gate. Either the spec gate is over-strict for those regimes, the random sweep was too coarse, or there is no edge there.

If W4 is excluded, the remaining tested windows (W2, W3, W5) average PF ≈ 1.31. That is *not* a license to drop W4 — survivorship bias is a known curve-fit vector — but it is a flag that the failure is concentrated in time, not pervasive.

---

## Three paths to close the thread

### Path A — Shelve (strict spec)

§10 says PF ≥ 1.30 after walk-forward. We are at 1.155. Spec says shelve. Write a brief postmortem alongside this doc, mark VWAPC "investigated, no clean edge", do not pursue further.

Defensible without debate. Keeps the gate honest — the gate exists so "almost passes" isn't promoted.

### Path B — Conditional research (one focused follow-up session)

Resolve three diagnostic questions in a single, time-boxed sprint. Do not iterate more than once — that is how curve-fits sneak in.

**Q1. What is W4 (2025-06-01 → 2025-09-01)?**  
Calendar-overlay the W4 OOS span. Identify whether the −20R drawdown traces to known external events (FOMC, CPI surprises, geopolitical, BoJ intervention, gold-specific news) or to a sustained regime change (e.g., volatility shift, trend reversal vs HTF EMA bias). Either:
- run `vwap_continuation_backtest.py` standalone on the W4 span only with a per-day pnl_R breakdown to localise when the loss accumulated, or
- annotate W4 manually with known calendar events.

If a clean externally-recognisable cause exists, the right move is a documented regime filter (e.g., skip days with major scheduled releases, or require ATR-based volatility within a band). Re-test under the filter.

If no clean cause exists, W4 is just bad luck or a regime VWAPC doesn't handle. Both readings argue for shelving rather than promoting.

**Q2. Why did W1 and W6 fail the train gate?**  
Re-run with a wider random sweep:

```
cd ~/omega_repo/backtest/nr2_20

python3 wf_compare.py \
    --bars /Users/jo/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv \
    --samples 200 \
    --train-months 6 --test-months 3 --step-months 3 \
    --spread-pips 0.2 --slippage-pips 0.1
```

If many combos now pass W1/W6, the original 100-sample sweep was just under-sampled. If none still pass at 200 samples, the strategy has genuinely no edge in those regimes — additional sweeping won't change that.

**Q3. Does an exhaustive grid close the PF gap legitimately?**  
Overnight run:

```
python3 wf_compare.py \
    --bars /Users/jo/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv \
    --samples 0 \
    --train-months 6 --test-months 3 --step-months 3 \
    --spread-pips 0.2 --slippage-pips 0.1
```

Then read `wf_compare_vwapc.csv` and look at per-window param picks. If the same params keep winning across at least 4 of 6 windows, the gap closes via genuine generalisation. If every window picks different params, the apparent improvement is curve-fit and not deployable.

**Decision criterion for Path B:**

Promote VWAPC to C++ port spec only if **all** of the following hold after the sprint:
- aggregate OOS PF ≥ 1.30 at tight costs (0.2 + 0.1)
- the same parameter set wins in at least 4 of 6 windows
- if W4 was filtered out by a regime rule, that rule is documented, externally-anchored (calendar event or ATR band), and not parameter-tuned to W4 specifically
- max DD remains ≤ 15.0R after any filter is applied

Otherwise, fall back to Path A.

### Path C — Promote as edge-marginal, broker-conditional

Only viable if Omega can demonstrate live spread + slippage of ≤ 0.2 + 0.1 pips on XAUUSD using brokerage statements over a recent 30-day period of *actual filled orders* — not a marketing page, not a quote-request average, real fills.

Even with that evidence the strategy still fails PF by 0.145 at the spec'd 1.30. So Path C requires either (a) a documented spec amendment lowering the PF gate with explicit rationale, or (b) shadow deployment with hard risk caps until live PF clears 1.30 over a meaningful sample (≥ 200 trades).

Low confidence this is the right call unless live realised costs are materially tighter than even the tight test costs.

---

## Recommendation

Path B with a hard time-box of one follow-up session (~2 hours of compute plus analysis). Decision at the end of that session against the criterion above. If the criterion isn't met, fall straight to Path A.

If you don't want a follow-up session at all, Path A directly. Cleaner, takes ten minutes to write the postmortem. The gate exists for a reason.

Path C is the wrong call to make on this evidence alone.

---

## NR2-20 disposition (pointer only)

NR2-20 is dead at both cost levels:
- PF: 1.028 → 1.052 (cost relief did almost nothing)
- Sharpe: 0.21 → 0.55 (still half the bar)
- Max DD: 22.52R → 21.92R (structural; well above the 15R cap regardless of cost)
- 2 of 6 windows had no signal under either cost level
- Filter selection drifted across windows (ema, both, vwap, both) — no stable pick

Recommend a separate killer postmortem doc, not bundled into this finalisation. NR2-20 doesn't share VWAPC's narrow-miss character and shouldn't be treated as if it does.

---

## Artifacts on disk after this session

- `~/omega_repo/backtest/nr2_20/resample_ticks_to_15m.py` — tick-to-15m bar resampler (new this session)
- `~/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv` — 50,719 bars, 2024-03-01 → 2026-04-24
- `~/omega_repo/backtest/nr2_20/wf_compare_nr2_20.csv` — per-window NR2-20 param picks (most recent run only)
- `~/omega_repo/backtest/nr2_20/wf_compare_vwapc.csv` — per-window VWAPC param picks (most recent run only)
- `~/omega_repo/docs/SESSION_2026-05-03_VWAPC_FINALISATION.md` — this doc

The per-window CSVs were overwritten by the cost-sensitivity run (no `--out-dir` differentiation in the script). If exact default-cost per-window params are needed for the postmortem, re-run the default-cost command from `SESSION_2026-05-03_NR2_20_HANDOFF.md` first.

No core engine, spec, or production-touchpoint files were modified this session. All work this session is additive.

— END FINALISATION —
