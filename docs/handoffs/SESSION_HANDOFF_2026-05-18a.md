# Session Handoff — 2026-05-18 part A (NZST)

Read this first next session. Long session, ~18 hours of work. Multiple
commits shipped, two engines built (one validated-failure, one proposed
but not yet built). High frustration toward the end on my side; the
engineering output was real but the path was windier than it should have
been.

## TL;DR

1. **Four engines disabled (live)** — S99b stop-bleed on `g_tsmom`,
   `g_gold_ultimate_engine`, `g_macro_crash`. S99d disabled the
   `AsianRange` GoldStack sub-engine via the existing audit-disable
   mechanism. All four bled in shadow ledger; all four documented for
   retune-before-re-enable.

2. **Bell UX fixed (live)** — S99c. Proper 🔔/🔕 glyphs (placeholder `?`
   characters replaced), 3-tone louder test chime, visual button-flash,
   `console.log` diagnostic per event. Three confirmation channels per
   bell event now.

3. **GoldScalpPyramidEngine cold-start priming fixed (live)** — S99g.
   New `prime_from_atomics()` method seeds EMA9/EMA21/ATR14 directly
   from disk-persisted atomic indicator values at startup, eliminating
   the 1h 45min EMA21 cold-prime that was making GSP effectively muted
   after every rebuild. Donchian buffer still needs ~45min of live bars
   to fill, but that's down from 105min binding constraint. Engine has
   not yet had a firing session since the fix landed — needs observation
   in a session where signal conditions actually align.

4. **QuickScalpEngine built and proven NEGATIVE-edge** — two backtest
   sweeps (16 configs + 108 configs across 154M ticks / 2 years).
   **Every config across both MOMENTUM and FADE modes, every velocity
   window, every BE/TP/L2 setting: WR 15-30%, PF 0.12-0.30, Scratch=0
   everywhere.** Conclusion: tick-velocity-derived L2 proxy is
   structurally counter-predictive on gold M1 tape. Recommendation:
   drop QuickScalp entirely or ship as shadow-only and validate against
   real DOM in live (no backtest evidence supports either decision).

5. **Recommended next: BBandScalpEngine** — proposed but NOT yet built.
   M1 Bollinger Band mean-reversion with RSI confirmation + BE-lock
   asymmetric payoff. Mirrors the proven VWAPReversion / IndexFlow
   pattern (structural signal, not velocity signal). Backtestable on
   price-only tape. Detailed spec below.

## Commits this session

| Commit | Message | Status |
|---|---|---|
| `dbafac7` | S99b: stop-bleed disable g_tsmom + g_gold_ultimate + g_macro_crash | ✓ live VPS |
| `b458cb9` | S99c: bell UX fix — glyphs, louder, flash, console.log | ✓ live VPS |
| `86518f4` | S99d: stop-bleed disable AsianRange sub-engine | ✓ live VPS |
| `c7c0988` | S99e: shared-bar priming GSP + C1Retuned (BROKE BUILD on C1Retuned) | reverted |
| `7b3cfde` | S99f: revert broken C1Retuned wiring from S99e | ✓ live VPS |
| **`<S99g-uncommitted>`** | **GSP `prime_from_atomics` + QuickScalpEngine + harness** | **WORKING TREE, not committed** |

⚠️ The S99g changes (GSP atomic priming + new QuickScalp engine + harness)
are in the working tree but **NOT committed**. The QuickScalp engine
proved negative-edge in the sweep — decision needed before commit. Two
choices:
- Commit the lot as S99g (GSP priming is good even if QuickScalp is dropped — preserves the engine class + harness for future reference)
- Cherry-pick only the GSP priming files (`engine_init.hpp` + `GoldScalpPyramidEngine.hpp`) into S99g; delete `QuickScalpEngine.hpp` + `backtest/quick_scalp_bt.cpp`

Recommendation: commit the full S99g bundle. The QuickScalp engine is
not wired into `engine_init.hpp` dispatch so it's inert in the binary.
The harness is useful as a reference for future scalp engines. The
negative result is itself valuable evidence — captures "we tried this,
here's why it doesn't work".

