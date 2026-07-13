# Gold Session Trend-Pullback Engine — faithful spec backtest (2026-07-14)

Operator supplied a full spec ("Gold Session Trend-Pullback Engine": London/NY session windows,
4-condition directional regime, 30-min opening-range breakout + impulse qualifiers, first-pullback
25–55% continuation entry, structural stop, cost-gated stop floor, partial @1.25R, true-BE,
hybrid ATR/swing trail, news/LBMA blocks, 1-win/session + 2-loss/day caps) and asked: what do we
have that fits, test it thoroughly, table the results.

## Verdict: NOT VIABLE — do not build. Spec-faithful = net negative; every salvage ablation fails
## the house gate (both-WF+, both-regimes+, 2×cost). Consistent with three standing priors.

## Harness

`backtest/gold_session_tp_bt.cpp` (kept in scratchpad + committed here) — M1-grade fills, 5m
decisions, DST-correct session conversion (US 2nd-Sun-Mar/1st-Sun-Nov, UK last-Sun-Mar/Oct,
verified against tape's COMEX close-gap hour), COMEX 17:00-ET day roll for PD-levels/VWAP anchor,
IBKR real cost (1.5bp/side comm + $0.30 spread + $0.03/side slip), R-normalized accounting
(risk-per-trade = 1R, costs inside fills). ENV knobs: BE_ARM, NEWS, LBMA, COST_MULT (selection+fills),
COST_EXTRA (fills only — honest 2×cost stress), SESS, IMPULSE, PULLBACK, REGIME.

Data: `XAUUSD_2022_2026.m1.csv` (2022-01-02..2026-06-26, 1.55M M1 bars) — the S-14l certified
4-seg duka stitch + histdata-2022 fill; re-staged via `stage_certified_data.sh` → CERTIFIED CLEAN
(stamp present). Clock verified true UTC (daily close gap 21:00 UTC Jul / 22:00 UTC Jan).

Known deviations (documented, none rescue the verdict): VWAP = equal-weight M1 typical price (tape
has no volume); tick-volume/activity qualifier not implementable on bar tape (range qualifier
carries it); spread-percentile gate not implementable (constant modelled spread); CPI/PCE dates not
embedded (FOMC list + computed NFP first-Friday only — blocks barely bind anyway: identical result
with NEWS=0).

## Results (2022-01..2026-06, netR = sum of R-multiples, R = per-trade risk)

| config | n | netR | avgR | PF | WR | maxDD(R) | WF H1 / H2 PF | 2022bear / 24-26bull PF |
|---|---|---|---|---|---|---|---|---|
| **SPEC-FAITHFUL (BE 0.8R)** | 113 | **−9.4** | −0.083 | **0.79** | 55% | −17.3 | 0.47 / 1.05 | 0.18 / 1.00 |
| spec, BE arm @1.5R | 110 | −6.8 | −0.062 | 0.87 | 42% | −19.8 | 0.60 / 1.08 | 0.31 / 1.01 |
| spec @ honest 2×cost (COST_EXTRA) | 121 | −38.1 | −0.315 | **0.40** | 53% | −40.8 | 0.30 / 0.49 | 0.23 / 0.50 |
| spec, news+LBMA blocks off | 113 | −9.4 | −0.083 | 0.79 | 55% | −17.3 | same | same |
| ablate: impulse qualifiers OFF | 119 | −3.5 | −0.029 | 0.92 | 56% | 0.52 / 1.26 | 0.28 / 1.17 |
| ablate: pullback OFF (chase the break) | 622 | **−108.0** | −0.174 | 0.61 | 58% | −120.6 | 0.45 / 0.74 | 0.25 / 0.71 |
| ablate: regime stack OFF (VWAP-dir only) | 150 | +9.0 | +0.060 | 1.17 | 65% | −6.3 | 1.02 / 1.27 | 0.91 / 1.17 |
| regime-OFF @ honest 2×cost | 159 | −35.0 | −0.220 | 0.53 | 60% | −39.3 | 0.36 / 0.65 | 0.34 / 0.60 |
| regime-OFF, NY-only | 113 | +10.6 | +0.094 | **1.28** | 66% | −5.2 | **1.00** / 1.54 | **0.84** / 1.32 |
| regime-OFF, LDN-only | 37 | −1.7 | −0.044 | 0.88 | 65% | −4.7 | 1.17 / 0.82 | 1.18 / 0.85 |
| regime-OFF, impulse-OFF | 160 | +13.4 | +0.084 | 1.26 | 66% | −9.4 | 1.01 / 1.44 | 0.92 / 1.28 |
| regime-OFF, BE @1.5R | 146 | −0.6 | −0.004 | 0.99 | 49% | −15.0 | 1.09 / 0.93 | 1.03 / 0.93 |

Base-spec exits: STOP n=28 net −35.6R · TRAIL/BE n=42 +6.3 · VIOLENT_OPP n=11 +7.1 ·
SESSION_END n=32 +12.8 · VWAP_CROSS/SLOPE_REV 0 (stops/trail always hit first).

## Reading

1. **The spec as written loses.** PF 0.79 over 4.5yr, catastrophic in the 2022 bear (PF 0.18),
   flat even in the 24-26 bull (PF 1.00). At honest 2×cost it's PF 0.40. The 2.5×cost stop floor
   also collapses selection if costs rise (COST_MULT=2 → n=7/4.5yr).
2. **The 4-condition regime stack is the single biggest drag** — by the time VWAP + EMA20>50(15m)
   + EMA20-slope(60m) + PD-mid all agree mid-session, the entry is late; dropping it (VWAP-direction
   only) swings the book +18R to PF 1.17. But even the best ablated cell (NY-only, PF 1.28) is
   **bull-half beta**: WF-H1 PF 1.00, 2022 bear PF 0.84, and 2×cost kills every cell (best 0.65).
   Nothing passes the house gate.
3. **The pullback requirement is the one validated idea** — chasing the raw break instead is
   −108R/PF 0.61. This confirms the spec's own thesis ("never chase") without making the engine viable.
4. **Priors reconfirmed, not overturned**: [[IntradayTimingAnchorScan]] (ORB-up break on gold =
   DEAD anchor, −2.7bps; VWAP reclaim flat), [[GoldIntradayBreakoutCrossRegime]] (faithful tick BT:
   intraday gold breakout has no cross-regime edge), [[GoldOrbRetraceEngine]] (ORB+retrace live
   0/7 — intraday-spot-CFD cost wall). The composite chain does not rescue the dead anchor.

## What we already have that fits (inventory)

| spec component | existing | status |
|---|---|---|
| ORB + retrace/pullback entry | GoldOrbRetraceEngine (+LDN sibling) | ⛔ RE-TOMBSTONED 06-23, live 0/7 |
| Session-windowed gold breakout | GoldSessionBreakoutEngine | disabled |
| Session straddle (the surviving session pattern) | XauStraddleM15/M30 | ✅ live, PF~1.6, weekend-flat |
| Impulse qualifier | ImpulseFilter (min_impulse_atr) | ✅ live on XauTF 1h/4h/D1 trend engines |
| VWAP regime | IndexVwapReclaimEngine / vwap studies | VWAP reclaim = flat/DEAD on gold |
| PDH/PD-mid anchor | IntradayTimingAnchorScan | PDH-break = the one good intraday anchor (unbuilt as engine) |
| Partial + BE + hybrid trail | XauTF family / straddle partials | ✅ live patterns |

## If anything is worth salvaging

Not this chain. The scan evidence says the intraday gold long anchor that *does* carry edge is the
**PDH break in the NY window** (+5.3bps/58%, both halves +), not the opening-range break. A
PDH-anchored NY-session continuation engine with the pullback discipline from this spec would be a
different study (separate pre-registration; not built here).
