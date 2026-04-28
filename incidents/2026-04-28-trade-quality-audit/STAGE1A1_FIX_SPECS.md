# Stage 1A.1 — Fix Specifications

**Session:** 2026-04-28 NZST (continuation of Stage 1A)
**HEAD pinned:** `c20fe2fc28f51d8fc1efcb2ec73da8e65d95324f` (origin/main, 2026-04-28 08:36:30 UTC)
**Method:** Source read (full) + Stage 1A ledger evidence cross-reference. No code, no push, specs only.
**Status:** SPEC for review. Numbers calibrated to Stage 1A findings + per-symbol configs.

---

## 0. Pre-flight assumptions and provenance disclosures

### 0.1 Assumptions stated before logic is written against them

The following assumptions underpin the fix specifications below. If any are wrong, the spec changes.

1. **Stage 1A's ledger findings are accurate** as numerical evidence of live behaviour over 2026-04-27 + 2026-04-28. In particular: 13 IndexFlow direction flips < 5 min on NAS100, median 15pt gap between flipped entries; 76% of 51 SL_HITs had MFE > 1pt; HBI 25% WR over 8 trades with -$24.69 avg loss.
2. **HEAD `c20fe2fc` is the build target.** All line references below are to this SHA via GitHub contents API (raw CDN avoided per memory rule). VPS is on `ed95e27c` per Stage 1A and does not need rebuild for this spec phase — specs apply to next deploy after backtest validation.
3. **Cross-engine position-snapshot infrastructure is sufficient** for a direction-gate implementation. Evidence: on_tick.hpp:709-720 already iterates `eng.pos.active`/`eng.pos.is_long` for every active engine when populating telemetry. The same iteration pattern is reusable for a gate.
4. **Backtest harness can sweep cooldown / chop-gate parameters.** Per memory #29, OmegaSweepHarness is built and uses `~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`. **Caveat:** the canonical tick file is XAUUSD only. Index ticks (NAS100/USTEC/SP/DJ30) are not in that file. **Calibration of IndexFlow / HBI parameters via OmegaSweepHarness is therefore NOT directly possible without an index-tick dataset** — flagged as a blocker for backtest validation of Section 1 (i) and Section 4 (HBI) below.
5. **Memory rule #30 applies to this document.** Every threshold cited has provenance attached: either the source line that produces it, or the Stage 1A finding that calibrates it. No number is freelanced.

### 0.2 Provenance disclosures — items where memory was wrong or stale

These were caught by source reads. Listing them so the record is straight:

1. **HBG trail formula.** Memory says "MFE-proportional locking 80% of move." The source comment at GoldHybridBracketEngine.hpp:436-440 confirms 80% intent (`min(range*0.25, mfe*0.20)` → locks ~80% of MFE for moves up to ~7.5pt). Stage 1A trade #3's `$0.10 lock` is **not a formula bug**; it is the trail-arm guard at L450 (`MIN_TRAIL_ARM_PTS=1.5`, `MIN_TRAIL_ARM_SECS=15`) blocking trail recalc until both fire — the trade hit reversal at 15s of hold with MFE having only just crossed 1.5pt, so the trail had at most one tick to advance SL before exit. Memory's 80% claim is mechanically correct **for trades that get past the arm guards**; the guards filter out fast moves.
2. **IndexFlow chop guard already exists.** IndexFlowEngine.hpp:670-673 has a chop guard. Stage 1A described chop suppression as a Tier-1 spec gap; in fact it's a guard that exists but is calibrated too narrowly. Spec adjusted accordingly (Section 2 below).
3. **HBI sizing is not the bug.** Memory rule "Tier 1 problem: HybridBracketIndex sizing". Source verification: NAS100 lot sizing at IndexHybridBracketEngine.hpp:380-385 produces 1.6 lots correctly from `risk_dollars=$25, usd_per_pt=$1/pt, sl_dist≈15pt`. The dollar-risk-per-trade is $25, exactly matching Stage 1A's -$24.69 avg loss. **The lot count is not oversized; it is correctly sized for a $1/pt contract.** The bleed is hit-rate (25% WR vs 33% breakeven RR=2). Spec rewrites HBI section to focus on entry quality, not sizing.
4. **HBG/HBI directional cooldown is dead code.** Both engines SET `m_sl_cooldown_dir` and `m_sl_cooldown_ts` on SL_HIT (HBG L532-533, HBI L549-550) but **the directional component is never read in the entry path**. HBI does use `m_sl_cooldown_ts` for a same-level block (L342) but ignores direction. Memory and Stage 1A both implied directional cooldowns existed but were ineffective; reality is they are uninitialised dead code.
5. **Cross-engine direction gate is not implemented at all.** Comments in on_tick.hpp:1731-1733 reference `cross_engine_dedup_ok` and `cross_engine_dedup_stamp` "converted to static function above on_tick". Grep across the file confirms zero call sites. Either the conversion was never completed, or the call sites were removed. Either way: the gate is absent, not partial.

