# SESSION HANDOFF — NR2-NR20 + Inside Bar (and VWAP comparison)
**Date:** 2026-05-03
**Author:** Jo (Trendiisales/Omega) + Claude session continuation
**Status:** Viability gate INCONCLUSIVE on 6m data → re-run on 2yr is the next single decision-maker.
**Sister docs:** `HANDOFF_FVG_BACKTEST.md`, `SESSION_2026-05-03_FVG_VERIFIER_PASS.md`

---

## TL;DR for the next session

1. **All code is built and verified to run** on Jo's Mac. No more engine work needed before the gate test.
2. **Two strategies in play** — NR2-NR20 compression-breakout (inspired by Zeiierman TV indicator), and VWAP-continuation (separate idea Jo asked to compare).
3. **Both currently fail the §10 acceptance gate** but only on a 6-month dataset that is too short for the proper 6m-train / 3m-test walk-forward methodology. Jo confirms 2 years of XAUUSD 15m bars is available.
4. **Single next action for the next session:** run the head-to-head walk-forward against the 2-year CSV and re-evaluate the gates. The strategies, parameters, costs, and gates are unchanged — this is purely a data-volume re-run.
5. **No core Omega code has been touched.** All work lives in `~/omega_repo/backtest/nr2_20/` and `~/omega_repo/docs/`.

---

## Files delivered this session

All under `~/omega_repo/backtest/nr2_20/`:

| File | Purpose | Touched by next session? |
|---|---|---|
| `HANDOFF_NR2_20_BACKTEST.md` | Spec — strategy rules, params, walk-forward methodology, §10 acceptance gate | No, just read it |
| `nr2_20_backtest.py` | NR2-20 compression-breakout engine + CLI | No |
| `wf_nr2_20.py` | Walk-forward driver, NR2-20 only | No (use wf_compare instead) |
| `vwap.py` | Shared VWAP + HTF-EMA utilities | No |
| `vwap_continuation_backtest.py` | Standalone VWAP-continuation engine + CLI | No |
| `wf_compare.py` | Head-to-head walk-forward (THIS is the gate) | Yes — runs the re-test |
| `README_NR2_20.md` | Operator-facing run instructions | Read for command syntax |

This handoff doc lives in `~/omega_repo/docs/SESSION_2026-05-03_NR2_20_HANDOFF.md`.

---

## What the strategies do (one paragraph each)

**NR2-20 compression-breakout** — Detects clusters of narrow-range and inside-bar candles to form a "compression box". On a candle close beyond the box edge, in the direction of a trend filter (EMA, VWAP, or both), enters with SL at the opposite box edge and TP at RR-multiple of risk. Default 15m XAUUSD. One position per engine.

**VWAP-continuation** — Uses higher-TF EMA trend (default 1H from resampled 15m bars) as the directional bias. Waits for price to cross daily VWAP in the trend direction, then waits for a pullback within a few pips of VWAP, then enters on a continuation candle that breaks the recent high (long) or low (short). SL at swing extreme + buffer, TP at RR multiple.

Trades CSV schema is identical between the two engines so they're directly comparable.

---

## What was tested today

Single CSV: `~/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2025-09-01_2026-03-01.csv`
- 11,698 15m bars
- Span: 2025-09-01 → 2026-02-27 (just under 6 months)
- Schema: `ts_unix,open,high,low,close,tick_count,spread_mean` (loader handles this natively)

### Single-shot runs (default params, all 6 months)

| | Trades | Win% | PF | Sharpe | Total R | Max DD R |
|---|---|---|---|---|---|---|
| NR2-20 + VWAP filter | 575 | 50.3 | 1.02 | 0.21 | +4.27 | 21.61 |
| VWAP-continuation | 271 | 54.6 | 1.20 | 2.47 | +23.96 | 9.54 |

VWAP-continuation looks better on the standalone. NR2-20 break-even.

### Walk-forward (2m train, 1m test, 1m step → 3 windows)

| Strategy | Window | Filter | PF | Sharpe | Trades | Win% |
|---|---|---|---|---|---|---|
| NR2-20 | W1 (Dec 2025) | ema | 1.31 | 3.48 | 56 | 48.2 |
| NR2-20 | W2 (Jan 2026) | both | 1.14 | 1.62 | 82 | 43.9 |
| NR2-20 | W3 (Feb 2026) | ema | 0.96 | -0.55 | 77 | 49.4 |
| VWAPC | W1 | n/a | 0.77 | -3.43 | 30 | 36.7 |
| VWAPC | W2 | n/a | 1.23 | 2.79 | 47 | 55.3 |
| VWAPC | W3 | n/a | 1.27 | 3.03 | 26 | 46.2 |

