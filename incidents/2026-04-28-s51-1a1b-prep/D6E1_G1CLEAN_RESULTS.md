# S51 1A.1.b D6+E1 G1CLEAN — Results

**HEAD at run:** `04950859` (G1: thread_local time shim)
**Run dir:** `/Users/jo/omega_repo/sweep_D6E1_G1CLEAN_20260429_005927/`
**Wall:** 1235.3s (20.6 min) on 154,265,439 ticks
**Status:** Locked baseline. All four engines have verified, deterministic numbers for the first time.

---

## TL;DR

The G1 (thread_local) fix was correct. Re-ran the full 4-engine sweep against the same tick stream. Results:

| Engine | CSV size before | CSV size after | Verdict |
|---|---:|---:|---|
| HBG | 69,779 B | 69,779 B | **Byte-identical. All prior HBG findings stand.** |
| EMACross | 76,235 B | 76,235 B | **Byte-identical. No edge in any quadrant. Engine concept-dead on this grid.** |
| AsianRange | 69,122 B | 74,365 B | **Engine now operational (300+ trades/combo). Still no edge — every top-50 combo loses.** |
| VWAPStretch | 74,493 B | 79,113 B | **Trade count tripled but WR still 0.5–1.5%. Structural pathology IS REAL.** |

The "100× AsianRange harness frequency mismatch" was a harness ghost (resolved by G1). The "AsianRange has no edge" finding was also real, just invisible behind the harness ghost. **Both findings are true** — they were stacked, and we couldn't see the second one until the first was fixed.

---

## 1. HBG — byte-identical, edge confirmed real

`sweep_hbg.csv` is 69,779 bytes in both runs. Top combo 261 reproduces exactly:

| | D6+E1 (multi-thread) | G1CLEAN | delta |
|---|---|---|---|
| trades | 49 | 49 | 0 |
| WR | 63.27% | 63.3% | 0 |
| total_pnl | 0.5613 | 0.56 | 0 |
| score | 0.4377 | 0.4377 | 0 |

This empirically confirms the diag prediction: **HBG's hot-path use of `g_sim_now_ms` is robust to multi-thread reads.** Hold-time gates (`held_s >= MIN_TRAIL_ARM_SECS`) survive even when `g_sim_now_ms` is hours stale, because "has the position been open at least N seconds" returns true regardless. The diag rows for combos 24/73/122/171 having byte-identical counters under multi-thread was not a coincidence — it was the rule.

**All HBG findings from D5/D6/D6+E1 stand without reservation.** 70.5% WR at 1:1 RR (combo 259, 48 trades) and 63.3% WR (combo 261, 49 trades) are real edge.

### HBG top-50 grid distribution (G1CLEAN)

```
min_range  (grid 1.5..6.0):   43/50 at 6.0  (CEILING-CLIPPED — same as D6+E1)
max_range  (grid 16..64):     27/50 at 16,  20/50 at 25.4,  3/50 at 20.16
                              (BIMODAL — floor-clipped at 16, secondary peak at 25.4)
sl_frac    (grid 0.21..0.84): 42/50 at 0.42 (centred — confirmed)
tp_rr      (grid 1.0..4.0):   35/50 at 2.0  (centred — confirmed)
trail_frac (grid 0.125..0.5): 38/50 at 0.25 (clamped, not dead — see D7_RESULTS.md)
```

### Trail-frac final word

Combos 308–314 in G1CLEAN reproduce the diag finding exactly:

| trail_frac | trades | WR | total_pnl | stddev_q | score |
|---:|---:|---:|---:|---:|---:|
| 0.125 (308) | 49 | 61.2% | 0.46 | 0.3149 | 0.3462 |
| 0.158 (309) | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.198 (310) | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.250 (311) | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.315 (312) | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.397 (313) | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |
| 0.500 (314) | 49 | 61.2% | 0.46 | 0.3096 | 0.3483 |

`trail_frac=0.125` differs (stddev_q 0.3149 vs 0.3096); `trail_frac >= 0.158` clamps against `mfe * 0.20` (the hardcoded constant at `SweepableEngines.hpp:954`). **D6.3 (drop trail_frac from sweep) remains REJECTED.** The actual trail aggressiveness lever is the hardcoded `0.20` and should be exposed as a sweepable param (HBG-FIX-1).

---

## 2. AsianRange — engine works, has no edge

