# Session Handoff — 2026-05-15 part B (NZST)

Direct follow-up to part-A (SESSION_HANDOFF_2026-05-15a.md —
GoldRegimeRouter plan-only session). This session was the **multi-regime
gold backtest audit + S96 re-enablement**.

## TL;DR

1. **Critical methodology gap identified and closed.** All prior "Ultimate"
   backtests (SPX, GER40, FTSE, NAS100, Gold) tested ONLY trend-following
   momentum (EMA cross + drift + RSI + ATR geometry). VWAPReversion
   (mean-reversion), Bracket (breakout), FVG (fair-value-gap), ThreeBar
   (continuation), EmaPullback (pullback), and TrendFollow multi-cell
   (Keltner/Donchian/ER/ADX) were NEVER backtested. Engines were being
   disabled/promoted based on single-regime tests — S91 disabled 9 gold
   engines without testing their strategy shapes. S95 nearly disabled
   g_vwap_rev_sp (mean-reversion) based on a trend-follow backtest.

2. **S96 landed (2 commits).** Built 4 standalone Dukascopy tick-replay
   harnesses covering 5 distinct gold strategy shapes. All 4 validated
   on 154M-tick XAUUSD fresh tape (Mar 2024 - Apr 2026). Re-enabled 6
   gold engines with per-cell pruning based on evidence.

3. **S95 also landed this session** (SPX trend-follow disable + harnesses).
   S94 was already committed at session start.

4. **FTSE/UK100 trend-follow backtest:** PF=0.73, no edge. Harness created
   but no engine changes.

5. **Next session focus:** Build the same per-strategy-shape harnesses for
   the disabled INDEX engines (SPX, NAS100, GER40, FTSE) to check whether
   non-trend-follow strategies (VWAPReversion, IndexFlow, Bracket, PDHL,
   CandleFlow) have independent edge on each index.

## Commits this session

| Commit | Message | Key files |
|--------|---------|-----------|
| 22a4642 | S95: disable SPX engines + SPX/GER40 backtest harnesses | engine_init.hpp, backtest/Spx500*.cpp, backtest/Ger40*.cpp |
| 18135bc | S96: re-enable 6 gold engines with per-cell pruning | engine_init.hpp, XauThreeBar30mEngine.hpp, XauTrendFollow4hEngine.hpp, EmaPullbackEngine.hpp |
| d7896d8 | S96-harnesses: standalone gold strategy backtest harnesses | backtest/XauFvg*.cpp, XauThreeBar30m*.cpp, XauTrendFollow*.cpp, XauEmaPullback*.cpp |

origin/main now at d7896d8.

## S96 gold re-enablement details

| Engine | Shape | Config | OOS Evidence |
|--------|-------|--------|-------------|
| g_xauusd_fvg | FVG 15m mitigation | shadow_mode=false | PF=2.12, 223 trades, WR=52.9% |
| g_xau_threebar_30m | 3-bar continuation 30m | enabled=true, long_only=true | PF=1.24, 155 trades (long-only; short PF=0.84) |
| g_xau_tf_2h | Multi-cell 2h trend-follow | enabled=true, shadow_mode=false | PF=1.29, 826 trades (all 4 cells profitable) |
| g_xau_tf_4h | Multi-cell 4h trend-follow | enabled=true, cell_enable_mask=0x29 | PF=1.31, 502 OOS trades (pruned InsideBar/ER20/ADX_Mom) |
| g_xau_tf_d1 | Multi-cell D1 trend-follow | enabled=true, shadow_mode=false | PF=1.43, 79 trades (all 3 cells profitable) |
| g_ema_pullback | EMA9/21 pullback H4+H6 | enabled=true, cell_enable_mask=0x0C | PF=1.54, 91 OOS trades (H4+H6 only; H1/H2 dilute) |

### New engine code additions (config knobs only, no logic changes)

- XauThreeBar30mEngine.hpp: bool long_only field + gate on _evaluate_signal()
- XauTrendFollow4hEngine.hpp: uint32_t cell_enable_mask field + gate on entry loop
- EmaPullbackEngine.hpp: uint32_t cell_enable_mask field + gate on on_h1_bar/on_tick dispatch

## Engines still disabled — the next-session audit targets

### Index engines disabled without multi-regime testing

