# XauTrendRider4h FLOORED giveback clip — certification findings (S-2026-07-17)

**Coverage hole:** XauTrendRider4h (bank-and-reload companion on the XauTrendFollow4h host
cells; `include/XauTrendRiderEngine.hpp`, engine_init L1516-1518: enabled=true
shadow_mode=false N=2.5 lot=0.01) is LIVE with NO giveback cover and no dedicated snapshot
book (`companion_coverage_audit` OWED class). Honest question certified here: the rider is
STRUCTURAL profit-taking already (banks +2.5×ATR per host cell and reloads;
ADVERSE-PROTECTION header = trail-only by design) — does a BE-ENTRY FLOORED clip cell add
STANDALONE net on the rider's real leg paths, or is the bank cadence already tight enough
that a clip book is redundant/negative?

**Verdict: PASS — the floored cell adds standalone value; the always-on mirror shape is
certified DEAD on this parent (fails the honest grain).**

## Harness / data (pre-flight gates run)

- **Entry stream:** `backtest/clip_path_xau_rider4h.cpp` drives the REAL
  `omega::XauTrendFollow4hEngine` at spot prod config (mask 0xC9, LOSS_CUT_PCT=1.5,
  vol-band 0.30/0.85 mask 0x8, min_impulse_atr=0.3, min_adx_entry=15.0, lot 0.01,
  max_spread 1.0) + the REAL `omega::XauTrendRiderEngine` (N=2.5, lot 0.01,
  init(kXauTfNumCells)) polled after every host on_tick/on_h4_bar exactly as
  tick_gold.hpp L1728 does live. Live gates replicated where standalone-reproducible:
  gold_d1_trend() fed per H4 close, gold_regime() fed per H1 close; gold_wt()/L2 gates
  fail open (live-only state) — same deviation class as every prior XauTf certification.
- **Data:** XAUUSD H1 2022-01-02 → 2026-07-13, 26,019 bars, spliced from
  `/Users/jo/Tick/XAU2022_bear_h1.csv` + `xau_h2023_24_m1.csv` (M1→H1) +
  `xau_1m_spliced_2024_2026.csv` (M1→H1) — builder `backtest/build_xau_h1_2022_2026.py`,
  output **`data_integrity_gate.py` CERTIFIED CLEAN** and staged via
  `stage_certified_data.sh` → `backtest/data/XAUUSD_2022_2026.h1.csv` (+ `.certified`
  stamp). Spans the 2022 bear, 2023-25 bull, and the 2026 4716→4000 crash.
- **Parent parity:** 511 parent trades net +$4,029 (2022-2026, all prod gates) →
  **876 rider legs**, gross +$4,328 pre-cost (live rider forward record ~+$3.1k since
  2026-06 wire — consistent order). Each rider leg (initial arm AND every bank-reload
  segment) = its own companion leg, exactly how the live snapshot keys StallCompanion rows.
- **Sweep:** `backtest/stall_clip_sweep_xau_rider4h.py` — exact StallCompanion be-mode
  semantics, HONEST fills (bank = actual price at detection; gap tails book real
  negatives, S-17f rule), judged STANDALONE (companion-independent rule; never vs WIDE).
- **$-basis:** rider lot 0.01 × tick_value_multiplier(XAUUSD)=100 = **$1.00/XAU-point**,
  so the IBS $1/pt sweep convention holds 1:1. Companion RT cost = IBKR XAU basis
  (2×0.00015×px + $0.30 spread ⇒ ~$0.9 at 2000, ~$1.6 at 4300).
- **Grain honesty:** every cell was run on BOTH path grains — H1-close (IBS precedent) AND
  an **intrabar worse-of stress** (adverse extreme → favourable extreme → close per bar,
  the whipsaw the live 60s drive can see but an H1-close path smooths away). Certification
  requires the cell to hold on both; the intrabar numbers are the quoted floor.

## What FAILED (the redundancy/kill axis — mechanism replicated, not strawman)

