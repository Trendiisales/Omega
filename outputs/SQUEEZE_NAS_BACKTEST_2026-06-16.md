# SqueezeSlingshot — NAS100 cross-regime backtest → DEAD (tombstone)

**Date:** 2026-06-16  **Verdict:** do NOT wire. Engine code kept as reference, unwired.

## What was tested
`backtest/squeeze_xregime_nas.cpp` drives the **real production path** —
`SqueezeSlingshotEngine<Traits>::on_tick` → internal bar aggregation → the
lookahead-free `SqueezeSlingshotCore` — over
`/Users/jo/Tick/xregime/NAS100_full_ds10.csv` (20.86M ticks; 2022 bear +
2024–26 bull; **source has no 2023 data**). One data pass feeds every tick to all
trait variants via a type-erased runner; trades collected through the engine's own
`on_close_cb`. **Cost-inclusive** (round-trip points subtracted in reporting) at
**1×/2×/3×** (2/4/6 pts). Buckets: per-year, BEAR'22 / BULL, walk-forward H1/H2.

Full lever space swept: timeframe {M30, H1, H4, D1} × min_tier {1,2,3} ×
atr_target {4, 6, 0=rollover} × long-only × strict-momo-below-zero × stop-width.

## Result — every variant fails the bar

| variant | ALL PF @1× | BEAR'22 | WF-H1 / WF-H2 | note |
|---|---|---|---|---|
| H1_prod      | 0.98 | 0.90 | 0.88 / 1.07 | neg; halves disagree |
| H1_tier2_t6  | 0.96 | 0.97 | 0.91 / 1.02 | neg everywhere |
| H1_strictC   | 1.08 | 0.87 | 0.88 / 1.32 | **all profit in 2025**; 1.08→0.99 across cost; WF split |
| H1_longonly  | 1.02 | 0.85 | 1.08 / 0.96 | bull-beta; 2026 PF0.50 |
| H1_rollover  | 1.01 | 0.90 | 0.87 / 1.13 | neg @2×; WF split |
| H1_tier3     | 1.13 | 25.16 | 2.21 / 0.62 | n=36 fat-tail noise (2025 PF0.35) |
| H1_wide      | 0.98 | 0.88 | 0.83 / 1.12 | neg |
| M30_prod     | 0.93 | 0.81 | 0.83 / 1.02 | worse |
| H4_prod      | 0.94 | 0.83 | 0.89 / 0.99 | neg |
| H4_rollover  | 0.98 | 0.73 | 0.85 / 1.09 | bull-only; bear awful |
| H4_longonly  | 1.07 | 0.81 | 0.96 / 1.18 | bull-beta; WF split |
| D1_prod      | 1.09 | 0.21 | 0.40 / 1.63 | n=37; BEAR −177/trade; pure regime split |
| D1_rollover  | 0.77 | 0.21 | 0.35 / 1.11 | neg |

## Why it's a tombstone (not a fast-tombstone)
- **2022 bear is negative in EVERY variant.** The hypothesis that a long+short
  squeeze would survive the bear better than long-only BigCapMomo is **falsified** —
  both sides lose in the bear (D1 BEAR PF 0.21).
- **No variant has both walk-forward halves positive** — WF-H1 negative, WF-H2
  positive is the textbook regime-luck signature.
- Every apparent positive is **bull-beta, single-regime (2025), cost-fragile, or
  fat-tail noise**. 2024 is negative across the board.
- The selectivity levers (tier2/tier3, strict-momo) — the rescue that worked for
  prior discretionary edges — here only thin the book into fat-tail noise. There is
  no live-profitable-human discretion behind a mechanical TTM indicator, so the
  "don't tombstone discretionary edges on literal rules" exception does not apply.

Per the **never-deploy-without-a-passing-backtest** rule, the gate blocks the
shadow-wire. Engine + core + harness are kept as reference; nothing wired into Omega.

## Keepers
- `backtest/squeeze_xregime_nas.cpp` — type-erased multi-trait one-pass x-regime harness.
- `include/SqueezeSlingshotCore.hpp`, `include/SqueezeSlingshotEngine.hpp` — unwired.
