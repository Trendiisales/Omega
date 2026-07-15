# Gold MEAN-REVERSION / FADE study — 10m (5m/15m context), MGC cost — NO-GO

**S-2026-07-15u.** Harness `backtest/gold_mr_10m_bt.cpp`. Answers the operator's question
"is there any OTHER engine we can build for the 10-min timeframe" by testing the one
mechanistically-distinct class the sub-30m trend study never swept.

## Why this study
The sub-30m trend/breakout study ([[GoldSub30mStudy2026H1]], `GOLD_SUB30M_2026H1_FINDINGS.md`)
swept ONLY trend families (Keltner / Donchian / dual-EMA TF) and explicitly left the
"ORB-family/scalp hands-off list untouched" (that findings, L70). At 10m trend is exhausted:
KELT dead, TF no-pass, only the slow-exit Donchian survived → already wired
(`GoldDon10m_30_35`, [[GoldBothWaysShortTfEngine]] S-14bi). The untested, mechanistically
OPPOSITE class is **mean-reversion / fade** — and the existing gold MR/scalp engines
(GoldReversalScalp, GoldSessionBreakout M5, GoldOrbRetrace, GoldPanicBounce) were all judged
under the OLD pessimistic SPOT cost (1.60pt RT); the MGC futures basis is 4× cheaper
(0.41pt RT), which already flipped one dead trend family (KELT M30) dead→viable. A fade book
would also be anti-correlated to the wired DON10m breakout → diversifying. So the re-open was
legitimate (new cost basis). This study settles it.

## Method (identical certified machinery to the trend study)
- **Data:** certified spliced spot XAUUSD 1m tape `/Users/jo/Tick/xau_1m_spliced_2024_2026.csv`
  (837,302 bars), resampled to 5m/10m/15m. Same honest MGC substitution (no MGC bars <30m
  exist); MGC cost basis 0.41pt RT + 2× stress.
- **Gate (identical all-6):** net>0 @1× AND @2×, PF≥1.3, both WF halves +, both legs +.
  6mo window 2026-01-14..07-15, WF split 04-14, FULL 2024-06.. for context. Gap-honest
  adverse-first ATR stops, no LC (registry §7). MR time stop 36 bars (short by design).
- **Mechanisms (both-ways FADE):** BBFADE (Bollinger band → SMA mean), KFADE (Keltner band →
  EMA20 mean, the exact opposite of the wired DON/KELT breakout entry), ZREV (z-score
  reversion), RSI2 (Connors RSI(2) reversion). 78 cells total.

## Verdict — NO-GO, structurally (not a cost story)
**0 / 78 cells pass the gate.** Every family, every TF (5m/10m/15m):
- **10m:** all 26 cells fail. Best is BBFADE n20 k3.0 s3.0 = **+$1,107 @1× / +$402 @2×,
  PF 1.06** — but WF-H2 **−$903** and FULL-history **−$2,880 PF 0.93**: a marginal
  6mo-window fluke, exactly what the gate catches. Everything else is net-negative.
- **5m / 15m:** same — all fail. The best-looking cell anywhere (5m KFADE k1.5 s2.5,
  +$7,761 @1×) is **short-leg-only** (S +$9,435 vs L −$1,674), WF-H2 negative, FULL
  −$12,050 PF 0.94 — regime-carried short in the 6mo bear, not an edge (legs- + FULL- + PF-).
- **Cost-share is LOW (3–8%)** → this is NOT friction. The mechanisms are gross-negative or
  barely-positive BEFORE costs. Symmetric with the sub-30m 1m finding ("structurally dead —
  NOT a cost story"): **gold TRENDS intraday at these grains, so systematically fading it
  bleeds.** This is the mirror image of why the DON breakout survives.

## Conclusion
No new fade/MR 10m gold engine. The wired **DON10m breakout is the only viable 10m mechanism**;
its opposite (fade) is now confirmed dead across BB/Keltner/z-score/RSI2, symmetric long+short,
at the fair futures cost. Do not re-open without a genuinely new basis (different mechanism
family, a regime/session filter that isolates a mean-reverting sub-window, or new data).

## What remains untested at 10m (lower odds, both still breakout-aligned)
1. **Session ORB** — opening-range breakout anchored to London/NY open (time-anchored, not
   rolling). Still a breakout (the winning direction), so competes with the already-wired DON10m.
2. **Session / hour-of-day GATE on the live DON10m** — winner-extension
   ([[feedback-extend-winning-engines-standing-order]]), not a new engine.

Cross-ref: [[GoldSub30mStudy2026H1]], [[GoldBothWaysShortTfEngine]], [[GoldShortTfBothWays2026H1]],
[[GoldDeepDive2026-07-08]], [[DataCertificationGate]].