**Aggregate vs §10 gate:**

| Metric | Threshold | NR2-20 OOS | VWAPC OOS |
|---|---|---|---|
| Profit factor | ≥ 1.30 | 1.13 FAIL | 1.09 FAIL |
| Sharpe (trade) | ≥ 1.0 | 1.51 PASS | 0.80 FAIL |
| Max DD (R) | ≤ 15.0 | 8.78 PASS | 5.94 PASS |
| Trade count | ≥ 200 | 215 PASS | 103 FAIL |
| Win rate | ≥ 0.38 | 0.47 PASS | 0.46 PASS |

**Cross-strategy daily-entry correlation:** −0.04 (effectively zero — strategies fire on different days, real diversification potential if both had edge).

---

## Verdict (cautious)

Both strategies **FAIL the §10 gate** but for diagnosable, possibly-fixable reasons:

**NR2-20** — single failed gate is PF (1.13 vs 1.30 needed). Sharpe is comfortably above bar, trade count cleared, DD healthy, win rate consistent. The shape of the failure is consistent with "needs more data" rather than "no edge". Walk-forward chose `ema` in 2 of 3 windows and `both` in 1 — mild filter inconsistency, but `ema` baseline plus the `both` option together produce defensible results.

**VWAPC** — failed PF (1.09), Sharpe (0.80), and trade count (103). Single window (W1) tanked the aggregate. W2 and W3 were genuinely good. High variance pattern is consistent with either (a) regime-dependent edge, or (b) curve-fit on the short 2-month train segments. Cannot distinguish on this dataset.

**Diversification check is the most positive finding** — −0.04 correlation means if both strategies prove out, they're essentially independent and should be run together. This makes the 2yr re-run worth doing for both.

**Do not promote either strategy to C++ on this evidence.** But also do not shelve. The gate isn't the right sample size to make either decision yet.

---

## Next session: exact actions

### Pre-flight check
1. Confirm 2yr XAUUSD CSV exists. Path is unknown to this handoff — Jo to provide. Expected naming convention based on existing file: something like `bars_XAUUSD_15min_2024-01-01_2026-03-01.csv` or similar in `~/omega_repo/fvg_phase0/XAUUSD_15min/`.
2. Confirm schema matches (header should have at minimum: a timestamp column named `ts`, `ts_unix`, or similar; plus `open`, `high`, `low`, `close`; volume optional). The loader is tolerant — see `nr2_20_backtest.py::load_bars_csv` for accepted column names.

### The run
With the 2-year CSV, the proper walk-forward methodology becomes feasible. Use the spec defaults (6m train / 3m test / 3m step):

```bash
cd ~/omega_repo/backtest/nr2_20

python3 wf_compare.py \
    --bars /Users/jo/omega_repo/fvg_phase0/XAUUSD_15min/<2yr-csv-name>.csv \
    --samples 100 \
    --train-months 6 --test-months 3 --step-months 3
```

This will produce 5 walk-forward windows from 2 years of data and aggregate the §10 verdict. Expected runtime: 10–30 minutes on Jo's Mac.

For a fuller search (slower, ~1–3 hrs):

```bash
python3 wf_compare.py \
    --bars <path> \
    --samples 200 \
    --train-months 6 --test-months 3 --step-months 3
```

For an exhaustive grid (overnight):

```bash
python3 wf_compare.py --bars <path> --samples 0
```

### How to read the output

The script prints per-window summaries to stderr, then a final aggregate block + acceptance gate verdict + diversification correlation. Look at:

1. **NR2-20 PF in aggregate.** If it now clears 1.30, the strategy is viable.
2. **VWAPC PF + Sharpe + trade count.** If trade count alone fails (which is just "not enough setups"), consider relaxing `--pullback-proximity-pips` higher (e.g. 15-20). If PF or Sharpe fail substantially, the strategy isn't there.
3. **Per-window NR2-20 trend filter choice.** If `both` keeps winning, that's a strong signal to bake `--trend-filter both` into the C++ port.
4. **Cross-strategy correlation.** If still <0.4, both strategies remain candidates for parallel deployment.
5. **Param stability across windows.** Look at `wf_compare_nr2_20.csv` and `wf_compare_vwapc.csv` — the per-window picked params. If the same params keep winning, generalization looks real. If every window picks different params, it's curve-fitting.

