# SURVIVOR-CELL MIMIC — BE-entry mimic test (S-2026-07-14, operator ask)

**Ask:** "mimic engine for this [companion-clips panel / XAU_4h_DonchN20]: as soon as the
system detects we have a profitable trade we open a mimic, the mimic trades until it
reverses and exits — independent, zero effect on the real trade."

The engine in the panel is a **SurvivorPortfolio cell** (`XAU_4h_DonchN20`), which the
shipped [[GoldTrendMimicLadder]] does NOT cover (it hooks XauTf4h/D1 + MgcFastDon + index
turtles). This study tests the SAME shipped BE-ENTRY leg mechanism on the survivor cells.

## Method (faithful)

- `backtest/clip_path_survivor.cpp` drives the REAL `omega::survivor::Portfolio` in the
  LIVE config (S-2026-07-08c: dedup_mode=1, USTEC_4h_RSI_N7 culled, asym sustained-bear
  long-veto both symbols — the survivor_gated_bt.cpp-validated proxy).
- Tapes: `XAUUSD_2022_2026.h4.csv` + `NSXUSD_2022_2026.h4.csv` (both CERTIFIED CLEAN by
  data_integrity_gate.py; NDX daily 2016+ seeds the veto). **Parity: 445 closed trades =
  exactly the audited survivor_gated_bt n=445.**
- Overlay: `backtest/mimic_ladder_overlay.cpp` (the shipped BE-ENTRY leg mechanism:
  leg PENDING at parent open → opens at first close ≥ +be favourable → peak-profit
  giveback trail = reversal exit → pre-arm lc cancel → independent cap; zero feedback).
- Gate: all-6 (net>0, PF≥1.3, both WF halves+, bull+, bear+) + 2× cost. Close-grade H4
  (same caveat class as the GoldTrendMimicLadder validation).

## Results

### XAU_4h_DonchN20 (361 parent trades, 2022-2026)
- **Shipped-default trigger (be 0.10%) FAILS** — full 540-point sweep: **0 passers**
  (H1 and bear negative everywhere). The cell trades too often (~84/yr) in chop.
- **PROFIT-CONFIRMED trigger PASSES**: be=1.0% (mimic opens only once the parent trade
  is +1.0% — literally "as soon as we detect we have a profitable trade", confirmed):
  - Candidate cell **be1.0 / arm0.25 / lc2 / cap30 / gb tight 10%**:
    net **+14.6%/leg** (4.3yr), **PF 2.05**, DD −3.2%, n=60 legs (301 cancelled pending),
    WF +3.9/+10.7, bull +10.5 / bear +4.1 → **all-6 PASS**; 2× cost PF 1.81 **PASS**.
  - Plateau: 21/27 neighbors (be 0.8-1.2 × lc 2-5 × arm 0-0.5) pass. be0.8/arm0.25
    stronger net (+23.1% PF2.33) — be1.0 chosen for PF-margin robustness at arm0.
  - lc verdict (mimic drawdown-cancel gate): **near-inert** at 1.5-10% (BE-entry means the
    leg starts in profit; identical results) — keep lc=2% as free backstop.

### USTEC_4h_ZMR (84 parent trades)
- Broad pass: 189/540 tight + 351/540 wide grid points all-6 PASS.
- Optimum **be0.15 / arm1.0 / lc2(inert) / cap20 / gb8**: net **+27.2%**, **PF 3.61**,
  DD −2.9%, n=51, WF +12.2/+15.0, bull +7.4 / bear +19.8; 2× cost PF 3.42 PASS.
- Note: mean-reversion cell — passes despite the earlier trend-rider audit excluding
  mean-rev from *clip* books; the BE-entry mimic is a different mechanism (it monetizes
  the post-entry drift of winning MR trades).

## Verdict

**Mimic viable on BOTH live survivor cells — with a PROFIT-CONFIRMED (be≈1%) trigger on
the gold Donchian cell, not the 0.10% BE default (that fails 540/540).** Standalone,
additive, zero parent impact ([[CompanionDominanceError]] respected — never compared to
riding the parent).

**NOT WIRED** — test-only per the operator ask. Wiring plan if ordered: one-way
`on_trend_open`-style hook in `SurvivorPortfolio::Cell` open path + two
GoldTrendMimicLadder-pattern books (SurvDonchN20Mimic be1.0/arm0.25/lc2/cap30/gb10,
SurvZmrMimic be0.15/arm1.0/lc2/cap20/gb8), fed on the cells' native H4 bar stream,
shadow first. Intrabar re-check owed before LIVE sizing (same as GoldTrendMimicLadder).
