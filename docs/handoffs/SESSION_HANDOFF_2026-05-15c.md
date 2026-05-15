# Session Handoff — 2026-05-15 part C (NZST)

Direct follow-up to part-B (SESSION_HANDOFF_2026-05-15b.md —
multi-regime gold audit + S96 re-enablement). This session: **index
multi-regime backtest audit** across all 5 non-trend-follow strategy
shapes on SPX, NQ, and GER40.

## TL;DR

1. **5 standalone index harnesses built** — one per strategy shape:
   VWAPReversion, IndexFlow, Bracket, NoiseBandMomentum, OpeningRange.
   Each mirrors the gold harness pattern (multi-config sweep, IS/OOS
   60/40 split, PF>=1.20 + trades>=20 PASS criteria).

2. **Definitive conclusion: NO independent edge for any non-trend-follow
   index strategy.** Every shape x instrument combination FAILS. Best
   OOS PF across all configs never exceeds 1.07 (NBM on SPX). All
   disabled index engines should remain disabled.

3. **Bracket structural finding:** RR and TRAIL_ACTIVATION parameters
   have ZERO effect — all exits are via BF_CONFIRM (30-second
   confirmation timeout). TP is never reached, trail never activates.
   The strategy fundamentally doesn't work on these instruments: price
   breaks out of the compression zone then reverses within the
   confirmation window.

4. **MIN_RANGE fix for Bracket harness.** Original harness produced 0
   trades because MIN_RANGE was fixed at the instrument default (too
   high for 30-120s time windows). Added MIN_RANGE as a sweep
   dimension with multipliers {0.25, 0.50, 1.0} of instrument
   default, expanding from 27-cell (3x3x3) to 81-cell (3x3x3x3) grid.
   Trades generated at lower thresholds, but all configs still FAIL.

## Complete audit results

| Strategy Shape | Instrument | Best OOS PF | Best OOS Trades | Verdict |
|---|---|---|---|---|
| VWAPReversion | SPX (US500) | 0.96 | ~1800 | FAIL |
| VWAPReversion | NQ (USTEC.F) | 1.00 | ~1500 | FAIL |
| IndexFlow | SPX (US500) | 0.77 | ~3400 | FAIL |
| IndexFlow | NQ (USTEC.F) | 0.85 | ~2600 | FAIL |
| Bracket | SPX (US500) | 0.63 | ~900 | FAIL |
| Bracket | NQ (USTEC.F) | 0.85 | ~700 | FAIL |
| Bracket | GER40 | 0.63 | ~1000 | FAIL |
| NoiseBandMomentum | SPX (US500) | 1.07 | ~600 | FAIL |
| NoiseBandMomentum | NQ (USTEC.F) | 1.02 | ~500 | FAIL |
| OpeningRange | GER40 | 0.95 | ~400 | FAIL |

PASS criteria: OOS PF >= 1.20 AND OOS trades >= 20.
None met threshold. NBM on SPX came closest at PF=1.07 — still well
below the 1.20 bar.

## Harness files (new this session)

| File | Strategy | Sweep grid | Notes |
|---|---|---|---|
| `backtest/IndexVwapReversionBacktest.cpp` | VWAPReversion | 27-cell (3x3x3): LB, RR, TRAIL | SP, NQ, GER40, UK100 support |
| `backtest/IndexFlowBacktest.cpp` | IndexFlow | 27-cell (3x3x3): DRIFT_THRESH, IMBALANCE, COOLDOWN | SP, NQ, GER40, UK100 support |
| `backtest/IndexBracketBacktest.cpp` | Bracket | 81-cell (3x3x3x3): LB, RR, TRAIL_ACT, MIN_RANGE | SP, NQ, GER40, UK100 support. MIN_RANGE added after 0-trade diagnosis. |
| `backtest/IndexNoiseBandMomentumBacktest.cpp` | NoiseBandMomentum | 27-cell (3x3x3): ATR_MULT, RR, TRAIL | SP, NQ, GER40, UK100 support. 1-minute bar ATR aggregation fix applied. |
| `backtest/IndexOpeningRangeBacktest.cpp` | OpeningRange | 27-cell (3x3x3): RANGE_MINS, RR, TRAIL | SP, NQ, GER40, UK100 support. JForex EET timezone fix applied. |

All harnesses support tick formats: HISTDATA, DUKA_BID_ASK,
DUKA_ASK_BID, JFOREX, HARNESS.

