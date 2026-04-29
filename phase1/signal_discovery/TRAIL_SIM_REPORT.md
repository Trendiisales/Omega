# Trail-Engine Simulation -- FINAL REPORT

_Generated: 2026-04-29  signal-discovery + trail simulation track_

## Verdict

**No tradeable edge found in any of the four candidate setup types under any
of 8+ trail/SL configurations, including tightened thresholds and session
restriction.**

Per the explicit directive in `SESSION_HANDOFF_2026-04-29_wrap.md` Step 6
("If no setup type clears all three gates, document the negative result,
surface for Jo, and stop. Do NOT build the engine on weak edge.") the
recommendation is to **not build** the simple intraday engine on these
patterns.

## What was tested

**Corpus:** XAUUSD 5-second mid-price OHLC bars, 2025-04-01 -> 2026-04-01
(4,162,560 bars, ~365 days, post-2025-04 microstructure regime).

**Setup types** (all bidirectional long+short):
- `compression_break`     -- low-vol coil into directional break
- `spike_reverse`         -- one-bar adverse z-score spike then mean-revert
- `momentum_pullback`     -- 2-min trend, 3-bar pullback, resumption
- `level_retest_reject`   -- recent 5-min S/R retested, rejected away

**Cost model:** 0.65 pt round-trip (matches measured avg_spread of 0.69pt
across the corpus).  Realistic broker cost is 0.7-1.0 pt round-trip
including slippage.

**Trail configs (8 tried):**
- arm thresholds:   0.5 / 2.0 / 3.0 / 5.0 pt MFE before trail engages
- trail lock fracs: 0.30 / 0.50 / 0.70 / 0.80 / 0.90 of MFE
- initial SL:       1.0 / 1.5 / 2.0 x atr60
- cooldown:         60 bars (5 min) between exit and next entry
- max hold:         360 bars (30 min)

**Variants tried beyond the base grid:**
- tightened thresholds: spike z>4 (vs >3), compress<0.30 (vs <0.50)
- session restriction:  UTC 07-16 (London/NY overlap only)

## Why the discovery numbers looked promising at first

The initial pass on `forward_returns_*.parquet` showed:
- average MFE per trade: +5.7 to +6.6 pt (peak favorable excursion in window)
- 80%+ of trades touch MFE > 1pt
- a naive "lock 80% of MFE / -3pt SL" formula gave **+0.006 to +0.162 pt
  net per trade** for 3 of 4 setups -- looks tradeable.

The naive formula assumes you exit at exactly 80% of peak MFE.  In practice
a real trail captures only the LATE-WINDOW portion of MFE because the
trail-stop fires the first time price retraces, which is typically
when MFE is small.

## What the proper bar-by-bar simulation shows

For every config tried on every setup, mean per-trade is **-0.55 to -0.68 pt**:

### spike_reverse (11,014 candidates, ~30/day before cooldown)

| config         | realized | mean   | win% | avg win | avg loss | t-stat | total pt |
|---|---:|---:|---:|---:|---:|---:|---:|
| a05_l80_sl15   | 9,442    | -0.563 | 16.4% | +0.46 | -0.76 | -70.6 | -5,315 |
| a20_l80_sl15   | 9,204    | -0.572 | 23.0% | +1.41 | -1.16 | -44.9 | -5,261 |
| a30_l50_sl15   | 8,922    | -0.584 | 17.3% | +2.29 | -1.18 | -31.1 | -5,213 |
| a50_l50_sl20   | 8,547    | -0.560 | 14.9% | +4.08 | -1.38 | -20.2 | -4,788 |

### compression_break (18,248 candidates, ~50/day before cooldown)

| config         | realized | mean   | win% | avg win | avg loss | t-stat | total pt |
|---|---:|---:|---:|---:|---:|---:|---:|
| a05_l80_sl15   | 7,691    | -0.592 | 15.8% | +0.45 | -0.79 | -59.5 | -4,554 |
| a20_l80_sl15   | 7,520    | -0.645 | 22.3% | +1.40 | -1.23 | -43.1 | -4,849 |
| a30_l50_sl15   | 7,318    | -0.675 | 16.8% | +2.19 | -1.25 | -34.8 | -4,939 |
| a50_l50_sl20   | 7,092    | -0.657 | 14.7% | +4.02 | -1.46 | -21.2 | -4,661 |

### Tightened spike_reverse (z>4) + session UTC 07-16

5 candidates / year (0.01/day) -- below the freq>=5/day gate; all 5 lost.

### Tightened compression_break (compress<0.30) + session UTC 07-16

311 candidates / year (0.85/day) -- below the freq>=5/day gate.  Realized
trades lose -0.63 pt mean.

## Diagnostic: what's killing the edge

1. Gross 30-min returns are essentially zero across all setups
   (-0.18 to +0.06 pt before cost).  No directional alpha.

2. Avg MFE (~6pt) ~= avg |MAE| (~6pt).  The volatility is symmetric.
   A trail can only profit from MFE/MAE asymmetry; symmetry kills it.

3. The trail captures the late-window portion of MFE, not the peak.
   When MFE peaks early and price reverts, the trail fires after only
   capturing a small fraction.  Avg win in the simulation is +0.45 to
   +4.08 pt depending on config -- always smaller than what the naive
   80%-of-peak formula predicts.

4. The 0.65 pt cost per trade requires AT LEAST 0.65 pt average win to
   break even.  Setups with high arm thresholds have higher avg wins
   but lower frequency (most candidates never reach the arm), which
   bumps avg loss as well because more candidates time-stop at flat or
   near-flat against the cost.

5. Tightening to z>4 or compress<0.30 cuts frequency to <1/day but does
   not improve the edge -- if anything makes it worse because the
   narrower-range entries are rarer at moments of true imbalance.

## Conclusions and recommended next steps

These four pattern-detection setups have no tradeable edge on 5-second
XAUUSD against a realistic 0.65-1.0 pt round-trip cost.  This is a robust
result across configurations.

Three credible next iterations remain:

**(a) Fundamentally different setup types.**  L2 / order-flow imbalance,
   limit-order liquidity provision (market-making), event-driven
   (scheduled news), correlation-pair (XAU vs DXY).  This is a 1-2 week
   research effort.

**(b) Existing engines, retuned.**  HBG, EmaCross, AsianRange, VwapStretch
   already exist with proven edge in their respective backtests.  If the
   real goal is "small quick trades up and down with locked profits", the
   right move may be to retune one of those (especially HBG, which has
   the MFE-proportional trail you spec'd) for shorter holding periods
   rather than build a new engine from scratch.

**(c) Build it anyway as a research-grade shadow engine.**  Accept the
   simulated negative expectancy and ship the engine in `shadow_mode=true`
   to learn from real-time execution patterns the simulation can't model
   (limit-order fills, queue position, asymmetric latency).  Expect to
   lose money on paper.  Only worth it if there's a specific
   live-microstructure hypothesis to test.

## Files produced this track

- `phase1/signal_discovery/aggregate_5s_bars.py`         -- duckdb aggregator
- `phase1/signal_discovery/discover_setups.py`           -- setup-detection
- `phase1/signal_discovery/simulate_trail.py`            -- bar-by-bar trail sim
- `phase1/signal_discovery/bars_5s.parquet`              -- 4.16M 5-sec bars
- `phase1/signal_discovery/bars_5s_YYYY_MM.parquet`      -- per-month bars (13 files)
- `phase1/signal_discovery/forward_returns_<setup>.parquet` -- raw entry returns (4 files)
- `phase1/signal_discovery/setup_catalog.md`             -- discovery catalog
- `phase1/signal_discovery/CHOSEN_SETUP.md`              -- (negative result)
- `phase1/signal_discovery/TRAIL_SIM_REPORT.md`          -- this file
