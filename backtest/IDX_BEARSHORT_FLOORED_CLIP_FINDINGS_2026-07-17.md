# IndexBearShort FLOORED giveback clip — certification findings (S-2026-07-17t)

**Incident (operator, verbatim: "I just watched it give back 400 dollars of profit ffs" /
"why would we not retain all the profit? fix this now"):** IndexBearShort NAS100 open short
(entry 28987.375, SHADOW) peaked ~+$425 (mfe 1.47%) and rode back to ~+$25 with NO companion
clip. Root cause: the leg's ONLY giveback cover was the shared `main` stall book, whose
`gate_pct=1.5` arm — a 1:1 transcription of the retired gold-era cron, never certified for
IndexBearShort — sat 0.03% ABOVE the peak, so `armed` never latched and the REVERSAL_CLIP
path was never live. Violates `feedback-profit-lock-mandatory`.

## Harness / data (pre-flight gates run)

- **Entry stream:** `backtest/clip_path_idx_bearshort.cpp` drives the REAL
  `omega::IndexBearShortEngine` (NAS100 DON24 / US500 DON48, prod cost pts 2.0/0.6, spread
  1.0/0.3 half) over H1 bars fed as 4 intrabar ticks — the engine's own aggregator, gates,
  SL/TP/manage all live. Output = per-H1-close path entry→natural exit per trade.
- **Data (both `data_integrity_gate.py` CERTIFIED CLEAN):**
  - NAS100: `/Users/jo/Tick/NDX_5m_2022_2026.csv` aggregated 5m→H1 (24,407 bars,
    2022-01→2026-04) → **90 parent legs**.
  - US500: `/Users/jo/Tick/SPXUSD_2022_2026.h1.csv` (24,434 bars, same span) → **74 legs**.
- **Sweep:** `backtest/stall_clip_sweep_idx_bearshort.py` — exact StallCompanion semantics at
  H1-close grain, $1/pt index CFD (sizing.hpp), companion RT cost charged per banked leg,
  HONEST fills (bank = actual close at detection; gap tails book real negatives — S-17f rule).
  Judged STANDALONE (companion-independent rule; never vs WIDE).

## What FAILED (the kill evidence — mechanism replicated, not strawman)

Sweeping always-on mirror books (the existing StallBook shapes):

- **PCT gauge** (gate 0.25–1.5 × rev_gb 0.3–0.8 × cold −50/−150/−300/OFF × reclip off /
  anchored 0.02): NAS 6/288 cells pass, only at LOOSE gb 0.6–0.8 (retains 20–40% of peak =
  does not fix the incident); tight-gb cells all negative. US500: 2/288.
- **USD gauge** (arm 25–150 × trail 25–150 × same cold/reclip grid): NAS 0/400, US500 3/400.
- **Mechanism:** an always-on mirror re-books every parent −1R loser via ENGINE_EXIT, and the
  default `cold_loss_omega=−50` (= 50 NAS pts, 0.2%) noise-cuts legs before the ride. Tight
  trails then cut winners short → classic anti-pattern on a Donchian 2R-TP trend parent.
- **Accounting note:** the raw StallBook `retrig` semantics re-bank the FULL from-parent-entry
  upnl after every reclip (bank at clip1 + full bank at clip2 ≥ clip1's segment again). An
  unanchored sweep "showed" +$16.9k/PF4.6 — inflation, the phantom-realized class. All
  certified numbers below use reclip=0 or LEVEL-ANCHORED reclip only (BeFloorOnOpen doctrine:
  reclip=0 OR anchored-reclip).

## What PASSED — BE-ENTRY FLOORED mimic (the mandated foundation architecture)

Books NOTHING until the leg's fav_usd ≥ confirm (parent losers never mirrored), opens
ANCHORED at the parent entry, clip level floored at anchor + companion RT cost, absolute-$
giveback trail, LEVEL-anchored reclip (reopen at peak+retrig+confirm, anchor = peak+retrig —
restart-safe, only persisted state is the clip peak).

**48/48 grid cells PASS per symbol** (confirm 10/25/50 × trail 25/50/75/100 × reclip 0/25 ×
cost ×1/×2) — full plateau, no cherry-pick. Chosen cell (uniform both symbols, tightest
certified trail per profit-lock rule):

| cell | n | net$ | PF | worst leg | WF-H1 | WF-H2 | 2×cost |
|---|---|---|---|---|---|---|---|
| **NAS100 confirm25/trail25/retrig25** | 146 | **+10,944** | 7.66 | −211 | +4,321 | +6,623 | +10,304 PF6.7 |
| **US500 confirm25/trail25/retrig25** | 70 | **+1,996** | 15.12 | −56 | +1,028 | +968 | +1,912 PF12.3 |

- Worst legs are honest H1 gap-through-floor tails (the floor LEVEL is ≥BE by construction —
  a config property, NOT an execution guarantee; the booked fill is real).
- Regime split: all entries are bear-regime by construction (sustained-bear gate) — regime
  column vacuous; WF time-halves both positive is the split that matters. n≥30 both symbols.
- **Incident replay:** $425 peak → confirm opens at +$25, trail $25 banks ~$396–400 at 60s
  live grain (H1-grain sim ~$390). vs the $25 actually retained.

## Shipped (this session)

1. **`include/StallCompanion.hpp`** — BE-ENTRY FLOORED mode (`confirm_usd>0`), flag-gated:
   existing books byte-identical behavior. New: `confirm_usd`, `floor_cost_usd`, `Pos.anchor`
   (persisted col 13, legacy rows load 0), `FLOOR_CLIP` reason, anchored ENGINE_EXIT.
2. **`include/engine_init.hpp`** — dedicated pair `idx_bearshort_clip_nas` / `_spx`
   (confirm25/trail25/retrig25, floor_cost 4.0/1.2, stall 9999); `IndexBearShort` added to the
   main-book EXG exclude; `IndexBearShort` excluded from the 4 `spx_turtle_clip*` books whose
   `include={"US500"}` symbol-substring silently also mirrored IBS US500 legs.
3. **Structural gate:** `scripts/companion_coverage_audit.sh` +
   `companion_coverage_allowlist.txt`, wired into `mac_canary_engines.sh` — every live
   snapshot engine tag must have a dedicated giveback book/mirror or a documented allowlist
   entry; regression-tested RED on the pre-fix state. 13 grandfathered OWED entries
   (main-defaults cover, certification debt visible).

## Deploy notes

- The open incident leg's `main`-book companion row must be PRE-CLEANED from
  `C:\Omega\stall\main\companion_positions.tsv` while the service is stopped (the EXG
  exclusion would otherwise ENGINE_EXIT-bank ~+$25 with no parent close → parity [8] flag).
  Same check for `spx_turtle_clip*` positions holding IBS US500 keys.
- Boot line to verify: `stall-companion zoo wired: 25 books` (was 23).
