# FX MAJORS — BOTH-DIRECTIONS MECHANISM SWEEP (2026-07-08, RESEARCH ONLY)

Operator order: per-pair tradeable mechanism for the FX majors, BOTH directions, ranked by
return. No wiring, no deploys. Tombstone respected: the naked Donchian/turtle archetype
(FxTurtleH4) was NOT retested.

Script: `backtest/fx_bothways_sweep.py` (+ `backtest/fx_bothways_prep.py` for derived data).
Raw log: `outputs/FX_BOTHWAYS_SWEEP_2026-07-08.txt` (1000+ ROW lines, pipe-delimited).

## Cost model (stated)
Base **2.0 bp round-trip** on majors — the validated figure the wired FX upjump ladder uses
(`rt_cost_bp ~2.0`); ExecutionCostGuard equivalence: $6/lot RT commission (~0.6bp on a 100k
lot) + ~1–1.5bp spread. Stress leg **4.0 bp (2x)** on every candidate. Anchors reproduced at
the original sweep2 per-pair costs (2.0/2.0/2.5/2.0bp).

## Data (all certified by backtest/data_integrity_gate.py this session)
| pair | file | span | gate |
|---|---|---|---|
| EURUSD | Tick/EURUSD_merged.h1.csv | Mar-25→Apr-26 (403d, 6920 bars) | CLEAN |
| GBPUSD | Tick/GBPUSD_befloor_h1.csv | Jan-25→Apr-26 (464d, 7928) | CLEAN |
| USDJPY | Tick/USDJPY_befloor_h1.csv | Jan-25→Apr-26 (464d, 7928) | CLEAN |
| AUDUSD | Tick/AUDUSD_befloor_h1.csv | Jan→Dec-25 (364d, 6225) | CLEAN |
| NZDUSD | Tick/fx_bothways_deriv/NZDUSD_2025_h1.csv (REBUILT from monthly ticks) | Jan→Dec-25 (6223) | CLEAN |
| USDCAD | Tick/USDCAD_befloor_h1.csv | Jan→Dec-25 (364d, 6225) | CLEAN |
| EURGBP | Tick/EURGBP_befloor_h1.csv | Jan-25→Apr-26 (464d, 7928) | CLEAN |
| GBPUSD regime | Tick/GBPUSD2022H2_befloor_h1.csv (2022-H2 dollar rally + Truss) | Jul→Dec-22 | CLEAN |
| USDJPY regime | Tick/fx_bothways_deriv/USDJPY_{2018,2020,2022,2024}_h1.csv (from M1) | 4 full years | CLEAN x4 |

