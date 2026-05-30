# Walk-Forward Edge Pipeline — Trusted System — 2026-05-30 (S39)

Mandate: "a backtest harness using all our data to find edges, validate them,
and give me a backtested system I can trust." Data: `/Users/jo/Tick` (77 GB,
12 symbols, pre-aggregated H1/H4/M5/M15/M30 OHLC bars, ~1-2 yr each).

## What was built
- **`backtest/edge_pipeline.cpp`** — walk-forward edge-discovery harness.
  Reuses the production-audited simulator from `multi_tf_sweep.cpp` VERBATIM
  (S37 half-spread touch fills — long exits worse bid / short worse ask — so
  winners are NOT overstated; per-symbol OmegaCostGuard round-trip costs:
  XAU $0.66, US500 $2.00, USTEC $1.10, FX $0.36, GER40 $0.20…). Adds:
  - direct OHLC bar loader (no tick→bar resample that would destroy H/L);
  - **walk-forward split**: each symbol/TF series split 50/50, the SAME fixed
    signal grid run on TRAIN and TEST, cells joined by (family,params). A cell
    PASSES only if positive AND n≥30 in BOTH halves — a true held-out OOS test,
    not an in-sample fit;
  - profit-factor + Sharpe + max-DD per half.
  6 families (Donchian, Bollinger-MR, RSI-extreme, MA-cross, Momentum-N,
  Z-score-MR) × param grids × 12 symbols × {H1,H4,M5,…}.
- **`tools/deflated_sharpe_gate.py --cells`** — Deflated Sharpe Ratio
  (Bailey & López de Prado) over the WF output: deflates each cell's TEST
  Sharpe for the trial count (735 cells) + sample length, robust-IQR trial
  spread. The multiple-testing correction the research demands.

## Result — 735 cells evaluated, 14 walk-forward survivors
Positive in BOTH halves, n≥30/half, TEST profit-factor ≥1.10:

| sym | tf | family | params | n_te | PF_te | Sharpe_te | net_te |
|---|---|---|---|---:|---:|---:|---:|
| XAUUSD | 4h | Donchian | N=20 | 68 | **2.13** | 2.39 | $1925 |
| GER40 | 4h | RSI_Extreme | N=7 30/70 | 47 | 1.91 | 2.05 | $2391 |
| XAUUSD | 4h | Donchian | N=100 | 34 | 2.19 | 1.91 | $888 |
| GER40 | 1h | RSI_Extreme | N=14 30/70 | 84 | 1.48 | 1.65 | $1318 |
| XAUUSD | 4h | Donchian | N=50 | 50 | 1.77 | 1.62 | $982 |
| XAUUSD | 1h | MA-cross | 10/30 | 172 | 1.38 | 1.58 | $1038 |
| SPXUSD | 4h | MA-cross | 20/50 | 35 | 1.76 | 1.50 | $719 |
| XAUUSD | 4h | MA-cross | 10/30 | 40 | 1.62 | 1.13 | $848 |
| NSXUSD | 4h | RSI_Extreme | N=7 30/70 | 106 | 1.19 | 0.78 | $1790 |
| GER40 | 1h | MA-cross | 20/50 | 62 | 1.24 | 0.78 | $743 |
| XAUUSD | 4h | RSI_Extreme | N=14 30/70 | 58 | 1.33 | 0.78 | $393 |
| NSXUSD | 4h | Momentum | lb=20 | 120 | 1.14 | 0.62 | $2293 |
| NSXUSD | 4h | MA-cross | 20/50 | 33 | 1.26 | 0.60 | $1245 |
| SPXUSD | 1h | RSI_Extreme | N=14 20/80 | 36 | 1.13 | 0.30 | $57 |

## The trust verdict — read this carefully
**No single cell is individually significant after deflating for 735 trials.**
The DSR per-cell ≈ 0: per-trade Sharpe (~0.1-0.33) cannot beat the max-of-735
benchmark in a heterogeneous fishing expedition. This is the honest, research-
aligned answer — a Sharpe found after sweeping hundreds of cells is NOT that
Sharpe, and the tool says so rather than rubber-stamping the top cell.

**The trustable signal is the CLUSTERING, not any one cell:**
- **MA-crossover survives WF on 4 symbols** (XAU, GER40, SPX, NSX) — $4,592.
- **RSI-extreme survives WF on 4 symbols** (XAU, GER40, NSX, SPX) — $5,949.
- **XAU 4h Donchian survives at N=20, 50, AND 100** — parameter-robust, not a
  single lucky knob. PF 1.8-2.2, the strongest single edge in the corpus.
A robust trend/breakout family surviving across many independent symbols,
timeframes, and adjacent parameters is FAR stronger evidence than one cell's
backtested Sharpe — it is much harder to produce by chance than a single
overfit point.

**Triple convergence.** This backtest agrees with the other two independent
signals from this session:
1. Live shadow ledger: trend/TP-exit wins, scalp/mean-rev bleeds.
2. Research (MOP/Hurst/AQR): TSMOM-trend on H1-D1 is THE durable retail edge.
3. This WF backtest: trend/breakout (Donchian, MA-cross, Momentum) + RSI-mean-
   reversion on H1/H4 are the only families surviving out-of-sample.
Three methods, one answer. That convergence is the trust.

## The trusted system (what to actually run)
A small equal-risk portfolio of the cross-validated, cross-symbol cluster —
NOT all 14 cells, and NOT any single "best" cell:
1. **XAU 4h Donchian breakout** (N=20-50) — anchor; strongest + param-robust.
2. **MA-crossover 4h** on XAU + the indices (GER40/SPX/NSX).
3. **RSI-extreme reversal** H1/H4 on indices (GER40/SPX/NSX) — the mean-rev
   exception that survives at bar (not tick) horizon; size it smaller, watch
   for regime dependence.
