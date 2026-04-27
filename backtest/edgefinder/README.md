# OmegaEdgeFinder

Edge-discovery framework for the Omega HFT system. Goal: given 154M ticks of
XAUUSD bid/ask (2024-03 → 2026-04), produce a ranked, statistically-honest
table of features whose value at bar close `t` predicts forward price action
in `(t, t+h]`, after costs, that survives an out-of-sample hold-out.

This is NOT a parameter sweeper, NOT a backtester, NOT a strategy generator.
It is a feature → forward-return statistics engine.

## Status

| Phase     | Component                          | Status            |
|-----------|------------------------------------|-------------------|
| Session A | EventExtractor (C++)               | DONE              |
| Session A | Forward-return + bracket simulator | DONE              |
| Session A | Binary panel writer + Python loader| DONE              |
| Session A | Leakage unit test                  | DONE (passing)    |
| Session B | RegimeSegmenter                    | not started       |
| Session B | EdgeProspector                     | not started       |
| Session B | MultipleTestingGate                | not started       |
| Session B | WalkForwardConfirm                 | not started       |
| Session B | EdgeReport                         | not started       |

## Architecture

```
Stage 1 (C++, run once, ~10 min):
   XAUUSD_2024-03_2026-04_combined.csv   (5 GB, 154M ticks)
       │
       ▼
   OmegaEdgeFinderExtract --in <csv> --out <panel.bin>
       │
       ▼
   bars_xauusd_full.bin                  (~770k rows × 96 fields, ~150 MB)

Stage 2 (Python, iterative, sub-second per query):
   load_panel(path)  → pandas.DataFrame
   regime.label_regimes(df)
   prospect.prospect_all_cells(df_train)
   mtc.apply_fdr / apply_n_gate / apply_stability_gate
   walkforward.confirm_oos(candidates, train, val, oos)
   report.emit(...)  → edges_ranked.csv + supporting artefacts
```

## Data partitioning (LOCKED)

| Partition | Window                       | Months | Use                                                                      |
|-----------|------------------------------|--------|--------------------------------------------------------------------------|
| TRAIN     | 2024-03-01 → 2025-12-31      | 22     | Feature exploration, edge ranking, all iteration                         |
| VAL       | 2026-01-01 → 2026-01-31      | 1      | Candidate filtering during search; touched ~10×                          |
| OOS       | 2026-02-01 → 2026-04-30      | 3      | Touched ONCE, after candidates are frozen; access logged to `oos_access.log` |

These boundaries are encoded as defaults in `analytics/load.py::partition_train_val_oos()`.
If OOS is contaminated by a process violation, rotate by sliding 3 months.

Quarterly stability buckets within TRAIN (used by stability gate, ≥3/4 same sign):
* Q1 2024-03 → 2024-08
* Q2 2024-09 → 2025-02
* Q3 2025-03 → 2025-08
* Q4 2025-09 → 2025-12

## File inventory (Session A deliverables)

```
backtest/edgefinder/
├── README.md                              ← this file
├── extractor/
│   ├── PanelSchema.hpp                    ← canonical PanelRow struct, packed, 600 bytes/row
│   ├── CivilTime.hpp                      ← pure-int UTC date arithmetic (Hinnant)
│   ├── RollingWindow.hpp                  ← Ring / MinMaxRing / RollingMean / RollingVar / RollingLinReg
│   ├── BarState.hpp                       ← per-bar accumulator + trailing rings
│   ├── ForwardTracker.hpp                 ← pending-row queue, fwd_ret + bracket simulator
│   ├── PanelWriter.hpp                    ← flat binary writer with 64-byte ASCII header
│   └── EventExtractor.cpp                 ← main; mmap CSV, parse ticks, drive state, write panel
├── analytics/
│   └── load.py                            ← Python loader (numpy struct dtype + pandas DataFrame)
└── tests/
    └── test_extractor_no_lookahead.cpp    ← leakage unit test (9 assertions, all passing)
```

CMakeLists targets added to root: `OmegaEdgeFinderExtract`, `OmegaEdgeFinderTest`.

## Build

Mac / Linux:
```bash
cd build && rm -rf * && cmake .. && cmake --build . --target OmegaEdgeFinderExtract
cd build && cmake --build . --target OmegaEdgeFinderTest && ./OmegaEdgeFinderTest
```

Windows VPS (MSVC):
```powershell
cmake --build build --config Release --target OmegaEdgeFinderExtract
cmake --build build --config Release --target OmegaEdgeFinderTest
.\build\Release\OmegaEdgeFinderTest.exe
```

The leakage test is the gate — if it fails, the panel produced by the extractor
is NOT trustworthy.

## Run extractor

```bash
./OmegaEdgeFinderExtract \
    --in  ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv \
    --out backtest/edgefinder/output/bars_xauusd_full.bin \
    --verbose
```

Optional date filters: `--from YYYY-MM-DD --to YYYY-MM-DD` (inclusive of from,
exclusive of to; both UTC).

Expected output for full CSV:
* runtime: ~10 min on M-series Mac, ~15 min on VPS
* rows written: ~770k (one per closed UTC minute, including weekend gaps as 0 rows)
* panel size: ~440 MB (770k × 600 bytes; gzip-compressible to ~80 MB)

