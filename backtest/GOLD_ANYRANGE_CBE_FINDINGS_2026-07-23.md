# Gold AnyRange CBE — certification findings (2026-07-23)

**VERDICT: NO-SHIP. All 12 configuration cells FAIL the all-6 cert gates.**
The session-agnostic generalization of the certified GoldDailyCbeEngine breaks
on the 2023-chop regime in every cell (PF 0.57-0.83, netR negative), and no
cell reaches the Asian baseline PF 2.39. The fixed Asian-session anchor is
load-bearing, not incidental.

Operator order: generalize the certified break→retrace(25%)→M1-close-confirm
mechanic from the fixed Asian range (00:00-08:00 UTC) to a ROLLING trailing
K-hour consolidation range so it can fire off ANY session's consolidation.
Harness: `backtest/gold_anyrange_cbe_bt.cpp` — extension of the certified
`backtest/gold_daily_cbe_bt.cpp` (data loading, DST-correct COMEX day roll,
cost model, and the certified SL/partial/trail management reused VERBATIM;
only the entry state machine replaced). LONG only. BE-ratchet OFF (certified-
dead on the parent: PF 2.39 → 1.61-1.85). Multi-day hold (TIMEEXIT=0).

Second-brain pre-check (`gold range breakout intraday`): no strong prior
tombstone (nearest: XauTurtleD1 DEAD — different mechanic). Proceeded per
operator order + certified parent.

## Data (certified)

Same splice as the 07-22 parent cert, rebuilt bit-identically: 2022-01-02 ..
2026-07-13 M1, **1,559,580 bars** from `/Users/jo/Tick/xau_h2022full_m1.csv` +
`xau_h2023_24_m1.csv` (rows < 2024-03-01, later file wins) +
`xau_1m_spliced_2024_2026.csv`, deduped on the two DST fall-back repeated hours
(2022-10-31, 2023-10-30; 120 dup rows). `data_integrity_gate.py`:
**CERTIFIED CLEAN** (median $2323, span 1653d, 100% regular M1).
Daily warmup seeded from `GC_F_daily_2016_2026_yahoo.csv` (verbatim parent).

## Baseline reproduction + cost-schedule note (load-bearing)

The current `gold_daily_cbe_bt.cpp` `side_cost` carries a **post-cert edit**:
IBKR $2.00/order minimum commission (`max(2.0, px*0.00015)`), which the 07-22
cert did NOT include (its recorded "RT ≈ $1.64/oz @ $4131" matches the pure
1.5bp schedule; the $2-min schedule gives RT $4.40/oz at 1-oz size). Both
schedules were run (`MINCOMM` env in the new harness). Baseline reproduction
on the rebuilt splice is EXACT under the cert-era schedule:

| baseline (Asian, SL1.75/TR2.0/BE-off) | n | netR | PF | bear22 | chop23 | bull24-26 | WF-H1 | WF-H2 | 2x netR | 2x PF |
|---|---|---|---|---|---|---|---|---|---|---|
| recorded 07-22 cert | 79 | +21.7 | 2.39 | 1.79 | 1.28 | 3.22 | 1.68 | 3.19 | +20.8 | 2.30 |
| reproduced, cert-era cost | 79 | +21.67 | 2.39 | 1.79 | 1.28 | 3.22 | 1.68 | 3.19 | +20.77 | 2.30 |
| reproduced, CURRENT $2-min cost | 79 | +18.98 | 2.13 | 1.47 | 1.10 | 2.97 | 1.42 | 2.98 | +15.39 | 1.84 |

Side note for the operator (baseline, not this variant): under the current
$2-minimum schedule the CERTIFIED baseline itself dips to chop23 PF 0.90 at
2x-cost stress. At 1-oz spot the $2 minimum is ~4.3x the proportional
commission — a size/venue question, not a signal question; flagged here only
because the harness now carries it.

## Sweep grid

- Range window K = trailing {4h, 6h, 8h} rolling high/low of M1 bars (window
  excludes the current bar; timestamp-aged so weekend gaps drop out; requires
  a >=75%-full window).
- Quiet gate at the break bar: width >= 0.4% of price (parent MINRNG verbatim)
  AND optionally width <= 1.0xATR14(D1) (tested with and without).
- Break = M1 high > rolling range high (range freezes) → retrace = M1 low <=
  RH − 0.25×range → confirm/entry = first M1 close > RH.
- Re-arm: {one-trade-per-day cap, re-arm after each completed trade}.
- Gates VERBATIM: prev D1 close > EMA200(D1), ATR14(D1) in P10-P90 of 120d.
- Management VERBATIM (certified): SL 1.75×ATR14(D1), 50% partial +1R,
  2.0×ATR trail off peak close (D1-close ratchet, ×0.75 after +2R), no BE.