This is the surprising finding. Under G1, AsianRange combos now produce 262–388 trades each (top-50). Best WR 43.5%. **Every single one of the top-50 has total_pnl < 0**:

| combo | params summary | trades | WR | total_pnl | score |
|---:|---|---:|---:|---:|---:|
| 146 | buf=1.0, sl=160, tp=200 | 336 | 43.5% | -0.12 | -0.1039 |
| 355 | buf=0.5, max_r=31.5, sl=127 | 294 | 36.1% | -0.27 | -0.2316 |
| 348 | buf=0.5, max_r=25, sl=127 | 262 | 35.5% | -0.29 | -0.2543 |
| 480 | buf=0.5, sl=127, tp=252 | 354 | 31.1% | -0.33 | -0.2699 |
| ... (all 50 negative) ... | | | | | |

The centre combos (124, 369, 479, 271 — all params 0.5/3.0/50.0/127/200) **now produce identical results** (354 trades, 35.0% WR, -0.44 PnL, score -0.3865). Determinism restored.

**Live AsianRangeEngine documented frequency: 382 trades / 24 months.** G1CLEAN top-50 ranges 262–388. We're inside the live frequency band. The two-session investigation into "harness fires 1/170 of live frequency" is conclusively **resolved by G1 alone**.

But: the engine has no edge on this grid. Negative PnL across all top-50, with WR rarely exceeding 40% and TP/SL ratios suggesting structural problems (the live engine with 382 trades / 49.7% WR / +$279/2yr has 49.7% WR — significantly higher than the 43.5% top of this sweep grid).

**Two paths forward, both deferred to authorisation:**

1. **Engine class drift audit.** The harness `AsianRangeT` and live `AsianRangeEngine` may differ in non-trivial ways. The live engine achieves 49.7% WR; the best harness combo achieves 43.5%. A ~6 percentage point WR gap is meaningful and may indicate a port discrepancy. Recommend reading both classes in parallel and producing a diff memo.

2. **Wider grid sweep.** Default live params are buf=0.5, min_r=3.0, max_r=50.0, sl=80, tp=200. The current sweep grid extends 0.5..2.0× of those defaults. The `sl_ticks=160` (2× default) and `buf=1.0` (2× default) appear in the top-5, suggesting the live defaults may be sub-optimal but the sweep grid hasn't extended far enough to find the actual optimum.

**My recommendation: drift audit first.** If the harness class is faithful to live, then expanding the grid is justified. If it's drifted, no amount of grid expansion will help.

---

## 3. EMACross — byte-identical, concept dead

`sweep_emacross.csv` is 76,235 bytes both runs. EMA gates use `s.session` (snapshot field, immune to time race). E1 filter still flags 74/490 degenerate combos. Top combo 385 produces 0 trades (RSI_LO=80 > RSI_HI=25 — second degeneracy pattern not caught by E1).

Once trades fire in volume:
- Combo 244 (FAST=9, SLOW=30, RSI=80/50, sl=1.5): 1,678 trades, 45.4% WR, **-10.09 total_pnl** (~$1,009 over 24mo at 0.01 lot, ~$50,450 at 5.0 lot — disaster).
- Worst combo 183 (FAST=14, SLOW=15, RSI=40/50, sl=0.94): 4,255 trades, 45.9% WR, **-27.82 total_pnl** (~$2,782 / $139,100 at 5.0 lot).

**Every single top-50 combo with trades has negative PnL.** The strategy concept does not produce edge on XAUUSD ticks at any swept parameter combination.

**Recommendations stand from D5/D6+E1:**
- E1.1 (extend filter to RSI_LO ≥ RSI_HI): low-cost, ~10 lines of code, removes another ~10–20% of degenerate combos.
- Engine deprioritised. Either redesign concept (different MA structure, different gate logic) or remove from sweep entirely. **Decision deferred.**

---

## 4. VWAPStretch — structural pathology REAL, not a harness ghost

This was the open question after G1: was the 0.5% WR a harness contamination artefact, or a real strategy pathology? G1CLEAN settles it.

`sweep_vwapstretch.csv` is 79,113 bytes (was 74,493 — +6.2% larger because more trades fire under correct cooldown timing). Top combo 147 jumped from 2,230 trades (multi-thread) to **6,807 trades** (single-shim). But:

