# Session 12 Tier 1 — Minimal-Baseline Sweeps: CULL

## Status
**REJECTED.** None of the three Tier 1 candidate engines produced a
Phase-1-gate-passing baseline on 2yr XAUUSD tick data (PF ≥ 1.20, n ≥ 100,
PnL > 0, and a pattern showing signal rather than grid-noise).

Harness: `backtest/minimal_sweep.cpp` (commit `95ee875b`).
Data: `/Users/jo/tick/2yr_XAUUSD_tick.csv`, 111,599,267 ticks,
  2023-09-27 → 2025-09-26.

## Hypothesis (Session 11 brief)
That filter-heavy shadow engines in Omega may suppress a core signal
that would show edge if stripped to minimal baseline — the same way
H4RegimeEngine's 7 filters reduced its core signal to 0 trades in 2yrs
while MinimalH4Breakout (same signal, no filters) validates at PF 1.55,
+$1425 over 2yr.

**Verdict: hypothesis falsified for all three targets.**

---

## Engine 1 — MinimalH1Pullback (proposed new engine)

**Design:** H1 EMA(fast/slow) trend filter + pullback-to-EMA_fast touch,
weekend gate only. No ADX, no RSI, no EMA-separation gate, no extreme gate,
no spread gate. Grid 72 configs: EF∈{10,20}, ES∈{50,100}, pbTol∈{0.20,0.40},
SL∈{1.0,1.5,2.0}×ATR, TP∈{2.0,3.0,4.0}×ATR.

### Result
- **Best config:** EF=10 ES=50 pbTol=0.20 SL=2.0 TP=4.0
  - n=491, WR=37.3%, PnL=+$947.85, MaxDD=-$472.35, PF=**1.21**
- **30 of 72 configs lose money.** PF range 0.86–1.21.
- Comparison: MinimalH4Breakout best PF=1.55, PnL=+$1425, n=182.
  H1 produces ~2.7× more entries per config but lower-quality each.

### Decision
**CULL. Do not build MinimalH1Pullback engine.**
Reasoning:
1. Best PF = 1.21 is right at the Phase-3 cost-stress gate. Pessimistic
   spread+slip will push it below 1.20 OOS.