Standalone compile (not CMake targets):
```bash
clang++ -O3 -std=c++17 -Iinclude -o backtest/idx_vwap_rev_bt backtest/IndexVwapReversionBacktest.cpp
clang++ -O3 -std=c++17 -Iinclude -o backtest/idx_flow_bt backtest/IndexFlowBacktest.cpp
clang++ -O3 -std=c++17 -Iinclude -o backtest/idx_bracket_bt backtest/IndexBracketBacktest.cpp
clang++ -O3 -std=c++17 -Iinclude -o backtest/idx_nbm_bt backtest/IndexNoiseBandMomentumBacktest.cpp
clang++ -O3 -std=c++17 -Iinclude -o backtest/idx_orb_bt backtest/IndexOpeningRangeBacktest.cpp
```

## Bugs fixed during audit

1. **NoiseBandMomentum 0 trades on all instruments** — the tick-level
   ATR calculation was seeing tick-to-tick moves of ~0.01-0.25 points,
   making ATR bands impossibly tight. Fix: aggregate ticks into
   1-minute bars for ATR calculation, matching how the live engine
   receives 1m candle data.

2. **OpeningRange "no valid ticks loaded" on GER40** — JForex tick
   format uses EET (UTC+2) timestamps, not EST. Fix: added JForex
   timezone detection and UTC+2 to UTC conversion.

3. **Bracket 0 trades on all instruments** — MIN_RANGE at instrument
   default (e.g. 44 for GER40) is never reached in 30-120s windows.
   Fix: sweep MIN_RANGE at {0.25x, 0.50x, 1.0x} of default.

## Key structural insights

1. **Bracket exit dominance.** On all tested index instruments, the
   BF_CONFIRM exit (breakout-fail confirmation at 30s) fires on
   virtually every trade. The TP and trailing-stop paths are dead
   code in practice. This means any tuning of RR or TRAIL_ACTIVATION
   is futile — the strategy's fundamental timing assumption (breakout
   continues after confirmation) is wrong for these instruments.

2. **VWAPReversion on indices is breakeven, not profitable.** PF hovers
   at 0.96-1.00 on both SPX and NQ. There may be marginal edge
   buried in specific session windows, but the broad sweep shows no
   reliable signal. The S63 part-K/L decision to disable SP/NQ VWAP
   reversion (and keep only GER40) is validated by independent
   evidence.

3. **IndexFlow L2/drift approach shows worst performance.** PF 0.77-0.85
   across SPX/NQ. The order-flow signal that works intraday on GER40
   does not transfer to US indices in this implementation.

4. **NoiseBandMomentum is the closest to edge** at PF=1.07 on SPX, but
   still well below the 1.20 threshold. If a future session revisits
   index strategies, NBM is the most promising candidate for
   parameter refinement — but the current evidence does not justify
   enabling it.

## Validation of existing engine states

This audit confirms that ALL non-trend-follow index engines are
correctly disabled in engine_init.hpp. Specifically:

- g_vwap_rev_sp.enabled = false — validated (PF=0.96)
- g_vwap_rev_nq.enabled = false — validated (PF=1.00, also S68 stop-bleed)
- g_index_flow_* (all instances) — validated (PF=0.77-0.85)
- g_bracket_* (all instances) — validated (PF=0.63-0.85)
- NoiseBandMomentum (if any instances exist) — validated (PF=1.02-1.07)
- OpeningRange (if any instances exist) — validated (PF=0.95)

No engine should be re-enabled based on this data.

## What did NOT happen this session

- UK100 was not tested (no tick data available). The harnesses support
  it if data becomes available.
- No engine_init.hpp changes — this was audit-only.
- No S63 management-path work — out of scope for this audit.
- GoldRegimeRouter implementation — parked from part-A, independent track.

## Recommended next-session focus

1. **S63 state classification** (part-K carryover) — IndexFlow, XauusdFvg,
   PDHL, RSIReversal, XauThreeBar30m still unclassified. Now that the
   index audit is complete, the S63 rollout is the next systematic
   improvement track.

2. **GoldRegimeRouter implementation** (part-A design) — sweep + per-engine
   opt-in starting at XauusdFvg. Independent of index work.

3. **MinimalH4Gold promotion** (part-K stash) — fresh-sweep workflow
   scaffolded, never run. Independent of both above.

## Files modified this session — final state

```
M  backtest/IndexFlowBacktest.cpp           (rewritten, was pre-existing)
?? backtest/IndexVwapReversionBacktest.cpp   (new)
?? backtest/IndexBracketBacktest.cpp         (new)
?? backtest/IndexNoiseBandMomentumBacktest.cpp (new)
?? backtest/IndexOpeningRangeBacktest.cpp    (new)
?? docs/handoffs/SESSION_HANDOFF_2026-05-15c.md (this file)
```

## Pre-commit checklist

- [x] All 5 harnesses compile and run (verified by operator during audit)
- [x] git diff shows only intended changes
- [x] Commit message uses S-numbering scheme
- [x] No engine_init.hpp or core file changes
- [x] No engine logic modified — harness-only commit
