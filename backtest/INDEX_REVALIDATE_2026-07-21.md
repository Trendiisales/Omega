# INDEX ENGINE FAITHFUL REVALIDATION — 2026-07-21

Faithful revalidation of the **LIVE** index engines: DJ30 + SPX D1 turtles and the
IndexBearShort NAS/SPX cells. Class-driven (real shipped engine classes, no ports),
integrity-gated data, real IBKR index cost. **Findings only — nothing wired/committed.**

## Method / provenance
- **Turtles** — `omega::NasTurtleD1Engine` via `backtest/index_turtle_d1_audit.cpp`
  (clang++ -O3). Data `/Users/jo/Tick/{DJ30,SPX}_daily_2016_2026.csv` (2016-01..2026-07,
  2,650 D1 bars each) — **integrity gate CERTIFIED CLEAN**. Live cfg replicated:
  `sl_atr_mult=2.0`, `tp_atr_mult=5.0`, `lookback=20`, `hold=20`, `ema100=off`,
  `acct_guard=off`, `ride_exit=false` (DJ30/SPX do NOT set ride_exit — only live NAS does).
  Cost = crossed spread (entry ask / exit bid); baseline half-spread 0.5.
- **Bear-short** — `omega::IndexBearShortEngine` via `backtest/ibs_real_engine.cpp`
  (--h1 mode, 4 ticks/bar). Data `/Users/jo/Tick/{NSXUSD,SPXUSD}_2022_2026.h1.csv`
  (2022-01..2026-04, ~24.4k H1 bars each) — **integrity gate CERTIFIED CLEAN**.
  Live cfg: NAS `DON=24 COST_PTS=2.0`; SPX `DON=48 COST_PTS=0.6`; both real-money LIVE
  (`shadow_mode=false` since S-2026-07-01).

## BASELINE REPRODUCED (gate before any variant) ✓
| cell | certified 2026-07-21 | reproduced | match |
|---|---|---|---|
| DJ30 turtle | PF 1.81 | **PF 1.82** (net +896, n79, WR65%) | ✓ |
| SPX turtle  | PF 1.77 | **PF 1.76** (net +1325, n88, WR58%) | ✓ |
| NAS bear-short 2022 | +1061pt | **+1204pt** (n34) | ✓ (data now to 2026-04) |
| SPX bear-short 2022 | +591pt PF1.59 | **+591pt** (n30) | ✓ exact |

