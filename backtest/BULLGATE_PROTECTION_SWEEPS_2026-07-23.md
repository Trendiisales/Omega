# BULL-GATE × PROTECTION SWEEPS — S-2026-07-23 (operator order: gate to bull, min DD without killing edge, all families)

Three parallel sweeps, all harnesses RE-RUN on integrity-certified data, honest fills.
Full agent outputs in scratchpad (`sweep3pct/5pct/refine_both/finalists`, `sweep_bigcap_ladder/ladder_h1/hi52`,
`nas_ladder_sweep2/3` + results json/csv). Nothing wired or deployed from these — build queue below.

## A. rdagent 3%/5% family (252 + ~60 cells)

CERTIFIED (all-6, traded-2022, ex-best PASS):
- **3% multi-day**: `same_close h3 + VS(hold→1 when SPY 20d-vol>median) + gate SPY>200DMA AND vol<20%` →
  n=7878 (~656/yr), PF 1.29, mDD 545 (units: summed per-trade %), MAR 8.3, 2022 traded n=148 +23.5% PF 1.10,
  WF+/+, 2× +3749, ex-RGTI PASS. Caveat: 2022 pass trades ~9% of bear signals, Jan-heavy — gate earns keep by refusal.
- **5% DayMover WIDE**: `threshold 7%, no gate` → n=328 (~46/yr), PF 3.27, mDD 270, MAR 12.7, 2022 +103% PF 1.52,
  ex-WDC PASS. Alt: `6% + rev3d-12 cut` (n=485, PF 3.16, 2022 +81.5%) for 2× flow. $10k book ≈ +$5.3k/7yr, mDD ≈ $421.
KEY: entry threshold IS the bear protection (2022 monotone g3 −992 → g8 +164). Bull-gates make 2022 WORSE
(gate-lag residual trades all losers). Hard init-stops flip 2022 negative; BE-and-ride floor kills 2022.
NO-SHIP: 3% 1-day-trail (thin + fails ex-best/IONQ); GB90 companion (no combo survives traded 2022; bull-only
standalone PF 3.03 available only under operator bear-waiver).

## B. Purged-panel engines

- **STOCK MOVERS bigcap ladder** (engine culled S-16l by operator): DD-min cert cell
  `THR3% next-close, TIGHT a0.5/s2, WIDE a8/g70, cap5, reclip5, LC off, GATE own-name>200SMA AND vol20<=vol100, 8bp`
  → n=839, +1328%, PF 1.74, DD 132, MAR 10.0, 2022 +83, 2× PF 1.68. Frontier: gate saves 86% DD for 76% net.
  NOTE: resurrecting this engine reverses an explicit operator kill (S-16l) — needs operator word.
- **US500 ladder**: two-tier — DD-min `W24/2.0 **g50**+BE-floor dma200-gate` PF 1.77 DD 16.4 2×PF 1.56 ALL-6
  (CORRECTED S-23: the "g70" label was wrong — the certified harness row ran g50+floor; keep70 vs keep50 is
  inert ±0.7 net per the C++ parity cert); max-net ungated g70 (PF 1.38, 2× PF 1.27 <1.3). 2022 was its BEST
  year ungated (+77) — gate is PF/DD improver.
  C++ PARITY (ladder_reconfig_parity_2026-07-23.cpp): live-arch config (be0.08/pend4, weekend layers ON,
  + dma200 gate + gb0.50) ALL-6 PASS at PF 1.52 / DD 19.9 / 2022 +4.2 / 2× 1.35 — the shippable truth.
- **GER40 ladder**: keep bull regime gate (load-bearing; volcalm does NOT rescue bear file −31%). Upgrade:
  `+volcalm+BE-floor` in-bull → PF 14.6, DD 3.4, MAR 14.0. Thin (n=86, 7-month file) — provisional.
- **GBPUSD ladder**: thr0.75 NO-SHIP as DD-optimized (H2 +1.8 ceiling). Migrate to `W48/thr1.0 g70 cap5 no-gate`
  → PF 2.24, DD 8.3, MAR 4.6, 2× PF 2.02. FX bull-gates INVERT the edge (dma200 → PF 0.66) — never gate FX.
  Bear slice untestable (data 2025-01..2026-04) — operator flag.
- **HI52**: best `x5/r10/spy200/vt20` Sharpe 1.41 vs overlay-matched control 1.278 → selection alpha +0.135-0.17
  (misses +0.2 Sharpe bar) BUT 2022 −8.8 vs −14.5 matched. Modest, overlay-assisted. Operator call.