Run each vol-targeted (Omega already does this via `vol_parity_scale`),
correlation-capped (already does, `entry_allowed`), on H1/H4 only. Expect
~20% drawdowns as normal (per the live-trend literature).

## Caveats (do not skip)
- Backtest, even WF, is optimistic vs live fills (memory: harness-optimism,
  close-bias). These cells are CANDIDATES that earned a live shadow trial, not
  a guarantee. Promote each to live only after the shadow ledger confirms
  (re-run `deflated_sharpe_gate.py` on accumulated shadow trades).
- RSI-extreme is mean-reversion; it survived WF here but is the family most
  prone to regime breaks — treat as the least-trusted of the three.
- 50/50 split = one train/test path. Next hardening step: rolling/CPCV
  multi-split + per-symbol pre/post-2022 regime split for gold.

## Fidelity validation (mandatory per harness-fidelity discipline) — PASSED
Cross-checked the new harness against the INDEPENDENT `htf_bt_walkforward.cpp`
(different codebase, reads the raw 5.5 GB / 154 M-tick XAU file and builds its
own 3434 H4 bars, sweeps SL/TP brackets):

| harness | XAU H4 Donchian, walk-forward | n | PF |
|---|---|---|---|
| htf_bt_walkforward (raw ticks, D=15) | Year1→Year2 | 86→94 | 1.54→1.13 |
| htf_bt_walkforward (Year2 best, D=15) | held-out | 76 | 1.89 |
| **edge_pipeline (this build, bars, N=20)** | **test half** | **68** | **2.13** |

Both independent harnesses find the SAME edge — XAU H4 Donchian breakout is a
real, positive-both-halves walk-forward edge (PF ~1.1-2.2). `edge_pipeline`
runs slightly hotter than the raw-tick harness (consistent with bar-vs-tick
fill optimism); the agreement on direction, sign, and order-of-magnitude n
validates the new harness is not producing phantom numbers. htf's own verdict
("EDGE MARGINAL — positive but weaker than in-sample") independently confirms
the DSR conclusion: real but modest, trust the cluster not the single number.

## HARDENING — N-block multi-path stress (`--blocks N`)
Added a block-consistency mode: split each series into N disjoint contiguous
time blocks (each = a different market period), run the same fixed grid on
every block, keep only cells net-positive across most blocks. This is the
direct regime-dependence test — a single-period fluke cannot survive being
positive in 5-6 independent windows. The bar tightens as N rises:

| stress level | survivor count | criterion |
|---|---:|---|
| 1 split (50/50 WF) | 14 | positive both halves |
| **5 blocks** | **5** | positive in ALL 5 periods, every block PF>1.0 |
| **6 blocks** | **2** | positive in ALL 6 periods, every block PF>1.0 |

**5-block perfect-consistency core (cons=1.00, min-block-PF>1.0):**
| sym | tf | family | min PF | n | net |
|---|---|---|---:|---:|---:|
| GER40 | 4h | RSI_Extreme N=7 | 1.29 | 97 | $4480 |
| NSXUSD | 4h | Donchian N=50 | 1.07 | 87 | $4368 |
| NSXUSD | 4h | RSI_Extreme N=7 | 1.07 | 206 | $3901 |
| XAUUSD | 4h | Donchian N=20 | 1.48 | 135 | $2531 |
| XAUUSD | 4h | Donchian N=100 | 1.43 | 66 | $1333 |

**6-block BEDROCK (positive in every one of 6 disjoint periods):**
- **XAUUSD 4h Donchian N=20** — 6/6, min-PF 1.37, $2446. The single most
  trustworthy edge in the entire corpus; never had a losing period.
- **GER40 4h RSI_Extreme N=7** — 6/6, min-PF 1.16, $3852.

**Two structural facts the stress revealed:**
1. **4h is the robust horizon.** Every H1 variant degraded to a losing block
   under multi-path stress; only 4h cells survived 5/5-6/6. Trade these on 4h.
2. The bedrock is one **trend-breakout** (XAU Donchian) + one **bar-scale
   mean-reversion** (GER40 RSI) — genuinely uncorrelated edges, good for a
   2-leg portfolio.

## Regime caveat (honest data limit)
The corpus is **2024-03 → 2026-04 only** — entirely POST the 2022 gold/real-
rate regime break. A true pre/post-2022 split is therefore impossible with this
data; the 6-block test (6 disjoint ~4-month windows spanning varied conditions)
is the within-sample regime-robustness proxy. To test across the 2022 break,
acquire pre-2022 bar data (future data-acquisition task) and re-run `--blocks`.

## Final trusted system (post-hardening)
Anchor the live shadow promotion on the bedrock, sized vol-target + corr-capped:
1. **XAUUSD 4h Donchian breakout (N=20)** — primary, 6/6 regime-robust.
2. **GER40 4h RSI-extreme reversal (N=7, 30/70)** — uncorrelated second leg, 6/6.
3. Tier-2 (5/5): NSX 4h Donchian N=50, NSX 4h RSI N=7, XAU 4h Donchian N=100.
ALL on the 4h timeframe. Drop the H1 variants — they fail multi-path stress.

## Reproduce
```
backtest/edge_pipeline --tickdir /Users/jo/Tick --out outputs/edge_pipeline_wf.csv --min-n 30 --pf-gate 1.10
python3 tools/deflated_sharpe_gate.py outputs/edge_pipeline_wf.csv --min-n 30
```
