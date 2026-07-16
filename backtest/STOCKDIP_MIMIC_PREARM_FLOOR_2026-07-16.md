# StockDip BE-Mimic — PRE-ARM REVERSAL FLOOR (operator: "must always exit if the price reverses")

**Date:** 2026-07-16 · **Engine:** `GoldTrendMimicBook` (`include/GoldTrendMimicLadder.hpp`) via
`stockdip_trend_mimic()`, StockDip DIP cells (`include/engine_init.hpp`). **Harness:**
`scratchpad/stockdip_mimic_prearm_floor.py` (exact port of `sim_leg` in `backtest/stockdip_mimic_bt.py`).
**Data:** `backtest/data/bigcap_daily_ohlc/<SYM>.csv` (11 DIP names, Yahoo split+div adj) — 3892 DIP entries.

## The gap
The shipped mimic (commit 5513d4fa) protects **armed** legs perfectly: post-arm peak-profit trail
`stop=(1-gb)·peak`, BE-floored (`peak≥arm_pct>0 ⇒ stop>0`) → an armed leg **always** exits on a
reversal and can never book negative. But a **pre-arm** leg (peak < arm_pct) had only ONE exit —
`LOSS_CUT` at `-lc_pct`. So a pre-arm leg that showed a small gain and then reversed would ride
back down through BE to `-lc%` before cutting. That is exactly the open **AVGO-W** leg the operator
flagged ("arm3/gb70 ⚠️ pre-arm, none yet") — no floor while it sat at +2.66% below the 3% arm.

## The fix — pre-arm BE-ratchet
New `Config::pre_arm_be_pct` (0 = disabled, default → every existing Gold book unchanged). Once a
NOT-yet-armed leg's peak reaches `pre_arm_be_pct` MFE, its floor ratchets from `-lc_pct` up to **BE**:
a reversal to `ret≤0` exits at 0 (`BE_FLOOR`), never rides a pre-arm winner back to the cut. Wired in
both exit paths (`on_h1_bar` close-grade — the path StockDip uses; and `on_tick` resting-exec).
Chosen threshold = **half of arm**: T `pre_arm_be=1.0` (arm2), W `pre_arm_be=1.5` (arm3).

## Backtest — all-6 PASS retained, protection metrics IMPROVE
Pooled, 11 names, net of 8bp RT, judged STANDALONE (independent engine — never vs parent/WIDE).

| Variant | net% | PF | mdd% | 2022 | BEAR | pre-arm legs floored | verdict |
|---|---|---|---|---|---|---|---|
| **T** base (arm2/gb50/lc2) | +3023 | 1.79 | −90.8 | −82 | +287 | — | VIABLE |
| **T** +pre_arm_be=1.0 | +2830 (−6%) | **1.90** | **−86.0** | −80 | +308 | 669 | **VIABLE** |
| **W** base (arm3/gb70/lc3) | +4085 | 1.78 | −167.3 | −161 | +310 | — | VIABLE |
| **W** +pre_arm_be=1.5 | +3959 (−3%) | **2.00** | **−112.2** | **−105** | +373 | 879 | **VIABLE** |

- **Worst clip unchanged** (−2.08% T / −3.08% W): legs that never reach the BE threshold still cut at
  `-lc`; the floor only bites legs that showed ≥ threshold MFE then reversed. Bounded risk preserved.
- Every threshold tested (0.5 / 1.0 / 1.5 / 2.0) stayed **all-6 PASS**; PF and mdd improved monotonically
  vs the baseline. Half-of-arm is the net/protection sweet spot (protects ~65-70% of pre-arm reversers
  for a 3-6% net give-up, while cutting mdd and 2022 bleed).
- Directly closes the flagged leg: AVGO-W peak 2.66% ≥ 1.5 → now BE-floored; a reversal exits at 0
  instead of −3%.

## Verdict
SHIP. Protection-improving (PF↑, mdd↓, bear↑, 2022 bleed↓), edge preserved, all-6 PASS both cells.
Mac canary GREEN (adverse-protection 0 new-violation, mimic drawdown-cancel 0 active-violation).
SHADOW deploy-forward (send_live_order no-ops until mode=LIVE). VPS rebuild owed to ship.
