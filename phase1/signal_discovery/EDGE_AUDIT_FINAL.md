# Signal-Discovery Edge Audit -- FINAL

_Generated: 2026-04-29  signal-discovery + trail/TP-SL/multi-timeframe audit_

## TL;DR

**No edge worth chasing exists in retail-pattern-detection on 5s-1min XAUUSD
against a realistic 0.65pt round-trip cost.**  This conclusion holds across
4 setup families, both directions (long & short, plus direction-flipped
variants), 8 trail configurations, 8 fixed TP/SL configurations, two bar
sizes (5-second and 1-minute), tightened thresholds, and London/NY session
restriction.

Tests run, all reproducible, files in `phase1/signal_discovery/`.

## What was tested -- exhaustive list

### Setup families (all bidirectional)
- `compression_break`     -- low-vol coil into directional break
- `spike_reverse`         -- 1-bar adverse z-score spike then mean-revert
- `momentum_pullback`     -- 24-bar trend, 3-bar pullback, resumption
- `level_retest_reject`   -- recent S/R retested, rejected away

### Bar sizes
- 5-second OHLC mid bars (4,162,560 bars over 365 days)
- 1-minute OHLC mid bars (354,392 bars over 365 days, resampled from 5s)

### Exit logics (all on the 5-second corpus)
- Fixed-horizon close-out: 30s / 2min / 5min / 15min / 30min
- 8 trail configurations: arm in {0.5, 2, 3, 5} pt MFE; lock in {0.3, 0.5, 0.7, 0.8, 0.9} of MFE; SL in {1.0, 1.5, 2.0} x atr60
- 8 fixed-TP/SL configurations: TP in {0.5, 0.7, 1, 1.5, 2, 3} pt; SL in {0.3, 0.5, 0.75, 1, 1.5} pt
- Time-stop ceiling: 30 minutes (360 bars at 5s)
- Cooldown: 5 minutes between exit and next entry (prevents the unrealistic 1000+/day raw-entry frequency)

### Direction variants
- Original: trade as defined
- Flipped: fade each setup (test the inverse hypothesis)

### Regime / period filters
- Full corpus
- Tightened thresholds (z>4 vs z>3, compress<0.30 vs <0.50)
- London/NY session only (UTC 07:00 - 16:00)

## Worst-case outcome across all tests

**Best per-trade mean across every config / setup / horizon / direction / filter combination tested:**
- Best: compression_break flipped at 60min net mean = -0.10 pt (t-stat -0.3, statistically zero)
- Best with statistically-meaningful frequency: level_retest_reject at 30min = -0.58 pt (t-stat -36)
- *Every* config returned negative net expectancy after the 0.65pt cost.

## Key diagnostic: why no edge

### 1. Gold at 5-second is microstructurally efficient

The Dukascopy tick feed shows avg spread 0.69pt and avg 5-second bar range
~1pt.  A single-bar movement just barely covers cost.  Any predictive
content in simple OHLC patterns at this granularity has been arbitraged out
by HFT firms with co-located servers, sub-millisecond latency, and L2/L3
order-book data.

### 2. Fixed-TP/SL hit rates match random-walk theory

Empirical TP-hit rates are within 1-2% of `SL/(TP+SL)`:

| TP (pt) | SL (pt) | theoretical | spike_reverse measured | compression_break measured |
|---:|---:|---:|---:|---:|
| 1.0 | 0.5 | 33.3% | 34.3% | 33.6% |
| 1.0 | 1.0 | 50.0% | 49.9% | 47.6% |
| 2.0 | 1.0 | 33.3% | 35.0% | 32.5% |
| 2.0 | 0.5 | 20.0% | 22.2% | 20.9% |

The setups carry **no exploitable directional information**.  Price is
essentially a martingale conditional on these signals.

### 3. The 75-85% MFE-touch rate is not a win rate

What we computed: "fraction of trades where peak favorable excursion within
the 30-min window exceeded X pt."  That is *not* a tradeable rate -- it
ignores whether MFE was reached BEFORE MAE.  When converted to a real
exit rule (TP / trail / time-stop), the win rate collapses to whatever
random-walk theory predicts.

### 4. Direction-flip confirms zero-information signals

Flipping every setup (trade the opposite direction) produces equally
negative t-stats.  If the original signals were significantly wrong, the
flip would be significantly right.  Both directions losing ~equally
indicates the entries carry no information at all.

### 5. Cost dominates the math

With gross 5-min returns at -0.18 to +0.06 pt across setups, the 0.65pt
cost is 4-15x larger than any directional effect we measured.  No exit-
logic optimization can recover edge from a zero-information entry.