## PER-CELL TABLE (faithful)
| cell | net | PF | worst | maxDD | 2022 bear | WF H1/H2 | 2x-cost | verdict |
|---|---|---|---|---|---|---|---|---|
| **DJ30 turtle** (D1, LIVE) | +896 | **1.82** | -67 | -168 | +38 / n5 **+** | +476/+420 **both+** | PF **1.81** hold (hs2.0) | **KEEP-LIVE** |
| **SPX turtle** (D1, LIVE) | +1325 | **1.76** | -101 | -267 | +41 / n5 **+** | +512/+813 **both+** | PF **1.77** hold (hs2.0) | **KEEP-LIVE** |
| **NAS bear-short** (H1 DON24, LIVE) | +2139pt | 1.15 | ~-362 | — | **+1204 / n34 +** | +703/+1435 **both+** | PF **1.14** both+ (cost4.0) | **KEEP-LIVE** (monitor) |
| **SPX bear-short** (H1 DON48, LIVE) | +424pt | 1.16 | ~-208 | — | **+591 / n30 +** | +601/**−177** ✗ | PF1.14 H2 **−200** ✗ | **SHADOW-ONLY** (demote) |

(turtle net/worst/DD in per-0.01-lot USD scale; bear-short in index points.)

### Bear-short regime slices (net pt / n)
- **NAS DON24:** 2022 bear **+1204/34** · 2023 −67/13 · 2024 **+1576/14** · 2025 **−978/20** ·
  2026 +404/11. 2024-26 bull combined **+1002** (cert +514). Cross-cycle net+, both WF halves+,
  2022 bear strongly+. 2025 was a clear losing year (bull-correction leak — the known residual
  the header warns about) but does not sink the cross-cycle edge.
- **SPX DON48:** 2022 bear **+591/30 (PF~1.59, matches cert exactly)** · 2023 +12/12 ·
  2024 +29/11 · 2025 **−367/14** · 2026 +157/9. **2024-26 bull combined −181 (NEGATIVE).**
  Full-history **WF H2 negative (−177, PF0.88)** → fails both-halves. 2x-cost worsens H2 to −200.
  Diagnostic: applying the NAS bull-bleed fix (DON48→24) to SPX makes it **worse**
  (net −36, H2 −493) — the SPX bull bleed is NOT DON-fixable. The engine's own comment admitted
  "SPX bull behaviour UNTESTED"; now tested = it bleeds. Real 2022-bear edge is intact and the
  bull bleed is small/near-flat (not catastrophic) → demote to SHADOW, do not scrap.

## PROTECTION VERDICTS
Operator rule: turtle/trend needs trail room — certify a profit-lock per cell (giveback stop
= entry+(1−gb)·(peak−entry)). The shipped engine exposes a BE-ratchet lever (`BE_ARM_PCT`/
`BE_BUFFER_PCT`); the giveback-fraction lock is a strictly-tighter member of the same "give back
less" family. Fresh faithful frontier (arm∈{2,3,5}%, buf 0.5%):

| cell | baseline PF | arm2 | arm3 | arm5 | maxDD effect | verdict |
|---|---|---|---|---|---|---|
| DJ30 turtle | **1.82** | 1.71 | 1.70 | 1.76 | none/worse (arm2 −322) | **LOCK-HURTS-EDGE** |
| SPX turtle  | **1.76** | 1.51 | 1.70 | 1.72 | none | **LOCK-HURTS-EDGE** |

Every lock level LOWERS PF vs baseline and never cuts maxDD — it trims the fat-tail winners the
turtle edge depends on. Consistent with the header's documented-negative BE-arm audit and the
2026-06-17 swing-protection sweep (tightening hurts trend/trail). **Keep the structural
Donchian/channel + timeout exit; do not add a profit-lock.** A giveback-fraction lock would trim
winners even harder → same direction. (Uniform giveback killed 5/7 books; per-cell result here is
LOCK-HURTS for both turtles.)

- **NAS/SPX bear-short protection:** structural Donchian swing-high SL + fixed 2R TP is the design
  (a trail gives it back on the bear counter-rally: PF0.87 vs fixed-TP1.60 — validated). Adequate;
  no cold long-side loss-cut applies (short-only bear engine). Keep as-is.

## VERDICT SUMMARY
1. **DJ30 turtle — KEEP-LIVE.** PF1.82 (cert✓), both WF+, 2022 bear+, 2x-cost PF1.81. Keep channel exit.
2. **SPX turtle — KEEP-LIVE.** PF1.76 (cert✓), both WF+, 2022 bear+, 2x-cost PF1.77. Keep channel exit.
3. **NAS bear-short — KEEP-LIVE (marginal, monitor).** Cross-cycle +2139pt PF1.15, 2022 bear +1204,
   both WF halves+, 2x-cost holds. Watch the 2025-style bull-correction bleed (−978pt that year).
4. **SPX bear-short — SHADOW-ONLY (demote from LIVE).** Real 2022-bear edge (+591 PF1.59, matches
   cert) but full-history WF **H2 negative** and 2024-26 bull era **−181pt**; DON24 fix worsens it.
   It is currently real-money LIVE and fails the both-halves gate → recommend `shadow_mode=true`
   until a bull-regime fix exists. Not a full scrap (bear edge is real, bull bleed is small).

*No SCRAP verdicts. No wiring/commit performed.*