- Costs: cert-era schedule (1:1 vs the PF 2.39 bar) AND current $2-min
  schedule; both at 1x and 2x stress.

## Results — cert-era cost basis (1:1 vs recorded baseline PF 2.39)

| cell (K, cap, rearm) | n | netR | PF | WR | maxDD | bear22 | chop23 | bull24-26 | WF-H1 | WF-H2 | 2x netR | 2x PF | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **baseline Asian (bar)** | 79 | +21.67 | 2.39 | 70% | 3.08R | 1.79 | 1.28 | 3.22 | 1.68 | 3.19 | +20.77 | 2.30 | PASS |
| K4h off per-day | 89 | +17.31 | 1.73 | 70% | 5.78R | 0.80 | 0.83 | 2.57 | 0.96 | 2.75 | +16.23 | 1.67 | **FAIL** |
| K4h off re-arm | 89 | +17.31 | 1.73 | 70% | 5.78R | 0.80 | 0.83 | 2.57 | 0.96 | 2.75 | +16.23 | 1.67 | **FAIL** |
| K4h 1.0xATR per-day | 89 | +17.24 | 1.73 | 70% | 5.78R | 0.80 | 0.83 | 2.56 | 0.96 | 2.74 | +16.17 | 1.67 | **FAIL** |
| K4h 1.0xATR re-arm | 89 | +17.24 | 1.73 | 70% | 5.78R | 0.80 | 0.83 | 2.56 | 0.96 | 2.74 | +16.17 | 1.67 | **FAIL** |
| K6h off per-day | 92 | +20.34 | 1.90 | 71% | 5.49R | 1.92 | 0.77 | 2.64 | 1.16 | 2.80 | +19.25 | 1.83 | **FAIL** |
| K6h off re-arm | 92 | +20.34 | 1.90 | 71% | 5.49R | 1.92 | 0.77 | 2.64 | 1.16 | 2.80 | +19.25 | 1.83 | **FAIL** |
| K6h 1.0xATR per-day | 92 | +20.34 | 1.90 | 71% | 5.49R | 1.92 | 0.77 | 2.64 | 1.16 | 2.80 | +19.25 | 1.83 | **FAIL** |
| K6h 1.0xATR re-arm | 92 | +20.34 | 1.90 | 71% | 5.49R | 1.92 | 0.77 | 2.64 | 1.16 | 2.80 | +19.25 | 1.83 | **FAIL** |
| K8h off per-day | 92 | +21.80 | 2.06 | 73% | 6.63R | 4.24 | 0.66 | 3.10 | 1.19 | 3.37 | +20.72 | 1.98 | **FAIL** |
| K8h off re-arm | 92 | +21.80 | 2.06 | 73% | 6.63R | 4.24 | 0.66 | 3.10 | 1.19 | 3.37 | +20.72 | 1.98 | **FAIL** |
| K8h 1.0xATR per-day | 90 | +21.29 | 2.03 | 72% | 6.63R | 4.24 | 0.66 | 3.05 | 1.15 | 3.37 | +20.23 | 1.96 | **FAIL** |
| K8h 1.0xATR re-arm | 90 | +21.29 | 2.03 | 72% | 6.63R | 4.24 | 0.66 | 3.05 | 1.15 | 3.37 | +20.23 | 1.96 | **FAIL** |

## Results — current authoritative cost basis (IBKR $2/order minimum, RT $4.40/oz)

