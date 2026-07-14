# MGC 1h TrendFollow port — faithful backtest (S-2026-07-14ay)

**Task:** port XauTrendFollow1h to MGC futures cost basis + full certification gate.
Candidate #1 from the 2026-07-14 gold scoping: the 1h member PASSED the Phase-1b
spot 8bp kill tier (+$7,325 PF1.40 both halves + 2× — `outputs/GOLD_PHASE1B_2026-07-11.md`)
but only the 4h/2h were ever ported to MGC (S-2026-07-07w, registry §7,
vault `MgcTrendFollowPort`). **BACKTEST + REPORT ONLY — nothing wired, nothing deployed.**

Harness: `backtest/mgc_tf1h_port_bt.cpp` — drives the REAL
`include/XauTrendFollow1hEngine.hpp` (same conventions as
`backtest/XauTrendFollow1hBacktest.cpp` + the `MGC=1` cost mode of
`XauTrendFollow4h2hBacktest.cpp`). Two fidelity layers:
- **H1 layer**: H1 OHLC, SL-first intrabar l→h→c, cross-spread fills, entries at bar close.
- **M30/M1 feed-path layer** (`M30=1`): sub-hour rows drive `on_tick` l→h→c with H1
  bucket closes — the production MgcFastDonchianFeed poll shape (registry §7 layer-2).
  Parity: at LC=0 the layers agree **to the dollar** (exits are level-fills,
  order-independent); on the 2022 bear leg M1 = M30 = H1 exactly (n=45, −$4,526.1).

## Production config mirrored (engine_init.hpp S118 block, HEAD 904973e2)

`cell_enable_mask=0x0F` (EmaCross_20_80 sl4.0 / Donchian_N40 sl5.0 / Pullback_EMA20
pb0.5 / Keltner_EMA50 k2.0) · `lot=0.01` · `max_spread=1.0` · `min_impulse_atr=0.5` ·
`er_gate_min=0.40` `er_gate_n=20` · `LOSS_CUT_PCT=0.5` (swept vs 0 per the 2h-port trap) ·
`use_vol_target=true` `vol_target_unit=0.10` (VT=1) · `pyramid_max_adds=2` `step=1.0`
`sl=3.0`. BE_ARM/BE_BUFFER/vol-band/reg-slope: production defaults (off).
Deviations, both standard for this family's certifications: shadow_mode=true for the cb
path; gold_regime()/gold_wt()/macro gates fail-open standalone (live wiring keeps them).
**VT=0 variant** = fixed 1 contract per entry (pyramid adds +1 each): vol-target's
fractional-oz unit/ATR sizing has no MGC equivalent below 1 contract; the 4h/2h port
wired fixed 1 micro. In the 2025-26 high-price era vol-target clamps to min anyway,
so VT=1 ≈ VT=0 there.

## Cost model (certified, per 1 MGC = 10 oz = $10/pt)

RT = $2.08 commission (0.208 pt) + 0.10 pt spread (crossed mechanically via bid/ask)
+ 0.10 pt slip ≈ **0.41 pt RT**. 2× stress = 0.82 pt. Comm+slip debited per closed
record × contracts. All $ figures below are **per 1-MGC-per-unit**, 4-cell ensemble
book (peak exposure with pyramid = 12 contracts; VT=1 can size 1–8 per unit).

## Data (all integrity-gated, CERTIFIED CLEAN — no rejects used)