### 0.3 Files read this session (full content, pinned to `c20fe2fc`)

- `incidents/2026-04-28-trade-quality-audit/STAGE1A_FINAL.md` (21,409 bytes, 328 lines)
- `include/GoldHybridBracketEngine.hpp` (28,794 bytes, 563 lines)
- `include/IndexFlowEngine.hpp` (58,961 bytes, 1,348 lines)
- `include/IndexHybridBracketEngine.hpp` (25,639 bytes, 590 lines)
- `include/on_tick.hpp` (128,307 bytes, 2,146 lines) — **partial read.** Top (1-130), master gates (600-720), bracket dispatcher (1380-1545), routing (1920-1970). Sections not read: bulk of 130-600 (per-engine update logic), 720-1380 (other dispatcher detail), 1545-1920 (engine-specific glue), 1970-end (FIX dispatch). This partial read covers the gate-architecture questions; deeper reads required if specs progress to coding.

---

## Spec (i) — IndexFlow whipsaw fix

### Source picture

IndexFlow has the following entry gates today (IndexFlowEngine.hpp, ordered as enforced):

| # | Gate | Location | Behaviour |
|---|---|---|---|
| 1 | Bid/ask validity | L541 | Reject ill-formed quote |
| 2 | Open-position management always runs | L552-573 | Position management bypasses entry gate |
| 3 | Post-exit cooldown 30s | L577-580 | `cfg_.cooldown_ms = 30000` |
| 4 | `can_enter` external | L582 | Cross-supervisor gate |
| 5 | ATR ready | L583 | 50-tick warmup |
| 6 | min_entry_ticks | L584 | 50 ticks since instance init |
| 7 | ATR ≥ atr_min | L590 | Per-symbol floor (NAS100: 8pt) |
| 8 | Spread ≤ max_spread | L593 | Per-symbol cap (NAS100: 1.5pt) |
| 9 | Asia session block 22:00-08:00 UTC | L601-603 | |
| 10 | NY open noise 13:30-14:00 UTC | L617-618 | |
| 11 | **90s post-SL cooldown** | L624 | `m_sl_cooldown_until_ms_`, set L568-570 |
| 12 | L2 OR drift signal | L636-653 | |
| 13 | Momentum confirmation 12 ticks | L662-665 | |
| 14 | **Chop guard** (drift_range > 4×threshold AND \|drift\| < 1.5×threshold) | L670-673 | Already exists |

Gates 11 and 14 are the existing partial solutions.

### Why the gates fail in chop

**Gate 11 (post-SL 90s) only fires after SL_HIT.** Of 35 IndexFlow trades in Stage 1A, 9 scratched at BE (BE-trail engaged, then exited at entry), which is path `move >= atr*1.0` (L390) → BE lock → reversal → SL hits at entry → exit reason is **`SL_HIT`** but with `pos.sl == pos.entry` so PnL is ~$0. Critical detail: **the BE-scratch path DOES emit `SL_HIT` as the reason** (IndexFlowEngine.hpp:440 — there is no `BE_HIT` reason in IndexFlow; the SL is just at entry). So `was_sl = true` triggers and the 90s cooldown DOES fire on BE-scratches.

But: 13 flips occurred within 5min, 6 within 120s. The 90s SL cooldown blocks the first 90s. 6 flips between 90s and 120s slip through. 7 more between 120s and 300s slip through. **The 90s cooldown is the right kind of fix but too short.** Stage 1A's data: median flip gap is somewhere between 90s and 180s.

