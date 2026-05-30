# Handoff — Omega S39b: harness fidelity + gold edge rethink (2026-05-30)

## State (all PUSHED to origin/main, HEAD 53c4c473; VPS untouched)
Everything this session is backtest/harness + `#ifdef OMEGA_BT_SHIM_ACTIVE`-guarded
or config-gated OFF — **the running London VPS binary is unaffected.** Nothing
deployed; no live behaviour changed.

7 commits pushed: 3fea9a00, a8b975e2, 5a02740e, 19daf8dc, fca88c13, 2042d82c,
c41859fb, 53c4c473.

## What was done (3 arcs)

### 1. Backtest harness fidelity — fixed every instance (valid/reproducible/correct)
- **Clock leak** (the big one): GoldStack sub-engines + GoldPositionManager gated
  fires/cooldowns on `std::chrono::steady_clock`/`system_clock`, which the sim
  shim does NOT redirect (only libc `time()`). 784 sim-days compress into ~840s
  wall → fire count == 840/cooldown_sec, engines looked "starved/edgeless" but
  were NEVER EXERCISED. Fixed at SOURCE in `include/GoldEngineStack.hpp` (guarded
  `#define steady_clock/system_clock OmegaBtClock`, paired #undef at EOF) +
  `BracketTrendState.hpp` (bias gate read wall-clock → was DEAD in every backtest).
- **Fill-price**: 4 legacy harnesses filled TP/SL at literal level (no spread
  cross, understated losses) — gold_scalp_pyramid_bt, IndexFlowBacktest,
  research_backtest, s33_revised. Fixed.
- **Determinism**: 4 midscalper sweeps used unseeded `rand()` → seeded srand(42).
- **goldstack_subs_audit**: sweep_size=0 + EWM-vol stubs fixed; added SESSION×HALF
  WF matrix. See [[feedback-harness-fidelity]] (3 documented bug cases now).
- **Verified-clean (false alarms, left alone):** edge_pipeline, open_orb, bb_*
  family all use SL-first (conservative) + cross-spread fills. An audit agent
  mislabeled SL-first as "optimistic" 6× — ALWAYS verify bias DIRECTION before
  "fixing". threebar30m synthetic ticks are documented-intentional.

### 2. Disabled GoldStack engines — proven genuinely edgeless
Once the clock fix let them fire (DynamicRange 7→23661, etc.), all 10 disabled
scalp/mean-rev subs are PF<1.05 over 2yr. The "abs-pt drift hid a gold edge"
thesis is CLOSED — they were never tested, and when tested have no edge. See
[[feedback-abs-pt-calibration]]. Only ember: VWAPSnapback Asia (regime-conditional).

### 3. Gold edge rethink — 3 robust trend-capture families found
`backtest/gold_characterize.py` (descriptive) + `backtest/gold_regime_edges.cpp`
(prod-fidelity: cross-spread fills, SL-first, WF split 2025-04, 6-block, cost-stress).
Gold = violently persistent bull (0.90 Bull persistence, 2.26x/2yr). 3 ROBUST
families (WF both halves + ≥5/6 blocks, 3x-cost-robust, param plateaus):
- **Vol-target Donchian N40** SL2.5-3.0: PF ~3.3, 6/6, lowest DD. (best PF)
- **Pullback-continuation** EMA20: PF ~2.7, +2098 (highest total), more trades —
  BEATS breakout-chasing.
- **Keltner** EMA50 k2.0: PF ~2.5, 6/6.
These are 3 UNCORRELATED entry mechanics on the same bull edge → ensemble.
DEAD (don't chase): NY-fade, vol-expansion, breakout-chase, tight dip-buy.
Regime-contingent/subsumed: day-of-week, overnight, Monday-gap.
CAVEAT: H1-sim → absolute PF~3 is harness-optimistic; robustness is trustworthy.
See [[omega-voldonchian-edge]].

### BUILT: XauTrendFollow1h vol-target + pyramiding (commit c41859fb)
The vol-target Donchian edge was ~80% already live (XauTrendFollow1h Donchian40
cell). Added vol-target sizing + pyramiding CONFIG-GATED, **default OFF** (legacy
behaviour byte-identical). Harness pyramid numbers: K2 lifts avg-win $79→$255
(3.2x), K3/step0.75 best expectancy ($63 vs $32), at higher DD/lower PF. Lot size
= pure linear risk dial (PF invariant). Added the engine to mac_canary_engines.sh.

## Next session — tasks (ranked)
1. **Activate vol-target Donchian in SHADOW** — uncomment the recommended block in
   `engine_init.hpp` (~L1462, g_xau_tf_1h): `use_vol_target=true; vol_target_unit=0.10;
   pyramid_max_adds=2; step=1.0; sl=3.0`. Rebuild, deploy to London VPS
   ([[omega-vps-connection]]), watch shadow ledger. Start K2. Mandatory: canary +
   OmegaBacktest build green + boot `[SEED]` line check.
2. **Add Pullback + Keltner cells to XauTrendFollow1h** (ensemble) — mirror the
   Donchian40 cell + the validated configs (pullback EMA20 pb0.5; keltner EMA50
   k2.0). One engine, full canary. Same vol-target/pyramid framework.
3. **Cross-symbol generalization** — run vol-target Donchian on GER40 + NSX H1
   (`/Users/jo/Tick/GER40_merged.h1.csv`, `NSXUSD_merged.h1.csv`). Needs per-symbol
   HS/vol_target calibration in gold_regime_edges.cpp. Memory says trend works on
   XAU/GER40/NSX → if it generalizes, that's a multi-instrument trend portfolio.
4. **Production-validate PF~3** — the harness is H1-sim-optimistic. Validate the
   live engine config via OmegaBacktest gold runner + shadow forward-test before
   believing the number / sizing up.

## Read first
- Memories: [[omega-voldonchian-edge]], [[feedback-harness-fidelity]],
  [[feedback-abs-pt-calibration]], [[chimera-harness-optimism]],
  [[feedback-cpp-first-research]], [[omega-vps-connection]].
- `backtest/gold_regime_edges.cpp` (the edge harness — extend for #2/#3),
  `backtest/gold_characterize.py`, `include/XauTrendFollow1hEngine.hpp` (the enhanced engine).

## Hard rules carried forward
- C++-first for any strategy (Python descriptive only).
- One engine/session, full warm-seed mandate, canary + OmegaBacktest green before commit.
- Backtest is optimistic; shadow ledger is the trusted signal.
- Verify audit-agent flags against source (bias direction!) before acting.
