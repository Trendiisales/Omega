# Disabled Engines Inventory  (S37-Z, 2026-05-28)

Source of truth: `include/globals.hpp` audit-disable block + grep across
`include/*.hpp` for `enabled = false` and `shadow_mode = true` pins.

Re-enable order: re-audit the engine against the linked evidence FIRST, then
flip the flag. Re-enable WITHOUT a fresh audit is a known failure mode (see
`g_disable_bracket_gold` re-enable + walk-forward kill 2026-05-28).

---

## globals.hpp `g_disable_*` flags  (engines off at dispatch sites)

| Flag | Default | Bleed evidence | Audit date | Engine class | Re-audit gate |
|---|---|---|---|---|---|
| `g_disable_candle_flow` | `true` | -3,967 pts / 4wk, 43.4% WR, -8.91 avg/trade | 2026-04-30 | `CandleFlowEngine` | n>=500 net-cost positive PF>=1.2 |
| `g_disable_bracket_gold` | `true` | -324 pts / 4wk, 12.8% WR + 2026-05-28 walk-fwd kill (PF 1.16 -> 0.71) | 2026-04-30 + 2026-05-28 | `omega::GoldBracketEngine` | Entry-logic redesign required (not retune) |
| `g_disable_index_flow` | `true` | -112 pts / 4wk across 4 index instances | 2026-04-30 | `omega::idx::IndexFlowEngine` x4 | n>=200 per-symbol, WF both halves positive |
| `g_disable_xauusd_fvg` | `true` | Never class-audited; S46 M5-FVG inline-reimpl had bar-extreme lookahead bias (+$33.5k standalone vs -$12.4k real class on 26mo XAU) | 2026-05-27 (S46) | `XauusdFvgEngine` | Real-class harness audit on 2yr corpus |
| `g_disable_session_momentum` | `true` | 53.3% WR but bleed dominated by negative tail trades (IMPULSE-regime session-open) | 2026-04-30 | GoldStack sub-engine | Tail-cut variant + retest |
| `g_disable_intraday_seasonality` | `true` | Sharpe=1.08 in sim, live edge collapsed (COMPRESSION/MEAN_REV t-stat bias) | 2026-04-30 | GoldStack sub-engine | New sim vs live divergence audit before any retest |
| `g_disable_vwap_snapback` | `true` | Sample never grew beyond a few trades despite repeat re-enables (MEAN_REV VWAP fade) | 2026-04-30 | GoldStack sub-engine | Trigger threshold relaxation + measurement |
| `g_disable_vwap_stretch_reversion` | `true` | Even with 2026-04-09 ewm_drift injection fix, live PnL net negative (2-sigma VWAP fade + decel) | 2026-04-30 | GoldStack sub-engine | Real-class harness on 2yr corpus |
| `g_disable_dxy_divergence` | `true` | Live trades insufficient to justify continued exposure (intermarket DXY vs XAU) | 2026-04-?? | GoldStack sub-engine | DXY feed liveness audit then re-test |
| `g_disable_asian_range` | `true` | -$5.28 first London fire + only $279 / 2yr at 49.7% WR (marginal edge) | S99d 2026-05-18 | GoldStack sub-engine | Retune in audit queue |

`g_disable_macro_crash` -- handled via `g_macro_crash.enabled=false` in
`engine_init.hpp` (not a globals flag). Audit: -10,849 pts / 4wk, 4.8% WR
(2026-04-30). Same re-audit gate as IndexFlow.

## Force-OFF at supervisor layer  (engines downstream of supervisor)

These are LIVE engines whose supervisors are force-off in `engine_init.hpp`
because the engine dispatch site is commented out. Evidence the supervisor
work was being wasted: S37-X observability fix 2026-05-28.