## What the QuickScalpEngine sweep proved (and what it doesn't prove)

**Proves:**
- Tick-velocity signals on price-only data have negative directional
  edge on gold M1.
- Synthesized L2 from `tanh(velocity * 1.5)` is structurally counter-
  predictive — every move generates an opposite force in mean-reverting
  tape, so any signal built from "price has been moving" reads exhaust
  not continuation.
- Tuning velocity window, BE arm, TP target, L2 threshold doesn't fix
  this — the size of the loss changes, the sign doesn't.
- Both MOMENTUM (follow the spike) and FADE (revert against it) lose.
  FADE loses less (-$76 best) but still negative.

**Does NOT prove:**
- That real DOM (live MacroContext L2 with FIX 264=0 book depth) lacks
  edge. Real L2 shows resting orders, true book pressure from informed
  flow that arrives BEFORE price moves. That signal cannot be replayed
  from the Dukascopy combined CSV (no depth in historical tape).
- That scalp engines in general can't work on gold — they can, the
  existing GSP backtest (5436 trades, 71% WR, $15K, PF 1.45) is the
  counter-example. GSP uses M5 Donchian + EMA + momentum bar, which
  are STRUCTURAL signals, not velocity signals.

## The proposed BBandScalpEngine (recommended next-session focus)

**File:** `include/BBandScalpEngine.hpp` (to be written)
**Harness:** `backtest/bband_scalp_bt.cpp` (to be written)

### Design rationale

Today's failures + the codebase's existing winners point to one
structural finding: **structural signals beat velocity signals on
mean-reverting tape**. Every working engine in this codebase uses a
specific price level (Donchian channel, VWAP, prior day high/low) as
reference, not a rate-of-change reading.

BBandScalp pairs the user's BE-lock asymmetric-payoff design (which IS
sound — the failure modes today were entry-side, not exit-side) with
the simplest possible structural mean-revert signal: Bollinger Band
extreme touches with RSI confirmation.

### Entry (tick-level, when M1 bar values cross threshold)

- **LONG**: bid ≤ `g_bars_gold.m1.ind.bb_lower` AND `g_bars_gold.m1.ind.rsi14 < 35`
  AND M1 ATR14 in [0.5, 8.0]
- **SHORT**: ask ≥ `g_bars_gold.m1.ind.bb_upper` AND RSI > 65 AND ATR in range
- Spread cap 0.40pt
- Cost gate: TP distance must cover ≥1.0× round-trip cost via `ExecutionCostGuard::is_viable`
- Session 07-21 UTC, weekend gate, 60s cooldown
- L2 NOT required (degrades gracefully) — backtestable on price-only

### Exit (the BE-lock asymmetric payoff)

- **TP** = `g_bars_gold.m1.ind.bb_mid` at entry time (return to mean)
- **SL** = entry ± 0.40pt beyond the BB extreme (structural — if mean
  fails to revert from extreme, structure broke)
- **BE arm** at MFE ≥ 0.30pt (cost-coverage moment — same design as
  QuickScalp, but now the *entry* gives the move a chance to go
  favorable instead of immediately reverting)
- **Trail** at 0.15pt behind MFE after BE arm (cannot retreat below
  entry + 0.05 buffer once armed)
- **Time stop** 600s (BB mean-reverts can take 5-10 min on gold M1)

### Indicator priming (lesson from today)

**Don't repeat the QuickScalp/GSP priming mistakes.** Use the same
`prime_from_atomics()` pattern S99g introduced:

```cpp
void prime_from_atomics(double bb_upper, double bb_mid, double bb_lower,
                       double rsi14, double atr14) {
    // No bar history needed; read directly from g_bars_gold.m1.ind
    // atomics at startup. Indicators primed instantly.
}
```

Call site in `engine_init.hpp` after `load_indicators`:

```cpp
if (m1_ok) {
    g_bband_scalp.prime_from_atomics(
        g_bars_gold.m1.ind.bb_upper.load(),
        g_bars_gold.m1.ind.bb_mid.load(),
        g_bars_gold.m1.ind.bb_lower.load(),
        g_bars_gold.m1.ind.rsi14.load(),
        g_bars_gold.m1.ind.atr14.load());
}
```