- **NZDUSD_befloor_h1.csv is gate-REJECTED** (90-day hole Jan–Mar 2026 — those tick months
  don't exist locally). Rebuilt clean 2025-only H1 from the 12 monthly histdata tick files.
- **USDCHF: no local data at all — skipped, not downloaded (per order).**
- 2022 dollar-rally regime data exists ONLY for GBPUSD (H2) and USDJPY (full year).
  EURUSD/AUD/NZD/CAD/EURGBP verdicts are single-regime (2025–26) — stated per pair below.

## Mechanisms tested (each both directions where the mechanism allows)
- **UPJUMP** — up-jump giveback ladder LONG (mechanics line-for-line = `fx_upjump_ladder_sweep2.py run()`,
  parity-tested vs the C++ engine; anchor reproduction below is exact to the bp).
- **DNJUMP** — the untested mirror: close ≤ −thr% under the prior-W-bar high → W-bar short
  window, same clip/arm/giveback/loss-cut/reclip machinery sign-mirrored.
- **RIDER** — jump-rider W∈{2,4}, thr 0.25–0.5%, both ways. Exits: FLIP (XAUUSD GO pattern)
  and BE-LOCK floors +5/+10/+25bp as RESTING intrabar stops. The known-trap close-eval BE
  (JumpRiderNAS100: all 164 BE exits red) was NOT used anywhere.
- **MRH4** — H4 z-band fade both sides (z-in 1.5/2.0/2.5, k=20), per-side sustained-trend
  veto (ConnorsNas asym-veto pattern: streak ≥24 H4 bars beyond SMA100), 150bp resting
  catastrophe cut.

Validation per cell: WF halves (time split), ex-best-trade, 2x-cost, 5-seed random-entry
control (over-random), plateau (neighbor cells), regime split where data exists.
PASS = n≥20, net>0 both halves, ex-best>0, 2x-cost>0, over-random>0, plateau not SPIKE.

## Anchors reproduced (Stage 0) — YES
| anchor | prior (sweep2, %×100=bp) | this session | match |
|---|---|---|---|
| EURUSD W48/0.5 L | +3970bp PF1.47 n507 | +3972bp PF1.47 n507 | EXACT |
| GBPUSD W48/1.0 L | +3740bp PF2.20 n240 | +3738bp PF2.20 n240 | EXACT |
| AUDUSD W96/1.0 L | (sweep2 pass) | +3092bp PF1.51 n220 WF+ | same file/mechanics |
| NZDUSD W24/1.5 L | +4120bp PF4.35 n100 (REJECTED file) | +3686bp PF4.02 n90 (clean rebuild) | holds on clean data |
| USDJPY W2/0.3 FLIP rider | IS+90/OOS+305 @3bp (WATCH) | +569bp PF1.12 n174 @2bp, all gates | holds |

## Sanity controls — behave as required
- **arm≤1% mirror-style cells** (WIDE arm 2.7·thr ≤ 1%): DNJUMP **31/35 negative, mean
  −3,092bp** (UPJUMP 27/35 negative, mean −1,153bp). Matches the historical "all negative"
  expectation → the short mirror is not fantasy-positive. The single big "passer" tagged
  arm≤1% (NZDUSD DNJUMP W48/0.25, +2996) is exactly this control cell class + plateau-SPIKE
  → discarded as the known trap.
- **BE-lock (resting) floors** are frequently net-positive (BEL25 best on USDJPY/NZDUSD)
  — confirming the death of close-eval BE does NOT extend to resting BE-lock stops.

## PER-PAIR RANKED TABLE (best validated config per pair/mechanism/direction)

| # | pair | mechanism | dir | config | n | net_bp | PF | halves | regime split | ex-best | 2x-cost | over-rnd | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | EURUSD | UPJUMP ladder | L | W48/thr0.5 | 507 | +3972 | 1.47 | +3260/+712 | no 2022 data (gap) | +3731 | +2958 | +2742 | **PASS** (plateau ok) |
| 2 | GBPUSD | UPJUMP ladder | L | W48/thr1.0 | 240 | +3738 | 2.20 | +3094/+644 | **2022H2 +1368 PF1.17, 2x +980 — survives** | +3567 | +3258 | +3828 | **PASS** (only cross-regime PASS) |
| 3 | AUDUSD | UPJUMP ladder | L | W72/thr0.75 (anchor W96/1.0 also passes +3092) | 335 | +3729 | 1.48 | +2639/+1091 | no 2022 data (gap) | +3285 | +3059 | +3712 | **PASS** (plateau ok) |
| 4 | NZDUSD | UPJUMP ladder | L | W24/thr1.5 | 90 | +3686 | 4.02 | +3230/+455 | no 2022 data (gap) | +3466 | +3506 | +3517 | **PASS** (plateau ok, clean rebuild) |
| 5 | USDCAD | DNJUMP ladder | S | W96/thr0.5 | 230 | +2241 | 1.58 | +1493/+748 | no 2022 data (gap; 2025 = CAD-strength year) | +2007 | +1781 | +2137 | **PASS** (plateau ok: neigh +541; W96/0.75, W72/0.75, W24/0.5, W48/0.5 also pass) — single-regime caveat |
| 6 | USDJPY | DNJUMP ladder | S | W48/thr1.5 (family: W12/1.5 +1932, W24/1.5 +1837, W48/2.0 +2666 all pass gates) | 130 | +2444 | 1.87 | +821/+1622 | mixed: Y2018 +1033, Y2020 +2900, Y2022 +1413, **Y2024 −1896** (W48/1.0 S) | +2216 | +2184 | +2267 | **WATCH** (thr≥1.5 sub-plateau real, formal plateau SPIKE, one regime year red) |
| 6b | USDJPY | RIDER | LS | W2/thr0.3 FLIP (BEL25 variant +1144 but plateau SPIKE) | 174 | +569 | 1.12 | +425/+144 | mixed: Y2018 −247, Y2020 +308, Y2022 +1568, Y2024 −716; W2/0.25 FLIP 3/4 years + | +252 | +221 | +700 | **WATCH** (all main gates pass; cross-year not consistent) |
| 7 | EURGBP | DNJUMP ladder | S | W24-48/thr0.75-1.0 (4 cells +215..+413) | 50–106 | +413 | 1.31 | +406/+8 | no 2022 data | +277 | +201 | +1124 | **WEED** (plateau SPIKE, net too small) |
| 7b | EURGBP | UPJUMP ladder | L | W72/thr1.0-1.5 cluster | 25–77 | +1043 | 1.92 | +1046/−3 | — | +878 | +889 | +1130 | **WEED** (half-2 flat/negative, n small) |
| 8 | USDCHF | — | — | — | — | — | — | — | — | — | — | — | **NO DATA** (skipped, not downloaded) |

### Explicit WEED-OUTS (both-directions completeness)
- **EURUSD SHORT**: all 28 DNJUMP cells negative (best −5bp). Euro dip-buying is one-sided. WEED.
- **GBPUSD SHORT**: 25/29 cells negative; lone W48/2.0 "pass" is plateau-SPIKE (neigh −1906). WEED.
- **AUDUSD SHORT**: 28/30 negative; W12/1.5 is a SPIKE (neigh −2796). WEED.
- **NZDUSD SHORT**: best cell is the arm≤1% control trap; 26/30 negative. WEED.
- **USDCAD LONG**: ALL 30 UPJUMP cells negative — perfect mirror of the short edge. WEED.
- **USDJPY LONG ladder**: 1/30 cells positive (W48/1.0, plateau SPIKE, cross-year: −1623/+911/+634/−533). WEED.
- **RIDER on EURUSD/GBPUSD/AUDUSD/EURGBP**: EURUSD passes gates but short leg is −418bp (long-only
  edge already better captured by the ladder); GBPUSD/AUDUSD/EURGBP best cells all plateau-SPIKE. WEED.
- **MRH4 (z-band fade + asym veto) on ALL pairs**: only AUDUSD z2.5 scrapes +212bp (n43); every
  regime run negative (GBP 2022H2, JPY all 4 years). The asym per-side veto does NOT rescue FX MR
  the way it did on indices. **WEED the mechanism on FX.**

## Regime-split detail (where 2022 data exists)
- **GBPUSD 2022H2 (dollar rally + Truss)**: UPJUMP L W48/1.0 → +1368bp PF1.17 n194, 2x +980.
  Halves −1633/+3001 (the Truss crash then reversal). It stays net-positive through the worst
  GBP regime on record — the only cross-regime-verified FX pass this session.
- **USDJPY 2018/2020/2022/2024**: long ladder fails 2/4 years; short ladder +3/4 years but
  −1896 in 2024 (yen-carry unwind year, ironically); rider FLIP +2/4 (big 2022 +1568).
  Nothing on USDJPY is cross-regime clean → WATCH tier only.

## FINAL RANKED RECOMMENDATION (wire order, if/when operator chooses to wire)
1. **GBPUSD UPJUMP LONG W48/1.0** — the only cross-regime PASS (survives 2022H2). Highest PF (2.20).
2. **EURUSD UPJUMP LONG W48/0.5** — biggest net (+3972bp), broadest plateau, n=507.
3. **AUDUSD UPJUMP LONG W72/0.75** (or keep anchor W96/1.0) — both pass all gates.
4. **NZDUSD UPJUMP LONG W24/1.5** — strongest PF-per-trade (4.02); now on gate-clean data.
5. **USDCAD DNJUMP SHORT W96/0.5** — the NEW finding: first genuine short-side FX ladder pass,
   broad W-plateau, all gates. Single-regime (2025 CAD-strength) — size smallest, treat as the
   experimental short book.
- WATCH (do not wire yet): USDJPY DNJUMP SHORT thr≥1.5 family; USDJPY RIDER W2/0.25–0.3 FLIP.
- WEED OUT (nothing passes): EURGBP, USDCHF (no data), all FX MRH4, all remaining short sides.

*Research only. Nothing wired, no engine files touched, no deploys.*
