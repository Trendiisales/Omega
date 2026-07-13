# SURVIVOR-CELL MIMIC — INTRABAR RE-CHECK (S-2026-07-14, owed before LIVE sizing)

**Ask:** the two survivor-cell mimic books wired SHADOW in S-2026-07-14h (commit 280f59e7)
were validated CLOSE-GRADE only (`SURVIVOR_MIMIC_FINDINGS_2026-07-14.md`). Before LIVE
sizing the operator requires the intrabar re-check, because be/arm/giveback/loss-cut
triggers are intrabar-path-sensitive. Books (engine_init.hpp, deployed):

- **XAU_4h_DonchN20** be1.0 / arm0.25 / lc2 / cap30 / gb0.10 / rt 5bp / pend6
- **USTEC_4h_ZMR**   be0.15 / arm1.0 / lc2 / cap20 / gb0.08 / rt 3bp / pend6

Judged **STANDALONE / additive** throughout — never compared to the parent or WIDE
([[CompanionDominanceError]] respected). Gate = all-6 (net>0, PF≥1.3, both WF halves+,
bull+, bear+) + 2×-cost, identical conventions to the canonical overlay.

## VERDICT

| book | close-grade (study) | intrabar executable truth | verdict |
|---|---|---|---|
| **XAU_4h_DonchN20** | +14.6%/leg PF2.05 all-6+2x PASS | **+11.3% PF1.58 all-6+2x PASS** (REST/M1) | **HOLDS — degraded but viable, resting-order execution REQUIRED** |
| **USTEC_4h_ZMR** | +27.2% PF3.61 all-6+2x PASS | **+8.3% PF1.21, bull −7.9 → FAIL** (REST/M1) | **FAILS — the close-grade PASS was a granularity artifact** |

## 1. Why a re-check was structurally necessary (found while building it)

The close-grade validation (`mimic_ladder_overlay --cfg`) and the DEPLOYED engine
(`GoldTrendMimicLadder.hpp on_h1_bar`) are **not the same mechanism**:

| | validation (close-grade) | deployed engine (live) |
|---|---|---|
| BE-entry trigger | H4 **close** clears +be | H4 bar favourable **EXTREME** touches +be |
| BE-entry fill | at the clearing close | **at the be level** |
| trail/lc detection | close only | 3-pt walk **[adverse → favourable → close]** per H4 bar |
| trail/lc fill | at the detecting close | **at the stop level** (as booked) — but the wired exec path sends **market orders at the close event**, so real fills ≠ booked fills |
| leg window | truncated at parent exit | **full window** (no parent feedback; legs run to trail/lc/cap) |

So three distinct numbers exist per book: what the study showed, what the shadow ledger
will PRINT (level fills), and what real money would EARN. This is the registry §5
model-fill lesson again, on a new engine.

## 2. Method

Harness: `backtest/survivor_mimic_intrabar_bt.cpp`.
Parent trades = identical front-end to `clip_path_survivor.cpp` (REAL
`omega::survivor::Portfolio`, live S-2026-07-08c config, NDX-seeded asym bear long-veto).
**Parity asserted: 445 closed trades = the audited survivor_gated_bt n=445**, and the
CLOSE-M mode reproduces the study figures **to the decimal** on both books
(XAU +14.6/PF2.05/n60/cx301 WF+3.9/+10.7 bull+10.5 bear+4.1; USTEC +27.2/PF3.61/n51).

Execution models (each at matched window `-M` = study framing, and full window `-F` =
deployed reality; each at 1× and 2× cost):

- **CLOSE** — the study mechanism (parity reference).
- **LIVE** — the deployed engine's exact 3-pt h/l/c walk, level fills = what the SHADOW
  ledger will print.
- **MKT** — same detection, fills at the detecting H4 **close** (market order) = real
  money under the CURRENT exec wiring at flip.
