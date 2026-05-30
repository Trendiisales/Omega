# Handoff — Omega edge discovery (S39, 2026-05-30)

## Mission for next session
1. **FIND NEW EDGES — operator is adamant gold has more than we've surfaced.**
2. **Audit DISABLED engine settings (gold-first) for missed edges** — the
   leading hypothesis is **abs-point calibration drift**: gold ran $2400→$4543,
   any engine with absolute-price thresholds calibrated at old prices is
   silently mis-gated (see memory `feedback-abs-pt-calibration`: "$2400→$4700
   silently disabled half GoldEngineStack"). A disabled engine killed by stale
   config ≠ no edge. Re-check, recalibrate to current price, re-test.
3. Continue building the validated new-edge engines (spec below).

## Read first (do NOT re-derive — already done)
- `outputs/EDGE_PIPELINE_S39_TRUSTED_SYSTEM_2026-05-30.md` — the full edge
  pipeline result: methodology, 14 WF survivors, 6-block stress, cost-stress,
  long/short, param-plateau, round-2 bracket/family hunt. THE core artifact.
- `outputs/NEW_EDGES_WIRING_SPEC_S39_2026-05-30.md` — what's already wired live
  vs the faithful build plan for new engines + the warm-seed checklist.
- `outputs/ANALYSIS_S39_LIVE_EDGE_AUDIT_2026-05-30.md` — VPS shadow-ledger
  audit; SHADOW-mode reality (no real money since 05-12); promotion roster.
- `docs/DISABLED_ENGINES_INVENTORY.md` — the disabled-engine list + re-audit
  pipeline (START HERE for task 2).
- Memory: `omega-edge-pipeline`, `omega-live-edge-audit`,
  `feedback-abs-pt-calibration`, `feedback-harness-fidelity`,
  `feedback-cpp-first-research`, `chimera-harness-optimism`.

## State (all on origin/main, HEAD 7a259082; VPS deployed + verified)
- VPS binary `1b33d11`, mode **SHADOW** (zero real orders — operator switched
  LIVE→SHADOW 05-12, "honest backtest across the month"). London VPS:
  `trader@185.167.119.59:2222`, repo at `C:\Omega`, deploy = `.\OMEGA.ps1 deploy`.
- Shipped this session: P0 IndexSwing ATR-relative ema_sep gate (deployed);
  `backtest/edge_pipeline.cpp` (WF harness, `--blocks/--cost-mult/--side`,
  finer grids, bracket+Keltner+VolExp families); `tools/deflated_sharpe_gate.py`
  (DSR, live + `--cells` modes).

## What is TRUE about edges (validated 4 independent ways)
Live shadow + quant research + WF backtest + every stress test all converge:
**the only cost-robust edge in 77GB of data is trend/breakout (Donchian,
Keltner, vol-expansion) on H1–H4, on XAU / GER40 / NSX.** Bedrock = **XAU 4h
Donchian N=20** (passes WF, 6/6 regime blocks, 2× cost, smooth N=15-40 plateau,
independent-harness fidelity, long-robust). Intraday 5m / scalp / mean-rev all
DIE on cost-stress. The XAU Donchian complex (4h N15-40 + 1h N30-55 wide-bracket
+ Keltner) is ALREADY wired + live in shadow (XauTrendFollow4h cells 0/3,
XauTrendFollow1h cells 0/1).

## Why the operator thinks gold has more edges — the open lead
We have only surfaced TREND edges on gold. The disabled GoldEngineStack
sub-engines (session_momentum, intraday_seasonality, vwap_snapback,
vwap_stretch_reversion, dxy_divergence, asian_range — all `g_disable_*=true` in
`include/globals.hpp` ~L313-330) were killed on bleed evidence — BUT several
predate the big gold rally and may be calibration-drifted, not edge-less.
`g_disable_bracket_gold` IS genuinely dead (thorough 2yr re-audit PF 0.705, see
its globals.hpp note). The others need the abs-pt-calibration check FIRST, then
a real-class re-audit. This is the most likely source of "missed gold edges."

## Recommended approach for task 2 (disabled-engine audit)
1. For each disabled gold sub-engine, grep its config in `engine_init.hpp` +
   the engine .hpp for ABSOLUTE price thresholds (pt/usd/price values in the
   100s–1000s, NOT ATR/pct/period). Compare the calibration era to $4543.
2. Where drifted, recalibrate to current price (or convert to ATR-relative,
   the drift-proof fix — same pattern as P0 IndexSwing this session).
3. Re-validate via the PRODUCTION engine class (NOT edge_pipeline's simplified
   fixed-bracket sim — fidelity gap is real, see spec doc) through the canonical
   re-audit pipeline in `DISABLED_ENGINES_INVENTORY.md` §Re-audit (real-class,
   tape-time shim, cost-subtracted, walk-forward, PF≥1.2 n≥500 both halves).
4. For NEW edge discovery, extend `edge_pipeline` further: more gold-specific
   families (opening-range/session breakout confined to London–NY overlap,
   pullback-continuation, multi-TF trend-confirm), and finer brackets. Then
   deflate with `deflated_sharpe_gate.py --cells` and 6-block/cost stress.

## New-engine build queue (per NEW_EDGES_WIRING_SPEC, one per session)
1. GER40 4h RSI-extreme (#2 edge, both-sides robust) — build a GER40 ensemble
   mirroring `Us30EnsembleEngine::_sig_rsi_extreme_h1()` (RSIReversalEngine is
   XAU-hardcoded, no clean reuse). Narrow param ridge → fragile, ship shadow.
2. NSX 4h vol-expansion (tier-2, new family).
3. GER40 1h Donchian N=15 (tier-2).
Each MUST follow the CLAUDE.md Engine Warm-Seed Mandate (seed CSV + `[SEED]`
boot line + dispatch + register + canary + OmegaBacktest build + deploy verify).

## Hard rules (learned/confirmed this session)
- **edge_pipeline is a SIMPLIFIED sim** (fixed ATR brackets, both-sides, bar
  touch-fills) ≠ production engines (signal/channel exits, often long-only,
  spread/ATR/cost gates). Use it to FIND candidates; re-validate via production
  class before promoting. (The XAU 1h case proved the gap.)
- **Don't rebuild what exists.** This session repeatedly found infra already
  present (vol-targeting, cluster caps, the XAU Donchian engine) — check first.
- **No multi-engine big-bang.** Repo has been burned (2026-05-20 zero-signal,
  2026-05-30 MSVC build fail). One engine/session, full warm-seed, canary +
  OmegaBacktest build green before commit.
- Backtest is optimistic; SHADOW ledger is the operator's trusted signal.
- Run `bash scripts/mac_canary_engines.sh` before any engine-header commit.

## Validation watch (Monday, market reopen)
- IndexSwing WR should lift off 46% baseline (ATR gate filters marginal
  crossovers). Re-pull `C:\Omega\logs\trades\omega_trade_closes.csv`, re-run
  `tools/deflated_sharpe_gate.py`.
- Dashboard showed HEALTH "2F 4W" — glance at `C:\Omega\logs\startup_report.txt`
  for the 2 fails.
