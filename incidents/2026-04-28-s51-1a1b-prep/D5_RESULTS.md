# S51 Stage 1A.1.b — D5 Sweep Results & Engine Pathology Audit

**HEAD at audit:** `d6ed7ba8813c0199d93934e8b23ae49c6762b49c` (post-Option C build-system fix)
**Sweep run HEAD:** same — d6ed7ba8 was the binary that produced these results
**Run date:** 2026-04-28 (Mac, OmegaSweepHarness X3, 1006.1s wall, 154,265,439 ticks)
**Status:** This file is the canonical record of D5 results, engine pathology findings, and proposed (UNAUTHORISED) fixes. Read this before any future engine-fix session.

> **NO ENGINE CODE WAS MODIFIED IN THIS WORK.**
> All "Recommended fix" sections below are *proposals* requiring explicit
> per-rule authorisation before any code change. The *only* file shipped
> in the commit that adds this memo is this memo itself.

---

## TL;DR

The sweep produced clean, well-formed output. Read of the data shows:

| Engine | Best score | Best n_trades | Verdict |
|---|---:|---:|---|
| **HBG** | +0.4032 | 44 | **Genuine but tiny edge.** 70.5% WR at 1:1 RR is real signal. Two grid-edge clusters indicate optimum is *outside* swept range. |
| **EMACross** | 0.0000 | 0 | **15 of top combos fire 0 trades.** Once trades fire, every combo loses money. ~20% of grid is degenerate (`fast_period >= slow_period`). |
| **AsianRange** | +0.0393 | 2 | **Engine fires 1-2 times in 24 months across all top combos.** Live equivalent simulated 382 trades / 2yr — **>100x mismatch**. Bug is harness-side. |
| **VWAPStretch** | -1.8205 | 2230 | **0.5% WR over thousands of trades.** TP almost never hits. Logic-level pathology, not just param-tuning. |

**Headline conclusion:** D1 (`MIN_TRAIL_ARM_PTS_STRONG`) and D2 (`DIR_SL_COOLDOWN_S`) are HBG-only live-engine modifications. Of the 4 engines swept, only HBG produced usable signal — and it's edge-clipping at grid boundaries, not near a clear interior optimum. Adding more sweepable params to a strategy whose existing 5 already produce a tiny micro-edge is unlikely to be the highest-value next move. Running an **expanded HBG-only grid** that walks off the current edges (D6, recommended below) is a higher-leverage move than D1/D2 right now.

For the three broken engines, the bugs are all reproducible and almost certainly fixable. **No fix is being shipped this turn.** Each is a code-level change that needs explicit authorisation per the user's standing rules.

---

## Numerical scale caveat (essential)

The harness measures PnL in `XAUUSD price-points × 0.01 lot` — the minimum-lot baseline. To convert the harness numbers to dollars: multiply by 100 (XAUUSD contract = 100 oz; 0.01 lot = 1 oz; 1 price-point on 1 oz = $1).

So HBG's top combo `total_pnl=0.47` = ~$47 over 24 months at 0.01 lot. Scale to 1.0 lot = ~$4,700. Scale to 5.0 lot = ~$23,500. EMACross's worst combo `total_pnl=-27.82` = ~$2,782 lost over 24 months at 0.01 lot. None of these are interpretable as strategy P&L until size + risk parameters are scaled.

The sweep is ranking **per-unit-lot edge**, not strategy P&L. That's the right design for parameter discovery. But don't read the raw numbers as dollars.

---

## Engine 1: HBG — usable signal, optimum outside the grid

### What we see
- Top-50 by score range from +0.40 down to +0.07.
- Top combo (`#343`): `min_range=6.0, max_range=25.0, sl_frac=0.25, tp_rr=1.0, trail_frac=0.25` → 44 trades, 70.5% WR, score 0.4032.
- 70.5% WR at 1:1 RR is **real edge** (break-even at 1:1 is 50% WR; +20pts above is meaningful).

### Three signal-degradation issues (not bugs, but observations)
1. **Trade counts too low for confidence.** Best combo: 44 trades / 24 months = 1.8 trades/month. Several top combos have n_trades < 10. Standard-error analysis on a 70% WR with n=10 gives ±15pts at 95% CI — could just as easily be a 55% WR. Real conclusions need n ≥ 100.
2. **Grid-edge clustering at `trail_frac=0.25`.** Of the top 35, **33 have `trail_frac=0.25`** — the lower bound of the geometric grid. The optimum may be at `trail_frac < 0.25`, which the sweep cannot see.
3. **Identical-result cluster at `combos #295-300, #199, #248, #21, #344, #297`.** All show 35 trades, 60% WR, total_pnl=0.09, score=0.0721. They differ only in `trail_frac`. Identical results across `trail_frac` values means **the trail logic literally never fires** at those settings — every position TPs or SLs before the trail engages. Effectively `trail_frac` is dead in those combos.