These were disabled by S94/S95 based on trend-follow-only backtests, or
disabled earlier without any backtest. Each needs per-strategy-shape
harnesses to determine if non-trend-follow strategies have independent edge:

**SPX (US500) — tick data at ~/Tick/SPXUSD/:**
- g_vwap_rev_sp (VWAPReversion, mean-reversion) — WRONGLY targeted by S95
  based on trend-follow backtest. Needs its own mean-reversion harness.
- g_trend_pb_sp (TrendPullback) — S95 disabled, trend-follow OOS failed.
  Correct disable.
- IndexFlowEngine x SP instance — check if disabled or shadow
- BracketEngine x SP instance — check status

**NAS100/USTEC — tick data location TBD (check ~/Tick/duka_ticks/):**
- g_vwap_rev_nq (VWAPReversion) — S68 disabled for bleed, but based on
  trend-follow evidence. Needs mean-reversion harness.
- g_trend_pb_nq — S94 disabled, consolidated into Nas100ShortEngine.
- g_ustec_tf_5m — S68 disabled. Trend-follow engine.
- g_ustec_tf_htf — S94 disabled, replaced by Nas100ShortEngine.
- IndexFlowEngine x NQ instances — check status

**GER40 — tick data at ~/Tick/GER40/ (JForex format, EET timezone):**
- g_vwap_rev_ger40 — currently ENABLED (state A, S63 active). Leave alone.
- g_trend_pb_ger40 — disabled, not live-validated.
- IndexFlowEngine instances — check status
- BracketEngine instances — check status

**FTSE (UK100) — tick data at ~/Tick/GBRIDXGBP/ (JForex format, EET):**
- Ftse100UltimateBacktest showed PF=0.73 for trend-follow. No FTSE-specific
  engines currently running.

### Strategy shapes needing index harnesses (next session)

Mirror what was done for gold this session:

1. **VWAPReversion harness** (mean-reversion) — test on SPX and NAS100.
   Engine uses VWAP extension + reversion entry, NOT trend-following.
2. **IndexFlow harness** (4 instances: SP/NQ/US30/NAS100) — determine
   strategy shape, build harness.
3. **Bracket harness** (breakout/range-compression) — test on each index.
4. **PDHL harness** (prev-day-high-low reversion) — test on each index.
5. **CandleFlow harness** — determine strategy shape, build harness.

### Tick data availability

| Instrument | Path | Format | Size |
|---|---|---|---|
| XAUUSD | ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv | Dukascopy ts_ms,ask,bid | 4.93 GB, 154M rows |
| SPXUSD | ~/Tick/SPXUSD/ | JForex .csv files | Previously validated |
| GER40 | ~/Tick/GER40/ | JForex EET .csv files | Previously validated |
| GBRIDXGBP | ~/Tick/GBRIDXGBP/ | JForex EET .csv files | Previously validated |
| NAS100 | Needs locating — check ~/Tick/duka_ticks/ or separate | Unknown | Unknown |

## Other pending work (lower priority than multi-regime audit)

- **S95 correction**: g_vwap_rev_sp.enabled=false in S95 is WRONG — it
  was disabled based on trend-follow evidence. The VWAPReversion harness
  for SPX will determine the correct action.
- **S66-followup-2**: GUI position sources for remaining engines (part-J).
- **GoldRegimeRouter** (part-A plan): design memo at
  outputs/GOLD_REGIME_ROUTER_DESIGN_2026-05-15.md. Deferred until
  multi-regime audit is complete.

## Important lessons from this session

1. **Never disable an engine based on a backtest of a DIFFERENT strategy
   shape.** Each strategy shape needs its own harness.

2. **Per-cell pruning is powerful.** TrendFollow 4h: 1.18->1.31 OOS.
   EmaPullback: 1.26->1.54 OOS. ThreeBar: 1.08->1.24 OOS.

3. **FVG is the strongest independent gold edge found.** OOS PF=2.12 with
   negative decay. Was sitting disabled in shadow mode the entire time.

## Pre-commit checklist for next session

Per CLAUDE.md: cmake --build build --target OmegaBacktest -j green on Mac.
git diff shows only intended changes. Comment blocks read before any
LOSS_CUT_PCT / BE_ARM_PCT / BE_BUFFER_PCT / enabled change.

## Stash state

Empty — no stashed changes.

## Files modified this session — final state

All committed and pushed to origin/main at d7896d8.