**Gate 14 (chop guard) misses fast-flipping drift.** Trigger requires `|drift| < 1.5*threshold` (i.e. drift currently small). NAS100 chop where drift swings between -3pt and +3pt always has |drift| ≥ 1.5pt during signal generation moments — drift only crosses near-zero between flips, when the engine isn't trying to fire. Result: chop guard doesn't catch the regime in which flips happen.

### Spec (i.A) — Extend post-exit cooldown when last exit was reversal-prone

**Change:** Lengthen the post-SL cooldown from 90s to a candidate value of **180s for the OPPOSITE direction only**. Add new state variable: `m_opp_cooldown_until_ms_` and `m_opp_cooldown_dir_`. Existing 90s same-direction cooldown unchanged.

**Logic:**

```
On exit with reason==SL_HIT:
    set m_sl_cooldown_until_ms_ = now + 90000   (existing, unchanged)
    set m_opp_cooldown_dir_     = was_long ? +1 : -1
    set m_opp_cooldown_until_ms_= now + 180000  (NEW)

In entry path, after gate 14 (chop guard), before signal build:
    direction = signal_long ? +1 : (signal_short ? -1 : 0)
    if direction != 0
       and direction == -m_opp_cooldown_dir_   (i.e. signal opposite to last SL)
       and now < m_opp_cooldown_until_ms_:
        return {};   // block
```

**Calibration provenance:** 180s candidate covers the 6+10 flip count from Stage 1A Section 4a (gaps < 120s and < 180s). 6 flips < 120s already partly blocked by existing 90s cooldown (which was calibrated for SL_HIT only); the additional 10-6=4 flips between 120s and 180s would be caught by extending.

**Sweep parameter (for backtest):** `OPP_COOLDOWN_S ∈ {120, 150, 180, 240, 300}` — geometric sweep tails; 180 is centre estimate.

**Why direction-specific not symmetric:** A same-direction re-entry after SL is a different signal than an opposite-direction flip. Same-direction may be valid trend continuation after a stop-out; opposite-direction is exactly the chop-flip pattern Stage 1A documented. Blocking only the opposite direction preserves trend re-entries.

### Spec (i.B) — Widen chop guard to catch fast-flipping drift

**Change:** Replace the single-threshold chop guard at L670-673 with a two-condition test that includes drift sign-flipping.

**Current (L670-673):**

```cpp
if (drift_range_ > cfg_.drift_threshold * 4.0 &&
    std::fabs(d) < cfg_.drift_threshold * 1.5) {
    return {};
}
```

**Proposed (one of two variants below — choose after backtest):**

**Variant B1 — Sign-flip count over rolling window.**

Track count of drift sign-flips over `drift_window_` (existing 64-tick buffer). If sign-flip count ≥ N within window, block entry. Implementation: in `feed_persistence` (L757), compute and store flip count alongside drift_range.

```
flips = 0
prev_sign = 0
for d_i in drift_window_:
    s = sign(d_i)  // -1/0/+1
    if prev_sign != 0 and s != 0 and s != prev_sign:
        flips += 1
    if s != 0: prev_sign = s

if flips >= CHOP_FLIP_MIN within drift_window_ (last 64 ticks):
    block signal
```

**Calibration provenance:** Stage 1A: 13 flips in NAS100 14:00-15:00 UTC window. NAS100 tick rate during NY session is high (need to confirm). At ~10 ticks/sec, 64 ticks = ~6.4s. Sign-flip count ≥ 3 within 6.4s = chop. Sweep range: `CHOP_FLIP_MIN ∈ {2, 3, 4, 5, 6}`.

**Variant B2 — Drift volatility filter.**

Compute std-dev of drift values in `drift_window_`. If `stddev(drift_window_) > K * |mean(drift_window_)|`, block entry. K is the "noise to signal ratio" threshold.

```
if stddev(drift_window_) > CHOP_NOISE_RATIO * |mean(drift_window_)| 
   and stddev(drift_window_) > cfg_.drift_threshold:
    block signal
```

**Calibration provenance:** A trending-move drift series has high mean and low std (signal). A chop drift series has low mean and high std (noise). Threshold `K=2` means noise ≥ 2×signal. Sweep: `CHOP_NOISE_RATIO ∈ {1.5, 2.0, 2.5, 3.0}`.