### Recommended next move (D6 — proposed, unauthorised)
**HBG-only re-sweep with extended grid.** Add 0.10/0.15/0.20 below the current `trail_frac` floor; add 0.6/0.7/0.8 above `sl_frac=0.5`; add 30/40 above `max_range=25`. Hold the others at top-combo values. This stays inside the X3 pairwise design and well under the 2k template-instantiation OOM ceiling. Result: tells us whether HBG's edge is genuinely interior or just a grid-boundary artefact.

### D1 / D2 status
- **D1 (`MIN_TRAIL_ARM_PTS_STRONG`):** the sweep just showed `trail_frac` already barely fires at current settings. Adding a "strong" tier on top of a feature that's effectively dormant in most combos will produce more dormancy, not more edge. **Deprioritise until D6 result.**
- **D2 (`DIR_SL_COOLDOWN_S`):** an engine firing 1.8 trades/month is already cooldown-limited by the bracket logic itself. Adding directional cooldown reduces an already-small trade count further. **Deprioritise until D6 result.**

---

## Engine 2: EMACross — strategy concept may be unsuitable; some grid is structurally degenerate

### What we see
- 15 top combos fire **0 trades** (score = 0.0000 sorts above all negatives). These are not "good"; they're idle.
- Once trades fire, every combo loses. Worst: combo #183, 4,255 trades, 45.9% WR, total_pnl = -27.82 (~$2,782 / 24mo at 0.01 lot).
- Several "top" combos have `fast_period=9, slow_period=8` — **a "fast" MA slower than the "slow" MA**. Degenerate by construction.

### Two findings

**Finding E1 — Degenerate grid points are not constrained.**
The harness instantiates `EMACrossT` with `FAST_PERIOD_T` and `SLOW_PERIOD_T` independently from a geometric grid each. There is no compile-time or runtime check that `FAST_PERIOD_T < SLOW_PERIOD_T`. The default is `FAST=9, SLOW=15`, so the geometric range maps to FAST in {4.5..18} and SLOW in {7.5..30}. There are intervals where FAST > SLOW. ~20% of the grid is structurally invalid.

**Finding E2 — All non-zero combos lose money.**
Of the ~390 combos that fire trades, every single one has negative `total_pnl`. The strategy concept (EMA cross + RSI gate + ATR-scaled SL) does not produce edge on XAUUSD at any of the swept parameter combinations. Possible causes:
- The 5 swept params miss the actual edge region (e.g. very long EMAs, very loose RSI, wide SL — the live `EMACrossEngine` in `include/EMACrossEngine.hpp` may have different defaults that *do* produce edge).
- The strategy is concept-incompatible with XAUUSD tick characteristics (mean-reverting microstructure beats trend-following EMA crosses on noisy tick data).
- The harness `EMACrossT` may have drifted from `EMACrossEngine` in a behaviour-relevant way (NOT YET CHECKED — diff against live source needed).

### Code-level reasoning

The class is at `include/SweepableEngines.hpp:956-1240`. Key observations:
- L991-998 (`rsi_allowed`): hardcoded RSI dead-bands at 30-35, 35-40, 50-55, 65-70 — these reject ~40% of all RSI states regardless of swept `RSI_LO_T` / `RSI_HI_T`. Even at "optimal" sweep settings, the engine rejects most of the time.
- L1001-1003 (`hour_allowed`): blocks h=5,12,13,14,16,17,18,19,20 — that's 9 hours/day blocked, leaving 15 hours available. London + NY active hours are mostly blocked.
- L1080: `if (std::fabs(_ema_fast - _ema_slow) > GAP_BLOCK) return;` where `GAP_BLOCK = 0.30`. After a clean cross, the gap typically *expands* — by the time other gates pass, the gap may be > 0.30 and the entry is blocked.

### Recommended fix (proposed, unauthorised)
**Three-step diagnostic before declaring the engine dead:**
1. Add a degenerate-grid filter to harness so `FAST >= SLOW` combos are not instantiated (or score-zeroed).
2. Diff `EMACrossT` (harness) against `EMACrossEngine` (live, `include/EMACrossEngine.hpp`) to confirm no behavioural drift. **If the live engine is functionally different, re-port HBG_T-style.**
3. Run a diagnostic sweep with much wider param ranges (FAST in {3..30}, SLOW in {30..200}, looser RSI, looser GAP_BLOCK) to determine whether *any* setting produces positive edge. If none, EMACross is concept-dead on XAUUSD ticks.

---

## Engine 3: AsianRange — harness reproduces 1/100 of live trade frequency

