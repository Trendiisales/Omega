# 2026-03-18 4-Cell Cluster Post-Mortem v2 (reproduction)

**Generated:** 2026-04-29T11:05:50.516371+00:00
**Window:** `2026-03-17 12:00:00+00:00` -> `2026-03-19 12:00:00+00:00`
**Source ledgers:** `phase1/trades_net/`

## Verdict

**4-cell cluster CONFIRMED.** All 4 of 4 C1_retuned cells took at least one loss inside the window.  This matches the portfolio halt criterion (`>= 4 cells losing same UTC session`) wired into `omega_config.ini`.

## Defense Gap (carried forward verbatim from CHOSEN.md)

Per `phase2/donchian_postregime/CHOSEN.md` -- the numbers in this report come from `phase1/sim_lib.py`, which has zero defensive machinery:

- **No spike-exit logic.**  No volatility-adaptive exit on adverse spike bars.
- **No trail logic.**  Trades sit at fixed SL until either SL or TP fires, or `max_hold_bars` times out.
- **No vol-adaptive SL.**  Stop is a fixed multiple of ATR at entry; it does not widen or tighten with regime.
- **No regime-aware exit.**  Long trades held through a sustained BEAR regime continue to sit at fixed SL.

Live Omega has all three: HBG runs bracket + MFE-proportional trail (locks 80% of move), MacroCrash provides spike defense (ATR>=12.0 / vol>=3.5x / drift>=10.0 thresholds in S44 spike-only retune, currently shadow-mode), and the bracket logic adapts to regime.

**Implication for this report:** the losses tabulated below are the strawman-simulator losses.  Live Omega would have intervened on at least the spike bars.  Do NOT treat the per-trade dollar figures as the live-capital risk; treat them as a research-grade lower bound on what bare sim_lib would absorb.

## Per-cell window summary

| cell | rows total | trades in window | wins | losses | net pnl |
|---|---:|---:|---:|---:|---:|
| `C1Retuned_donchian_H1_long` | 509 | 1 | 0 | 1 | $-66.38 |
| `C1Retuned_bollinger_H2_long` | 133 | 1 | 0 | 1 | $-51.03 |
| `C1Retuned_bollinger_H4_long` | 59 | 2 | 0 | 2 | $-145.02 |
| `C1Retuned_bollinger_H6_long` | 40 | 1 | 0 | 1 | $-94.91 |

## Per-cell loss-trade detail

### C1Retuned_donchian_H1_long

| entry_time                       | exit_time                        |   entry_px |   exit_px | exit_reason   |   pnl_pts_net |
|:---------------------------------|:---------------------------------|-----------:|----------:|:--------------|--------------:|
| 2026-03-17 05:00:00.314000+00:00 | 2026-03-18 10:35:34.458000+00:00 |  5038.3850 | 4972.0580 | SL_HIT        |      -66.3770 |

**Exit reason histogram (all window trades, not just losses):** `{'SL_HIT': 1}`

### C1Retuned_bollinger_H2_long

| entry_time                       | exit_time                        |   entry_px |   exit_px | exit_reason   |   pnl_pts_net |
|:---------------------------------|:---------------------------------|-----------:|----------:|:--------------|--------------:|
| 2026-03-18 18:00:00.013000+00:00 | 2026-03-18 19:51:31.263000+00:00 |  4891.3220 | 4840.3450 | SL_HIT        |      -51.0270 |

**Exit reason histogram (all window trades, not just losses):** `{'SL_HIT': 1}`

### C1Retuned_bollinger_H4_long

| entry_time                       | exit_time                        |   entry_px |   exit_px | exit_reason   |   pnl_pts_net |
|:---------------------------------|:---------------------------------|-----------:|----------:|:--------------|--------------:|
| 2026-03-15 22:00:01.361000+00:00 | 2026-03-18 11:56:07.728000+00:00 |  5003.1010 | 4928.9050 | SL_HIT        |      -74.2460 |
| 2026-03-19 04:00:00.256000+00:00 | 2026-03-19 06:42:54.095000+00:00 |  4851.1650 | 4780.4450 | SL_HIT        |      -70.7700 |

**Exit reason histogram (all window trades, not just losses):** `{'SL_HIT': 2}`

### C1Retuned_bollinger_H6_long

| entry_time                       | exit_time                        |   entry_px |   exit_px | exit_reason   |   pnl_pts_net |
|:---------------------------------|:---------------------------------|-----------:|----------:|:--------------|--------------:|
| 2026-03-16 06:00:00.087000+00:00 | 2026-03-18 12:01:09.558000+00:00 |  5017.8020 | 4922.9450 | SL_HIT        |      -94.9070 |

**Exit reason histogram (all window trades, not just losses):** `{'SL_HIT': 1}`

## Stage-2 (sim_lib defense parity) -- the gate before live capital

Per CHOSEN.md status block -- shadow paper-trading is BLOCKED on these numbers.  `sim_lib.py` must gain spike-exit + trail + vol-adaptive SL matching live HBG behaviour before the C1 portfolio re-run becomes decision-grade.

Concrete next steps (next session, NOT in scope here):

1. Add `spike_exit(adverse_atr_mult=2.0)` to `sim_lib.py`'s position-management loop, mirroring `MacroCrashEngine`'s spike-detector logic.
2. Add `trail_exit(mfe_lock_frac=0.80)` mirroring HBG's MFE-proportional trail.
3. Add `vol_adaptive_sl(min_atr_mult, max_atr_mult, regime)` widening SL on BEAR regime entries and tightening on calm regimes.
4. Re-run `phase2/portfolio_C1_C2.py` with retuned Donchian H1 `(20, 3.0, 5.0)` AFTER (1)-(3) land.
5. Compare the new (post-Stage-2) per-trade losses on 2026-03-18 against this v2 baseline.  Either the cluster shrinks (defense did its job) or it persists (the cluster is regime-driven and asymmetric long-only exposure is the actual problem).

---

_This report was produced by `cluster_postmortem_2026_03_18_v2.py`.  It does not modify any ledger or engine code; it is a read-only diagnostic._
