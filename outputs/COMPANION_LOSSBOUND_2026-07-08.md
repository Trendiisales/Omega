# COMPANION LOSS-BOUND STUDY — 2026-07-08

**Operator question** (after the 2× XauTrendFollow4h companion `LOSS_CUT_CLIP` at **−$50.57**
each — LONG @4176.78, mfe $24 → never armed → rode the 60pt/30min drop into the −50 floor):
*can companion-book losses be bounded MORE aggressively without killing the edge?*

**Answer: yes — but only regime-conditionally.** Flat tightening of `cold_loss_omega` fails at
every level; the velocity-freeze idea is a measured no-op; a **bear-only tighter cut
(`cold_loss_bear = −35`, active only while gold is price-bear) keeps 96% of net and is wired**
on the 4h + 1h USD books.

---

## Method (faithful, anchored)

- Parent trades from the REAL engine classes via `backtest/clip_path_xau_tf.cpp`
  (existing harness). **Anchor reproduced first**: on the 2yr file the original
  `dollar_trail_companion.py` sweep reproduced handoff 2026-07-03m **byte-for-byte**
  (354 trades; arm15/trail15 net $2829.3 PF 1.51 MAR 4.91; arm30/trail15 net $3003 MAR 5.04).
- Study data: **certified** `/Users/jo/Tick/XAUUSD_2022_2026.h1.csv` + `.h4.csv`
  (2022-01-02 → 2026-04-24; `data_integrity_gate.py` CERTIFIED CLEAN both).