### What we see
- All top-50 combos fire 1-2 trades total over 24 months.
- Live `AsianRangeEngine` (in `include/GoldEngineStack.hpp:1149-1255`) has a **simulated** result documented at L3903: `382T | WR=49.7% | $279/2yr | Sharpe=1.60`.
- The harness produces ~1% of the live engine's trade frequency. **This is not a strategy bug; it's a harness-vs-live drift bug.**

### Investigation done

I diffed `AsianRangeT` (harness, `SweepableEngines.hpp:248-342`) against `AsianRangeEngine` (live, `GoldEngineStack.hpp:1149-1255`). They are line-by-line nearly identical. Both:
- Build asian_hi/lo during 0-7 UTC
- Fire on break of asian_hi+BUFFER (long) or asian_lo-BUFFER (short)
- Allow one long + one short per day
- Use 600s cooldown
- Apply session and spread guards

**The classes themselves are functionally equivalent.** The difference must be in what they're being fed or in the surrounding harness path.

Three candidate root-cause locations checked:

1. **Time source — verified correct.** `sweep_now_sec()` (L179-185) reads `omega::bt::g_sim_now_ms` when `OMEGA_BT_SHIM_ACTIVE` is defined. `run_asian_sweep` calls `omega::bt::set_sim_time(r.ts_ms)` at `OmegaSweepHarness.cpp:923` per tick. ✓
2. **GoldSnapshot construction — verified consistent.** `SnapshotBuilder` (`OmegaSweepHarness.cpp:480-549`) builds a snapshot per tick with `bid`, `ask`, `mid`, `spread`, `vwap`, `volatility`, `session`, `vwap_day` derived from tick timestamp via `gmtime_r`. The fields the live engine consumes (`session`, `mid`, `bid`, `ask`, `spread`, `is_valid()`) are all populated correctly. ✓
3. **Session classification — likely culprit.** `SnapshotBuilder::session_for_hour` (L491-497):
   ```
   h ∈ [0,7) → ASIAN
   h ∈ [7,12) → LONDON
   h ∈ [12,16) → OVERLAP
   h ∈ [16,21) → NEWYORK
   else → UNKNOWN
   ```
   `AsianRangeT::process` rejects `s.session == UNKNOWN` at L286.
   
   **For h ∈ [21,24) → UNKNOWN.** That's 3/24 = 12.5% of ticks blocked.
   
   Combined with the engine's own time-window check (`h ≥ 7 AND h < 11`, FIRE_END_H=11) — only 4/24 = 16.7% of ticks reach the firing branch.
   
   Cross-multiply: live engine fires 0.52 trades/day. Harness fires 0.003 trades/day. **170× mismatch.** Session/UNKNOWN gating cannot explain this alone — both classes have identical gates.

### Most likely root cause: the harness is NOT calling `sweep_now_sec()` correctly within engine instances

**Hypothesis:** the OmegaTimeShim defines a header-replaced `std::chrono::system_clock::now()` and `std::time(nullptr)` at compile-time, but the engine's `last_signal_s_ = -3700` initialiser at `SweepableEngines.hpp:268` uses a literal — not the shim. On first signal, `now_s = sweep_now_sec()` returns ~1.7e9 (sim time). `elapsed = 1.7e9 - (-3700) = ~1.7e9 seconds`. Cooldown passes.

But — wait, that is correct behaviour. So that's not it.

**Other hypothesis:** the `last_day_ = -1` initialiser at L263 plus the daily reset at L291-297 should fire on the very first tick the engine sees (because `yday != -1` always on the first tick). At that point asian_hi_=0, asian_lo_=1e9 (just reset). If the first tick is in firing hours [7,11), the L307 check (`asian_hi_ <= 0 || asian_lo_ >= 1e8`) blocks the signal — correct.

**Realised gap:** I cannot identify the specific bug from static reading. Need a runtime diagnostic.

### Recommended next move (D7 — proposed, unauthorised)
**Add diagnostic counters to `AsianRangeT` and rerun a one-engine targeted sweep:**
- Counter for ticks_seen
- Counter for ticks_with_session_unknown
- Counter for ticks_in_asian_session (h<7)
- Counter for ticks_in_fire_window (7≤h<11)
- Counter for ticks_with_valid_range (rng ∈ [MIN_RANGE_T, MAX_RANGE_T])
- Counter for ticks_breaking_high
- Counter for ticks_breaking_low
- Counter for cooldown_blocks
- Counter for already_fired_blocks

Then run **only** the default-param combo for 1M ticks and dump the counters. The mismatched counter will reveal the bug. Cost: <50 lines of new code in `AsianRangeT`, ~5 minutes to run, decisive.

---

## Engine 4: VWAPStretch — fundamental logic problem, not just bad params

### What we see
- All top-50 combos have **negative PnL**.
- Top combo (`#1`): SL=20, TP=55, COOLDOWN=300, SIGMA=2.0, VOL=40 → 2,230 trades, **0.5% WR**, total_pnl = -4.37.
- WR < 1% on thousands of trades is structural — TP almost never hits.