| file | role | coverage | gate |
|---|---|---|---|
| /Users/jo/Tick/mgc_30m_hist.csv | bull axis src (real MGC 30m) | 2024-06-03 → 2026-06-03 | CLEAN |
| /Users/jo/Tick/mgc_2024_2026.h1.csv | bull H1 (via mgc_30m_to_h1_h4.py, 11,817 bars) | same | CLEAN (src gated + script's inline gate) |
| /Users/jo/Tick/XAUUSD_2022_2023.h1.csv | bear axis (SPOT bars @ MGC cost — MGC has no 2022 history, registry §7) | 2022-01-02 → 2023-12-29 | CLEAN |
| /Users/jo/Tick/XAU2022_m30.csv + xau_m1_2022bear.csv | bear core-leg feed-path check | 2022-06-01 → 2022-09-30 | CLEAN |
| data/mgc_h1_hist.csv (nightly refresher) | splice tail for the 6-mo slice | 2026-04-14 → 2026-07-13 | CLEAN |
| scratch mgc_h1_spliced.csv | 6-mo-slice axis (30m-H1 + refresher tail; 818 overlap bars, max close diff 0.011%) | 2024-06-03 → 2026-07-13 | CLEAN |

## Gate table (headline = VT=0 fixed-contract, feed-path layer for bull, H1 layer for 2022-23)

**BULL axis — real MGC bars 2024-06-03 → 2026-06-03:**

| config | n | net $ | PF | WF halves | worst | maxDD | t/wk |
|---|---|---|---|---|---|---|---|
| LC=0.5 (prod) 1× | 399 | +74,056 | 1.59 | +21,068 / +52,988 ✓ | −3,898 | 48,450 | 3.8 |
| LC=0.5 2× | 399 | +71,945 | 1.56 | ✓ | −3,909 | 48,826 | 3.8 |
| **LC=0 1×** | **314** | **+139,868** | **2.30** | **+29,931 / +109,937 ✓** | **−2,641** | **40,624** | **3.0** |
| LC=0 2× | 314 | +137,427 | 2.27 | ✓ | −2,648 | 40,872 | 3.0 |
| (VT=1 prod-mirror: LC=0.5 +91,959 PF1.67 ✓; LC=0 +159,206 PF2.37 ✓, worst −2,641, DD 40,624) | | | | | | | |

Per-cell (LC=0 1×): Donchian +13,383 PF1.53 · EmaCross +33,508 PF2.14 · Keltner
+41,009 PF2.53 · Pullback +51,968 PF3.02 — **all four cells positive.**

**BEAR axis — spot 2022-23 H1 @ MGC costs:**

| config | n | net $ | PF | WF halves | worst | maxDD | t/wk |
|---|---|---|---|---|---|---|---|
| LC=0.5 (prod) 1× | 328 | −10,932 | 0.79 | −11,143 / +212 ✗ | −1,149 | 29,473 | 3.2 |
| LC=0.5 2× | 332 | −13,360 | 0.75 | ✗ | −1,159 | 31,488 | 3.2 |
| **LC=0 1×** | **285** | **+17,810** | **1.50** | **+6,245 / +11,565 ✓** | **−606** | **9,553** | **2.7** |
| LC=0 2× | 287 | +16,241 | 1.44 | ✓ | −613 | 9,929 | 2.8 |
| (VT=1: LC=0.5 −340 PF1.00 ✗; LC=0 +46,824 PF1.66 ✓ 2× +43,388 PF1.59 ✓) | | | | | | | |

Core bear leg Jun–Sep 2022 isolated (M1 = M30 = H1 exact parity): LC=0 n=45
**−$4,526 PF0.19** — the ungated long-only chassis bleeds in a straight-down leg,
exactly like the spot family and the 4h/2h port (phase1b: "bear protection lives in
the live gates" — gold_regime/macro veto, fail-open here). The full 2022-23 axis
passes because the 2022 rallies + 2023 recovery pay for the leg.

## LOSS_CUT sweep — the 2h-port trap re-confirmed (3rd family member)

Spot `LOSS_CUT_PCT=0.5` (≈22 pt at 4400, inside the 4–5×ATR stops) costs ~47% of
bull net (+139.9k → +74.1k), flips the 2022-23 axis **negative with halves-FAIL**
(+17.8k → −10.9k) and the 6-mo slice negative on the H1 layer (−8,970 PF0.86).
LC=0 (engine ATR stop as sole protection) dominates everywhere that matters.
**LC=0 is the keeper — identical to the wired MGC 2h verdict (`MgcTrendFollowPort`).**

## 6-month slice (2026-01-14 → 2026-07-14 — the volatile top+crash window)

Spliced certified H1 axis (data to 2026-07-13):
- **LC=0: n=76, +$29,123, PF 1.54, worst −$2,641** (slice-internal maxDD large —
  the window contains the 4900→4000 crash; book DD $45,340 incl. pre-slice peak).
- LC=0.5 (prod): n=114, **−$8,970, PF 0.86** — negative in exactly the tape the
  operator is watching.
- M30 feed-path layer (data to Jun-03 only): LC=0 +$33,839 PF1.69; LC=0.5 +$1,387 PF1.03.

## Adverse-protection verdict (mandatory)

Keeper (LC=0, fixed 1 contract): protection = the engine's per-cell 4–5×ATR stops +
pyramid trail (3×ATR) + Donchian low exit. Worst closed record **−$2,641** (record
may include 2 pyramid adds = up to 3 MGC), book maxDD **$40,624** (bull axis,
4-cell ensemble, peak 12 MGC ≈ $530k notional at 4400 — operator sizes).
Fill caveat: engine SL books AT the stop level (live is tick-dense; on bar replay
this is optimistic on gap-through bars). Bound from the honest-fill LC runs (LOSS_CUT
exits at bid): worst observed honest fill −$3,898 on the 2026 crash bar. LC=0.5 is
NOT the answer to that gap risk — it pays ~$66k of bull edge and still eats the same
crash bar (its own worst is the −$3,898).

## Verdict

**PASS — wire-eligible pending operator — at LC=0, fixed 1 MGC per unit (VT=0),
all other production params unchanged.** All gates green at 1× and 2× cost, WF both
halves on both axes, all four cells positive, 3.0 trades/wk (≈ doubles the MGC
family's turnover next to the 4h/2h book), 6-mo slice +$29.1k PF1.54.
Production `LOSS_CUT_PCT=0.5` must NOT travel with the port (bear axis + slice FAIL);
this is now a 3-for-3 family trap (2h parity, phase1b 2h spot, this).
Honest negatives: the isolated Jun–Sep 2022 bear leg bleeds −$4.5k/4mo ungated —
the port inherits the family's dependence on the live regime gates; and ~all of the
bull-axis edge magnitude rides the 2024-26 gold bull (long-only chassis; same basis
the 4h/2h port was certified and wired on, and this port's 2022-23 axis is the only
one in the family that passes WF both halves).

Runs reproducible: build line in the harness header; env matrix
`VT×LC×COSTX×M30×SLICE_START/END` as documented there. 2026-07-14.
