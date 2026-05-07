# HANDOFF — XAUUSD NR2-NR20 + Inside Bar Compression Breakout Engine
## Backtest Spec & Viability Gate

**Status:** PROPOSED — viability check only. No C++ engine exists yet.
**Author:** Session 2026-05-03
**Owner:** Jo (Trendiisales/Omega)
**Inspired by:** Zeiierman "Smart NR2-NR20 and Inside Bar" (TradingView script a8pEQinU — closed source; this spec is built from the publicly described rules in the indicator's video walkthrough plus standard Toby Crabel NR/IB literature, not from the Pine code itself).

---

## 1. Purpose

Determine whether a Narrow-Range + Inside-Bar compression-breakout strategy on XAUUSD 15m has independent edge that justifies building a 6th open-position source for Omega (alongside the existing 5, including the FVG §7 engine just shipped in shadow mode on `df12012`).

This document defines the **deterministic backtest rules**. If the Python prototype + walk-forward (`nr2_20_backtest.py`, `wf_nr2_20.py`) clears the acceptance criteria in §10, we promote to a C++ engine `XauusdNr2_20Engine.hpp` modeled on `XauusdFvgEngine.hpp` and add a row-for-row verifier `backtest/verify_xauusd_nr2_20.cpp` per the established Omega pattern.

If the walk-forward fails the gate, we kill the idea here. No C++ work, no shadow deployment.

---

## 2. Data Requirements

- Symbol: XAUUSD
- Timeframe: 15 minute (matches existing FVG baseline `XAUUSD_15min_top10_be0.0_sl2.5_tp5.0_wf2025-12-01`)
- Format: OHLC + timestamp, UTC, gap-aware (weekend gaps preserved as missing bars, not zero-filled)
- Source: same HISTDATA pipeline used for FVG (`fvg_phase0/` data dirs on Mac)
- Minimum history: 24 months for walk-forward (4 train/test windows of 6m each)

**Bar schema (CSV):**
```
ts,open,high,low,close,volume
2024-01-01T00:00:00Z,2062.50,2063.10,2062.40,2062.85,142
...
```

---

## 3. Definitions

### 3.1 Range
For bar `i`: `range_i = high_i - low_i`

### 3.2 Narrow Range NR_n
Bar `i` is `NR_n` if `range_i == min(range_{i-n+1} .. range_i)` over the last `n` bars including itself.

A single bar can simultaneously be NR_2, NR_4, NR_7, NR_20, etc. We classify by the **largest n** the bar qualifies for, capped at `MAX_NR` (default 20).

### 3.3 Inside Bar (IB)
Bar `i` is an Inside Bar if `high_i < high_{i-1}` AND `low_i > low_{i-1}`.

A bar may be both NR_n and IB; both flags can be true.

### 3.4 Qualifying Compression Bar (QCB)
A bar is a QCB if `nr_class >= MIN_NR` (default 4) **OR** `is_ib == True`.

This is the building block of compression clusters.

### 3.5 Compression Cluster & Box
A compression cluster forms when **at least `MIN_QCB`** (default 2) QCBs occur within the last `CLUSTER_LOOKBACK` bars (default 6, including current).

When a cluster forms on bar `i`:
- Let `S` = the set of QCB bars in the lookback window
- Let `span_lo = min(index in S)`, `span_hi = i`
- **Box top** = `max(high_j)` for `j ∈ [span_lo, span_hi]`
- **Box bottom** = `min(low_j)` for `j ∈ [span_lo, span_hi]`
- **Formation bar** = `i` (the bar that completed the cluster)
- **Formation timestamp** = `ts_i`
- **Formation session** = `session_for_ts(ts_i)` (per Omega session rules — match `XauusdFvgEngine` formation-bar session assignment, NOT entry-bar)

Box is **ARMED** for up to `BOX_EXPIRY` bars (default 8) after formation. If price doesn't break out within that window, box invalidates.

### 3.6 Trend Filter
- MA type: EMA
- MA period: `TREND_MA_PERIOD` (default 50)
- MA source: close
- **Trend up** if `EMA_i > EMA_{i-TREND_SLOPE_LOOKBACK}` (default lookback 3 bars, i.e. EMA rose over last 3 bars)
- **Trend down** if `EMA_i < EMA_{i-TREND_SLOPE_LOOKBACK}`
- **Flat** otherwise — no trades

### 3.7 Sessions
Three sessions, per Omega convention:
- ASIA: 22:00–07:00 UTC
- LONDON: 07:00–13:00 UTC
- NY: 13:00–22:00 UTC

Session is captured at **formation time**, not entry time (matches the v3-alignment fix in `XauusdFvgEngine.hpp` you just shipped).

---

## 4. State Machine

```
        +---------+   QCB count >= MIN_QCB
        | IDLE    | --------------------------+
        +---------+   in last CLUSTER_LOOKBACK |
            ^                                  v
            |                            +-----------+
            | box expires                | ARMED     |
            | OR breakout vs wrong trend | (box live)|
            |                            +-----------+
            |                                  |
            |                                  | close > box_top (LONG)
            |                                  | OR close < box_bot (SHORT)
            |                                  | AND trend aligned
            |                                  v
            |                            +-----------+
            +----------------------------| IN_TRADE  |
                 SL / TP / TIME_STOP     +-----------+
```

One active position per engine instance. While `IN_TRADE`, no new boxes are armed (cluster detection still runs in the background but ARMED state is suppressed).

---

## 5. Entry Rules

### 5.1 LONG entry
All conditions on bar `i`:
1. There is an ARMED box (formed on bar `f`, where `i - f <= BOX_EXPIRY`)
2. Trend is **up** as of bar `i`
3. `close_i > box_top`
4. No open position

→ Enter LONG at `close_i`. Entry timestamp = `ts_i`. Entry bar sequence = `m_bars_seen` (matches FVG engine's `entry_bar_seq` field for time-stop accounting that skips weekend gaps).

### 5.2 SHORT entry
Mirror of LONG:
1. ARMED box exists
2. Trend is **down**
3. `close_i < box_bottom`
4. No open position

→ Enter SHORT at `close_i`.

### 5.3 Conflict resolution
If a single bar produces both a LONG and a SHORT signal (possible with a flat MA flipping direction inside the bar), reject the trade. Log as `ENTRY_REJECT_CONFLICT`.

---

## 6. Exit Rules

For each open position, evaluated on every subsequent bar:

### 6.1 Stop Loss
- LONG: `low_j <= box_bottom` → exit at `box_bottom`, reason `SL`
- SHORT: `high_j >= box_top` → exit at `box_top`, reason `SL`

### 6.2 Take Profit
- LONG: `high_j >= entry + (entry - box_bottom) * RR` → exit at TP price, reason `TP`
- SHORT: `low_j <= entry - (box_top - entry) * RR` → exit at TP price, reason `TP`
- `RR` default = 1.0; sweep also tests 1.5

### 6.3 Time stop
- `bars_held = m_bars_seen - entry_bar_seq` (sequential trading bars; weekend gaps skipped, mirroring FVG fix)
- If `bars_held >= TIME_STOP_BARS` (default 64, ≈ 16 hours on 15m), exit at `close_j`, reason `TIME_STOP`

### 6.4 Same-bar SL+TP ambiguity
If a bar's range covers both SL and TP levels, default to **SL** (conservative). Configurable via `--same-bar-resolution {sl_first,tp_first,reject}`.

---

## 7. Box Invalidation (no trade)

A box expires unused if:
- `m_bars_seen - formation_bar > BOX_EXPIRY` AND no entry was triggered
- A breakout occurred but trend was wrong direction (long breakout in down-trend, or vice versa)
- Logged for diagnostic: `BOX_EXPIRED` / `BOX_REJECT_TREND`

---

## 8. Parameters (defaults & sweep ranges)

| Param | Default | Sweep range | Notes |
|---|---|---|---|
| `MAX_NR` | 20 | fixed | matches indicator title |
| `MIN_NR` | 4 | {2, 4, 7} | Crabel canonical NR4/NR7 |
| `MIN_QCB` | 2 | {2, 3} | min qualifying bars for cluster |
| `CLUSTER_LOOKBACK` | 6 | {4, 6, 10} | bars to scan for QCBs |
| `BOX_EXPIRY` | 8 | {5, 8, 12} | bars after formation before box voids |
| `TREND_MA_PERIOD` | 50 | {20, 50, 100} | EMA period |
| `TREND_SLOPE_LOOKBACK` | 3 | {1, 3, 5} | bars for slope calc |
| `RR` | 1.0 | {1.0, 1.5, 2.0} | risk/reward |
| `TIME_STOP_BARS` | 64 | {32, 64, 128} | sequential trading bars |
| `SAME_BAR_RESOLUTION` | sl_first | {sl_first, reject} | conservative default |

Total grid: 3·2·3·3·3·3·3·3·2 = 8,748 combinations. Walk-forward will sample, not exhaust.

---

## 9. Walk-Forward Methodology

Mirroring `HANDOFF_FVG_BACKTEST.md` §2 and the existing `wf2025-12-01` window naming.

**Window scheme:** rolling 6-month train / 3-month test, step 3 months.

| Window | Train | Test |
|---|---|---|
| W1 | 2024-01 → 2024-06 | 2024-07 → 2024-09 |
| W2 | 2024-04 → 2024-09 | 2024-10 → 2024-12 |
| W3 | 2024-07 → 2024-12 | 2025-01 → 2025-03 |
| W4 | 2024-10 → 2025-03 | 2025-04 → 2025-06 |
| W5 | 2025-01 → 2025-06 | 2025-07 → 2025-09 |
| W6 | 2025-04 → 2025-09 | 2025-10 → 2025-12 |
| W7 | 2025-07 → 2025-12 | 2026-01 → 2026-03 |

For each window:
1. Run grid (or random sample of 500 combos from the 8,748) on **train** segment
2. Pick top-10 param sets by combined score: `0.6 * Sharpe + 0.4 * normalized_PF` (subject to `min_trades >= 30` and `max_DD <= 15%`)
3. Apply each top-10 set to **test** segment
4. Report mean test-segment metrics for the top-10

Final evaluation: equal-weight aggregate of all test segments across W1–W7.

---

## 10. Acceptance Criteria for C++ Promotion

The strategy must clear **all** of these on aggregated out-of-sample test segments:

| Metric | Threshold | Rationale |
|---|---|---|
| Profit factor | ≥ 1.30 | meaningful edge after costs |
| Sharpe (trade-level, annualized) | ≥ 1.0 | consistency, not just total return |
| Max drawdown | ≤ 15% of equity | risk-managed |
| Trade count | ≥ 200 across all windows | statistical significance |
| Win rate | ≥ 38% | consistent with breakout-style RR |
| Correlation w/ FVG signal series | < 0.4 | adds independent information |
| Param stability | top-10 params overlap ≥ 50% across windows | not curve-fit |

**Cost model:** assume 0.5 pip spread + 0.3 pip slippage per side on XAUUSD, applied at entry and exit prices.

If **any** threshold fails, do not proceed to C++. Document failure mode and shelve.

---

## 11. Deliverables (this session)

1. ✅ This spec doc (`HANDOFF_NR2_20_BACKTEST.md`)
2. ✅ `nr2_20_backtest.py` — single-run backtest, argparse-driven, emits `nr2_20_trades.csv`
3. ✅ `wf_nr2_20.py` — walk-forward driver wrapping the above
4. ✅ `README_NR2_20.md` — Mac-side run instructions for Jo
5. ✅ Self-test against synthetic OHLC fixture (in `nr2_20_backtest.py --self-test`)

**Not in scope this session:**
- C++ engine
- C++ verifier
- Symbols.ini / DEPLOY_OMEGA.ps1 changes
- omega_config.ini changes

Touching core code is gated on walk-forward results. No core changes per Jo's standing rule.

---

## 12. Output Schema (`nr2_20_trades.csv`)

```
trade_id,entry_ts,exit_ts,side,entry_px,exit_px,pnl_quote,pnl_R,bars_held,exit_reason,box_top,box_bottom,formation_ts,formation_bar,session,trend_ma_at_entry,nr_class_count,ib_count
```

Field notes:
- `pnl_R`: signed PnL in R-multiples (`(exit - entry) * sign / (entry - sl_price)`)
- `pnl_quote`: signed PnL in price units (gold dollars per troy oz, before contract size)
- `nr_class_count`: number of QCBs in the formation cluster that were NR-classified
- `ib_count`: number of QCBs that were Inside Bars
- `session`: ASIA / LONDON / NY (from formation bar)

Schema is intentionally close to the FVG engine's expected output to make later C++ verifier porting trivial.

---

## 13. Known Risks Going In

1. **Closed-source reference.** Our signal series will diverge from Zeiierman's TradingView indicator. We're testing the *idea*, not replicating his exact bars. If the user wants a 1:1 match with the TV indicator, this approach won't deliver it.
2. **XAUUSD news risk.** NFP, FOMC, geopolitical headlines collapse compression patterns. The strategy may show edge in calm regimes and bleed in event-driven ones. Walk-forward should capture this if windows span enough event clusters.
3. **Signal correlation with FVG.** Both engines target XAUUSD compression-then-expansion. If correlation > 0.4, the engine adds risk without diversification benefit. §10 gates this explicitly.
4. **1:1 RR sensitivity.** Below ~50% win rate the strategy bleeds. Trend filter is the make-or-break — sweep `TREND_MA_PERIOD` aggressively.
5. **Box construction is the soft spot.** Zeiierman's "Smart" cluster logic is opaque; ours is a defensible reconstruction but not the same. If results look weak, the next iteration is to re-spec the box (e.g. ATR-bounded, ADX-adaptive lookback).

---

## 14. Pointers

- Sister doc: `HANDOFF_FVG_BACKTEST.md` (FVG §7 baseline, same shape)
- Engine template to mirror if/when promoted: `include/XauusdFvgEngine.hpp` post-§7 fixes
- Verifier template: `backtest/verify_xauusd_fvg.cpp`
- Deploy script: `DEPLOY_OMEGA.ps1` (would gain a `XauusdNr2_20` MIN/MAX_RANGE block)

— END SPEC —