### Why a 0.5% WR is structural

VWAPStretch trades **mean reversion**: enter SHORT when price stretches above VWAP by ≥ 2σ, expecting reversion to VWAP. The `entry` is at `s.bid` (for short) or `s.ask` (for long).

With SL=20 ticks ($0.20) and TP=55 ticks ($0.55):
- After entering short at +2σ above VWAP, price needs to drop by **$0.55 to hit TP**.
- Or rise by **$0.20 to hit SL**.
- $0.20 vs $0.55 in **opposite directions** — the SL is much closer in price terms.
- For mean-reversion, this is a **bad trade structure**: price typically continues stretching by another 0.5σ before reverting; that's enough to hit a $0.20 stop.

### The actual logic flaw

At `SweepableEngines.hpp:447-449`:
```cpp
const double z = (s.mid - s.vwap) / sigma;
z_ = z;
if (std::fabs(z) < SIGMA_ENTRY_T) return noSignal();
```

Then at L451: `if (!is_decelerating()) return noSignal();`

`is_decelerating()` (L382-389) checks if the last 5 mids have less price-difference variance than the previous 5 mids. The intent: only enter when price is "slowing down". But the check requires a `slow > 0.01` threshold (L388) — **on tick-level data with 0.01 spread granularity, this threshold is essentially always satisfied**, making the deceleration check trivial.

**The deeper problem:** entering at +2σ above VWAP is structurally late. By the time you're at 2σ, the move has typically run; momentum continues for another 0.5-1σ before reversion. With SL=20 ticks and SL << TP in points, you stop out 99% of the time during that continuation phase.

### Recommended fix (proposed, unauthorised — diagnostic + experiment)

**Two-step approach:**

1. **Diagnostic instrumentation.** Add counters for:
   - Number of times `z > 2.0` reached (how often does the engine arm?)
   - Distribution of `z` at exit (do positions exit on a TP-hit at lower z, or on SL with z still extreme?)
   - Time-to-exit distribution

2. **Two structural experiments:**
   - **Experiment A:** Reverse the SL/TP relationship. SL > TP is unusual but for mean-reversion entries against a stretched move, you may need a wider SL to survive momentum continuation, with a tighter TP at VWAP.
   - **Experiment B:** Move the entry threshold lower (`SIGMA_ENTRY = 1.0` or 1.5 instead of 2.0). Earlier entry = less momentum left to continue against you.

These experiments are **harness-side parameter changes**, no live-code modification. Could be added to a D8 sweep with extended VWAPStretch grid.

---

## Aggregate next-step recommendation

| Step | Description | Risk | Auth required? |
|---|---|---|---|
| **D6** | HBG-only extended-grid re-sweep | None (harness-side params) | No — sweep grid changes only |
| **D7** | AsianRange diagnostic counter pass | Low (instrumentation only, no live code) | YES — adds new code to `AsianRangeT` |
| **E1-fix** | EMACross degenerate-grid filter | None (harness-side) | No — harness logic |
| **E2-diag** | EMACross diff vs live + diagnostic sweep | Low (instrumentation) | YES — adds counters + may diff against live |
| **D8** | VWAPStretch SL/TP/SIGMA experiment | None (harness-side params) | No — sweep grid only |

D6 is the lowest-risk, highest-information move. **Does HBG have edge that holds when the grid extends past current boundaries?** Run that first.

If D6 confirms HBG edge exists in the interior of an extended grid, **D1/D2 are deprioritised indefinitely** in favour of expanding HBG's own param space.

If D6 shows HBG's edge collapses when the grid extends, then we have a different conversation: HBG's "edge" is a curve-fit artefact at grid boundaries, and the entire S51 sweep program needs rethinking.

---

## Memory rule deltas (for `memory_user_edits` action when ready)

1. **Add:** `D5 baseline sweep ran 2026-04-28 at HEAD d6ed7ba8, 16.8 min, 154M ticks. HBG produced usable signal at grid edges (best total_pnl=0.47, score=0.40, 44 trades). EMACross/AsianRange/VWAPStretch all broken: 0-2 trades or 0.5% WR. AsianRange harness reproduces ~1/170 of live trade frequency; bug is harness-side, not yet root-caused.`
2. **Add:** `Sweep CSVs measure PnL in price-points × 0.01 lot. Multiply by 100 to get $-equivalent at 0.01 lot baseline. Don't read raw harness numbers as dollars.`
3. **Add:** `D1 (MIN_TRAIL_ARM_PTS_STRONG) and D2 (DIR_SL_COOLDOWN_S) deprioritised after D5: HBG's trail logic already barely fires at current settings, and an engine firing 1.8 trades/month doesn't benefit from more cooldowns. D6 (HBG extended-grid re-sweep) is higher leverage.`