- **REST** — resting orders: entry stop at the be level (touch fill, gap = worse-of open);
  trail/lc stops resting at levels that update only at H4 closes (the engine's cadence);
  intrabar touch fills at the level, gap-through fills at the open = real money if the
  flip is wired with resting orders. **The decision-grade model.**
- **FINE** — mimic managed continuously at fine-bar cadence (peak/stop update intrabar),
  gap-aware fills. Not the deployed cadence; bounds the path-sensitivity.

Fine granularities: H1 and M1, both symbols, full coverage (covered=361/361 and 84/84
parents at both grades). H1→M1 results converge (deltas ≤ a few % net, same PASS/FAIL
pattern) → M1 quoted as the honest bound.

## 3. Results — XAU_4h_DonchN20 (361 parents, 2022-2026, cost 5bp rt / 2× = 10bp)

| model | n | cx | net%/leg | PF | DD | WF H1/H2 | bull/bear | all-6 | 2×-cost |
|---|---|---|---|---|---|---|---|---|---|
| CLOSE-M (study) | 60 | 301 | +14.6 | 2.05 | −3.2 | +3.9/+10.7 | +10.5/+4.1 | PASS | +12.2 PF1.81 PASS |
| LIVE-F (shadow will print) | 98 | 263 | **+38.0** | 2.78 | −5.5 | +17.2/+20.9 | +27.8/+10.2 | PASS | PASS |
| MKT-F (current wiring, real fills) | 98 | 263 | +10.1 | 1.47 | −4.5 | **−1.8**/+11.9 | +7.2/+2.8 | **FAIL (WF-H1)** | +5.2 PF1.22 |
| REST-F/H1 | 98 | 263 | +14.0 | 1.71 | −4.8 | +7.8/+6.2 | +9.7/+4.3 | PASS | +9.1 PF1.43 PASS |
| **REST-F/M1** | 98 | 263 | **+11.3** | **1.58** | −5.3 | +6.6/+4.7 | +9.1/+2.2 | **PASS** | **+6.4 PF1.31 PASS** |
| FINE-F/M1 | 98 | 263 | +9.4 | 1.72 | −3.9 | +6.3/+3.1 | +5.3/+4.0 | PASS | +4.5 PF1.33 PASS |

Worst leg −2.05% (lc-bounded), worst MAE −2.18% (M1). Reasons at REST-F: 90 trail / 6 lc
/ 2 cap.

**Reading:** the gold book's edge is REAL at intrabar truth — every resting-order model
passes all-6 + 2×-cost at both granularities — but the honest magnitude is **~+11%/leg
over 4.3yr (PF≈1.6)**, i.e. ~25% below the close-grade study figure and **3–4× below
what the shadow ledger will print** (+38.0 level-fill). Two caveats:

1. **Market-at-close execution fails WF-H1 (−1.8).** The current exec wiring
   (`send_live_order` market order at the H4-close detection) gives away the level-fill
   edge. LIVE flip must place **resting orders** at the engine's levels.
2. Diagnostic probe (not a re-tune): the fine-grade shrink is largely **arm=0.25% arming
   on intrabar noise**; arm=0.5 doubles fine-grade net (+23 to +27, PF 2.3–2.5) at every
   gb. Parameter-local — the deployed config passes as-is; a re-calibration would need its
   own full validation before any change.

## 4. Results — USTEC_4h_ZMR (84 parents, cost 3bp rt / 2× = 6bp)

| model | n | cx | net% | PF | DD | WF H1/H2 | bull/bear | all-6 | 2×-cost |
|---|---|---|---|---|---|---|---|---|---|
| CLOSE-M (study) | 51 | 33 | +27.2 | 3.61 | −2.9 | +12.2/+15.0 | +7.4/+19.8 | PASS | +26.1 PF3.42 PASS |
| LIVE-F (shadow will print) | 69 | 15 | +20.2 | 1.50 | −4.5 | +11.7/+8.5 | +1.1/+19.1 | PASS | PASS |
| MKT-F (current wiring, real fills) | 69 | 15 | +12.0 | 1.33 | **−9.2** | +13.5/**−1.4** | **−10.6**/+22.6 | **FAIL** | +10.0 PF1.27 |
| REST-F/H1 | 69 | 15 | +9.1 | 1.23 | −6.0 | +4.9/+4.2 | **−7.1**/+16.2 | **FAIL** | +7.0 PF1.17 |
| **REST-F/M1** | 69 | 15 | **+8.3** | **1.21** | −6.0 | +4.1/+4.2 | **−7.9**/+16.2 | **FAIL** | +6.2 PF1.15 |
| FINE-F/M1 | 69 | 15 | +4.9 | 1.12 | −8.9 | +5.5/**−0.6** | **−7.1**/+12.0 | **FAIL** | +2.8 PF1.07 |

**Reading:** every executable model FAILS all-6 — the **bull-regime leg is decisively
negative (−7..−11%)** and 2×-cost PF ≤1.27 everywhere. The +27.2/PF3.61 close-grade PASS
was substantially a granularity artifact: at close grade 50/51 legs rode to the parent
exit (mo=50, only 1 trail exit) and banked the ZMR post-entry drift; at intrabar truth the
gb=8% trail keeps getting ratcheted by intrabar spikes and clipped in the 2023-25 bull
chop, and the full window adds 19 lc hits (−2%) on legs that outlive the parent.
Bear side stays strongly positive (+12..+23) — the edge that exists is bear-regime-only.

**Not parameter-local:** the fine-grade gb/arm probe (12 cells, gb 0.08–0.50 × arm 1/2)
fails all-6 in 11/12; the lone pass (gb0.50/arm2.0 +40.5 PF1.92, bull +2.0 marginal) is a
different mechanism (cap-exit dominant, 34/69) and would need its own full validation +
plateau — do not promote off one corner cell.

## 5. Which triggers moved, and why

1. **BE-entry (extreme-touch vs close-clear):** n booked 60→98 (XAU) / 51→69 (USTEC).
   The deployed engine enters on any H4 bar whose EXTREME touches +be — the extra ~38/18
   legs are touch-and-fade moves the close-grade study never opened. They enter at the be
   level and mostly trail out small; on gold they add net, on USTEC they add bull-chop
   losers.
2. **Giveback trail = the dominant degradation channel.** At fine resolution the peak
   ratchets on intrabar spikes, so the (1−gb)·peak stop rises earlier and exits fire
   smaller (XAU trail exits 11→90 of n; USTEC 1→44). Same phenomenon as the intrabar-
   giveback whipsaw documented in `MIMIC_REVERSAL_INTRADAY_FINDINGS.md` §3, milder here
   because the stop is level-filled, not close-filled.
3. **Loss-cut:** near-inert at matched window (confirms the study's "lc near-inert"
   verdict) but NOT inert at the deployed full window on USTEC: 19 lc exits on legs that
   outlive the parent; bounds the worst leg at −2.0..−2.6% and drives DD to −9. On gold,
   6-10 lc exits, worst leg −2.05%. The lc=2 backstop is doing real work — keep it.
4. **Level-fill vs market-fill:** the shadow ledger books level fills; its forward
   figures will OVERSTATE the executable book ~3–4× on gold. Read the SHADOW forward
   record with that caveat until the exec path is resting-order.

## 6. Data used + integrity gate (all gated; REJECTED = not used, no exceptions)

| file | role | gate |
|---|---|---|
| /Users/jo/Tick/XAUUSD_2022_2026.h4.csv | parent tape XAU | CERTIFIED CLEAN |
| /Users/jo/Tick/NSXUSD_2022_2026.h4.csv | parent tape USTEC | CERTIFIED CLEAN |
| /Users/jo/Tick/NDX_daily_2016_2026.csv | bear-veto seed | CERTIFIED CLEAN |
| /Users/jo/Tick/XAUUSD_2022_2026.h1.csv | fine H1 XAU | CERTIFIED CLEAN |
| /Users/jo/Tick/NSXUSD_2022_2026.h1.csv | fine H1 USTEC | CERTIFIED CLEAN |
| scratch NSXUSD_2022_2026.m1.csv | fine M1 USTEC | CERTIFIED CLEAN (2nd build) |
| scratch XAUUSD_2022_2026.m1.csv | fine M1 XAU | CERTIFIED CLEAN (2nd build) |

M1 build notes (both first builds REJECTED by the gate — fixed the data, no overrides):
- **NSX M1** from all 52 histdata tick months (306,217,912 ticks, same clock convention
  as the H1 tape). First build REJECTED: 762h hole = May-2025 — the month lives in a
  dir with a SPACE (`"...T202505 (2)"`) and the file list got word-split (the registry
  §9 zsh-trap, new variant). Rebuilt space-safe → CERTIFIED.
- **XAU M1** stitched from the 4 duka M1 segments (xau_m1_2022bear, xau_h2023_24_m1,
  xau_m1_2024_2026, xau_h2026mar_jun_m1). First build REJECTED: 94-day hole =
  2022-10..12 (+ missing Jan–May 2022 head). Filled from the /Users/jo/Tick/XAUUSD
  histdata tick zips with an EMPIRICALLY calibrated +5h clock shift (June-2022 overlap
  vs duka: mean |Δclose| = 0.0002 — same underlying feed) → CERTIFIED.
- Duka `usatechidxusd-tick-2022-01-01-2026-06-15*.csv` files are TRUNCATED (end 2022-03 /
  2022-05 despite the filename) — not used; noted so nobody trusts the filename again.

## 7. Recommendation (sizing itself = operator's decision)

1. **XAU_4h_DonchN20 mimic: eligible for LIVE sizing consideration** — but ONLY with
   **resting-order execution** (entry stop at the be level; trail/lc as resting stops
   updated at H4 closes). Size against the honest executable figure **+11.3%/leg,
   PF 1.58, DD −5.3%, worst leg −2.05% (REST-F/M1)** — NOT the close-grade +14.6 and NOT
   the shadow ledger's level-fill print (which will show ~3–4× more). If the flip keeps
   market-at-close orders, it is NOT ready (WF-H1 negative).
2. **USTEC_4h_ZMR mimic: NOT ready for LIVE sizing — intrabar FAIL.** Keep SHADOW only
   (with the overstatement caveat) or disable. The surviving structure is bear-only
   (+16% bear vs −8% bull at REST): a risk-off/bear regime gate is the one credible
   salvage path, but it must be validated as its own study before any wire change
   (companion regime-gate rule; do not dismiss, gate).
3. Follow-ups owed if pursued: (a) resting-order exec path for mimic legs at flip,
   (b) optional arm re-calibration study on gold (arm 0.5 doubles fine-grade net —
   plateau + full gate first), (c) USTEC bear-gate study.

*Harness: `backtest/survivor_mimic_intrabar_bt.cpp` (parity-locked to the study; CLOSE-M
reproduces it to the decimal). Runs: scratchpad `run_h1.txt` / `run_m1.txt` /
`run_m1_probe.txt`, granularities H1+M1 converge. S-2026-07-14, intrabar re-check owed by
S-2026-07-14h — now PAID.*