Always-on USD mirror (the existing StallBook shape, arm 25-100 × trail 25-100 ×
cold −50/OFF × reclip 0/25): on the H1-close grain a few borderline cells pass (PF ≤1.38),
but on the honest intrabar grain the whole grid **collapses to PF 0.92-1.11, 0/64 PASS**
(worst −181, WF-H2 negative in half the grid). Mechanism: an always-on mirror re-books
every rider loser (340/876 legs, −$16.5k of leg gross — the final host-exit legs that
carry the host's own per-cell risk) and whipsaws on the reload cadence. Same anti-pattern
as IBS. **Do not wire a mirror/pct book on this engine.**

## What PASSED — BE-ENTRY FLOORED mimic (the mandated foundation architecture)

Books NOTHING until the leg's fav_usd ≥ confirm (rider losers never mirrored), opens
ANCHORED at the leg entry, clip level floored at anchor + companion RT cost, absolute-$
giveback trail, LEVEL-anchored reclip. **60/60 grid cells PASS on BOTH grains** (confirm
10/25/50 × trail 25/50/75/100 × reclip 0/25 × cost ×1/×2 × both reclip variants) — full
plateau, no cherry-pick, bear column positive everywhere, every calendar year positive
(2022 bear +$1,043; 2026 crash +$3,266 at the chosen cell, intrabar grain).

Chosen cell — **confirm25 / trail25 / retrig25, gap-25 reclip** (uniform with the
IBS-certified pair AND the shipped S-17u StallCompanion be-mode reclip the operator is
standardizing on):

| grain / variant | n banks | net$ | PF | worst leg | WF-H1 | WF-H2 | bear$ |
|---|---|---|---|---|---|---|---|
| intrabar gap25 ×1cost | 459 | **+13,226** | 17.70 | −36.0 | +3,318 | +9,908 | +1,003 |
| intrabar gap25 ×2cost | 459 | **+12,652** | 14.95 | −37.4 | +3,215 | +9,437 | +948 |
| H1-close gap25 ×1cost | 403 | +15,627 | 77.4 | −20.9 | +3,435 | +12,193 | +1,161 |
| intrabar cert-reclip ×1 | 422 | +12,182 | 21.44 | −36.0 | +3,287 | +8,895 | +991 |
| intrabar cert-reclip ×2 | 422 | +11,696 | 18.57 | −37.4 | +3,186 | +8,510 | +946 |

- 381/876 legs ever confirm; the other 495 (every straight-adverse rider leg) book $0.
- Bank split (intrabar gap25): ENGINE_EXIT 286 banks +$12.6k (confirmed winners carried to
  the rider's own bank/host exit — the additive mirror-of-winners component) +
  FLOOR_CLIP 173 banks +$599 (the giveback-trail capture on legs that rode back).
- Worst legs are honest gap-through-floor fills (floor LEVEL ≥BE is a config property, NOT
  an execution guarantee; the booked fill is real).
- The high PF is structural to the shape at this leg geometry (the rider's own +2.5×ATR
  bank caps per-leg giveback, so the floored book's loss tail is thin); the intrabar grain
  is the honest lower band, and 2×cost holds it.

## Honest framing of "does it fix a $400-class incident"

The rider's giveback exposure is structurally bounded per leg (~one bank ≈ 2.5×ATR ≈
$25-100 at 0.01 lot) because it banks-and-reloads — there is no NAS-incident-class
unbounded ride-back on a single leg. The clip's value here is therefore mostly (a) the
loser-filtered winner mirror (additive standalone book) and (b) trimming the final-leg
ride-backs (FLOOR_CLIP component). Both are net-positive after 2×cost on the worse-of
grain in both WF halves and both regimes — the cell earns its wire on its own book.

## Wiring recommendation (parent session)

- `StallBook` be-mode cell: `confirm_usd=25 / trail_usd=25 / retrig_usd=25`,
  **`floor_cost_usd=1.6`** (IBKR XAU RT at ~4300: 2×0.00015×px×$1/pt + $0.30 spread;
  confirm ≥ 2×RT with huge margin), `stall_bars=9999`, include tag `XauTrendRider4h`.
- Exclude `XauTrendRider4h` from the shared `main` book (dedicated-cover doctrine, IBS
  precedent) — and note the rider must first be snapshot-visible (register_source) for the
  book to see its legs.
- NOTE the StallCompanion be-mode anchor is carried in fav_usd units (valid while parent
  size × tick-mult = $1/pt — true here at 0.01 lot XAU ×100).

## Repro

```
python3 backtest/build_xau_h1_2022_2026.py /tmp/XAUUSD_2022_2026.h1.csv   # or use the
# staged certified copy: backtest/data/XAUUSD_2022_2026.h1.csv (.certified stamp present)
c++ -std=c++17 -O2 -Iinclude backtest/clip_path_xau_rider4h.cpp -o /tmp/cpxr
/tmp/cpxr backtest/data/XAUUSD_2022_2026.h1.csv legs.csv 0.15       # H1-close grain
/tmp/cpxr backtest/data/XAUUSD_2022_2026.h1.csv legs_ib.csv 0.15 1  # intrabar worse-of
python3 backtest/stall_clip_sweep_xau_rider4h.py legs.csv XauTrendRider4h
```