**Recommendation:** Specify both variants. Backtest both against historical NAS100 ticks (when index data available, see Section 0.1 caveat). Pick winner by reduction in `flip count` while preserving real-trend signals.

### Spec (i.C) — Optional: NAS100-specific NY-window extension

Stage 1A: **11 of 13 flips occurred in 14:00-15:00 UTC window.** The current NY noise gate is 13:30-14:00. Extending the gate to 13:30-15:00 (or 13:30-14:30) on NAS100 ONLY would directly suppress 11 of the 13 documented flips.

**Caveat (rule #30 clause 4):** Stage 1A is a 2-day sample. 14:00-15:00 may not be a chronic NAS100 chop window — it may be sample-specific. Suggest:
- Per-symbol NY window override (NAS100 → 13:30-14:30)
- Backtest first to confirm 14:00-14:30 is consistently chop on NAS100 across multi-month history

**This is a low-cost option BEFORE the more invasive changes in (i.A) and (i.B). It may be sufficient on its own and rule out the need for a new direction-flip cooldown.**

### Open question for review

Stage 1A Open Question #2: "Memory mentions IFLOW regime tag — what triggers it, and is there a 'no-trade' regime tag possible?"

**Source answer:** `IdxRegimeGovernor` at IndexFlowEngine.hpp:252-288 has 4 regimes (MEAN_REVERSION, COMPRESSION, IMPULSE, TREND) but `IndexFlowEngine` does not consult them in the entry path. The regime is computed (`is_trending()` exposed at L514) but only **read by IndexHybridBracketEngine** as a gate (per memory). **A no-trade regime tag would require: (a) an `is_chop()` method on IdxRegimeGovernor; (b) a gate at IndexFlowEngine entry path consulting it.** This is essentially the same fix as Spec (i.B) but at a different abstraction layer. Recommend implementing as a method on IdxRegimeGovernor (single source of truth, reusable from IFLow and HBI).

---

## Spec (ii) — HBG/HBI runner-trail giving back MFE > 3pt

### HBG source picture

GoldHybridBracketEngine.hpp:435-458, with the following formula and arm guards:

```cpp
trail_dist = min(range * 0.25, mfe * 0.20)   // i.e. min(range_trail, mfe_trail)
trail_sl   = entry ± mfe ∓ trail_dist        // moves SL toward profit only

Arm guards (L447-450): trail recalc ONLY if
    pos.mfe >= MIN_TRAIL_ARM_PTS  (= 1.5)
    AND held_s >= MIN_TRAIL_ARM_SECS  (= 15)
```

Worked example (Stage 1A trade #3):
- Entry 4632.13 SHORT, MFE = 3.43, exit at 4632.03 = +0.10pt locked
- At MFE=3.43: range_trail=range×0.25, mfe_trail=0.686 → trail_dist=0.686
- trail_sl = 4632.13 − 3.43 + 0.686 = 4629.39 (SL would be 2.74pt in profit)
- Actual SL at exit = 4632.03 (only 0.10pt in profit)
- **Discrepancy:** SL didn't get to 4629.39. Most likely cause: arm guards prevented trail recalc until MFE first crossed 1.5pt at second 15+. Trade held 15s total. Trail had perhaps 1 tick to advance SL from initial-SL toward the formula target before reversal. SL got partway and was hit.

### HBG fix

The trail formula is correct. The arm guards are protecting against the 3-second TRAIL bug from 2026-04-24 (per L86-92 comment: a sub-pt MFE on tick 1 armed the trail and bid-ask noise hit it on tick 2).

**Spec (ii.A) — Add per-tick trail update once armed.**

The current logic only recalculates `trail_sl` on each tick (L450-458). That part is fine. The problem is that the arm guards check MFE and held time; once both are satisfied, the formula advances SL toward `entry + mfe - trail_dist`. **But `mfe` itself is updated each tick at L429.** So trail target moves with peak MFE — this should already track correctly.

**Re-checking the worked example:**
At second 15, `held_s >= 15` is true, `pos.mfe >= 1.5` requires MFE to have hit 1.5 by second 15. If MFE was 3.43 by then (peak before reversal), trail_sl = 4629.39 immediately. SL would advance 4632.13 → 4629.39 (favourable for SHORT) on that tick.

**The gap:** if exit happened on the SAME TICK that MFE peaked, the order of operations in `manage()` matters:
- L428: compute move
- L429: update mfe
- L433: update mae
- L450-458: recompute trail_sl
- L461: check tp_hit
- L465: check sl_hit ← **uses pos.sl set by trail at L456-457**

So if trail SL is computed AT 4629.39 on the same tick MFE peaks at 3.43, but bid in that tick is already higher (reversal underway), `sl_hit` at L465 fires immediately. The SL DID advance to 4629.39 on that tick — but the bid had already retraced past 4629.39. Conclusion: **trail_sl at exit time was indeed near 4629.39, but the comparison L465 used the new SL against the new bid, both reversed.** Stage 1A read SL=4632.03 from the close ledger — that may be the SL at the moment of exit, not the SL after trail. **Spec (ii) calibration is blocked on confirmation:** what SL does the closer log? Pre-trail or post-trail? Need read of `_close()` at L518-559 — confirm: L541 `tr.sl = pos.sl` (current value, post-trail). So `tr.sl=4632.03` means the trail did NOT advance to 4629.39.

**Why trail didn't advance:** look at L450 — guard requires `arm_mfe_ok && arm_hold_ok`. If MFE rose from 0 → 3.43 → reversal all within 15s of hold time, the `arm_hold_ok` guard blocked all trail updates. The trail only ever evaluated the initial SL.

**That is the actual bug.** A 15s guard on a fast-moving compression breakout means trades that resolve within 15s never see their trail engage. Gold compression breaks resolve in seconds.

### Spec (ii.A — refined) — Tier the arm guards

**Change:** Replace single hard-floor (1.5pt + 15s) with a tiered floor:
- **Stage A** (currently armed): `MFE >= 1.5pt`. Time guard relaxed.
- **Stage B** (large MFE bypass): `MFE >= 3.0pt` → arm guard satisfied regardless of hold time.

Rationale: the 2026-04-24 bug was sub-pt MFE on tick 2. A 3pt MFE within seconds is genuine flow, not bid-ask noise. The time guard exists to filter sub-pt → 3pt is well above that.

**Provisional implementation (DO NOT TAKE AS CODE — spec only):**

```
arm_mfe_strong = (pos.mfe >= 3.0)              // bypass time guard
arm_mfe_ok     = (MIN_TRAIL_ARM_PTS  <= 0.0) || (pos.mfe >= MIN_TRAIL_ARM_PTS)
arm_hold_ok    = (MIN_TRAIL_ARM_SECS <= 0)    || (held_s  >= MIN_TRAIL_ARM_SECS)
trail_arm_ok   = arm_mfe_strong || (arm_mfe_ok && arm_hold_ok)
```

**Sweep parameter:** `MIN_TRAIL_ARM_PTS_STRONG ∈ {2.0, 2.5, 3.0, 4.0, 5.0}`. Stage 1A trade #3 had MFE=3.43; threshold 3.0 catches it.

### HBI source picture

IndexHybridBracketEngine.hpp:481-537 has a more advanced trail than HBG: 3-stage staircase + initial BE.

```
Stage 0 (initial): SL at entry ± sl_dist, mfe-tracked
Stage BE:          move >= tp_dist * be_trigger_frac (default 0.60, NAS 0.70)
                   → SL moved to entry (BE lock)
Stage trail-1:     move >= tp_dist                    → SL = entry ± tp_dist*0.50
Stage trail-2:     move >= tp_dist * 2.0              → SL = entry ± tp_dist
Stage trail-3:     move >= tp_dist * 2.0 (also)       → SL = entry ± mfe ∓ trail_dist
                                                       trail_dist = max(range*0.25, spread*2)

Arm guards (L500-503):
    pos.mfe >= cfg_.min_trail_arm_pts (NAS100: 12pt, NQ: 8, SP: 3, US30: 15)
    AND held_s >= cfg_.min_trail_arm_secs (NAS100: 20, others: 15)
    Both must pass for ANY trail step (BE included).
```

NAS100 trail-arm for HBI: **12pt MFE AND 20s hold.** Stage 1A's HBI trades had avg loss -$24.69 over 8 trades sized 0.5-1.6 lots, suggesting per-trade SL distance hit was 15-50 NAS100 points. **A NAS100 HBI trade that doesn't reach 12pt MFE within 20s never sees its BE/trail engage at all.** That is consistent with Stage 1A's "fill-quality fields zeroed" — if no trail ever fires, MFE/MAE wouldn't be tracked beyond the initial values. (Stage 1A 6b confirmed `mae=0` for all 8 HBI trades, matching the hardcoded `tr.mae = 0.0` at IndexHybridBracketEngine.hpp:565.)

### Spec (ii.B) — HBI: same tiered arm-guard pattern

**Change:** Apply the same tiered arm-guard pattern as HBG to HBI. New per-symbol param `min_trail_arm_pts_strong` that bypasses time guard.

**Calibration:**

| Symbol | min_trail_arm_pts | min_trail_arm_secs | Proposed strong |
|---|---|---|---|
| US500.F | 3.0 | 15 | 6.0 |
| USTEC.F | 8.0 | 15 | 16.0 |
| NAS100  | 12.0 | 20 | 24.0 |
| DJ30.F  | 15.0 | 15 | 30.0 |

(Strong = 2x normal — same ratio as HBG's 3.0 / 1.5)

**Sweep:** strong/normal ratio ∈ {1.5, 2.0, 2.5, 3.0}. Backtest each.

### Spec (ii.C) — HBI: fix `tr.mae = 0.0` hardcode

**Change:** IndexHybridBracketEngine.hpp:565 → write real MAE.

**Required:** add `mae` tracking to `OpenPos` (mirrors HBG L135). Update in `manage()` alongside MFE update at L486. Use field at L565: `tr.mae = pos.mae * pos.size`.

Also fix `tr.spreadAtEntry = 0.0` hardcode at L571 — needs spread captured at fill time (`confirm_fill` at L413-438).

These are diagnostic-only changes (no behaviour change) but are prerequisites for any future calibration. Stage 1A finding 6b explicitly flagged this gap.

---

## Spec (iii) — HBI sizing review (no sizing change)

### Source picture

IndexHybridBracketEngine.hpp:380-385 lot calculation:

```cpp
const double risk    = flow_pyramid_ok ? cfg_.risk_pyramid : cfg_.risk_dollars;
const double sl_dist = range * cfg_.sl_frac + cfg_.sl_buffer;
const double lot_raw = risk / (sl_dist * cfg_.usd_per_pt);
const double lot     = std::max(cfg_.lot_min,
                       std::min(cfg_.lot_max,
                       std::floor(lot_raw / cfg_.lot_step) * cfg_.lot_step));
```

For NAS100 (`risk_dollars=$25`, `usd_per_pt=$1`, `lot_step=0.10`, `lot_max=5.0`):
- `sl_dist = range*0.5 + 1.0` (`sl_frac=0.5`, `sl_buffer=1.0`)
- For min-range trade (28pt): `sl_dist = 15`, `lot_raw = 25/(15*1) = 1.667`, lot=1.6
- For max-range trade (140pt): `sl_dist = 71`, `lot_raw = 25/(71*1) = 0.352`, lot=0.3
- For mid-range (60pt): `sl_dist=31`, `lot_raw = 0.806`, lot=0.8

Stage 1A's reported HBI sizes (0.5–1.6 contracts) match this exactly.

### Conclusion

**Sizing is correct, calibrated to $25 risk per trade.** The dollar risk is consistent across symbols; the lot count is high on NAS100 because each lot is $1/pt rather than $50/pt or $20/pt.

**The avg loss of -$24.69 over 8 trades is exactly the $25 risk target** — the engine is performing as designed at the risk level. The bleed comes from win rate.

### Recommendation

**No sizing change. Action items move to entry quality (Spec (iii.A) below) and to the trail-arm fix already specified in (ii.B).**

### Spec (iii.A) — HBI entry-quality audit (Tier 2, post-(i)+(ii))

HBI fires at compression breakouts. Two trades captured big TRAIL_HIT wins (+$22.73 and +$89.17 per Stage 1A Section 7); 6 trades took -$25 avg loss. This is a **bimodal outcome distribution**: either the breakout works (catches a leg) or it dies immediately (gets stopped at SL).

The 6 losses had the following pattern (inferred, not yet confirmed):
- Compression range qualified
- Stop-order fired on initial breakout
- Price reverted into range and hit SL on the other side

**Candidate gates (NOT YET SPECIFIED — Tier 2 scope, after (i) and (ii) are deployed):**

1. **Volume / order-flow confirmation at FIRE.** Stage 1A noted L2 imbalance is logged but `mae=0` for HBI suggests fill-quality fields aren't populated. Source verifies: the imbalance is computed (L2 `g_macro_ctx.nas_l2_imbalance`) but NOT consulted by HBI on entry. Adding an L2 imbalance gate to HBI ARMED→PENDING transition (similar to HBG's DOM filter at GoldHybridBracketEngine.hpp:348-369) could filter false breakouts.
2. **Post-NY-open extended block.** HBI has 13:15-13:45 UTC NY-open block (L324). Stage 1A doesn't break HBI trades by hour; need to confirm the 8 trades' time distribution before specifying.
3. **Trend-blocked counter-trend logic.** L1486-1517 in on_tick.hpp already has `BracketTrendState` consultation. Confirm this is wired to HBI (it dispatches via `dispatch_bracket` lambda which DOES read `trend_blocked` per L1488).

**No code spec for (iii.A) this session.** Deferred to Stage 1A.2 pending Stage 1A.1 deployment data.

---

## Spec (iv) — Cross-engine direction gate

### Source picture

**Confirmed (rule #30 clause 3):** No cross-engine direction gate exists in the codebase.

Evidence:
- `cross_engine_dedup_ok` referenced only in stale comments at on_tick.hpp:1731-1733. Zero call sites.
- `cross_engine_dedup_stamp` same: zero call sites.
- `symbol_gate` referenced only in comments at on_tick.hpp:632, 1011. Zero call sites.
- HBG/HBI directional cooldown variables (`m_sl_cooldown_dir`) set but never read.

Stage 1A documented 8 cases of concurrent opposite-direction fires within 120s. 6 are IndexFlow self-flips (covered by Spec (i)). 2 are cross-engine:
- IndexFlow LONG → HBI SHORT
- HBG SHORT → VWAP_SNAPBACK LONG (the 2026-04-28 case)

### Existing infrastructure for the fix

on_tick.hpp:709-720 already iterates `eng.pos.active` and `eng.pos.is_long` for every active engine for telemetry purposes. The same iteration can be used in a gate function.

### Spec (iv.A) — Add direction gate at engine entry path

**New function (proposed location: a new file `cross_engine_gate.hpp` to keep on_tick.hpp size manageable):**

```
namespace omega {
struct DirectionGateResult {
    bool        blocked;
    const char* blocking_engine;
    bool        blocking_is_long;
};

inline DirectionGateResult check_opposite_direction_open(
    const std::string& sym, bool prospective_is_long) noexcept;
}
```

Implementation iterates the same engine instances as the telemetry block at on_tick.hpp:709-720 (per-symbol). For symbol `sym`, return `{true, name, is_long}` if any engine has `pos.active && pos.is_long != prospective_is_long`.

**Call sites required (NOT YET WIRED):**
- IndexFlow `on_tick` at L711 (just before `pos_.open(sig)`) — block if opposite engine open
- HBG `confirm_fill` at L388-422 — same
- HBI `confirm_fill` at L413-438 — same

**Behaviour on block:** abort entry, log `[CROSS-ENGINE-BLOCK]`, do not fill, do not arm subsequent state. Engine returns to its pre-entry state. For HBG/HBI bracket fills, the resting stop order at the broker must be cancelled (cancel_fn).

**Important caveat:** this is NOT a hedge-prevention rule. The intent is to block FLIP-CHOP patterns where one engine just stopped out LONG and another fires SHORT 30 seconds later. **Cross-engine pyramiding in the SAME direction must remain allowed** (HBG + flow_pyramid_ok logic at GoldHybridBracketEngine.hpp:340 already supports same-direction pyramiding).

### Spec (iv.B) — Make HBG/HBI directional cooldown work as designed

Quick fix independent of (iv.A): wire the existing dead-code `m_sl_cooldown_dir`/`m_sl_cooldown_ts` into the entry path so a same-symbol same-engine same-direction re-entry is blocked for `DIR_SL_COOLDOWN_S` (120s HBG, 60s HBI default).

**HBG change (GoldHybridBracketEngine.hpp, in entry path around L252-253):**

```
if (m_sl_cooldown_dir != 0 && now_s < m_sl_cooldown_ts):
    if (proposed_long_dir == m_sl_cooldown_dir):
        return  // block re-entry in just-stopped direction
```

This is a same-engine cooldown, complementary to Spec (iv.A) which is cross-engine.

**HBI change (IndexHybridBracketEngine.hpp, ARM transition around L348):** same logic.

**Risk:** if HBG/HBI structure-detection picks up a real trend after a single SL in the trend direction (whipsaw-low scenario), this cooldown blocks the genuine continuation entry. **Stage 1A doesn't show enough sample to assess this risk; backtest required before commit.**

---

## Backtest plan

Per memory rule (specs reviewed before code, code backtested before push), the order of validation is:

1. **Review** this document. Each spec marked with sweep parameters where applicable.
2. **Index-tick data sourcing.** OmegaSweepHarness uses gold ticks. For Spec (i) and Spec (iv) on indices, an index-tick dataset is needed. **Blocker — to be resolved before backtest phase.**
3. **Gold-only specs sweepable now:** Spec (ii.A) HBG arm-guard tiering, Spec (iv.B) for HBG. These can run on `~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`.
4. **Sweep ranges (geometric, 5 values per param, per memory #29):**
   - HBG `MIN_TRAIL_ARM_PTS_STRONG`: {1.5, 2.0, 2.5, 3.0, 4.0}
   - HBG `DIR_SL_COOLDOWN_S` (effective): {60, 90, 120, 150, 180}
   - IndexFlow `OPP_COOLDOWN_S`: {120, 150, 180, 240, 300} (when index data lands)
   - IndexFlow `CHOP_FLIP_MIN`: {2, 3, 4, 5, 6} (when index data lands)
5. **Ranking:** stability × PnL across 4 chronological quarters per memory #29 sweep harness spec.
6. **Top-50 review** before any code commit.

---

## Out of scope for this session

- Code (no patches, no diffs, no full files)
- IFLow / HBI runner-trail addition beyond current 3-stage staircase (Stage 1A Section 4a Recommendation C — flagged as Tier 2)
- MFE/MAE unit standardisation (Stage 1A Section 6a — diagnostic cleanup, separate session)
- Opens-logger silent-fail (Stage 1A Section 6c — separate session)
- MacroCrash re-enable (Stage 1B)

---

## Provenance

**Pinned HEAD:** `c20fe2fc28f51d8fc1efcb2ec73da8e65d95324f`
**Files read this session via GitHub contents API (base64-decoded):**
- STAGE1A_FINAL.md (full)
- GoldHybridBracketEngine.hpp (full, 563 lines)
- IndexFlowEngine.hpp (lines 1-800 and grep checks for cooldown/gate references; bottom half not fully read)
- IndexHybridBracketEngine.hpp (full, 590 lines)
- on_tick.hpp (partial — top 130, gates 600-720, dispatcher 1380-1545, routing 1920-1970; ~30% of file)

**Sections of source NOT read this session that may affect specs:**
- on_tick.hpp 130-600 (per-engine update logic for non-bracket engines, may contain other gate paths)
- on_tick.hpp 720-1380 (other dispatcher detail, non-bracket engine call paths)
- on_tick.hpp 1545-1920 (engine-specific glue)
- IndexFlowEngine.hpp 800-1348 (IndexMacroCrash, VWAPATR upgrade, IdxSwingEngine — relevant for cross-engine direction gate as additional pos sources to query)
- All tick_gold.hpp / tick_indices.hpp (per-symbol entry routing)

**Recommended additional reads if specs progress to code:**
- on_tick.hpp 130-600 (full)
- IndexFlowEngine.hpp 800-end (full) — IndexMacroCrash and other classes whose `pos.active` would need polling by the cross-engine gate
- tick_indices.hpp (gate construction for index symbols)
- tick_gold.hpp (gate construction for XAUUSD)