| Supervisor | Where dispatch is commented | Reason |
|---|---|---|
| `g_sup_sp` (US500.F) | `tick_indices.hpp:101-105` | SIM: SP breakout WR 31.6% -$105 |
| `g_sup_nq` (USTEC.F) | `tick_indices.hpp:440-445` | SIM: BracketEngine on indices, no edge |
| `g_sup_us30` (DJ30.F) | `tick_indices.hpp:667-669` | Same as SP |
| `g_sup_nas100` (NAS100) | `tick_indices.hpp:1030-1033` | Same |
| `g_sup_uk100` (UK100) | `tick_indices.hpp:944-947` | EU index breakout, no edge |
| `g_sup_estx50` (ESTX50) | `tick_indices.hpp:973-976` | Same |
| `g_sup_ger30` (GER40) | (no live dispatch site found) | Same |
| `g_sup_eurusd/gbpusd/audusd/nzdusd/usdjpy` | `tick_fx.hpp` never calls `sup_decision()` | FX BracketEngines pinned shadow (S8) |

Per-symbol BracketEngine instances (`g_bracket_*` for indices/FX) exist but
fire ZERO entries because of these dispatch comments + FORCE-OFF flags.

## Shadow-pin instances  (audit-disable adjacent)

Engines kept compiled + warm but `shadow_mode=true` hardcoded regardless of
`g_cfg.mode`. Re-enable requires explicit operator authorisation.

| Instance | Where pinned | Reason |
|---|---|---|
| `g_eng_eurusd/gbpusd/audusd/nzdusd/usdjpy` (5 FX BracketEngines) | `engine_init.hpp:143-157` | S8: 2026-05-06 GBPUSD LondonOpen -$45.50 trigger. Promote only after 2-week paper showing >=30 trades WR>=35% net positive. |
| `g_idx_macro_crash_*` (4 instances) | `IndexFlowEngine.hpp:816` | "NEVER set shadow_mode=false without explicit authorization" |
| ECE (EMACrossEngine) | `EMACrossEngine.hpp:5-21` (S18 SOFT CULL) | -$74 / 10d at 30% WR. Lift = set ECE_CULLED=false. |
| `g_idd_sp/us30/uk100` (new, S37-Z 2026-05-28) | `engine_init.hpp:157+` | First-30-trade pilot pin. Promote per-symbol after 30 shadow trades net positive. |

## S37 trail-audit tombstones  (header notes only, code unchanged)

Per `outputs/SESSION_HANDOFF_2026-05-27b.md`. These engines had a
TrendRider-style `stage_trail` audit and verdict was NEGATIVE; no flag
was flipped but a header comment was added so the next audit doesn't
re-discover the dead end.

* `g_donchian` -- TP 2.5R, multi-day D1 cells, trail cut at +0.5N kills winners
* `g_minimal_h4_us30`, `g_minimal_h4_ger40` -- TP 4*ATR, same family
* `g_xau_pullback_cont_d1`, `g_xau_swing_break_d1`, `g_xau_doji_rej_d1` -- D1 5*ATR
* `g_xau_outside_bar_d1`, `g_xau_turtle_d1` -- TP 3*ATR, tighter than pcH4
* `g_xau_stop_run_d1` -- TP 2*ATR (catastrophic predicted, untested)
* `g_xau_tf_{1h,2h,4h,d1}` -- multi-cell, TP 3-6*ATR; only EmaCross8_21 cell
  (bit 6, off by default) could survive trail; flagged for separate test.
* `g_eur_gbp_pairs` -- mean-rev z-out exit, trail conflicts by design

## Re-audit pipeline  (canonical, per `feedback-harness-fidelity`)

For every entry on the lists above, before flipping any flag:

1. **Real class only.** Use the production engine class via a harness like
   `backtest/bracket_gold_2yr_audit.cpp` -- NO inline reimpl
   (S46 lookahead bias).
2. **Tape time.** Force-include `backtest/OmegaTimeShim.hpp` in the harness
   so engine session/cooldown gates resolve against tape time, not wall
   clock.
3. **Cost subtraction.** Match `apply_realistic_costs` model in the harness
   (XAU 0.010% one-way slip, etc.).
4. **Walk-forward split.** First half train, second half test on
   IDENTICAL params. Edge that fails out-of-sample is regime artifact.
5. **Acceptance gate.** PF >= 1.2, n >= 500 net-of-cost, WF both halves
   positive (Sharpe >= 0.5 each). If only one half passes -> MARGINAL,
   ship shadow only.
6. **Flip + ship.** Update the bleed comment in `include/globals.hpp`
   audit-disable block with new evidence + commit hash. Re-audit BEFORE
   flip is the rule, not after.