## Use the panel from Python

```python
from analytics.load import load_panel, partition_train_val_oos

df = load_panel('backtest/edgefinder/output/bars_xauusd_full.bin')
df_train, df_val, df_oos = partition_train_val_oos(df)

# Sanity:
print(df.shape, df.columns.tolist()[:10])
print(df['warmed_up'].mean(), '≈ fraction of fully-warmed bars')
print(df.dropna(subset=['fwd_ret_60m_pts']).shape)
```

## Forward-return columns

Fixed-horizon (drift detection):
* `fwd_ret_1m_pts`, `fwd_ret_5m_pts`, `fwd_ret_15m_pts`, `fwd_ret_60m_pts`, `fwd_ret_240m_pts`

First-touch (path classification):
* `first_touch_5m`  ∈ {-1, 0, +1}, ±10 pts threshold
* `first_touch_15m` ∈ {-1, 0, +1}, ±20 pts
* `first_touch_60m` ∈ {-1, 0, +1}, ±50 pts

Bracket scenarios (realisable PnL, LONG side; analytics derives short by negation):
| idx | horizon | sl_pts | tp_pts |
|-----|---------|--------|--------|
| 0   |  5m     |  10    |  20    |
| 1   | 15m     |  20    |  50    |
| 2   | 15m     |  30    |  60    |
| 3   | 60m     |  50    | 100    |
| 4   | 60m     | 100    | 200    |
| 5   | 240m    | 100    | 300    |

Cells: `fwd_bracket_pts_<i>`, `fwd_bracket_outcome_<i>` (+1 TP, -1 SL, 0 MtM).

## Schema versioning

`PanelSchema.hpp::PANEL_SCHEMA_VERSION` (currently 1) is written into the
64-byte panel header. `analytics/load.py` validates the version on read and
refuses to open a panel from an older schema. Bump the version on ANY change
to PanelRow layout.

## Leakage discipline (the most important property)

The extractor is structured so that the features written to row `t` are
computable strictly from data at-or-before bar `t` close. Specifically:
* Bar-internal stats (open/high/low/close/range/wicks/spread/tick_count) come
  from ticks within bar `t` and are sealed at bar close.
* Trailing technicals (EMAs, RSI, ATR, range_20, BB, vol_60) are computed from
  the rolling state BEFORE bar `t` is added — i.e. from bars 1..t-1. After
  the row is emitted, bar `t`'s close is pushed into the rolling state for
  use by bar `t+1`.
* Forward returns are pre-computed at extraction time by the ForwardTracker:
  a deque of pending rows is updated by every subsequent tick until each row's
  longest-horizon deadline expires, at which point the row is flushed to the
  panel writer with all forward cells filled.

This is verified by `tests/test_extractor_no_lookahead.cpp`. Test feeds 100
flat minutes at price 2000.0, then a single-bar +50 spike at minute 100, then
240 more flat minutes at 2050. Asserts that bar 99's close, ema_9, ema_50,
atr_14, range_20bar_hi/lo, and vwap_session all equal exactly 2000.0 — i.e.
no leakage of the spike. Also asserts bar 99's `fwd_ret_1m_pts` / `_5m_pts` /
`_60m_pts` are ≈ +50, and bracket [0] (5m, 10sl, 20tp) resolves to TP-hit
with PnL +20.

## Known limitations / Session A scope notes

* Brackets are LONG-side only on the panel. Short-direction edges are derived
  in analytics by negating the long bracket result; for cases where TP and SL
  asymmetry matters for shorts, fall back to fwd_ret_*_pts as the realiser.
* Bracket SL/TP touch-tests use mid (not bid/ask). Spread cost is captured
  separately as `spread_median_pts` per row; cost-curve reports apply it
  uniformly. This is the right separation: brackets test the underlying price
  behaviour; spread cost is layered in by the analytics.
* Volume column is absent — Dukascopy XAUUSD has no real volume; `tick_count`
  is the density proxy.
* Weekend / holiday gaps are simply absent from the panel (no bar emitted for
  minutes with zero ticks). Continuity-sensitive features are NaN-padded
  appropriately by the rolling primitives.
* Stage 2 (analytics) is not yet built. `analytics/load.py` is the only Python
  in this deliverable.

## DUKA parser bug discovered in OmegaSweepHarness

While building the Session A extractor I found a bug in the DUKA timestamp
parsing routine that is also present in `backtest/OmegaSweepHarness.cpp`
(lines 289-302, hash f7983462). Specifically, after scanning past the date
field, the code does NOT advance past the comma before scanning the time
field, which makes the second `while (p < end && *p != ',') ++p;` loop exit
immediately without moving p. Net effect: the time field is mis-parsed.

**The Session A extractor has the bug fixed.** OmegaSweepHarness has NOT been
modified — the rule is no core-code changes without explicit authorisation,
and the sweep is currently working (presumably because the real CSV format
has a header line and / or some data shape that desyncs the bug into yielding
correct results, or because the time field never matters for the harness's
result). Flagged for future investigation; do NOT touch without verifying
the harness output matches before/after change on a known-good CSV.