- New harness `backtest/companion_lossbound_sweep.py` = live-faithful `StallBook` USD-mode
  replay (arm/trail/RETRIG/cold/bull-gate exactly as `include/StallCompanion.hpp`), evaluated
  per H1 bar **intrabar adverse-first**, clip exits booked at the crossed level −$1 poll slip
  (today's live cut booked −50.57 vs the −50 floor).
- Two model-integrity fixes applied before trusting anything:
  1. **Dedupe on live keying** — StallBook keys by `book|eng|sym|entry`, so same-entry engine
     cells collapse to ONE companion (697 → 523 4h parents).
  2. **Economic accounting** — the live ledger banks parent-entry-anchored upnl on EVERY retrig
     cycle (overlapping price segments count multiple times; it is how the banked CSVs read, but
     it is not a per-contract PnL). Verdicts below use **econ** = per-cycle contract PnL (cycle
     enters at its retrig crossing); **ledger** (what the desk banks) shown alongside.
- Tombstone pre-check: no prior kill on cold-level sweep / velocity freeze / bear-conditional
  cut (`second-brain` + `TOMBSTONES.tsv`). Relevant prior: [[CompanionColdCutFloor]] — the −50
  floor was an operator risk choice over a trail-only BT verdict, never itself swept.
- All-6 bar: net>0, PF≥1.3, both WF halves>0, 2022-window≥0, ex-best>0, 2×-cost>0. Judged
  STANDALONE (never vs WIDE).

Baselines (live configs, cold −50): **4h a/b** (arm30/trail15/retrig15): econ net **$7,770**
PF 3.09, ledger $30,363, worst leg −71, 54 loss-cuts. **1h a/b** (arm15/trail30/retrig30):
econ $5,031 PF 2.68, ledger $15,828, worst −54. **2h a/b** (arm40/trail10, bull-gated): econ
$1,720 PF 1.15 — fails the PF bar at EVERY cell incl. its live baseline (pre-existing
standalone-viability flag on the 2h book, not caused by any lever here).

---

## Lever 1 — FLAT cold-loss sweep: **REJECTED**

XAU_TF4H (523 parents, econ | ledger):

| cold | n | econ net$ | %of −50 | ledger$ | PF | worst | 2022$ | 2×C$ | nLC | all-6 |
|------|-----|------|-----|-------|------|-----|-----|------|-----|----|
| none | 721 | 11,917 | 153% | 37,132 | 3.80 | −368 | 1,108 | 11,062 | 0 | P |
| **−50 (live)** | 695 | **7,770** | 100% | 30,363 | 3.09 | −71 | 1,108 | 6,959 | 54 | P |
| −35 | 674 | 5,113 | 66% | 25,099 | 2.42 | −58 | 882 | 4,338 | 110 | P |
| −25 | 620 | 3,010 | 39% | 8,803 | 1.95 | −51 | 640 | 2,319 | 179 | P |
| −20 | 599 | 1,815 | 23% | 5,304 | 1.64 | −43 | 458 | 1,150 | 244 | P |
| −15 | 595 | 559 | 7% | 2,741 | 1.21 | −40 | 236 | **−99** | 329 | **FAIL** |

Every tightening step multiplies the cut count (54→329) by converting dip-then-recover winners
into realized losses — exactly the "eats winners-that-dipped" failure. The wire bar (keep ≥90%
of net) is missed at **every** level; −15 fails all-6 outright (2×-cost negative). 1h transfers
the same shape (−35 keeps 95%, −25 86%, −20 68%). 2h: worse at every tighter level.
Note the tail: the true worst legs are **weekend/session gaps** (ledger worst −191 on the
2026-01-28 gap, entry 5513) — a cut executes AFTER the gap, so NO cold level bounds them.

## Lever 2 — Velocity freeze (block new opens/re-opens while last closed H1 bar ≤ −$X): **REJECTED (no-op)**

4h: X ∈ {10…60} moves econ net by ±0.6% (7,759–7,818 vs 7,770), worst unchanged −71, nLC 53-54
vs 54. 1h: identical no-op. Why: companions open when the PARENT enters, and the trend-follow
parents essentially never enter fresh gold longs inside a falling H1 bar — the arming-into-a-knife
class this targets barely exists. Today's −51s were companions opened EARLIER whose parent bled;
a freeze on new arms cannot touch that class. (2h only: X=10 adds ~+28% econ, but the 2h book
fails the PF bar regardless.) Consistent with `DDVEL_THRESHOLD_STUDY_2026-07-08` — velocity
gates on this system price out positively-skewed entries, not tails.

## Lever 3 — REGIME-CONDITIONAL cold (tighter only while gold price-bear): **PASS → WIRED at −35**

Price-bear = `RegimeState` PRICE core (H1 EMA200 falling w/ persist100 + EMA50<EMA200 +
c<EMA200; 28.4% of 2022-2026 H1 bars). Deliberately **NOT** `long_blocked()`: the macro-hostile
overlay is the trend engines' BEST cohort (GOLD_DEEP_DIVE_2026-07-08) and is not historically
replayable; the price core is, and the deep-dive counter-finding therefore does NOT transfer
against this lever — the bear-bucket companion cycles contribute only ~9% of econ net, so
tightening there is near-free. The data decided.

XAU_TF4H, `cold = −X while price-bear, −50 otherwise`:

| bearcold | econ net$ | %of −50 | ledger$ | %ledger | PF | worst | 2022$ | 2×C$ | nLC | all-6 |
|----------|------|-----|--------|-----|------|-----|-----|------|-----|----|
| **−35 (wired)** | **7,433** | **96%** | 29,910 | 98.5% | 3.03 | **−52** | 1,044 | 6,623 | 64 | **P** |
| −25 | 7,068 | 91% | 19,860 | 65% | 2.99 | −52 | 1,014 | 6,292 | 75 | P |
| −20 | 6,727 | 87% | 18,379 | 61% | 2.91 | −52 | 977 | 5,958 | 89 | P |
| −15 | 6,525 | 84% | 16,932 | 56% | 2.89 | −52 | 928 | 5,762 | 121 | P |

- Worst econ leg **−71 → −52** (the −71 class = retrig cycles re-opened at high parent-anchored
  fav then cut at the parent-anchored −50; bear-cold bounds them earlier).
- Smooth monotone plateau −35→−15, all pass all-6 — a plateau, not a spike.
- 1h transfer: −35 keeps 96% ($4,808/5,031), all-6 PASS; −25 keeps 94%. 2h: no measurable
  benefit (bull-gate already keeps it out of bears) — left unwired.
- −25 is the more aggressive alternative (still 91% econ) but costs 35% of LEDGER net (bear
  retrig cycles killed before they re-bank); −35 is near-free on BOTH accountings. Operator can
  dial `cold_loss_bear` to −25 later with this table as the price list.

## Lever 4 — Combos: dominated

Flat-cold × freeze combos inherit lever 1's damage (best combo −25/frz15: 40% of net). Bear-cold
needs no freeze (freeze is a no-op). No combo beats bear-cold −35 alone.

---

## What was wired (committed locally, NOT pushed/deployed)

- `include/StallCompanion.hpp`: `StallBook::Config.cold_loss_bear` (0=disabled; <0 = tighter
  LOSS_CUT for GOLD legs while price-bear). Guarded: can never LOOSEN the baseline floor;
  unknown regime (−1) ⇒ baseline (fail-safe). `step()`/`maybe_drive()` carry the regime flag
  (`maybe_drive` param defaulted −1 → all other callers unchanged).
- `include/on_tick.hpp`: drive site passes `gold_regime().warm() ? is_bear() : −1`.
- `include/engine_init.hpp`: `cold_loss_bear=-35` on **xau_tf4h_usd_a/b + xau_tf1h_usd_a/b**
  only. 2h/d1/gvb/index/% books untouched (default 0 = exact prior behavior).
- Unit test `backtest/stallbook_bearcold_test.cpp` — ALL PASS (bear cut at −35 books observed
  upnl; non-bear/unknown/disabled/non-gold stay at −50; trail path untouched).
- Harness `backtest/companion_lossbound_sweep.py` committed for re-runs.
- OmegaBacktest build green; `mac_canary_engines.sh` green (0 violations).

**Today's event under the wire:** IF gold was price-bear at the time, each −$51 clip becomes
≈ −$36 (and, at the operator-dialable −25 setting, ≈ −$26). If the regime was NEUTRAL/BULL the
cut stays −50 by design — that is the finding, not a gap: unconditional tightening measurably
destroys the books' edge.

## Honest flags

1. **Gap tail is unfixable by any cut level** — worst ledger legs are opening gaps (−191 class);
   they book at market past every floor. Only non-participation (retirement watermark, already
   live at −$300) bounds those.
2. **Ledger vs econ accounting**: the banked CSVs re-bank parent-anchored upnl each retrig
   cycle (overlap). Standalone viability here is judged on econ per-cycle contracts; by that
   measure 4h (PF 3.09) and 1h (PF 2.68) are healthy, but the **2h USD book is econ-marginal
   (PF 1.15) at its LIVE config** on 2022-2026 — pre-existing, surfaced for a future session.
3. Replay is H1-bar granular vs live 60s polls; adverse-first ordering + $1 slip keep it
   conservative. Regime replayed is the price core only (macro-hostile feed has no history).
