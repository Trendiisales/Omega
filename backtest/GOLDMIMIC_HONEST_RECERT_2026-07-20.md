# GoldTrendMimicLadder HONEST-LEDGER RE-CERT — 2026-07-20

**Order:** S-2026-07-20 operator honest-ledger audit (crypto S-20z parity; Omega booking fix
shipped 58a478e2). Re-certify ALL 13 wired GoldTrendMimicLadder books by driving the REAL
`include/GoldTrendMimicLadder.hpp` header (honest booking already in it) over each book's REAL
parent entry stream, at 1x and 2x cost.

**Harness:** `backtest/goldmimic_honest_recert_bt.cpp` — parity COPY of
`clip_path_noprebe_floor.cpp` with the drifts fixed: per-cell `pend_bars`
(engine_init parity: 12 for the 5 new books, 4 XauTfD1, 5 turtles, 6 elsewhere), retired
XauTf2h/MgcFastDon cells dropped, real entry streams for 12/13 books (Donch-20 recon =
XAU_4h_DonchN20's real trigger), 1x/2x cost from one clip stream (rt never alters exits).
Raw outputs: `backtest/goldmimic_honest_2026-07-20/` (report, integrity log, entry streams,
producer logs, derived bars).

**Data:** every file `data_integrity_gate.py`-gated before use. One REJECT:
`/Users/jo/Tick/2yr_XAUUSD_daily.csv` (1 backward ts, 154h jump — not chronological). NOT
used; D1 derived fresh from the CERTIFIED `2yr_XAUUSD_tick_fresh.h1.csv` and re-gated CLEAN
(`goldmimic_honest_2026-07-20/xau_d1_from_h1.csv`). All other sources CERTIFIED CLEAN
(2yr XAU h4/h1/m30, mgc_30m_spliced + 2024-26 h4/h1, NDX/SPX/DJ30 dailies, xau 1m splice,
3 derived files).

**Entry-stream parity:** MgcTF4h producer reproduced registry refs (REGIME=1 n=303 +$4,348
PF1.49 vs REGIME=0 ref n291 +$4,209 PF1.50). XauTf4h 354 / XauTfD1 116 trades from the real
TF engines; turtles 87/88/79 from the real NasTurtleD1 (sl_atr 2.0 live parity); the 5 new
books + GoldDon10m from the certified DUMP_ENTRIES producers (MgcTf1h trigger px re-derived
bar-close+spread per the S-14bb convention).

## VERDICT: 12 of 13 books COLLAPSED under honest booking. 1 MARGINAL (DJ30Turtle). 0 SURVIVE.

Per-book, pooled legs, native-grain, 1x cost (2x in brackets). net in %-points summed over
clips; worst = worst NET clip; nNeg = clips net<0. nNeg=0 appears NOWHERE (the old clamp
tautology tell is gone — every book now shows its real tail).

| book (grain) | ent | n | net 1x [2x] | PF 1x [2x] | WF H1/H2 | bull / bear | worst | nNeg | old cert | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| XauTf4h (H4) | 354 | 1260 | −455.1 [−644.1] | 0.28 [0.17] | −222/−233 | −369 / −87 | −2.84 | 932 | +142 ALL6 | **COLLAPSED** |
| XauTfD1 (D1) | 116 | 206 | −154.0 [−184.9] | 0.33 [0.26] | −42/−112 | −154 / n0 | −10.85 | 137 | +118 ALL6 | **COLLAPSED** |
| MgcTF4h (H4, rt5) | 303 | 470 | −22.6 [−46.1] | 0.86 [0.73] | −16/−7 | −30 / +7 | −3.78 | 283 | +179.8 PF14.6 | **COLLAPSED** (PF14.6 was the impossible-PF tell) |
| NAS100Turtle (D1) | 87 | 158 | −16.0 [−22.3] | 0.83 [0.77] | −24/+8 | −11 / −5 | −4.02 | 89 | +160 T+W | **COLLAPSED** |
| US500Turtle (D1) | 88 | 164 | −23.9 [−30.5] | 0.66 [0.60] | −10/−14 | −20 / −4 | −2.05 | 110 | +93 T+W | **COLLAPSED** |
| DJ30Turtle (D1) | 79 | 144 | **+15.6 [+9.8]** | 1.34 [1.20] | +9/+6 | +21 / −5(n14) | −2.10 | 76 | +120 T+W | **MARGINAL** — only positive book; fails bear split + PF@2x |
| XAU_4h_DonchN20 (H4) **LIVE 1 MGC** | 332 | 123 | −42.1 [−48.2] | 0.31 [0.27] | −24/−18 | −42 / n0 | −3.77 | 98 | +23 floored / +13.9%/leg PF2.79 live basis | **COLLAPSED** |
| XAU_4h_DonchN20 @H1 x4-scaled (grain-fairness for the resting book) | 332 | 121 | −26.0 [−32.0] | 0.31 [0.25] | −17/−9 | −26 / n0 | −2.10 | 97 | — | **COLLAPSED at fine grain too** |
| MgcTf1h (H1) | 399 | 664 | −159.6 [−192.8] | 0.19 [0.14] | −72/−88 | −110 / −50 | −1.79 | 583 | +76 T+W | **COLLAPSED** |
| GoldKeltM30 (M30) | 1111 | 1862 | −320.8 [−413.9] | 0.26 [0.19] | −135/−186 | −173 / −148 | −2.46 | 1522 | +166 T+W | **COLLAPSED** |
| GoldTfBw1040 (H1) | 889 | 1564 | −274.9 [−353.1] | 0.33 [0.25] | −116/−159 | −152 / −123 | −2.23 | 1233 | +113 T+W | **COLLAPSED** |
| GoldTfBw20100 (H1) | 742 | 1322 | −285.1 [−351.2] | 0.25 [0.19] | −110/−176 | −164 / −121 | −7.55 | 1070 | +88 T+W | **COLLAPSED** |
| GoldDonH1 (H1) | 360 | 614 | −131.5 [−162.2] | 0.30 [0.24] | −52/−80 | −94 / −38 | −3.83 | 508 | +51 T+W | **COLLAPSED** |
| GoldDon10m (10m) **LIVE 2 MGC** | 1359 | 1860 | −226.6 [−319.6] | 0.26 [0.19] | −108/−119 | −128 / −99 | −1.98 | 1778 | T+W +$26.5k 6mo (pre-floor design) | **COLLAPSED** |
| GoldDon10m @1m TRUTH x10-scaled | 1359 | 1882 | −170.1 [−264.2] | 0.11 [0.07] | −76/−94 | −82 / −88 | −0.90 | 1860 | — | **COLLAPSED at 1m truth** (6mo: T −$10.9k / W −$11.2k vs old +$14.0k/+$12.5k) |

## Why (mechanism, verified — not a rig artifact)

1. **The collapse is exactly the S-20z booking-honesty channel.** Cross-check: re-clamping
   ONLY the BE_FLOOR clips of the SAME honest clip streams back to gross 0.0 (the old S-17c
   convention) flips every book back to the old-cert ballpark:
   XauTf4h −455→+38, XauTfD1 −154→+48, MgcTF4h −23→+116, DJ30 +16→+58, MgcTf1h −160→+6,
   KeltM30 −321→+39, Bw1040 −275→+70, Bw20100 −285→+40, GoldDonH1 −132→+32,
   DonchN20 −42→+13, GoldDon10m −227→−8. Remaining gap to the old figures = the second
   inflation channel (TRAIL_STOP booked at the resting-stop LEVEL vs the honest pierce
   extreme) + regenerated entry streams. The harness mechanism is faithful; the old nets
   lived in the clamp.
2. **Structural root cause:** with `no_prebe_loss` exit timing (exit at first ret<=0 touch)
   the books floor out 74–99% of entered legs (reason split in the raw report — e.g.
   GoldDon10m 1778/1860 clips are BE_FLOOR). Under the clamp each of those churns booked
   −cost only; under honest booking each books its real pierce (bar-grain adverse extreme;
   at 1m truth ~−0.10% avg — still negative) PLUS cost. arm_pct is far above be_entry_pct,
   so the trail rarely arms; the few TRAIL winners (+35..+173 per book) nowhere near cover
   the floored churn (−45..−628 per book). The S-17c claim "floored-on-open IMPROVED net on
   nearly every cell" was an artifact of the 0.0-clamp booking — same shape as the crypto
   S-20 re-cert (369/380 FAIL).
3. **Grain fairness for the two LIVE books was tested and does not save them.** The resting
   tick path books smaller pierces than bar extremes, but the fine-grain variants
   (DonchN20 @H1 x4, GoldDon10m @1m x10) still collapse: the bleed is churn-count x
   (pierce+cost), not pierce depth.

## Caveats / honesty notes

- Bar-grain worst-of pierce booking IS production booking for the bar-fed shadow books
  (registry `on_bar` → `on_h1_bar` adverse-first). For the two resting live books the
  production tick path sits between the bar-grain and fine-grain rows; both bound negative.
- MgcTF4h calendar-2022 bear axis not re-run (no 2022 MGC data); moot — the recent axis
  already fails at 1x.
- Old-cert baselines quoted from engine_init.hpp comments / handoff-20s appendix; the
  "T+W" pooled figures sum the per-leg old-cert nets.
- DJ30Turtle is net-positive both costs with WF both halves + (PF1.34/1.20) but fails the
  bear split (−5.4 on n14) and PF>=1.3 at 2x — MARGINAL, not a pass; also its family
  siblings (NAS100/US500, same chassis) both fail, which argues fluke over edge.
- Judged STANDALONE per the companion-independent-engine rule; no comparison to any parent
  WIDE ride anywhere in this study.

## Operator decision points (no action taken — audit only, no commits/deploys/config edits)

- **XAU_4h_DonchN20 (LIVE 1 MGC) and GoldDon10m (LIVE 2 MGC) fail the honest re-cert at
  every tested grain** — the operator's call on keeping them live; their forward honest
  ledger (post-58a478e2) is now the arbiter and per this study should trend negative.
- The 10 shadow books have no honest-basis support; retire-vs-redesign is an operator call.
  Any redesign must confront the churn structure (be_entry within noise distance of the
  trigger + far arm), not the booking.

*Session S-2026-07-20 resume agent. Harness `backtest/goldmimic_honest_recert_bt.cpp`; raw
tee `backtest/goldmimic_honest_2026-07-20/honest_recert_report.txt`; integrity log
`.../integrity_gate.log`; entry streams `.../entries/`; derived+gated bars in the same dir.
No include/*.hpp or engine_init.hpp edits; nothing committed.*
