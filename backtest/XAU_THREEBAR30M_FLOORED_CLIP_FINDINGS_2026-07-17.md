# XauThreeBar30m FLOORED giveback clip — certification findings (S-2026-07-17, IBS S-17t pattern)

**Why:** after the IndexBearShort $400-giveback fix (S-2026-07-17t), `XauThreeBar30m`
(engine_init ~L4001: enabled=true, shadow_mode=false = LIVE on IBKR 4002 paper, long-only,
lot 0.01) is the remaining LIVE engine whose only giveback cover is the shared `main` stall
book's UNCERTIFIED gold-era defaults (`gate_pct=1.5`, `cold_loss=-50`). This certifies a
dedicated BE-ENTRY FLOORED clip cell for it, same doctrine (`feedback-profit-lock-mandatory`,
BeFloorOnOpenFoundation), judged STANDALONE (companion-independent rule; never vs WIDE).

## Harness / data (pre-flight gates run)

- **Entry stream:** `backtest/clip_path_xau_threebar30m.cpp` drives the REAL
  `omega::XauThreeBar30mEngine` at the FULL LIVE engine_init.hpp config (long_only, lot 0.01,
  S35-P4 be_trigger 1.0×ATR / trail 0.75×ATR / atr_floor 0.30, S63 LOSS_CUT 0.05 / BE_ARM
  0.03 / BE_BUFFER 0.012, S88 slope_12 + vol_band 0.30–0.85, HMM off) over M30 bars fed as 4
  intrabar ticks (o,l,h,c adverse-first), EXTERNAL Wilder ATR14 (without it the vol_band
  window never fills — the live gate silently drops out), `gold_regime()` tick-fed from the
  same stream (REGIME_BEAR_LONG_BLOCK live; ~13d cold warm = prod cold-start fail-open).
  All three gate classes observed firing (VOL_BAND_OUT / SLOPE_GATE / REGIME_BEAR_LONG_BLOCK).
  → **584 parent legs**, 2022-01 → 2026-04.
