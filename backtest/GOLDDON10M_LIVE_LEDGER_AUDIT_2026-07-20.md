# GoldDon10m LIVE-LEDGER ADVERSARIAL AUDIT — 2026-07-20 (handoff-20t item 2)

Operator question: "GoldDon10m was working fine in backtests — why failing now, more lies?"
Decisive test (per ENGINE_BACKTEST_REGISTRY rule 1): diff the honest-harness verdict against the
LIVE shadow/trade ledger on omega-new — the primary forward record. Receipts below, no assertions.

## VPS state verified
- omega-new repo HEAD = running binary `Git hash: 58a478e` = origin/main tip at audit time.
- HONEST booking (S-20z, clamps removed) went LIVE 2026-07-20 05:47 box time → honest forward
  window at audit ≈ 13h. Everything older is the clamped-booking era.

## Live ledger — every surviving GoldDon* row (deduped across ledger generations)
Sources: omega_trade_closes.csv (+ _2026-07-19/-20 dailies, .pre_cutover_zero_20260718,
.bak_20260717_ibsrebook, .pre_reset_20260713), dedup key = entry_ts|engine. Size = 1 oz.

| book              | n | exit mix                    | net $  |
|-------------------|---|-----------------------------|--------|
| GoldDon10mMimicT  | 6 | 2 TRAIL_STOP +$99.47, 4 BE_FLOOR −$7.99 | **+$91.48** |
| GoldDon10mMimicW  | 6 | 2 TRAIL_STOP +$85.97, 4 BE_FLOOR −$7.99 | **+$77.98** |
| GoldDon15mMimicT  | 3 | 3 LOSS_CUT                  | **−$67.45** |
| GoldDon15mMimicW  | 3 | 3 LOSS_CUT                  | **−$67.45** |

Window: 2026-07-14 → 2026-07-20 (ledger was cutover-zeroed 07-18 at live-arm; earlier
generations recovered from .bak/.pre_reset files). goldmimic_golddon10m_state.txt book0
cum: +1.60% gross / +1.55% net. goldmimic_*_closed.csv confirmed append-only dead log
(8 rows, NOT a PnL source — project-goldmimic-closed-csv-dedup).

## What the live record proves
1. **Floor-churn is real and pays real money.** 4 of 6 recent 10m entries exited BE_FLOOR at
   gross $0.00 / net −$2.00 each (round-trip cost on 1 oz @ ~$4,000 = 5bp). The honest harness's
   kill mechanism is churn frequency × real churn cost: 1778/1860 entered legs (96%) BE_FLOOR
   over the 6-mo 1m-truth replay. Live churn share so far: 4/6 (67%) — small n, same mechanism,
   live and undeniable.
2. **The old "+$14.0k/+$12.5k 6-mo" was NEVER a live record.** It was the clamped-booking
   backtest cert: BE_FLOOR exits booked at the floor LEVEL (gross clamped ≥0, pierce ignored),
   so ~1,800 churn legs were treated as free. Re-clamping the honest harness reproduces the old
   cert numbers exactly (verified S-20ae) — the delta between +$14k and −$10.9k IS the booking,
   not a market change and not a harness bug.
3. **Live 6-day window is +$91 (10m T)** on 2 trail rides. That does not overturn a 6-month
   distributional verdict (n=6 vs n≈1,860 legs); it shows the engine still catches rides AND
   bleeds churn. GoldDon15m's own live record is already negative (−$67.45, 3/3 LOSS_CUT,
   zero wins).

## Answer to the operator
Not lies in the harness — the lie was in the OLD booking. The engine wasn't "working fine in
backtests"; the backtests were reading a ledger that clamped every floor-churn to $0. The live
ledger now books what the harness books (−$2/churn, real loss-cuts). Honest forward record
(started 2026-07-20 05:47) is the deciding evidence stream from here.

## Feeds operator decision (queued): pause live MGC books?
- GoldDon15m: live record itself negative, honest cert FAIL → pause defensible on live evidence alone.
- GoldDon10m: honest 6-mo cert −$10.9k/−$11.2k; live 6-day +$91 (2 rides). Mechanism confirmed.
- XAU_4h_DonchN20: honest cert −42%; live pre-cutover row +$25.3 (1 trade, RECLAIM).
Operator call. If kept running, the honest forward ledger now accrues the true number either way.