## Where edge HAS been found in this codebase

Looking at the engines that pass their backtests: HBG, EmaCross,
AsianRange, VwapStretch, CFE.  They share characteristics that the 5-sec
pattern setups lack:

- **Longer time horizons.**  All operate on 1-minute or higher bars.
- **Multi-bar confirmation.**  Not single-bar entries.
- **Session structure exploitation.**  AsianRange trades break of overnight range.
- **Momentum continuation, not pattern reversion.**  HBG, EmaCross, VwapStretch follow trend.
- **ATR-scaled stops and trails.**  Sized to absorb noise without stopping out.
- **Macro-regime filtering.**  RISK_OFF / RISK_ON, vol-regime gates.

## Constructive next moves -- ranked by viability

### A. Retune HBG for shorter holding periods  (1-3 days work)

HBG already has the architecture you spec'd: bracket entries, MFE-
proportional 80% trail, vol-adaptive SL, pyramiding.  It already passes
its backtests.  What it doesn't have is "small quick trades" -- because
its parameters target longer holds.

A *parameter retune* (smaller TP targets, tighter trail-arm threshold,
shorter time-stop) gives you the engine you described without building
new code.  The wave-2 sweep harness (already shipped this morning as
`backtest/run_post_regime_sweep.sh`) is the right tool to find the
parameter combo.

**Risk:** retuning HBG to fire more frequently may degrade its existing
edge.  Sweep mitigates by testing many parameter combos.

### B. Limit-order liquidity-provider engine  (1-2 weeks work)

Instead of crossing the spread on every entry (paying 0.65pt), place
limit orders inside the spread to *earn* the spread.  Math flips:
gross + ~0.3pt = net (instead of gross - 0.65 = net).  Three of four
setups would flip to ~+0.3pt net.

**Architecture work:** limit-order placement, queue position tracking,
fill-or-cancel timing, partial-fill handling, latency-tolerant cancellation.
Significant new infrastructure.

**Risk:** queue position is unpredictable without L2 data; adverse
selection (your limit orders fill exactly when price moves against you)
may reduce the apparent gain.

### C. Event-driven engine  (3-7 days work)

Trade only around scheduled events: NFP, FOMC, CPI, London fix, market
open/close.  Microstructure is fundamentally different in event windows
and existing edges may concentrate there.

**Setup:** download event calendar, filter entries to N minutes before/
after event, recompute statistics on the much smaller event-only sample.

**Risk:** small sample size (events are rare); edge if found may not
generalize to all event types.

### D. L2 / order-flow imbalance research  (data-acquisition first)

Where modern intraday edge actually lives.  Requires L2 tick feed (which
Dukascopy doesn't provide -- need a different data source like LMAX,
ICE, or a broker with L2 API).

**Risk:** new data pipeline, new feature engineering, new architecture.
Not viable until L2 feed is available.

### E. Stop chasing 5-second-bar pattern engines

Accept that this branch of research is exhausted on this data set.  Focus
remaining effort on engines/timeframes where edge has been demonstrated:
the existing 1m+ momentum-continuation and session-structure engines.

## Recommended action

**Path A** (HBG retune via the wave-2 sweep harness) is the highest
expected value next move.  It uses the existing proven architecture,
respects the audit's negative finding (don't build a new pattern engine),
and produces a testable engine on Omega within 1-3 days.

The wave-2 sweep harness is already shipped (`audit-fixes-20`).  Once
that runs on the Mac, the next session ranks parameter combos targeting
shorter holds + tighter trails, applies the winners to
`include/SweepableEnginesCRTP.hpp`, and ships the retuned HBG variant
to shadow.

## Files this audit produced

```
phase1/signal_discovery/
  aggregate_5s_bars.py             # tick CSV -> 5s OHLC bars (per-month + merge)
  bars_5s_YYYY_MM.parquet          # per-month 5s bars (13 files)
  bars_5s.parquet                  # merged 5s bars, 4.16M rows, 87.5MB
  bars_1m.parquet                  # 1m resample, 354K rows
  discover_setups.py               # entry-signal detector + forward-return engine
  forward_returns_*.parquet        # raw per-entry forward returns (4 setups)
  setup_catalog.md                 # initial discovery catalog
  CHOSEN_SETUP.md                  # negative-result memo
  simulate_trail.py                # bar-by-bar trail engine simulator
  TRAIL_SIM_REPORT.md              # trail-grid results
  EDGE_AUDIT_FINAL.md              # this file
```

All scripts re-runnable.  All parquet artifacts deterministic from the
underlying tick CSVs.  Total disk: ~110MB.