2. Best config is the PF-winner of 72 probes — classical overfitting risk;
   no cluster of adjacent configs produces similar edge (cfg #8 at same SL/TP
   with pbTol=0.20 is PF 1.06, cfg #17 with pbTol=0.40 is PF 1.08 — the
   edge doesn't generalize across the grid).
3. Second disproof of H1 timeframe (Session 11 Donchian at H1 also failed).
4. DD-to-PnL ratio 50% is too high for production size-discipline.

---

## Engine 2 — MinimalPullbackCont (session-gate-relaxed probe of PullbackContEngine)

**Design:** Tick-based port of `include/PullbackContEngine.hpp` with the
production h07/h17/h23 session gate REMOVED. Weekend gate only. Grid 54
configs: MOVE∈{15,20,30}pt, PB∈{0.20,0.33,0.50}, HOLD∈{300,600}s,
SL∈{4.0,6.0,8.0}pt. Hour mask = -1 (all hours).

### Result
- **ALL 54 configs lose money.** PF range 0.03–0.82.
- Best (by PnL): MV=15 PB=0.20 HOLD=300 SL=6.0, n=134, WR=37.3%,
  PnL=-$72.05, PF=0.82.
- 18 configs fire zero trades (PB=0.50 never hits before HOLD expires).

### Decision
**CULL probe. KEEP PRODUCTION ENGINE.**
Reasoning:
1. Without the h07/h17/h23 gate, the strategy bleeds across every
   parameter combination. The production engine's documented WR on h23
   (85.6%) and h17 (80.7%) cannot be recovered by removing the gate —
   the gate IS the edge, not a filter.
2. This is a rare clean negative result: Session 12's hypothesis asked
   whether the filter hides edge. Answer: no, the filter IS the edge.
3. `include/PullbackContEngine.hpp` remains untouched. Production
   deployment continues as-is in shadow mode until live-log validation
   confirms the backtest WR.

### Implication for future work
Do not apply the "minimal = no filters" approach to any engine where the
filters are **session-based** and the backtest explicitly documented
per-session metrics. Only strip filters that are **feature-based**
(ADX, EMA-sep, extreme gates, spread gates, etc.) where the backtest
did not justify each filter individually.

---

## Engine 3 — MinimalRSIExtremeTurn (relaxed-band probe of RSIExtremeTurnEngine)

**Design:** M1 bar-based port of `include/RSIExtremeTurnEngine.hpp` with
relaxed RSI bands, no DOM, no session slot, no cost-gate. Grid 162
configs: LOW∈{20,25,30}, HIGH∈{70,75,80}, sustained∈{2,3,4},
turn_pts∈{0.3,0.5,1.0}, SL∈{0.5,1.0}×ATR. Exit: RSI cross-back through
55/45, SL, or 300s timeout.

### Result
- **ALL 162 configs lose money.** PF range 0.03–0.57.
- Best (by PnL): LO=20 HI=80 SUS=4 TURN=1.0 SL=0.50, n=267, WR=8.2%,
  PnL=-$60.73, MaxDD=-$67.54, PF=0.55.
- WR pattern across the entire grid: 4.7–23%. Structurally losing.

### Decision
**CULL. Do not build MinimalRSIExtremeTurn.**
Reasoning:
1. The engine's header claims 75% WR / $20 PnL on a 2-day backtest
   (12 trades). **That sample size cannot validate any strategy.** The
   2yr result here is the statistically meaningful number.
2. WR consistently 5–23% across all 162 configs means the strategy is
   systematically loss-biased, not a parameter-tuning problem.
3. Tight ATR-based SL on M1 bars (ATR≈1.5pt × 0.5 = 0.75pt stop, vs
   0.3pt spread) means stops are ~2.5× spread — nearly any tick noise
   kicks the position out. Widening SL to ATR×3 would not rescue this
   because the signal itself (RSI turn after sustained extreme) does
   not produce directional follow-through on XAUUSD at M1 scale.
4. **Recommendation:** Remove or disable `include/RSIExtremeTurnEngine.hpp`
   in a future session. Current status in engine stack should be
   confirmed (shadow-mode only, per header) before any live wiring.

---

## Overall Tier 1 Conclusions

| Engine                     | Configs | Profitable | Best PF | Verdict |
|----------------------------|---------|------------|---------|---------|
| MinimalH1Pullback          | 72      | 42/72      | 1.21    | CULL    |
| MinimalPullbackCont (all-hrs) | 54   | 0/54       | 0.82    | CULL probe, keep production |
| MinimalRSIExtremeTurn      | 162     | 0/162      | 0.57    | CULL    |

**MinimalH4Breakout (Session 11) remains the sole validated minimal-strip
engine** in Omega's history. Its edge (PF 1.55, +$1425/2yr) is confirmed
by contrast: none of three additional candidates clear the same gate.

### Follow-up actions for future sessions
1. **Tier 2 re-validations** (this is next): GoldFlow, DomPersist,
   BBMeanReversion, CandleFlow — already-culled engines, sweep on 2yr
   data with minimal baseline to confirm cull was correct.
2. **Tier 4 substrategy sweeps**: GoldEngineStack substrategies
   (CompressionBreakout, ImpulseContinuation, VWAPSnapback, LiquiditySweep)
   individually on 2yr data. These have never been isolated-sweep
   validated; current production metrics are from the stack aggregate.
3. **Disable / remove RSIExtremeTurnEngine**: its 2-day validation is
   invalid; its 2yr result is systematically losing.

### Artifact inventory (generated on Jo's Mac, not in repo)
- `h1_pullback_minimal_results.txt` + `_best_trades.csv` + `_best_equity.csv`
- `pullback_cont_minimal_results.txt` + `_best_trades.csv` + `_best_equity.csv`
- `rsi_turn_minimal_results.txt` + `_best_trades.csv` + `_best_equity.csv`

---

*Document generated 2026-04-24, Session 12 Stage 2, Claude.*
*Repo HEAD at cull decision: `95ee875b` (S12 Stage 1 harness commit).*