### Decision tree

| Outcome | Action |
|---|---|
| Both pass gate | Spec the C++ ports for both engines. Plan shadow deployment of NR2-20 first, VWAPC second, monitor signal correlation in production. |
| Only NR2-20 passes | Spec C++ port for NR2-20 only. Shelve VWAPC postmortem in a sibling doc. |
| Only VWAPC passes | Surprising given today's signal; spec C++ port for VWAPC; shelve NR2-20. |
| Both fail | Run the cost sensitivity test below. If still fail, write killer postmortem and shelve both. |

### Cost sensitivity (only run if gate fails on 2yr data)

If both fail with default cost model (0.5 pip spread + 0.3 pip slippage), check whether tighter broker costs would change the verdict:

```bash
python3 wf_compare.py --bars <path> --samples 100 \
    --train-months 6 --test-months 3 --step-months 3 \
    --spread-pips 0.2 --slippage-pips 0.1
```

If the strategy clears the gate at tight costs but fails at default, it's edge-marginal — only viable on a tight-spread broker. Document this and decide whether to deploy.

---

## Known engine details to keep in mind

These are quirks the next session should know without re-deriving them:

1. **NR2-20 box-span fix (already in code):** The box edges are computed from QCBs only, not all bars in the cluster span. This is documented in `_try_form_box`. A previous bug included the breakout bar's own high in the box, defeating the entry check. Fixed and verified.

2. **NR2-20 `--trend-filter both` is conservative.** Requires EMA slope AND (close-vs-VWAP + VWAP slope) to all agree. Will produce ~30-50% fewer trades than ema-only but higher conviction.

3. **VWAPC HTF EMA needs warmup.** Default `--htf-ema-period 50` × `--htf-minutes 60` = 50 hours of warmup before the EMA returns a trend. With short test segments, may be starved. Solutions: shorter `--htf-ema-period` (20 or 30), or use `--htf-minutes 60` (faster updates than 240).

4. **Daily VWAP reset is at 00:00 UTC.** That's the institutional default. If Jo's traders use a different anchor, change `--vwap-reset` to `session` (Asia/London/NY boundaries).

5. **Cost model (0.5 + 0.3 pip per side) is retail-typical for XAUUSD.** Tighten if Omega's actual broker is better.

6. **Trades CSV schema is identical between NR2-20 and VWAPC** so any post-processing or correlation analysis can be unified. See `HANDOFF_NR2_20_BACKTEST.md` §12.

7. **No omega_config.ini, symbols.ini, or DEPLOY_OMEGA.ps1 changes.** This work is entirely Python research, no production touchpoints.

8. **Engine bar loops are written in plain Python intentionally** — they mirror the future C++ port loop exactly so the eventual `XauusdNr2_20Engine.hpp` verifier (`backtest/verify_xauusd_nr2_20.cpp` per Omega convention) will be a row-for-row port like the FVG engine just shipped.

---

## Things still missing (not blocking)

- **No FVG-vs-NR2-20 correlation gate has been run.** Spec §10 lists it but it requires running both engines on the same bars and computing trade-entry correlation. Run only after gate passes.
- **No long-window walk-forward results** — the entire reason for this handoff. Blocked on data.
- **Exhaustive grid not yet attempted** — even on 2yr data this would be slow; only worth it if random-sample results are borderline.

---

## Reference card

```
~/omega_repo/backtest/nr2_20/
├── HANDOFF_NR2_20_BACKTEST.md     spec
├── nr2_20_backtest.py             NR2-20 engine
├── wf_nr2_20.py                   NR2-20 walk-forward
├── vwap.py                        shared VWAP utils
├── vwap_continuation_backtest.py  VWAPC engine
├── wf_compare.py                  HEAD-TO-HEAD WALK-FORWARD ← run this
└── README_NR2_20.md               operator manual
```

```
~/omega_repo/docs/
├── HANDOFF_FVG_BACKTEST.md
├── SESSION_2026-05-02_FVG_S7_HANDOFF.md
├── SESSION_2026-05-03_FVG_VERIFIER_PASS.md
└── SESSION_2026-05-03_NR2_20_HANDOFF.md  (this file)
```

```
~/omega_repo/fvg_phase0/XAUUSD_15min/
└── bars_XAUUSD_15min_2025-09-01_2026-03-01.csv  (the 6m CSV used today)
└── <2yr CSV to be added by Jo>                  (REQUIRED for next session)
```

— END HANDOFF —
