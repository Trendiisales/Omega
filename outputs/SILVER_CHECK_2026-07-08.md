# SILVER INDEPENDENT-EDGE CHECK — 2026-07-08 (research only, nothing wired)

**Operator question:** does silver carry ANY independently-wireable edge once real costs are applied?

**Verdict: NO. All four ideas DEAD at spot-silver costs.** Every positive cell anywhere in the
sweep is carried by the Jan-2026 squeeze ($17.6 → $121.7 → $62); no book survives WF halves +
the 2022 window + 2x-cost together. The gold-signal control does NOT beat silver's own signals
(silver is not *pure* gold noise — its own timing is slightly better on itself), but the absolute
edge is un-monetizable at silver's 9–16bp RT cost.

## Verdict table

| Idea | Verdict | Key numbers |
|---|---|---|
| (a) XauTF trend clone (real XauTrendFollow4h mask 0xC9 / 2h engines) on XAG H4/H1 | **DEAD** | 4h: n=734, net +61.8 pts, PF 1.14, WF −17.5/+79.3 (H1 fails), 2022 −9.4, 2x PF 1.06 w/ exBest −13.6. **Pre-2025 whole book = −15.1 pts**; net is one cell (Keltner_EMA50 runner, +50.4 of its +55.2 from 2025-26). 2h: PF 1.01, 2x −68.3. Spike not plateau. |
| (a-control) GOLD-signal vs OWN-signal on silver (identical close-fill machinery, overlap 22-01..26-04) | own wins | 4h: OWN +468.6/PF 4.81 vs GOLD +392.7/PF 3.09; 2h: OWN +9.1 vs GOLD −20.2. The "no independent signal" kill does NOT fire — but neither book is wireable (close-fill machinery flatters both; the honest engine-fill book above is the standard). |
| (b) MGC-style Donchian breakout LONG (Nin 20/40, Nout=Nin/2 close-exit, SMA200 gate) | **DEAD** | Certified M30 2022-24: ALL cells negative, PF 0.48–0.77. H1 2022-26 best (Nin=40 gated): net +25.3, PF 1.31, but WF −5.7/+31.0 (H1 fails), 2022 −1.8 → squeeze-carried. |
| (c) TSMOM {42,63,84}d vol-tgt 10% ann, both dirs, SI=F D1 2015-2026 | **DEAD** | Net % of notional over 10.5y: 42d −9.6, 63d −80.1, 84d −54.6 (all worse at 2x). No lookback positive even before stress. |
| (d) Up-jump ladder on XAG H1 (W×thr sweep, thr-scaled clips) | **DEAD** (bull-beta trap) | Best cell W=96/thr=2: +420.1%, PF 1.29 (<1.3), WF Y, 2x +206.2 — BUT **random-entry control = +255.8 (61% of the net)** and all neighbor cells (48/2, 96/1, 96/3) fail WF → spike, not plateau. Exactly the predicted squeeze/bull-beta trap. |

All books n≥30 (smallest cell n=81). Standards applied: all-6 (net>0, PF≥1.3, WF halves +,
2022 honest, ex-best +, 2x-cost +) + plateau-not-spike + random control on (d).
Tombstone pre-check: no strong prior (second-brain + TOMBSTONES.tsv); registry already records
the XAG BE-floor RETIRED (every cell negative incl. the squeeze) — consistent with today's result.

## Bottom line for the operator

**Silver is squeeze beta, not edge: every framework's profit lives in Jan-2026, its own signals
beat gold's but neither covers silver's 3–4x-gold costs — nothing to wire.**

## Cost basis (stated per order)

- Spot XAG: commission RT = 2 × 0.00025 × px (0.05% RT); spread = **0.030 observed**
  (avg across the 34.3M histdata ticks 2022-01..2024-01 — worse than the 0.02 assumption;
  silver spot spread is proportionally ~3–4x gold's ~4bp). Total ≈ 13–16bp RT at 2022-24
  prices, ≈ 9bp at 2026 prices. Engine-dump fills embed spread via bid/ask = px ∓ 0.015;
  commission added in scoring. 2x stress doubles both legs everywhere.
- (c) SI futures basis: 4bp RT (2bp + tick slippage) 1x, 8bp 2x — generous; still dead.
- (d) ladder rt = 16bp of px per clip (spread+comm at median px 27.4), 32bp at 2x.

## Data (STEP 1 record)

- **Fetched:** 25 histdata XAGUSD tick months (2022-01..2024-01), 0 failures, headless
  cookie+tk recipe → `/Users/jo/Tick/XAGUSD/HISTDATA_COM_ASCII_XAGUSD_T2022*..202401.zip`
  (34.3M ticks). Fetcher: `backtest/xag_fetch_histdata.py` (canonical copy also at
  `/Users/jo/Tick/XAGUSD/fetch_histdata_xag.py`).
- **Built** (`backtest/xag_build_bars.py`; EST→UTC +5h, mid of bid/ask):
  - `/Users/jo/Tick/XAGUSD_2022_2026.h1.csv` — 25,879 bars, 2022-01-02..2026-07-06
    (histdata span merged with existing `/Users/jo/Tick/xag/XAGUSD_h1_clean.csv` 2024-01-15+;
    seam sanity: 303 overlap hours, median close diff 0.017%, p95 0.044%).
  - `/Users/jo/Tick/XAGUSD_2022_2026.h4.csv` — 7,180 bars (resampled from merged H1).
  - `/Users/jo/Tick/XAGUSD_2022_2024.m30.csv` — 23,123 bars (tick span only).
- **Integrity gate:** M30 **CERTIFIED CLEAN**. Full-span H1/H4 FAIL only the 3x-median price
  heuristic — the **documented false positive**: the Jan-2026 squeeze is REAL, cross-verified
  vs independent yahoo SI=F daily (max close 115.08 on 2026-01-26, matching path); 0 h1→h1
  jumps >10% (max 9.12%). Data NOT "fixed", per standing note (ENGINE_BACKTEST_REGISTRY §5).
- SI=F daily 2015-06..2026-07 already cached (`/Users/jo/Tick/SI_F_daily_2016_2026_yahoo.csv`).

## Method notes

- (a) drives the REAL prod engines (`backtest/xag_tf_dump.cpp`, mirrors xau_tf4h/2h_rider_dump:
  H4/H1 bars + next-bar l/h ticks for intrabar SL/TP, gold regime gate off, mask 0xC9 prod).
- (a-control) maps trade windows (entry_ts→exit_ts, side) onto silver H1 closes with identical
  fill machinery for both signal sources. The mapped books run far above the engine books
  because close-fills after intrabar SL hits capture silver's post-stop whipsaw recovery —
  known bias, harmless for the *relative* comparison, never quoted as absolute performance.
- (d) reuses `omega_upjump_ladder_bt.py` run/stats verbatim (TIGHT/WIDE/STACKED clips, cap 5,
  LC 5×thr, EOW flush), 5-seed random-window control.
- Full sweep output: `outputs/SILVER_CHECK_2026-07-08_run.txt`.
  Harness: `backtest/silver_check_bt.py` (+ `backtest/xag_tf_dump.cpp`).