| cell (K, cap, rearm) | n | netR | PF | WR | maxDD | bear22 | chop23 | bull24-26 | WF-H1 | WF-H2 | 2x netR | 2x PF | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **baseline Asian (bar)** | 79 | +18.98 | 2.13 | 67% | 3.34R | 1.47 | 1.10 | 2.97 | 1.42 | 2.98 | +15.39 | 1.84 | PASS(1x)* |
| K4h off per-day | 89 | +14.09 | 1.55 | 67% | 6.78R | 0.67 | 0.71 | 2.39 | 0.82 | 2.58 | +9.79 | 1.35 | **FAIL** |
| K4h off re-arm | 89 | +14.09 | 1.55 | 67% | 6.78R | 0.67 | 0.71 | 2.39 | 0.82 | 2.58 | +9.79 | 1.35 | **FAIL** |
| K4h 1.0xATR per-day | 89 | +14.02 | 1.55 | 67% | 6.78R | 0.67 | 0.71 | 2.39 | 0.82 | 2.58 | +9.72 | 1.35 | **FAIL** |
| K4h 1.0xATR re-arm | 89 | +14.02 | 1.55 | 67% | 6.78R | 0.67 | 0.71 | 2.39 | 0.82 | 2.58 | +9.72 | 1.35 | **FAIL** |
| K6h off per-day | 92 | +17.10 | 1.71 | 71% | 6.71R | 1.58 | 0.66 | 2.44 | 0.99 | 2.63 | +12.77 | 1.49 | **FAIL** |
| K6h off re-arm | 92 | +17.10 | 1.71 | 71% | 6.71R | 1.58 | 0.66 | 2.44 | 0.99 | 2.63 | +12.77 | 1.49 | **FAIL** |
| K6h 1.0xATR per-day | 92 | +17.10 | 1.71 | 71% | 6.71R | 1.58 | 0.66 | 2.44 | 0.99 | 2.63 | +12.77 | 1.49 | **FAIL** |
| K6h 1.0xATR re-arm | 92 | +17.10 | 1.71 | 71% | 6.71R | 1.58 | 0.66 | 2.44 | 0.99 | 2.63 | +12.77 | 1.49 | **FAIL** |
| K8h off per-day | 92 | +18.56 | 1.84 | 71% | 7.97R | 3.40 | 0.57 | 2.86 | 1.01 | 3.16 | +14.23 | 1.59 | **FAIL** |
| K8h off re-arm | 92 | +18.56 | 1.84 | 71% | 7.97R | 3.40 | 0.57 | 2.86 | 1.01 | 3.16 | +14.23 | 1.59 | **FAIL** |
| K8h 1.0xATR per-day | 90 | +18.12 | 1.82 | 71% | 7.96R | 3.40 | 0.57 | 2.82 | 0.98 | 3.16 | +13.90 | 1.58 | **FAIL** |
| K8h 1.0xATR re-arm | 90 | +18.12 | 1.82 | 71% | 7.96R | 3.40 | 0.57 | 2.82 | 0.98 | 3.16 | +13.90 | 1.58 | **FAIL** |

\* baseline under the $2-min schedule dips to chop23 0.90 at 2x stress — venue/size question, see note above.

## Gate scorecard (all-6, no exceptions)

Every cell: net+ ✓, PF>=1.3 ✓, 2x-cost net+ ✓ — but **2023chop NEGATIVE in
all 12 cells** (PF 0.57-0.83, worst at K8), K4 additionally fails 2022bear
(0.67-0.80) and WF-H1 (0.82-0.96), K6 WF-H1 marginal (0.99-1.16). No cell
beats or matches baseline: best variant ALL-PF 2.06 (K8h, cert-era) vs
baseline 2.39, and it degrades chop23 from 1.28 to 0.66 — far below the
"no regime below 1.0" ship condition. **NO-SHIP.**

## Ablation findings

- **Re-arm is a NO-OP** (identical results per-day vs re-arm, all K): with the
  certified multi-day management no trade ever closes on its entry day, so the
  same-day re-entry window the policy would open never occurs. The "any-session
  true" re-arm cannot add trades under this management.
- **1.0xATR width cap is near-inert** (−0.0 to −0.5R): a rolling 4-8h range
  that already passes the >=0.4%-of-price floor almost never exceeds 1x daily
  ATR; the trending-pseudo-range failure mode it targets is rare at these K.
- **K plateau is smooth** (1.73 → 1.90 → 2.06 cert-era), no cliff — the grid
  is well-behaved; it just tops out below the bar with a broken regime.
- **Mechanism**: n rises only 79→92 (+16%) — the rolling range mostly re-finds
  the overnight consolidation later and looser, entering off ANY intraday
  shelf. In trending bull tape that's fine (bull24-26 up to 3.10); in 2023
  chop the non-session ranges break and reverse (chop23 +1.28 → 0.66-0.83).
  The Asian-session anchor (a genuine liquidity/session boundary) is doing
  real selection work the rolling window cannot replicate.

## Disposition

- `include/GoldDailyCbeEngine.hpp` untouched — the certified Asian engine
  stands as-is. Nothing wired; cert-only session per order.
- AnyRange/rolling-consolidation generalization of the CBE mechanic:
  **tombstone this basis** (12-cell grid, both cost bases, both stresses —
  regime-broken everywhere). Any revival needs a NEW basis (e.g. a session-
  boundary-aware range set, not a rolling window).
- Harness kept: `backtest/gold_anyrange_cbe_bt.cpp` (g++ -O2, env-driven;
  `MINCOMM=0` reproduces the 07-22 cert cost schedule exactly).
