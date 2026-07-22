# Gold Daily CBE — certification findings (2026-07-22, S-2026-07-22i)

Operator-pasted spec: "Gold Daily Engine: Cost-Covered BE + ATR Management +
Anti-Whipsaw Trail (Long & Short)". Harness `backtest/gold_daily_cbe_bt.cpp`,
M1-grade fills, DST-correct COMEX day roll, R-normalized (costs inside fills).

Data: 2022-01-02..2026-07-13 M1 splice (1,559,580 bars), rebuilt from
`/Users/jo/Tick/xau_h2022full_m1.csv` + `xau_h2023_24_m1.csv` +
`xau_1m_spliced_2024_2026.csv` (later file wins overlap), data_integrity_gate
CERTIFIED CLEAN, clock verified true UTC (Jul close 21:00 / Jan 22:00).
Daily-indicator warmup seeded from GC_F_daily_2016_2026_yahoo.csv (GC≈spot for
a 200-day trend filter; documented deviation).

Cost: IBKR SPOT gold (operator: "we are not using mgc") — 1.5bp/side commission
+ half of MEASURED live spread (l2_ticks_XAUUSD 07-20/21, 824k ticks, median
$0.30-0.36/oz → $0.17/side) + $0.03/side slip. RT ≈ $1.64/oz at $4131.

## Verdicts

**SPEC-AS-WRITTEN = DEAD.** Two structural flaws:
1. BE trigger "+10 pips" ($1.00) = 0.03× daily ATR = noise level. Every trade
   BE-locks then wiggles out at ≈ −(cost)/R. LONG PF 0.00-0.15, WR 0-3%.
   (At the honest spot RT the +$0.30 lock books a guaranteed −$1.3 net/oz.)
2. Flat-by-NY-close + SL sized 1.75× daily ATR ⇒ partial@1R mathematically
   unreachable intraday ⇒ pure cost bleed even with the BE fixed.

**LONG, BE-ratchet OFF, multi-day ATR trail = PASS (certified cell):**

| cell (spot cost) | n | netR | PF | bear22 | chop23 | bull24-26 | WF H1/H2 |
|---|---|---|---|---|---|---|---|
| **CERT: SL1.75 TR2.0 BE-off** | 79 | +21.7 | **2.39** | 1.79 | 1.28 | 3.22 | 1.68/3.19 |
| 2× cost stress | 79 | +20.8 | 2.30 | 1.69 | 1.22 | 3.12 | 1.60/3.09 |
| plateau SL1.5/TR1.5 | 104 | +23.5 | 2.04 | | | | 1.55/2.54 |
| plateau SL1.5/TR2.5 | 73 | +25.2 | 2.44 | | | | 1.49/3.78 |
| plateau SL2.0/TR1.5 | 74 | +19.8 | 2.38 | | | | 1.42/4.13 |
| plateau SL2.0/TR2.5 | 58 | +19.1 | 2.51 | | | | 1.40/4.72 |

Ablations: CONFIRM=0 (raw break) 2.48 — retrace-confirm kept per spec (H1 similar);
ATRBAND=0 → maxDD 5.19R + H1 1.27 (band kept); MINRNG 0.2% slightly worse (0.4% kept);
STRUCT stop = identical (ATR stop always wider here).

BE-ratchet sweep (why the ratchet is OFF): ATR-scaled BE 0.3×/0.5× drops PF to
1.61/1.85 AND flips bear+chop negative (0.92/0.72, 0.81/1.09). $-fixed triggers
worse. Consistent with the S-2026-07-21c finding: profit-locks hurt trend engines.

**SHORT = DEAD every config** (PF ≤ 0.60; BE-off 1.20 on n=21 = thin, one-sided).
Not built.

## Shipped config (engine `include/GoldDailyCbeEngine.hpp`)

Asian range 00:00-08:00 UTC (full-session-observed days only) → break above
Asian high → retrace 25% into range → M1 close back above → LONG. Filters:
prev D1 close > EMA200(D1), ATR14(D1) in P10-P90 of trailing 120d, range ≥ 0.4%
of price. SL 1.75×ATR14(D1). 50% partial at +1R. Runner: 2.0×ATR trail off peak
M1 close, ratcheted on D1 closes, ×0.75 after +2R. No BE-ratchet. No time exit.
Max 1 entry/day. LONG only. 1 oz SPOT (XAUUSD.S → CMDTY/SMART), live.
Expected cadence ~1.5 trades/month.