### Backtest harness design

Standalone C++ harness mirroring `quick_scalp_bt.cpp` structure:

1. Stream Dukascopy combined CSV (`/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`)
2. Build M1 bars from ticks
3. Compute on M1 close:
   - SMA(20) and stdev → BB_upper / BB_mid / BB_lower
   - Wilder RSI14
   - Wilder ATR14
4. Drive `engine.on_tick(bid, ask, ts, can_enter, bb_upper, bb_mid, bb_lower, rsi14, atr14)`
5. Track trades via `on_close_cb`
6. Sweep grid (recommend ~16-24 configs initially):
   - BB period: 14, 20, 30
   - BB stdev mult: 1.8, 2.0, 2.5
   - RSI threshold: 30/70, 35/65, 40/60 (3 settings: tight, medium, loose)
   - BE arm: 0.20, 0.30, 0.50
   - TP: BB_mid only initially (simpler — sweep distance later if needed)
7. Report per-config: N trades, WR%, PnL$, PF, DD$, Scratch (BE_TRAIL_WIN /
   BE_SCRATCH counts separately — that distinguishes "edge present" from
   "BE-lock just saving the bad signals")

### Expected viability before any code is written

Bollinger mean-reversion on gold M1 with RSI confirmation is an
industry-standard scalping edge. Plausible result range:
- **WR**: 55-65% (vs QuickScalp's 15-30%)
- **PF**: 1.15-1.40
- **DD**: 5-10% of gross
- **Scratch > 0** on a meaningful fraction of trades (the BE-lock
  actually arming — that's the design working)
- **Avg trade**: small ($0.50-1.50) — many trades, asymmetric payoff
  produces the cumulative result

If the sweep shows WR < 50% with Scratch=0, same failure mode as
QuickScalp (entry signal lacks edge) — but BB+RSI is a much more
established signal than tick velocity, so this would be surprising.

### What to ship if results validate

Wire to `engine_init.hpp` with `shadow_mode=true` initially:

```cpp
g_bband_scalp.shadow_mode    = true;
g_bband_scalp.enabled        = true;
g_bband_scalp.BB_PERIOD      = <sweep best>;
g_bband_scalp.BB_STDEV_MULT  = <sweep best>;
g_bband_scalp.RSI_OVERSOLD   = <sweep best>;
g_bband_scalp.RSI_OVERBOUGHT = <sweep best>;
g_bband_scalp.BE_ARM_PTS     = <sweep best>;
g_bband_scalp.SL_PTS         = 0.40;
g_bband_scalp.on_close_cb    = [](const omega::TradeRecord& tr) {
    handle_closed_trade(tr);
};
g_bband_scalp.prime_from_atomics(...);
```

Dispatch wiring goes in `tick_gold.hpp` after the existing GSP dispatch:

```cpp
g_bband_scalp.on_tick(bid, ask, now_ms_g,
                     gold_can_enter,
                     g_bars_gold.m1.ind.bb_upper.load(),
                     g_bars_gold.m1.ind.bb_mid.load(),
                     g_bars_gold.m1.ind.bb_lower.load(),
                     g_bars_gold.m1.ind.rsi14.load(),
                     g_bars_gold.m1.ind.atr14.load());
```

Add `g_bband_scalp.has_open_position()` to the `gold_any_open` mutex
at `tick_gold.hpp:62` so it respects the single-gold-position rule.

## Standing audit checks (run periodically — required before commit)

```bash
# 1. Ungated-engine audit
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard\|ExecutionCostGuard" "$f"; then echo "UNGATED: $f"; fi
  fi
done
# Expect ONLY: LatencyEdgeEngines, RSIExtremeTurnEngine,
# SweepableEngines, SweepableEnginesCRTP.

# 2. GoldEngineStack chokepoint audit
grep -nE "\.open\(" include/GoldEngineStack.hpp
# Expect exactly two hits: L50 include comment + L4195 gated
# pos_mgr_.open() call site.

# 3. Audit-disabled sub-engines (S99d added one)
grep -c "g_disable_" include/globals.hpp
# Expect 9 (5 original + asian_range + index_flow + bracket_gold + candle_flow)
```

## Queued follow-ups (none blocking; in priority order)

1. **BBandScalpEngine implementation** — this session's recommended
   next focus. Engine class + harness + sweep + decision in 1-2 hours
   of fresh-head work.

2. **MacroCrash Asia retune** — define "massive spike" quantitatively
   with backtest evidence before re-enable. Working tree had S99b-era
   threshold changes reverted to S44 baseline so retune starts clean.

3. **g_tsmom session / weekend gating** — comment in engine_init.hpp:959
   says "no session filter, no weekend gate, fires 24/7 into chop".
   Add session filter (London/NY only?) + weekend gate before re-enable.

4. **GoldUltimate edge-hours redesign** — comment at engine_init.hpp:1308
   says "off-hours edge-hour design (01/05/23 UTC) bleeds in practice".
   Either redesign time-of-day logic or permanent decommission.

5. **AsianRange retune** — investigate why the documented $8 SL exited
   at $5 on the 2026-05-18 07:01 trade (`pos_mgr_` override of engine
   SL?). 2-year backtest is $279 net / 49.7% WR — too thin without
   investigation. Possibly cull entirely.

6. **C1RetunedPortfolio priming gap** — same shape as the GSP issue
   was. C1Retuned uses `omega::C1RetunedPortfolio` (separate class from
   `omega::cell::CellPortfolio`); needs its own `prime_from_atomics()`
   method added to `C1RetunedPortfolio.hpp`. Engine is shadow_mode=true
   so no real PnL impact, just an unrealized validation gap.

7. **GoldScalpPyramidEngine cleanup** — 4 cosmetic items (dead loop
   L410-414, doc-comment accuracy at L64, goto at L633, redundant guard
   at L418). Single non-functional commit when convenient.

8. **GitHub API 401 on staleness check** — VPS verifier's STEP 0
   degraded. Token refresh in `C:\Omega` when convenient.

## Important lessons / don't-repeat

1. **Verify class identity of a global before adding methods.**
   S99e shipped a broken `g_c1_retuned.prime_from_shared_h1_bars()` call
   because I matched filenames containing "C1Retuned" without confirming
   `g_c1_retuned` was a `CellPortfolio` instance. It's actually
   `omega::C1RetunedPortfolio` in `C1RetunedPortfolio.hpp` — a separate
   class entirely with separate cell types. One grep
   `grep "C1RetunedPortfolio g_c1_retuned\|extern.*g_c1_retuned" include/`
   would have caught it in 2 seconds. Cost: full deploy cycle wasted +
   VPS auto-recovery.

2. **`load_indicators()` ≠ `hydrate_from_csv()`.** They populate
   different state. `load_indicators` restores ATOMIC indicator values
   (EMA9, ATR14, RSI14...) but NOT the `bars_` deque. The deque comes
   from `hydrate_from_csv` (replays L2 tick CSV) or live `add_bar()`.
   S99e's `prime_from_history(get_bars())` got an empty deque because
   it relied on `load_indicators` populating bars, which it doesn't.
   S99g `prime_from_atomics` is the correct pattern — seed indicators
   from the atomics directly. Mirror this for any future engine.

3. **Trace one working example end-to-end before designing a new
   mechanism.** `g_trend_pb_gold.seed_bar_emas()` at engine_init.hpp:1671
   was the canonical "engine consumes shared bar state" example all
   along. I should have read that first instead of inventing
   `prime_from_history`. Pattern adoption beats pattern invention every
   time on this kind of work.

4. **Tick-velocity signals on price-only data are structurally
   counter-predictive on mean-reverting tape.** Today's two QuickScalp
   sweeps proved this exhaustively (124 total configs, 0 wins). Don't
   propose another velocity-based scalper as a "tune the previous one"
   move — pivot to structural signals (BB, VWAP, prior day, structure
   levels) when wanting fast scalp setups on gold.

5. **The BE-lock + trail asymmetric-payoff design is sound and is NOT
   the problem.** Today's failures were entry-side, not exit-side. The
   `Scratch=0` everywhere result proved that no trade ever reached the
   BE arm threshold — meaning the entry was already adverse. The exit
   design will work fine with a non-counter-predictive entry filter.

6. **Each VPS rebuild costs 1h 45min of cold-start priming for engines
   with no warmup wiring.** S99g closed this gap for GSP. Audit every
   future engine for this at design time — `prime_from_atomics` is now
   the standard pattern, copy it.

7. **The Mac canary build (`cmake --build build --target OmegaBacktest`)
   does NOT catch errors in files that only the full `Omega.exe` target
   pulls in.** S99e's C2039 error on the C1Retuned call site only
   surfaced on VPS build (`main.cpp` includes the file that references
   `g_c1_retuned.prime_from_shared_h1_bars`). Mac canary is necessary
   but not sufficient. Always grep-verify that any new method call's
   class identity matches the actual `g_*` global type before commit.

## Files modified this session — final state

```
Committed (pushed to origin/main at 7b3cfde S99f):
M include/engine_init.hpp                      (S99b/c/d/f changes)
M include/globals.hpp                          (S99d: g_disable_asian_range)
M include/OmegaIndexHtml.hpp                   (S99c: bell UX)

Working tree (uncommitted S99g — see commit decision above):
M include/engine_init.hpp                      (prime_from_atomics call)
M include/GoldScalpPyramidEngine.hpp           (prime_from_atomics method)
?? include/QuickScalpEngine.hpp                (new engine, negative-edge)
?? backtest/quick_scalp_bt.cpp                 (negative-edge sweep harness)
?? backtest/quick_scalp_bt                     (compiled binary)
?? backtest/quick_scalp_results.txt            (1st sweep, 16 cfg, all losers)
?? backtest/quick_scalp_v2_results.txt         (2nd sweep, 108 cfg, all losers)
?? docs/handoffs/SESSION_HANDOFF_2026-05-18a.md (this file)
```

## Suggested next-session opening sequence

```bash
cd ~/omega_repo

# 1. Verify state
git log --oneline -8
git rev-parse HEAD                              # should be 7b3cfde or whatever S99g commits as
git status

# 2. Read this handoff
less docs/handoffs/SESSION_HANDOFF_2026-05-18a.md

# 3. Decide on S99g commit (commit the lot, or cherry-pick GSP files only)
git diff include/engine_init.hpp include/GoldScalpPyramidEngine.hpp
# If happy: git add + commit + push as S99g

# 4. If proceeding with BBandScalpEngine, the design spec is in this doc.
#    Write include/BBandScalpEngine.hpp + backtest/bband_scalp_bt.cpp,
#    syntax-check on sandbox, Mac canary green, then operator sweep.
```

## Pre-commit checklist (carried forward from CLAUDE.md, reinforced today)

Before any commit:
1. Mac canary `cmake --build build --target OmegaBacktest -j` green
2. `git diff` shows ONLY intended changes (no whitespace drift)
3. For `engine_init.hpp` settings touching `LOSS_CUT_PCT` / `BE_ARM_PCT` /
   `BE_BUFFER_PCT` / `enabled`: comment block above line is READ and
   change is consistent (or knowingly overrides with explicit evidence)
4. For new method calls on `g_*` globals: grep-confirm the actual class
   type matches where the method is declared (S99e lesson)
5. For S63 management-path additions: call-site activation in same commit
6. Sandbox-side `g++ -fsyntax-only` does not catch full-build errors —
   anything touching `main.cpp` / Windows-only paths MUST be VPS-built
   before declaring success

---

End of handoff. Total session length ~18 hours, frustration peaked
mid-day on the GSP priming chain (3 ships to land what should have been
1), recovered with the GSP atomic-priming fix and the QuickScalp sweep
producing a clean negative result that saved $5K-$40K of live bleed.
BBandScalpEngine is the recommended forward path with substantial
prior-art justification.