- **PATH GRAIN = M1 closes** (the honest grain, and the key deviation from the IBS H1
  harness): this parent's S63 cuts make the median hold < 30 min — at M30-close grain the
  paths were 1.5 rows/leg (vacuous, cannot certify a 60s-drive companion). M1 closes in
  `[entryTs, exitTs)` ARE the live StallCompanion 60s marks; the last row is the ≤60s-stale
  mark the live book banks on ENGINE_EXIT (never the parent's own fill). 12,786 path rows,
  21.9/leg.
- **Data (all `data_integrity_gate.py` CERTIFIED CLEAN):**
  - M30 drive file (49,451 bars 2022-01-02→2026-04-24, max gap 74h = Easter): spliced from
    `/Users/jo/Tick/XAUUSD_2022_2023.m15.csv` (CLEAN, m15→M30) + HISTDATA T202401/02 ticks
    (EST+5h→UTC, mid) + `/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m30.csv` (CLEAN, from 2024-03).
  - M1 path file (1,482,819 bars, same span, CLEAN): `/Users/jo/Tick/xau_h2022full_m1.csv` +
    `xau_h2023_24_m1.csv` + `xau_m1_2024_2026.csv`.
- **$-basis (stated, honest):** XAUUSD live lot 0.01 × tick_value $100/pt/lot (sizing.hpp)
  = **$1/pt at live lot**. StallCompanion be-mode gauges fav_usd in RAW price points (its
  header NOTE: valid while size × tick-mult == 1) — for this parent 0.01 × 100 = 1.0
  EXACTLY, so companion-USD == points == real USD. Companion RT cost charged per banked leg
  = IBKR basis 2×1.5bp×entry + $0.30 spread (project-ibkr-cost-basis): $0.84 (2022 prices)
  → $1.71 (gold 4700). HONEST fills throughout (bank = actual M1 mark at detection; gap
  tails book real negatives — S-17f rule).
- **Sweep:** `backtest/stall_clip_sweep_xau_threebar30m.py` — exact StallCompanion.hpp
  S-17t be-mode semantics, both reclip variants (A: shipped STD; B: gap-25 formula the
  operator is switching the IBS books to).

## What FAILED (kill evidence — mechanism replicated, not strawman)

- **The LIVE main-book cover itself** (always-on PCT mirror, gate 1.5 / gb 0.5 / cold −50 /
  retrig 0.05): **n=584, net −$400.41, PF 0.63, WF −188/−213.** The arm is DEAD WEIGHT: max
  ride in 4.3yr = mfe 0.9% ($38 at $1/pt) < gate 1.5%, so results are byte-identical for
  every gate ≥ 0.8 — the book just re-books all 584 parent legs (median mfe $0.76; the
  −1R/LOSS_CUT losers dominate) minus companion cost. This is the incident class: cover
  that never arms + mirrors every loser. Whole PCT grid 0/15 cells, whole always-on USD
  grid (arm 5–50 × trail 3–25) 0/16 — all net −$398..−$485.
- **IBS-scale FLOORED grid (the mandated confirm 10/25/50 × trail 25/50/75/100 × reclip
  0/25):** out of scale for this parent at live lot. confirm50: **0 legs ever**. confirm25
  (incl. the operator-uniform confirm25/trail25/retrig25): **n=7, net +$179.41, WF-H1 =
  $0.00 → FAIL** — all 7 confirms are 2025–26 high-ATR era, nothing in the first half; and
  trail 25–100 never binds (identical numbers across all trails = no profit-lock function).
  confirm10 passes (n=35, +$399.85) but the trail is equally inert at ≥$15. **The uniform
  IBS cell is NOT certifiable on this parent** — its rides are $1–38, not $100s. Mechanism,
  not force-fit: parent TP = 4×ATR ≈ $12–40 at $1/pt.

## What PASSED — BE-ENTRY FLOORED mimic at GOLD scale (mandated foundation shape)

Gold-scaled grid confirm 3/5/10 × trail 3/5/10/15/25 × retrig 0/5 × cost ×1/×2, both reclip
variants: **60/60 cells PASS per variant** (net>0, PF≥1.3, WF both halves>0) — full plateau,
no cherry-pick. confirm=3 violates the confirm ≥ 2×RT-cost recipe at current gold (2×$1.71
= $3.42), so the certified floor of the confirm axis is 5.

**Chosen cell (tightest certified trail per profit-lock): confirm 5 / trail 3 / retrig 5,
floor_cost 2.0** — WF midpoint 2024-08-12, worst legs are honest M1 gap-through-floor tails:

| variant | n | net$ | PF | worst | WF-H1 | WF-H2 | 2×cost |
|---|---|---|---|---|---|---|---|
| **A STD-reclip (S-17t shipped: reopen ≥ peak+retrig+confirm, anchor=peak+retrig)** | 96 | **+501.82** | 111.4 | −1.50 | +141.33 | +360.49 | +383.50 PF39.4 worst −3.33 |
| **B GAP-reclip (operator switch: reopen ≥ peak+retrig, anchor=peak+retrig−confirm)** | 101 | **+536.10** | 135.9 | −1.11 | +141.33 | +394.76 | +408.63 PF49.2 worst −2.84 |

- Variant B ≥ variant A on every axis here (5 extra reclip legs, better worst) — certifying
  XauThreeBar30m on the SAME gap-reclip formula as the IBS books is supported, not just
  permitted.
- Yearly (B, ×1): 2022 +37 (n=8) / 2023 +57 / 2024 +61 / 2025 +243 / 2026 +138 — every year
  positive incl. the 2022 bear (thin by construction: long-only parent + regime gate fire
  little in a bear; regime split bull +495 / bear +41).
- Bank mix (B): 66 FLOOR_CLIP / 35 ENGINE_EXIT; parent losers never mirrored (BE-ENTRY).
- **Ride-retention sanity (incident-replay style):** 35 legs reached mfe ≥ $10; their peaks
  summed gave back $197 to the parent's own natural exits (last marks $441). Chosen cell
  banks $322 (A) / $357 (B) standalone on those legs. Largest ride (2026-03-06, entry
  5130.40, mfe +$38.30): A banks +13.66 then a reclip gap-tail −1.50 = **+12.16**; B banks
  +13.66 then +3.50 = **+17.16** (the −1.50 is a real gap-through-floor fill — the floor
  LEVEL is ≥BE by construction, a config property, NOT an execution guarantee).

## Wiring recommendation (for the parent session — nothing in include/ touched here)

- Dedicated book `xau_threebar30m_clip`: `confirm_usd=5, trail_usd=3, retrig_usd=5,
  floor_cost_usd=2.0` (companion RT cost in USD at live lot, ceiling: 2×1.5bp×4700 + $0.30
  spread = $1.71 → wire 2.0), `stall_bars=9999`, include by ENGINE tag `XauThreeBar30m`.
- Add `XauThreeBar30m` to the main-book EXG exclude (kills the certified-negative −$400
  main-defaults mirror) and check no other gold book's include-substring silently covers
  these legs (the spx_turtle_clip lesson).
- Units caveat: thresholds are in PRICE POINTS via the $1/pt identity (0.01 lot × 100).
  A lot resize changes realized USD but NOT the companion's point-gauged thresholds —
  re-certify before any resize (StallCompanion be-mode NOTE).
- Deploy: pre-clean any live `stall/main/companion_positions.tsv` XauThreeBar30m row while
  the service is stopped (EXG exclusion would ENGINE_EXIT-bank it with no parent close →
  parity [8] flag).

## Files

- Harness: `backtest/clip_path_xau_threebar30m.cpp` (build: `g++ -O2 -std=c++17 -I include`)
- Sweep: `backtest/stall_clip_sweep_xau_threebar30m.py`
- Scratch data (session): `xau_m30_2022_2026.csv`, `xau_m1_2022_2026.csv`,
  `xau_threebar30m_paths_m1.csv`, full grid `sweep_out.txt` in the session scratchpad.