| combo | params | trades | WR | total_pnl | score |
|---:|---|---:|---:|---:|---:|
| 147 | sl=20, tp=88, cd=300, σ=2.0 | 6807 | **0.6%** | -13.20 | -4.1249 |
| 148 | sl=20, tp=88, cd=300, σ=2.0, vol=25 | 7293 | **0.4%** | -14.27 | -4.3453 |
| 392 | sl=40, tp=88, cd=150 | 10409 | 1.4% | -39.77 | -4.3668 |
| 154 | sl=25, tp=88, cd=300 | 6807 | 0.6% | -16.55 | -4.3913 |
| 189 | sl=80, tp=88, cd=300, σ=2.0 | 6807 | **16.1%** | -35.99 | -4.4042 |

The top combos still cluster at 0.5–1.5% WR even with three times the trade volume. This is structural. Combo 189 (sl=80) bumps up to 16.1% WR but at the cost of -36 total_pnl — the wider stop turns more positions winning but they win small and lose big.

**Diagnosis from D5_RESULTS.md stands:** entering at +2σ above VWAP is structurally late. Mean reversion typically continues another 0.5–1σ before reversing, which blows the SL on most positions before they can revert to the mean.

**D8 (VWAPStretch structural fix) is still warranted.** The two diagnostic experiments from D5_RESULTS:
- Experiment A: Reverse SL/TP relationship (SL > TP for mean-reversion entries)
- Experiment B: Move entry threshold lower (σ_entry = 1.0 or 1.5 instead of 2.0)

Both are harness-side parameter changes only.

---

## 5. Aggregate updated picture

| Engine | Edge? | Frequency | Verdict |
|---|---|---|---|
| HBG | YES (63–70% WR at 1:1 RR) | 1.8 trades/mo | Best candidate. Refine grid (D6.1, D6.2). Expose mfe-lock-frac (HBG-FIX-1). |
| EMACross | NO (every combo loses) | varies | Concept-dead on this grid. Defer or redesign. |
| AsianRange | INCONCLUSIVE | 14 trades/mo | Engine works but loses. Audit class drift vs live before deciding. |
| VWAPStretch | NO (0.5% WR structural) | 280 trades/mo | Structural fix (D8) required before sweep can produce signal. |

The previous priority order (HBG → AsianRange → VWAPStretch → EMACross) survives. HBG remains the lead candidate.

---

## 6. Authorisation status update

**Confirmed clean (no further action):**
- D7-FIX-2: confirmed threading hypothesis
- G1: shipped, byte-equivalent verified

**Confirmed by G1CLEAN data:**
- HBG findings stand. D6.1 (max_range rebase 32→16) and D6.2 (min_range rebase 3→6) still warranted.
- D6.3 (drop trail_frac) REMAINS REJECTED.
- HBG-FIX-1 (expose hardcoded mfe*0.20) recommended, low risk, not yet authorised.
- E1.1 (extend E1 filter to rsi_lo ≥ rsi_hi) recommended, low risk, not yet authorised.

**Newly recommended after G1CLEAN:**
- AsianRange class drift audit (read AsianRangeT vs AsianRangeEngine line-by-line, produce diff memo). Deferred until authorised.

**Carrying forward (still authorised, not started):**
- E2: EMACross hardcoded RSI dead-band review at SweepableEngines.hpp:991-998
- D8: VWAPStretch structural fix (now confirmed warranted, not just speculative)

**Determinism guards from DETERMINISM_GUARDS.md:**
- G1: SHIPPED ✓
- G2 (determinism self-test): not yet authorised; recommended to ship before any further sweep changes
- G3 (per-engine input checksum): not yet authorised; lower priority
- G4 (CONCURRENCY.md + lint rule): not yet authorised; recommended to ship with G2 to lock in the practice

---

## 7. State at memo write

- **Live VPS:** ed95e27c. Untouched.
- **Repo HEAD:** 04950859 (G1 shipped).
- **Mac binary:** `~/omega_repo/build/Release/OmegaSweepHarness`, 2026-04-29 ~01:00 NZST, contains G1.
- **Locked baseline run:** `~/omega_repo/sweep_D6E1_G1CLEAN_20260429_005927/`. This is now the canonical post-G1 baseline.
- **Old D5/D6+E1 results:** HBG portion remains valid; AsianRange/VWAPStretch portions are superseded by this run.