## C. NAS100 ladder (data + rebuild)

- DATA: `/Users/jo/Tick/NSXUSD_2022_2026.h1.csv` is NOW CERTIFIED CLEAN (Jul-8 rebuild; harness/engine "7 missing
  months" comments are STALE — months verified real vs independent NDX 5m, 0 zero-range bars). Fix comments when touched.
- W24/1.5 as-shipped: NO-SHIP CONFIRMED on clean data (PF 1.20, real not data artifact). Live arm0/g10 opt-in
  (S-21) is **honest-fill NEGATIVE −35% PF 0.88** — should be pulled.
- CERT REPLACEMENT (all-18 protection plateau, random-window control passed):
  `W48/thr2.0, ndx200 bull-gate (NDX close>200DMA prior-day), BE-ENTRY confirm=0.5·thr (1.0%), floor anchored
  epx·(1+RT), g50, tight OFF (ON = max-net variant), cap0, rt3bp` → n=352, +80.2%, PF 1.88, DD 14.2, MAR 5.66,
  2022 +5.3 (gated subset), WF +42.7/+37.4, 2× PF 1.73. Confirm depth is THE lever (6bp→PF1.24; 1%→PF1.88).
  200DMA >> rvol gates here (OPPOSITE of 3% family). Wiring needs: new gate fn + config at engine_init ~L2315
  (FxMimicLadderCompanion NAS100 cell) + faithful C++ parity cert (pend_bars→≈W semantics differ from live pend4).

## C++ PARITY CERTS (S-23, backtest/ladder_reconfig_parity_2026-07-23.cpp — real FxLadderPair; n-parity EXACT on all 4)
- **NAS100: STRUCTURAL DIVERGENCE.** The python-certified architecture (pre-arm floor STOP at epx·(1+RT)
  + tight-leg OFF) does not exist in FxLadderPair — pre-arm losers ride to window flush (DD 33 vs 14).
  As-wireable C++ truth: ndx200/no-weekend variant PF 1.49 all-6 PASS; with live weekend layers PF 1.39 and
  date-split WF-H2 flips negative. Capturing PF 1.88 needs a pre-arm-floor-stop engine config (operator call).
- **US500: PASS** — wire target = live-arch PF 1.52 row above.
- **GBPUSD: PASS at python-parity (PF 2.14)**; live-arch overlay (be0.08/pend4/wknd) = +15.2%/PF 1.62 — the
  honest expectation if migrated config-only onto the live cell. rt is 2.0bp (doc's 2.4 was wrong).
- **GER40: PASS** (PF 12.5 parity; weekend layers cut to 7.5 — still stellar; volcalm = vol24<=vol240 on H1,
  NOT the bigcap vol20/vol100 variant). 7-month file — provisional stands.
- **Weekend Layer2+3 degrade every one of these H1 cells** (python certs never included them). They are the
  operator's Apr-2025 tariff-gap risk cap — re-check explicitly before any wire that keeps them.
- DualMom cert restated (edge-search reproduction): live keff mechanism = Sharpe 1.66/mdd 29.9 (NOT 1.86/21.5,
  which was gross-exposure scaling the engine doesn't run); still beats ctrl 1.34. K5+gross-overlay = 1.90/21.0
  candidate, needs cert + engine mechanism change.

## Cross-family lever truths
- LOSS_CUT inert on every daily/laddered cell (gap-through) — keep only as backstop.
- g70 ≥ g50 everywhere on ladders; cap inert on H1; BE-floor clamp free-to-cheap on every H1 index cell.
- Bull-gates: transform index/equity cells; INVERT FX; on fast-entry families (3%/5% baskets) gate-lag makes
  2022 worse — entry-threshold or composite trend+calm beats plain 200DMA there.

## BUILD QUEUE (operator "if good then build" — order by confidence; each needs C++ parity cert + canary + proving size + [vps-pospar] coverage + vault page)
1. NAS100 ladder reconfig (engine exists; config + gate fn; PULL the negative arm0/g10 opt-in regardless).
2. US500 ladder dma200+BE-floor+g70 (engine exists; config + gate fn).
3. GBPUSD ladder thr0.75→thr1.0 g70 (config only; bear-untestable flag).
4. GER40 +volcalm+BE-floor (config; thin-data provisional).
5. 5% DayMover thr7% — NEW engine (real exec path, python→C++ or DualMom-pattern wire).
6. 3% G4+VS — NEW engine (same).
7. STOCK MOVERS ladder resurrect — BLOCKED on operator (reverses S-16l kill).
8. HI52 — operator call (modest edge).
