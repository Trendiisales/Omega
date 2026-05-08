# AUDIT — S15 Engine Catalog (Omega)

Date prepared: 2026-05-08
Branch / HEAD: `main` @ `313b0aa06688169da84b77125bce06babfb64a2c`
Mode at audit time: `mode=SHADOW` (no live orders firing)
Audit scope: per user direction, restricted to the **handoff catalog** (~30 engines named in `HANDOFF_S15`). Engines observed in the shadow tape but not in the catalog are summarised in the Appendix only.
Deliverable contract: findings only — no code changes (per user preference "Never modify core code unless instructed clearly").
Companion file: [`AUDIT_S15_P0_FINDINGS.md`](computer:///Users/jo/omega_repo/AUDIT_S15_P0_FINDINGS.md) — same-day triage one-pager.

---

## PREAMBLE — Drift between handoff and reality

The S15 handoff was authoritative for *intent* but stale on detail. Discrepancies that affected the audit:

1. **Git HEAD has moved well past the handoff snapshot.** The handoff names commit `4827ad4` (S13) and a "S14 Donchian breakout fail-safe" as HEAD. Reality on `main` shows `audit-fixes-15` … `audit-fixes-31`, `cell-refactor-phase-1/2`, `fix(hbi): extend NAS100 NY-noise gate`, `fix/heap-corruption-engine-dispatch-2026-05-01`, plus the Tier-1 portfolio commits (`audit-fixes-22` Tsmom, `audit-fixes-23` Donchian, `audit-fixes-24` EmaPullback, `audit-fixes-27` TrendRider) all post-dating the handoff window. There is also a `backtest/audit_results/S17_AUDIT.md` already on disk — somebody has done a parallel pass.
2. **Shadow CSV schema is different from what the handoff predicted.** The handoff promised columns `id, symbol, side, engine, entryPrice, exitPrice, sl, tp, size, pnl, mfe, mae, entryTs, exitTs, exitReason, regime, atr_at_entry, spreadAtEntry, shadow`. Actual `audit_input/omega_shadow.csv` columns are `ts_unix, symbol, side, engine, entry_px, exit_px, pnl, mfe, mae, hold_sec, exit_reason, spread_at_entry, latency_ms, regime` — no `sl`, `tp`, `size`, `atr_at_entry`, `shadow` flag. Stats tables below follow the actual schema.
3. **The shadow CSV PRE-DATES the Tier-1 portfolios.** Tape covers 2025-01-24 → 2026-04-08; Tier-1 (Tsmom/Donchian/EmaPullback/TrendRider/C1Retuned) all shipped on or after 2026-04-29. **None of the five Tier-1 portfolios produced trades in this tape.** Their sections below are sourced from code review only — `Trades in shadow tape` is N=0 by construction, not by lack of fires. Treat the live-vs-backtest-WR comparison the handoff requested as a deferred item until a fresh CSV that covers post-2026-04-30 deployment is supplied.
4. **`audit_input/omega_shadow_signals.csv` was promised by the handoff but is not present** (only `omega_shadow.csv` and `omega_shadow_export.zip`). All gate-firing checks (RISK_OFF / max_concurrent / cooldown) below are derived from closed trades only.
5. **The shadow tape contains 25 distinct engine labels, only ~8 of which are in the handoff catalog.** Labels like `GoldFlowEngine`, `GoldSilverLeadLag`, `WickRejection`, `DXYDivergence`, `PYRAMID`, `XAUUSD_BRACKET`, `HybridBracketIndex/Gold`, `DomPersistEngine`, `VWAPStretchReversion`, `TickScalpEngine`, `BBMeanRev`, `IntradaySeasonality`, `AsianRange`, `LondonFixMomentum`, `MeanReversion`, `SessionMomentum`, `TurtleTick`, `CompBreakout`, `DynamicRange`, `MacroCrash`, `CandleFlowEngine` are visible but not all are in the handoff list. Per the user's scope decision, those non-catalog labels are not audited as standalone engines but appear in the Appendix tape stats so cross-engine signals (negative-hold-sec, RISK_OFF leaks) are still surfaced.
6. **`ts_unix` resolution sanity:** several rows in the tape have `hold_sec = -1774312273` — a known-bad pattern from a unit-mismatch on the dollar-stop close path. See P0 below.

These items are themselves audit findings and are referenced by P1/P2 entries in their respective sections.

---

## CRITICAL P0 — Dollar-stop close path: unit mismatch creates negative `hold_sec` and (pre-fix) immediate re-entry loop

**Root cause:** Pre-fix, the dollar-stop branch in `include/on_tick.hpp` passed `ds_now` (unix seconds, e.g. `1776088361`) where the engine's `force_close(...)` expected `now_ms` (unix milliseconds, e.g. `1776088361000`). The engine's ledger-record path then performs `tr.exitTs = static_cast<int64_t>(now_ms / 1000)` — when fed seconds-as-ms it produces `exitTs ≈ entryTs / 1000`, so `hold_sec = exitTs − entryTs ≈ −entryTs`. The cooldown register, `m_cooldown_until = now_ms + COOLDOWN_MS`, is also poisoned: with `now_ms` actually in seconds, the cooldown ends roughly 1 700 s in the past — i.e. immediately — enabling a same-tick re-entry storm. This is the same defect that produced the **2026-04-15 phantom-fire incident (61 entries / 41 sec / −$9 907)** referenced in the head of `omega_config.ini`.

**Fix point:** `include/on_tick.hpp:929-932` (`MacroCrash` block) now passes `ds_now_ms`:

> `// CRITICAL FIX: pass ds_now_ms (milliseconds) not ds_now (seconds).`
> `// ds_now in seconds caused cooldown to be set ~1700s in the past`
> `// (seconds treated as ms), allowing immediate re-entry loop.`
> `g_macro_crash.force_close(s_xau_bid, s_xau_ask, ds_now_ms);`

The `MacroCrashEngine::force_close` ledger record at `MacroCrashEngine.hpp:1043-1044` is now correct:

> `tr.entryTs = static_cast<int64_t>(pos.entry_ms / 1000);`
> `tr.exitTs  = static_cast<int64_t>(now_ms / 1000);`

**Open exposure (this is why it stays P0):**

The fix at `on_tick.hpp:929-932` lives only inside the `g_macro_crash.has_open_position()` branch. The shadow tape contains **48 negative-`hold_sec` rows** distributed across THREE engines:

| Engine               | Negative-hold rows | Period of incidents (ts_unix → UTC) |
|----------------------|--------------------|--------------------------------------|
| `MacroCrash`         | 42                 | clustered 2026-04-08 14:32–14:33 UTC |
| `CandleFlowEngine`   | 5                  | spread across early 2026             |
| `HybridBracketGold`  | 1                  | single instance                      |

The MacroCrash cluster is the textbook signature of the bug (and is what the Apr-21 fix addressed). The 5 rows on `CandleFlowEngine` and the single `HybridBracketGold` row indicate **the same unit-mismatch pattern is present in those engines' close paths** — the `on_tick.hpp` fix was not ported to them. `on_tick.hpp:948-979` shows GoldStack has its *own* dollar-stop block which uses `ds_now` (seconds) for log throttling but I could not, in this audit, locate where it ends up calling `force_close` for `CandleFlowEngine` / `HybridBracketGold` — that path needs to be traced and the same `ms`-not-`sec` fix applied.

**Detail and proposed fix scoping:** see `AUDIT_S15_P0_FINDINGS.md`.

---

# Tier-1 portfolios

Common context: all five portfolios are `enabled=true, shadow_mode=true`, `block_on_risk_off=true`, and use `phase1/signal_discovery/tsmom_warmup_H1.csv` as the warmup file. None of the five appear in the supplied shadow tape (CSV pre-dates ship date 2026-04-29 → 2026-04-30). The per-cell INI sub-sections (e.g. `[tsmom_h1_long]`) are explicitly **documentation-only** at runtime per `omega_config.ini:542-545` — flipping `enabled=false` in the sub-section will NOT disable the cell. Operators who don't read the INI comment carefully will be surprised. Tier-1 issues below assume that constraint.

## TsmomEngine
**Status:** active / shadow_only — 5 long cells (H1/H2/H4/H6/D1)
**Config source:** `omega_config.ini` `[tsmom]` L506-595, `include/TsmomEngine.hpp`, registered at `engine_init.hpp:2027-2031` with prefix `Tsmom_`.
**Trades in shadow tape (last 30 days):** N=0 (CSV pre-dates ship). Backtest baseline (`POST_CUT_FULL_REPORT.md`): H1 53.2 %WR / +$17 482, H2 55.1 %WR / +$12 952, H4 61.4 %WR / +$15 885, H6 57.8 %WR / +$13 380, D1 56.5 %WR / +$9 109. Total +$68 808 across 7 120 trades.
**Risk read:** Conservative profile post-fix. `risk_pct=0.005`, `max_lot_cap=0.02` (S12 tightened from 0.05 after a Tsmom_H1_long lost $23.65 on a 4.45 pt adverse move — config L514-521). `max_concurrent=5` across cells. Worst-case all-five-cells-SL ≈ 5 × 0.02 lot × 3.0×ATR × pt_value = ~3% portfolio drawdown. Live tape would have caught the prior 0.05 cap incident; 0.02 cap is well-clear.
**Issues flagged:**
  - [P1] MAE_EXIT cooldown gates re-entry on the *firing cell only*, not portfolio-level. If H1 and H2 cells fire MAE cascades on the same tape event, both can re-enter inside the per-cell cooldown window. `TsmomEngine.hpp` L408-425.
  - [P1] `TSMOM_TRAIL_ENABLED` defaults `false` (`TsmomEngine.hpp` L254-256). Backtest L242-246 says trail HURTS results by ~35%. The flag is correct; flag exists as a P1 because S8 re-validation against the live sim is not yet complete and the engine is one config flip from a known-bad state.
  - [P1] Per-cell INI sub-sections look configurable but are dead at runtime. `omega_config.ini:542-545` documents this but operators editing `[tsmom_h1_long] enabled=false` will not get the disable they expect. P1 because it is an operator-trap not a code bug.
  - [P2] Warmup CSV failure (`TsmomEngine.hpp` L830-833) silently falls through to cold start; lookback=20 means the H1 cell idles ~21 H1 bars before firing. No stderr warn.
  - [P2] Intrabar MAE_EXIT (`TsmomEngine.hpp` L483-509) fires before the hard SL check at L511-523. Order is correct, but the synthetic exit_px does not necessarily match a tradeable bid/ask — only the MFE/MAE log line records the intent.
**Quick-win improvements:**
  - [1h] On warmup CSV miss, log to stderr at `WARN` level and to the GUI status panel; do not start trading until operator acknowledges. Gates the silent cold-start.
  - [1h] Wire the per-cell `[tsmom_h*]` `enabled=` keys into a hot-reload path so the documentation-only sections stop being a lie.
  - [1 session] Add portfolio-level MAE-cascade cooldown: if any 2 of 5 cells trip MAE_EXIT inside `mae_cooldown_bars`, freeze the *whole* portfolio for that window.
**Defer:**
  - Re-run `phase1/signal_discovery/post_cut_revalidate_all.py` against fresh shadow tape (post-ship) to confirm S12 MAE_EXIT economics still hold the +$17 482 H1 baseline.

## DonchianEngine
**Status:** active / shadow_only — 7 cells (H2 long; H4 long+short; H6 long+short; D1 long+short)
**Config source:** `omega_config.ini` `[donchian]` L611-697, `include/DonchianEngine.hpp`, registered `engine_init.hpp:2037-2041` prefix `Donchian_`.
**Trades in shadow tape (last 30 days):** N=0 (pre-ship). Backtest baseline: 328 trades/yr, +$5 620 = 47% of unshipped post-cut edge.
**Risk read:** Single position per cell (drive_cell gate L611-629), `risk_pct=0.005`, `max_lot_cap=0.05`, `max_concurrent=7`. Worst-case 7-cell simultaneous SL ≈ 7 × 0.05 × 1.0×ATR ≈ 3.5% drawdown. S14 breakout fail-safe (`DonchianEngine.hpp` L289-310) caps invalidated breakouts on the next parent-TF bar.
**Issues flagged:**
  - [P1] S14 fail-safe runs on the next bar, but `on_tick()` intrabar SL/TP fills (L392-409) fire BEFORE the bar-level breakout check. A losing trade can be filled at the bar-close tick (after the entry bar but before the next bar's open) and the fail-safe then closes a fait accompli loss. Backtest matched this only if the simulator allows tick-level intrabar fills; if it modelled bar-close-only fills the live behaviour will be tighter than backtest.
  - [P2] `signal_cooldown_left_` (L275-276) decrements every bar regardless of position state. A signal that misfires while a position is open burns cooldown without producing a trade; the next eligible re-entry can then be outside the intended cooldown window. Drift vs. Python `sig_donchian` semantics on high-fire-rate windows.
  - [P2] Warmup CSV silent failure same pattern as Tsmom.
**Quick-win improvements:**
  - [1h] Replay the 2026-05-08 11h Asian Donchian H2 long loss against the S14 fail-safe to confirm the cap is ≤ 1 bar of loss as designed.
  - [1h] Reset `signal_cooldown_left_` to 0 (or the post-trade default) only when a trade actually opens. Aligns with backtest semantics.
**Defer:**
  - Validate signal_cooldown drift quantitatively once shadow tape covers ≥ 50 Donchian trades.

## EmaPullbackEngine
**Status:** active / shadow_only — 4 long cells (H1/H2/H4/H6, long-only)
**Config source:** `omega_config.ini` `[ema_pullback]` L709-770, `include/EmaPullbackEngine.hpp`, registered `engine_init.hpp:2042-2046` prefix `EmaPullback_`.
**Trades in shadow tape (last 30 days):** N=0 (pre-ship). Backtest baseline: 796 trades/yr, +$4 006/yr/unit.
**Risk read:** Single position per cell, `risk_pct=0.005`, `max_lot_cap=0.05`, `max_concurrent=4`. Cleanest of the Tier-1 set — `has_first_bar_` gate (L275) enforces a 2-bar minimum before any signal, EWM `prev()` is bar-stamped not tick-stamped (L151), and `on_tick()` does only intrabar SL/TP without touching the EMA state. No issues at the engine-logic level.
**Issues flagged:**
  - [P2] Warmup CSV silent failure same pattern as Tsmom/Donchian.
**Quick-win improvements:** None at the engine layer.
**Defer:**
  - Verify EWM warmup actually emits the 50+ EMA samples its header documents (L47-49).

## TrendRiderEngine
**Status:** active / shadow_only — 6 cells (H2 long+short; H4 long+short; H6 long; D1 long)
**Config source:** `omega_config.ini` `[trend_rider]` L799-862, `include/TrendRiderEngine.hpp`, registered `engine_init.hpp:2047-2051` prefix `TrendRider_`.
**Trades in shadow tape (last 30 days):** N=0 (pre-ship). Backtest baseline: 184 trades/yr, PF 2.0, +$19 633/yr at 0.05 lot baseline.
**Risk read:** Highest-risk Tier-1 by design. `risk_pct=0.040` (8× the others), `max_lot_cap=0.50` (10× the others), no fixed TP, no time exit — only stage trail and initial SL close trades. Sub-engine head-comment in `omega_config.ini:792-797` projects a worst-case 6-cell simultaneous-SL of ~24% portfolio drawdown ("recoverable, well clear of margin_call"). Code review concurs broadly, but flags a discrepancy below.
**Issues flagged:**
  - [P0] **Risk-sizing comment vs. code disagree.** `TrendRiderEngine.hpp` L489-496 narrates "worst case if all 6 cells hit initial-SL on the same day = 6% portfolio drawdown". The arithmetic at `max_lot_cap=0.50` × 6 cells × 1.5×ATR doesn't reach 6%; it lands closer to the INI-comment's 24%. The conservative narration in the engine itself is more dangerous than the realistic projection in the INI because it under-sells the risk to anyone reading only the engine. **Without a bounded shadow run (≥ 50 trades) we have no live data to ground-truth either number.**
  - [P1] Stage-trail (L272-293 `on_bar`, L381-402 `on_tick`) is tick-responsive but bar-counted. A position can have `pos_stage_=1` (tightened trail) at `pos_bars_held_=0`. Documented design but worth surfacing because operators reasoning about the trail expect bar-aligned stage transitions.
  - [P1] No post-RISK_OFF entry pause if a sequence of cells just took stops — `block_on_risk_off` only governs new entries during RISK_OFF; once regime returns to NEUTRAL, all six cells re-arm with no inter-cell cooldown. A whipsaw between RISK_OFF and NEUTRAL inside a single tape session can fire the full sizing twice.
  - [P2] Warmup CSV silent failure (L726-730). With `period=40` the cold-start window for D1 is ~40 days.
**Quick-win improvements:**
  - [1h] Drop `max_lot_cap` from 0.50 to 0.20 until the first 50 closed shadow trades broadly match backtest WR/PF (this is what was done for Tsmom). Reversible via INI hot-reload.
  - [1 session] Add a "first-N-trades sentinel": if the first 20 closed shadow trades have a worse WR than backtest by >10pp, auto-flip cells to `enabled=false` and emit a GUI alert.
**Defer:**
  - Conviction-tiered Kelly re-tuning of `max_lot_cap` once shadow data validates the backtest.

## C1RetunedPortfolio
**Status:** active / shadow_only — 4 cells (Donchian H1 long retuned; Bollinger H2/H4/H6 long)
**Config source:** `omega_config.ini` `[c1_retuned]` L463-483, `include/C1RetunedPortfolio.hpp`, no separate registry entry — wired alongside Tsmom/Donchian via individual cell snapshots.
**Trades in shadow tape (last 30 days):** N=0 (pre-ship). Backtest baseline: +74.12% / -5.85% MaxDD / PF 1.486 / Sharpe 2.651 / WR 55.2%. Walk-forward TRAIN/VAL/TEST all PASS.
**Risk read:** `risk_pct=0.005`, `max_lot_cap=0.05` (tightened from backtest 0.10 — config L470-472 explicitly notes this is pending 100-trade live validation). Halt logic (`C1RetunedPortfolio.hpp` L606, L642-649) trips on cluster_days ≥ 1 (all 4 cells closing losers same UTC day) or max_dd ≤ -7.5%. Halt blocks new entries; existing positions still managed.
**Issues flagged:**
  - [P1] Cluster-day boundary is UTC midnight (`C1RetunedPortfolio.hpp` L628). Cells close on different timeframes (H1 / H2 / H4 / H6) and may all close losers across an 18 h window that spans two UTC days — cluster detection misses this case. Session-based clustering (Tokyo / London / NY) would be more sympathetic to how losses correlate.
  - [P1] When `halt_tripped_` becomes true there is no auto-recovery window. Halt persists until human intervention. No alarm path documented; operator may not notice until daily review.
  - [P2] H2/H4/H6 are synthesised from H1 bars (`C1BarSynth::on_h1_bar` L111-127) using bid/ask mid. Wide-spread H1 bars produce mids that don't represent the true higher-TF close. Backtest used computed-on-bar mids; live difference is small but non-zero.
  - [P2] `max_lot_cap=0.05` tightening is INI-level; there's no auto-trigger to relax it once the 100-shadow-trade threshold is reached. Manual ratchet.
**Quick-win improvements:**
  - [1h] When `halt_tripped_` flips true, write a single-line entry to the GUI alert queue with timestamp + reason + cell PnL contributions.
  - [1 session] Validate observed cluster-day frequency against backtest expectation once the first 30+ days of shadow tape post-ship are available.
**Defer:**
  - Migrate cluster-day detection from UTC-day boundary to session boundary (Tokyo / London / NY).

---

# Tier-2 single-engine breakouts

## MinimalH4Breakout (XAUUSD)
**Status:** active / shadow_only.
**Config source:** `omega_config.ini` `[minimal_h4]` L433-446, `include/MinimalH4Breakout.hpp` L77-81, init at `engine_init.hpp:566-572`.
**Trades in shadow tape:** Not visible under this label (engine emits `MinimalH4Breakout` per init log; no occurrences in the 900-row tape). Either no entries fired in window or label is mapped differently.
**Risk read:** Pure Donchian, no filters. SL/TP via ATR multiplier. Weekend gate. OOS-validated PF 1.35.
**Issues flagged:**
  - [P0] **The `[minimal_h4]` INI section is dead config.** `engine_init.hpp:568-577` constructs the engine via `make_minimal_h4_gold_params()` (constructor defaults only). There is no `apply_engine_config(MinimalH4Breakout&)` overload in `engine_config.hpp`. Every key in the INI section (`donchian_bars`, `sl_mult`, `tp_mult`, `max_spread`, `timeout_h4_bars`, `cooldown_h4_bars`, `weekend_close_gate`, `long_only`) is read by no one. Operators editing the INI to tune the engine think they're tuning it; they're not.
  - [P1] Cold-start window: `donchian_bars=10` requires 40+ wall-clock hours of H4 bars to warm. `engine_init.hpp:908-916` seeds from `g_bars_gold.h4` if available; if cold the warning at L916 prints and the engine simply waits. Unlike `MinimalH4US30Breakout` (which has a CSV warm-load path at `engine_init.hpp:788-805`), the gold variant has no CSV fallback.
  - [P2] No live-vs-backtest validation hook; the only signal that the engine is healthy is "did a trade fire this week".
**Quick-win improvements:**
  - [1h] Add `apply_engine_config(MinimalH4Breakout&)` in `engine_config.hpp` reading `[minimal_h4]` keys. Unlocks INI tuning without recompile.
  - [1h] Add a CSV warm-restart path analogous to `MinimalH4US30Breakout::load_state` (L788-805 of engine_init.hpp) so first-deploy doesn't burn 40 h of wall clock.
**Defer:**
  - `long_only` toggle (S47 T4b) is feature-complete but gated behind the dead config above; revisit after the apply_engine_config fix.

## MinimalH4US30Breakout (DJ30.F)
**Status:** active / shadow_only.
**Config source:** `omega_config.ini` `[minimal_h4_us30]` L867-881, `include/MinimalH4US30Breakout.hpp` L99-103, init `engine_init.hpp:768-805`.
**Trades in shadow tape:** Not visible under this label.
**Risk read:** Self-contained H4 OHLC aggregator + Wilder ATR14 from raw tick stream (no `g_bars_us30` dependency). 27/27 backtest configs profitable, PF 1.54 / +$637 / 184 trades / 2y. Warm-restart via `logs/bars_us30_h4.dat`.
**Issues flagged:**
  - [P0] **Same dead-config bug as `MinimalH4Breakout`.** `engine_init.hpp:774-786` uses `make_minimal_h4_us30_params()` only — no `apply_engine_config(MinimalH4US30Breakout&)` overload. INI keys go unread, including `dollars_per_point=5.0` (which would need to change for any other DJ30-shaped index family).
  - [P2] `load_state` accepts files up to 8h old (`MinimalH4US30Breakout.hpp` L478, L569). H4 boundary is 4h. An 8h-old file is 2 H4 bars stale; tightens to 6h would be safer.
**Quick-win improvements:**
  - [1h] Add `apply_engine_config(MinimalH4US30Breakout&)` overload. Same shape as the MinimalH4 one.
**Defer:**
  - Tighten `max_age_sec` from 8h → 6h or document the 2-bar staleness assumption.

## BreakoutEngine (XAUUSD compression breakout family)
**Status:** active. Per-instrument CRTP variants. INI partly applied (`apply_engine_config` exists for some symbols; per-instrument constructor constants override the INI for `momentum_threshold` and `min_breakout_move_pct` per the `omega_config.ini:77-79` warning).
**Config source:** `omega_config.ini` `[breakout]` L81-92, `include/BreakoutEngine.hpp` L22-102.
**Trades in shadow tape:** Likely the source of the `CompBreakout` and `WickRejection` labels (16 + 2 = 18 trades, 43.8 % / 0 % WR respectively). Engine name in the tape doesn't exactly match the class name; treat as informational only.
**Risk read:** Phase machine FLAT → COMPRESSION → BREAKOUT_WATCH → IN_TRADE. ATR-decay compression, momentum gate, then volume confirmation.
**Issues flagged:**
  - [P2] Spread gate evaluated AFTER compression detected (L105-138). A bracket can arm in tight 0.02% spread and fire entry when spread re-widens to 0.05%+, breaking the cost model assumption (`cost = spread * 0.3` at L79).
  - [P2] `momentum_threshold` is symmetric for LONG and SHORT (L113-122). Volume expansion patterns aren't symmetric on indices (up-breaks expand, down-breaks contract). No short-specific gate.
  - [P2] `MAX_TRADES_PER_CYCLE` is read from INI but its enforcement point isn't visible in this header — likely upstream in `trade_loop.hpp` or `order_exec.hpp`. Cannot verify rate-limit correctness from this audit.
**Quick-win improvements:**
  - [2h] Move spread gate to the FLAT → COMPRESSION transition, not the BREAKOUT entry. Stops post-news compression-arm-then-fire failure mode.
**Defer:**
  - Audit `trade_loop.hpp` for `MAX_TRADES_PER_CYCLE` / `MAX_TRADES_PER_MINUTE` enforcement.

## BracketEngine (HBG / HybridBracket family)
**Status:** active. 8 instances: XAUUSD + 7 indices (US500/US30/NAS100/USTEC/GER40/UK100/ESTX50) + Brent. Apr-29 audit raised XAUUSD `min_range_pct` from 0.004 → 0.015 (was firing on noise below typical 0.22pt spread).
**Config source:** `omega_config.ini` `[bracket_gold]` L355-367 + per-symbol overrides `engine_init.hpp:1025-1115`, `include/BracketEngine.hpp` L74-241.
**Trades in shadow tape:** `XAUUSD_BRACKET` 28 trades, 39.3 %WR, net -$94.79; `HybridBracketGold` 10 trades 50 %WR +$20.75 (1 negative-hold-sec row — see P0); `HybridBracketIndex` 41 trades, 43.9 %WR, net -$184.14, mean hold 566s.
**Risk read:** Two-sided pending bracket above prior high / below prior low. First fill cancels the other. Multiple stage trails + breakout-fail / confirm gates. Per-symbol `MAX_SL_DIST_PTS` calibrated post-S22.
**Issues flagged:**
  - [P0 — see top of doc] HybridBracketGold contributes 1 row to the 48-row negative-`hold_sec` cluster. Inspect its dollar-stop close path the same way `MacroCrash` was fixed.
  - [P1] `CONFIRM_SECS` (e.g. 30 s on gold, L176) and `PENDING_TIMEOUT_SEC` (300 s, L136) overlap badly when the broker fill is slow. If `CONFIRM_SECS=30` but typical fill latency is 45 s, `BREAKOUT_FAIL_CONFIRM` fires on a position the engine already considered a fill candidate. NAS100 is special-cased to 45 s (`engine_init.hpp:1084-1087`) suggesting awareness but no formal rule. The HybridBracketIndex tape has 28 SL_HIT, 8 BE_HIT, 1 FORCE_CLOSE, 3 TRAIL_HIT — proportions consistent with confirm-window cuts dominating.
  - [P2] `MAX_SL_DIST_PTS` was calibrated on a 62-trade XAUUSD sample 2026-04-13 → 24. Sample is now 14 days old. Re-validation against the 2026-05-01 → 08 tape would be cheap insurance.
  - [P2] `WHIPSAW_OVERLAP_K` lockout (L193-196) compares new range to the *stopped* range only — not to multiple recent stopped ranges. A 3-pt range compressing 3 times in a 20-pt move can re-arm on a non-overlapping window each time and still chop. Lockout duration constant (`WHIPSAW_LOCKOUT_MAX_MS`) is not visible in the header — presumed elsewhere.
**Quick-win improvements:**
  - [1h] Set `CONFIRM_SECS >= P50(recent_fill_latency_ms / 1000) + 10s` per symbol; recompute weekly from shadow `latency_ms` column.
  - [2h] Re-validate `MAX_SL_DIST_PTS` on 2026-05-01 → 08 live tape; update per-symbol caps in `engine_init.hpp:1060-1114` if the tail of distribution has shifted.
**Defer:**
  - Formalise the multi-range whipsaw lockout (consider a rolling 3-stopped-range overlap test).

---

# Symbol-specific engines

`SymbolEngines.hpp` defines a CRTP family: SpEngine, NqEngine, OilEngine, NbmEngine (and the index/FX variants). Per-symbol constants are baked into constructors; INI sections at `[sp]` `[nq]` `[us30]` `[nas100]` `[oil]` `[brent]` `[fx]` `[gbpusd]` `[audusd]` `[nzdusd]` `[usdjpy]` `[eu_index]` carry overrides.

## SpEngine (US500.F)
**Status:** active. macro-gated, blocks Asia slot.
**Config source:** `omega_config.ini` `[sp]` L198-208, `include/SymbolEngines.hpp` L196-250.
**Trades in shadow tape:** Not visible under `Sp` / `SpEngine` / `US500*` engine label. (Note: shadow tape contains `IndexFlow` 17 trades on indices, possibly overlapping signals.)
**Risk read:** Compression breakout + VIX panic gate + ES/NQ divergence confirmation + L2 imbalance. RR ≈ 3:1.
**Issues flagged:**
  - [P2] `div_threshold=0.0060` is shared with NQ (L201, L255). ES and NQ correlations vary intraday (rotation periods desync); single threshold ignores that.
  - [P2] `session_slot` is updated every tick but the slot transition latency vs. the gate check (L227 / L278) means 1-2 Asia-session ticks can leak through near the slot boundary.
**Quick-win improvements:**
  - [1h] Per-symbol `div_threshold` keys (`sp_div_threshold`, `nq_div_threshold`) in the INI.
**Defer:** None.

## NqEngine (USTEC.F)
**Status:** active.
**Config source:** `omega_config.ini` `[nq]` L213-223, `SymbolEngines.hpp` L252-300.
**Trades in shadow tape:** Same caveat as SpEngine. Note `IndexFlow` runs the same symbols and shows WR 29.4% (below the handoff's 35% red flag).
**Risk read:** Mirrors SpEngine. Slightly looser compression threshold (vol_thresh 0.070).
**Issues flagged:**
  - [P2] Same `div_threshold` split issue as SpEngine.
**Quick-win improvements:** Per-symbol `div_threshold` keys (covered above).
**Defer:** None.

## OilEngine (USOIL.F)
**Status:** active. EIA window auto-blocked (Wed 14:25-15:00 UTC).
**Config source:** `omega_config.ini` `[oil]` L251-261, `SymbolEngines.hpp` (variant in same file).
**Trades in shadow tape:** Not visible under `Oil` label.
**Risk read:** Wider TP/SL (1.5%/0.5%), higher vol threshold, longer hold (1800s).
**Issues flagged:**
  - [P2] EIA window is hard-coded; if EIA shifts publishing time the gate is stale. No INI key.
**Quick-win improvements:**
  - [1h] Move EIA window to INI keys.
**Defer:** None.

## NbmEngine (NextBarMomentum) — see also Cross-asset section below
**Status:** mostly disabled. Only `GOLD_LONDON` instance enabled (`engine_init.hpp:1557`); SP/NQ/NAS/US30/Oil disabled L1551-1558.
**Config source:** `CrossAssetEngines.hpp` L1327-1400; `engine_init.hpp:1551-1564`.
**Trades in shadow tape:** No tape rows under `NBM` label.
**Risk read:** ATR-anchored breakout from a session-open price plus single-fire-per-session gate.
**Issues flagged:**
  - [P0] **GOLD_LONDON instance uses `OPEN_HOUR=13, OPEN_MIN=30` (NY open), not 07:00-08:00 UTC (London open).** The engine name says LONDON, the timing fires NY. `CrossAssetEngines.hpp` L1357.
  - [P2] Hardcoded 0.01 lot size, no vol-gate.
**Quick-win improvements:**
  - [1h] Fix `GOLD_LONDON` open hour to 07:00 UTC (or remove the session gate so it fires whenever the ATR-breakout signal triggers in London hours).
**Defer:**
  - Vol-aware sizing once enabled.

## IndexSwingEngine (instantiated as g_iswing_sp / g_iswing_nq)
**Status:** active. shadow_mode=true (locked).
**Config source:** `omega_types.hpp:308-309`, no INI section.
**Trades in shadow tape:** No trades visible under `IndexSwing` / `ISwing` labels.
**Risk read:** H1-close-vs-EMA9/50 swing breakout. Buffer/SL/TP hard-coded into constructor: SP `(8.0, 0.5, 0.5)`, NQ `(25.0, 1.5, 0.2)`.
**Issues flagged:**
  - [P1] **TP% asymmetry between SP (0.5%) and NQ (0.2%) is undocumented.** Same backtest? Hand-tuned post hoc? Without a comment, future operators won't know whether the asymmetry is signal or noise.
  - [P2] No INI section — every tweak requires recompile and restart.
**Quick-win improvements:**
  - [1h] Add `[index_swing]` INI section (`buffer_sp`, `buffer_nq`, `tp_pct_sp`, `tp_pct_nq`) plus `apply_engine_config(IndexSwingEngine&)` overloads.
  - [1h] Add a one-line code comment explaining why `tp_pct_nq < tp_pct_sp` (or admit it's empirical and flag for re-test).
**Defer:**
  - A/B test unified `tp_pct` (e.g. 0.35%) vs. current split.

---

# Cross-asset engines (`CrossAssetEngines.hpp`)

## EsNqDivergence
**Status:** ticking_but_disabled. `[cross_asset] esnq_enabled=false` (`omega_config.ini:401`). Engine ticks every loop and drains open positions; `enabled=false` only blocks new entries.
**Trades in shadow tape:** None.
**Risk read:** 3-tick confirm window prevents single-tick false positives. Per-side cooldown 120s. TP 0.08% / SL 0.05%.
**Issues flagged:**
  - [P1] `confirm_count_` (`CrossAssetEngines.hpp` L433-434) does not reset when the confirmed direction flips. ES_lags-twice-then-NQ_lags counts the prior 2 ticks toward the new direction. Edge case (rapid divergence flips) but produces a false latch.
  - [P2] Engine ticks unconditionally — per the audit-team suspicion, this is real but cheap. The `enabled=false` gate is correct but the engine logs nothing about the wasted ticks.
**Quick-win improvements:**
  - [1h] Reset `confirm_count_` to 0 on direction change.
**Defer:**
  - 30-trade live validation gate before flipping `esnq_enabled=true`.

## OilFade (`OilEventFade`)
**Status:** shadow_only. `g_ca_eia_fade.enabled=false` (`engine_init.hpp:1568`).
**Trades in shadow tape:** None.
**Risk read:** EIA Wed 14:30 UTC baseline + 15s delay + spike detection. Weekly-armed gate.
**Issues flagged:**
  - [P2] `reset()` (L551-556) checks `tm_wday==1` only. A wall-clock backward jump mid-week (NTP correction, container restart) can leave `armed_=true` outside the EIA window.
  - [P2] Spike threshold `SPIKE_THRESH_PCT=0.30` is purely percentage; on thin tape that's $1.50 absolute on $5 000 oil — below typical execution cost.
**Quick-win improvements:**
  - [1h] Tighten reset to `(tm_wday==1 && tm_hour==0)` and idempotent.
**Defer:**
  - Live EIA feed integration + dollar-floor on spike threshold.

## BrentWtiSpread
**Status:** shadow_only. `g_ca_brent_wti.enabled=false` (`engine_init.hpp:1569`).
**Trades in shadow tape:** None.
**Issues flagged:**
  - [P2] `SPREAD_THRESH=$5.00` is absolute. Post-OPEC days normalise $5+ as intraday noise.
  - [P2] Brent mid is computed from a possibly-stale L2 source; no tick-staleness gate.
**Quick-win improvements:** None warranted while engine is disabled.
**Defer:**
  - Spread vol-gate + geopolitical event blackout.

## FxCascade
**Status:** active. enabled=true (`CrossAssetEngines.hpp:674`). Per-pair gates allow concurrent positions on GBPUSD/AUDUSD/NZDUSD.
**Trades in shadow tape:** No tape rows visible under FxCascade label specifically (FX engines may report via separate session-open engine labels, e.g. `EurusdLondonOpen` etc., which are NOT in handoff catalog and thus not audited as standalone).
**Risk read:** 200ms cascade window (tightened from 500ms), 60s per-pair cooldown.
**Issues flagged:**
  - [P1] Per-pair cooldowns reset only at entry (L811), not at exit. If GBPUSD entry at T0 holds for 120s, AUDUSD becomes eligible again at T+60 and can fire while GBPUSD is still open. Multi-leg correlated exposure not aggregated. Structural design gap.
  - [P2] `notify_eurusd_signal()` (L679-687) sets `armed_ts_ms` every tick without checking if the cascade window is already open. Rapid EUR signals stretch the window indefinitely.
  - [P2] `TP_PCT=0.08%` cleared a "ratio ~1.5" cost guard at design time; cost guard removed; no current verification that 0.08% covers the $10/lot FX execution floor at live spreads.
**Quick-win improvements:**
  - [1h] In `notify_eurusd_signal`, only set `armed_=true / armed_ts_ms=now` if `!armed_ || now - armed_ts_ms > CASCADE_WINDOW_MS`.
  - [1 session] Verify TP_PCT clears cost on live GBPUSD tick data.
**Defer:**
  - Coordinator that applies per-pair cooldowns from CLOSE rather than ENTRY.

## CarryUnwind
**Status:** active code-path but no live VIX/USDJPY data wired in `engine_init.hpp` (no init call). Effectively dormant.
**Trades in shadow tape:** None.
**Issues flagged:**
  - [P2] EMA `alpha` (L843-844) is tick-based not time-based. At 1 tick/s VIX feed, the 20-tick halflife is ~20 s, not the 2 s the comment implies.
  - [P2] Realised-vol buffer (L876-902) primes before fullness check (L880); first 29 ticks of a cold start all have the same price → realvol=0 → filter fires false positive on warmup.
**Quick-win improvements:**
  - [1h] Add a stale-price gate: if oldest `rv_window_` tick > 120s old, return `{}`.
**Defer:**
  - Wire live VIX/USDJPY feeds and re-validate before enabling.

## OpeningRangeBreakout (5 instances: US, GER40, UK100, ESTX50, Gold-implicit)
**Status:** all 4 named instances disabled (`engine_init.hpp:1561-1564`).
**Trades in shadow tape:** None.
**Risk read:** Time-anchored ORB on first 30 min after open, single-fire-per-session.
**Issues flagged:**
  - [P0] **Session UTC times hard-coded and wrong for all non-US symbols.** US 13:30 UTC correct; GER40 hardcoded 13:30 UTC (Xetra opens 08:00 UTC); UK100 hardcoded 13:30 UTC (LSE opens 08:00 UTC); ESTX50 hardcoded 13:30 UTC (Euronext opens 09:00 UTC). The engines are presently disabled so this is dormant, but if they're flipped on without fixing this they will fire ORB at NY open across European cash equities — wrong market.
  - [P1] `opening_` flag never reset daily. After the first fire, `opening_=false` and stays false — no `last_opened_day_` reset visible at L1029. Symptom: ORB fires once ever per process restart, not once per day.
**Quick-win improvements:**
  - [1h] Move per-symbol `OPEN_HOUR` / `OPEN_MIN` into INI; default GER40 08:00, UK100 08:00, ESTX50 09:00, US 13:30.
  - [1h] Add `if (today != last_opened_day_) { opening_=true; last_opened_day_=today; }` at the top of `on_tick`.
**Defer:**
  - 30-day live validation per-symbol after both fixes.

## VWAPReversion (4 instances: XAUUSD + 3 indices)
**Status:** active for XAUUSD (shadowing); index instances exist but their wiring is patchier.
**Config source:** `CrossAssetEngines.hpp` L1072-1325, per-instrument tuning at `engine_init.hpp:442-460`.
**Trades in shadow tape:** `VWAPReversion` 10 trades, 40 %WR, net **+$513.09**, mean hold 10 191 s. Two outsized winners drove the result (US500.F SHORT +$460.96 / 27 994 s; USTEC.F SHORT +$575.17 / 28 035 s). Note: no `VWAPReversion` rows have `BE_HIT` / `TP_HIT` exit reasons — every exit is `FORCE_CLOSE`. The engine's stated mechanics produce TP/SL/BE exits; the tape suggests the engine isn't closing its own positions.
**Risk read:** Daily-cumul VWAP reset at UTC midnight. Fade ≥ 2σ extension. Hold time clearly oversized.
**Issues flagged:**
  - [P1] **Multi-symbol engine queries `bracket_trend_bias("XAUUSD")` hard-coded** (`CrossAssetEngines.hpp` L1160-1161). Index instances reading XAUUSD bias is silently incorrect.
  - [P1] All 10 tape rows exit on `FORCE_CLOSE` — the engine's BE/TP/SL paths are not the closer. Either an external supervisor is closing them or the engine's own close path is dead. Worth tracing.
  - [P2] VWAP reset point is "UTC midnight" per comment but I could not confirm an explicit reset call inside this class — `cumul_pv` / `cumul_vol` updates are visible but reset is not. If a session restart skips the reset, stale VWAP carries over.
  - [P2] `RECENT_MOVE_MIN` hardcoded per symbol (0.30 pt gold, 0.50 pt indices, L1159). No vol-scale.
**Quick-win improvements:**
  - [1h] Pass `sym` to `bracket_trend_bias()`; remove the hardcoded "XAUUSD" string.
  - [1 session] Add explicit VWAP reset on the first tick of each UTC day, with a log line; assert reset has happened by the second tick.
**Defer:**
  - Investigate why every `VWAPReversion` exit is `FORCE_CLOSE`. Likely the engine's own close path is not wiring through the live mode.

## TrendPullbackEngine (4 instances: XAUUSD, GER40, NQ, SP — all shadow_mode=true)
**Status:** XAUUSD instance is `enabled=false` (`engine_init.hpp:541`, "no edge on XAUUSD" — S44 backtest net -$0.70/trade). Indices wired but stale. Tape carries 2 rows under `TrendPullback` label, both winners (the +$3 157 GOLD.F LONG that ran 24 260 s on 2026-03-27 and a small follow-up).
**Config source:** `engine_init.hpp:473-527` per-symbol tuning, `CrossAssetEngines.hpp:1403-2149`.
**Risk read:** EMA-pullback continuation + CVD + news-aware SL widening + pyramiding + TOD weighting + consecutive-SL direction block.
**Issues flagged:**
  - [P0] **`DAILY_LOSS_CAP` gate is structurally broken.** `engine_init.hpp:510` sets gold cap = $150. Implementation at `CrossAssetEngines.hpp:2000-2009` checks `daily_pnl_ <= -DAILY_LOSS_CAP` at entry, but `daily_pnl_` is only updated by `record_daily_pnl()` called from `_close()` (L1870) — i.e. AFTER each trade closes. First trade of the day loses $100 → cap not yet tripped. Second trade enters → loses $60 → total $-160 → cap fires AT THE END OF THAT TRADE, not before it. Effectively the cap blocks new entries only after it has already been broken by the in-progress position. Indices have no cap set so default to disabled. **For gold the intended $150 protection is not protecting; for indices it's not configured.** The XAUUSD instance being `enabled=false` means this is dormant for live but flips on as soon as TrendPullback re-enables on any symbol.
  - [P1] Consecutive-SL direction block (`m_consec_sl_long_` L1681-1698) increments on `IMM_REVERSAL` and `TIME_STOP` not just `SL_HIT`. Threshold hardcoded at 2; pause hardcoded at 600s. No INI knobs.
  - [P2] `m5_trend_state_` only populated by external `set_m5_trend()` setter (L1951-1954). For index instances, no caller is wired to update it. Default is 0 (flat/unknown) — gate is permissive when it should be blocking.
  - [P2] `avg_atr20_` similarly externally fed via `set_avg_atr()`. No safeguard if zero/stale.
  - [P2] TOD weighting (L2016-2024) hardcoded 0.5× during 10:00-13:30 UTC. If empirical data shows mid-session is profitable, the static 0.5× harms.
**Quick-win improvements:**
  - [1h] **Restructure DAILY_LOSS_CAP to gate on `realised_pnl_today + open_position_max_loss`** rather than only on closed `daily_pnl_`. Track open positions' SL distances at entry, sum, compare to cap at each new entry.
  - [1h] INI keys `consec_sl_threshold`, `direction_block_duration_sec` to replace hardcoded constants.
**Defer:**
  - Empirical re-tune of TOD coefficient.

## NextBarMomentum (NBM)
Covered under symbol-specific NbmEngine above. Single GOLD_LONDON instance enabled with the wrong session timing.

---

# Gold-stack engines (under `GoldEngineStack`)

## GoldEngineStack (outer)
**Status:** active, `kShadowDefault`. Wired through `tick_gold.hpp` every XAUUSD tick.
**Config source:** `omega_config.ini` `[gold_stack]` L372-395, `include/GoldEngineStack.hpp`, sub-engine disables at `engine_init.hpp:1502-1546`.
**Trades in shadow tape:** Sub-engines bubble up under their own labels. Aggregate gold-stack contribution dominated by `CandleFlowEngine` (203 trades, net -$621), `XAUUSD_BRACKET` (28 trades, net -$95), `DomPersistEngine` (21 trades, +$72), and `TickScalpEngine` (107 trades, -$43). Several disabled sub-engines (SessionMomentum, IntradaySeasonality, VWAPSnapback, VWAPStretchReversion, DXYDivergence) still appear in the tape historically, indicating they fired before the 2026-04-30 audit-disable.
**Risk read:** Per-sub-engine audit-disable flags (`engine_init.hpp:1511-1539`). `min_entry_gap_sec=20` here is **not** an override of the global 90 s gate — both apply, effective gate is `max(20, 90) = 90`. Lock/trail constants live in INI and are read.
**Issues flagged:**
  - [P0 — see top] CandleFlowEngine contributes 5 of the 48 negative-`hold_sec` rows. Sub-engine close paths likely re-use the same dollar-stop pattern fixed only in `MacroCrash`. Trace `on_tick.hpp:948-1000` (the `g_gold_stack` dollar-stop block) to confirm whether `force_close` is ever called with seconds-not-ms inside any sub-engine path.
  - [P2] Sub-engine disable gate (`set_subengine_audit_disabled`) is per-tick early-return inside each sub-engine's `process()` — confirmed by the comment at the `EngineBase::setEnabled` reset hook. Risk that future sub-engines forget the gate; document the convention in a header comment.
  - [P2] VWAP daily reset is tick-driven; if no XAUUSD tick lands within ~60 s of UTC midnight the reset fires up to a minute late.
**Quick-win improvements:**
  - [1h] Audit `tick_gold.hpp` and `on_tick.hpp:948-1000` for any `force_close(..., ds_now)` (seconds) inside a GoldStack sub-engine path. Replace with `ds_now_ms`.
  - [1h] Add a single-line comment in `EngineBase` documenting the early-return gate convention so future sub-engines don't omit it.
**Defer:**
  - VWAP reset timing on overnight gaps (low priority; XAUUSD is 24h).

## CompressionBreakout (gold-stack sub-engine)
**Status:** label appears in tape; not located by name in `GoldEngineStack.hpp` during agent review — possibly aliased / renamed.
**Trades in shadow tape:** `CompBreakout` 16 trades, 43.8 %WR, net -$13.25, top exit `TIMEOUT`.
**Issues flagged:**
  - [P1] **Engine inventory mismatch.** The handoff lists `CompressionBreakout` as a gold-stack sub-engine. The header file does not contain a class with that exact name visible to the audit pass. Resolve: rename in handoff or add the class.
**Quick-win improvements:** None until inventory resolved.
**Defer:**
  - Reconcile name vs. tape label.

## ImpulseEntry (gold-stack sub-engine)
**Status:** label not in tape; class not found by exact name. Likely aliased to `MomentumContinuationEngine` (`GoldEngineStack.hpp:399`).
**Issues flagged:**
  - [P1] Same inventory mismatch as CompressionBreakout. Either rename or document the alias.
**Defer:**
  - Reconcile.

## SessionMomentum (gold-stack sub-engine)
**Status:** disabled (`engine_init.hpp:1511`).
**Trades in shadow tape:** `SessionMomentum` 3 trades, 33.3 %WR, net -$37.26 — pre-disable history.
**Issues flagged:**
  - [P2] Recent-move detector compares to mid 5 ticks ago (`GoldEngineStack.hpp` L346-351). On thin Asia tape 5 ticks can be 2-5 s; on London open 0.3-0.5 s. Same threshold across both.
  - [P2] Hardcoded `bracket_trend_bias("XAUUSD")` — same multi-symbol risk as VWAPReversion.
**Defer:**
  - Tick-rate adaptive recent-move window before re-enabling.

## VWAPSnap (gold-stack sub-engine — `VWAPSnapback`)
**Status:** disabled (`engine_init.hpp:1517`).
**Trades in shadow tape:** No rows (label `VWAPStretchReversion` 14 rows, 21.4 %WR, also disabled — different sub-engine).
**Issues flagged:**
  - [P2] No size scaling on confidence; static 0.01 lot.
**Defer:**
  - Vol-aware sizing.

## SweepProEngine (gold-stack sub-engine, `LiquiditySweepPro`)
**Status:** active (`enabled_=true`, `GoldEngineStack.hpp:1986`).
**Trades in shadow tape:** Not visible under `SweepPro` label specifically. Tape labels `WickRejection` (2 trades, both losers) and `GoldFlowEngine` (1 trade, loser) might overlap.
**Issues flagged:**
  - [P1] **"Sweep" semantics are misleading.** Code triggers on momentum exhaustion near a clustered price level (cluster = 8+ ticks within 0.35 pt over the last 120 ticks). The engine name implies a wick-rejection / stop-hunt-reversal pattern; the implementation is "momentum fade near liquidity". Operators reasoning about the engine will mis-model its behaviour. P1 because it's a comprehension hazard, not a code defect.
  - [P2] `CLUSTER_RANGE` 0.35 pt and `MIN_CLUSTER` 8 ticks are not vol-scaled.
**Quick-win improvements:**
  - [1h] Rewrite the class header comment to spell out "sweep = momentum exhaustion near liquidity, not structural wick rejection". 5-line comment fix.
**Defer:**
  - Vol-calibration of cluster size.

## SweepPressureEngine (gold-stack sub-engine, `LiquiditySweepPressure`)
**Status:** disabled (`enabled_=false`, `GoldEngineStack.hpp:2077`). 51-trade backtest at 29 %WR / -$12.10 drove the cull.
**Trades in shadow tape:** None under this label.
**Issues flagged:** None new (cull decision documented).
**Defer:**
  - Document re-enable criteria (specific WR / EV thresholds).

## IntradaySeasonality (gold-stack sub-engine — counted as gold-stack per the audit-disable list)
**Status:** disabled (`engine_init.hpp:1514`).
**Trades in shadow tape:** `IntradaySeasonality` 3 trades, 0 %WR, net -$11.36 — pre-disable history.
**Issues flagged:**
  - [P2] Hardcoded t-stat table inside the header. Drift in seasonality requires recompile to retune.
**Quick-win improvements:**
  - [1h] Move the t-stat table to `omega_config.ini` `[gold_stack_intraday]` or to an external CSV with a `WARN` on missing.
**Defer:**
  - Live seasonality drift detection.

## PDHLReversionEngine
**Status:** active (`enabled=true`, `shadow_mode=true`). Called from `tick_gold.hpp:2256-2271`.
**Config source:** `include/PDHLReversionEngine.hpp` L1-259, `engine_init.hpp:1880-1906`.
**Trades in shadow tape:** No rows under `PDHL` / `PDHLReversion` label. Engine fires inside PDH-PDL only; sample window may not have triggered.
**Risk read:** 2y/111M-tick backtest puts PDH-PDL mean-reversion as the only statistically significant intraday edge (EV +1.732 pt). Tight structural SL (0.4×ATR), max 15-min hold. L2 confirmation gates.
**Issues flagged:**
  - [P1] **Stale signature: `on_tick(int depth_bid, int depth_ask, ...)` exists, but the call site in `tick_gold.hpp:2266` passes literal `0, 0`.** S13 cTrader cull removed the source of those values; engine ignores them and uses `g_l2_imbalance` instead (correct). The signature is technically working but is a maintenance hazard — future ports of the engine will assume the `int depth_*` parameters mean something.
  - [P1] L2-flip exit (L210-217) requires `MIN_HOLD_MS=20s`. On noisy L2 (early Asia, holiday tape), 20 s is thin enough that whipsaw exit-and-re-entry around the same price level is plausible. No re-entry cooldown after L2-flip exit.
  - [P2] Spread gate `MAX_SPREAD_PTS=1.5` is permissive vs. typical 0.30-0.50 pt; thin-Asia widening above 1.5 pt is rare but possible. Session_slot==0 dead-zone is gated correctly (L117).
**Quick-win improvements:**
  - [1h] Remove or deprecate the unused `depth_bid` / `depth_ask` parameters from `on_tick`. Replace with a single `l2_imbalance` arg; drop the call-site `0, 0`.
  - [1h] Add a 60-s post-L2-flip-exit cooldown to suppress whipsaw on noisy L2.
**Defer:**
  - Validate `ewm_drift=1.5pt` fallback threshold on a 2026 tape with no real-L2 scenarios.

---

# Index / L2 flow engines

## IndexFlowEngine
**Status:** active. Single header, internal init.
**Config source:** `include/IndexFlowEngine.hpp`. Reads `g_macro_ctx.sp_l2_imbalance` per L554 comment. Post-S13 confirmed clean — no cTrader-derived dependencies.
**Trades in shadow tape:** `IndexFlow` 17 trades, **WR 29.4%** (below the handoff's 35% red flag), net -$54.54, mean hold 427 s. Exit reasons: 11 SL_HIT, 1 BE_HIT, 1 TP_HIT, 1 TRAIL_HIT.
**Risk read:** L2-imbalance entry on 30/60-tick windows + EWM drift confirmation + ATR floor.
**Issues flagged:**
  - [P1] **Live WR 29.4% under the handoff's 35% red-flag threshold.** Sample size (17) is small, but the direction is the wrong way. Worth a focused 30-trade reassessment with the post-S13 L2 wiring before declaring permanent.
  - [P2] Per-symbol ATR floors: are they re-validated for current vol regime? Engine-level audit can't tell.
**Quick-win improvements:**
  - [1 session] Replay the 17 IndexFlow trades against post-S13 L2 to confirm signal quality vs. the cTrader-era data the engine was originally tuned on.
**Defer:**
  - Re-tune entry thresholds if the 30-trade gate confirms underperformance.

## IndexMacroCrashEngine (`MacroCrashEngine`)
**Status:** active in code (`enabled` defaults true), shadow_mode-pinned in `engine_init.hpp:209-303`. The Apr-15 phantom-fire incident is the system's most-cited correctness incident.
**Config source:** `include/MacroCrashEngine.hpp` + init block at `engine_init.hpp:209-310`.
**Trades in shadow tape:** `MacroCrash` **45 trades, WR 8.9%, net -$2 707.94, mean -$60.18/trade**. **42 of 45 trades have negative `hold_sec`. 30 of 45 exit on `DOLLAR_STOP`.** Worst 9 trades all MacroCrash, all DOLLAR_STOP, all on 2026-04-08 14:32-33 UTC — single phantom-fire cluster.
**Risk read:** The cluster pattern signature (multiple entries inside the same minute on DOLLAR_STOP) is exactly the failure mode the on_tick.hpp Apr-21 fix was meant to stop. The fix is present at `on_tick.hpp:929-932`. The 2026-04-08 cluster pre-dates the fix.
**Issues flagged:**
  - [P0 — see top of doc] Unit-mismatch close path. Verified fixed for MacroCrash. **NOT verified for the other engines that show negative-hold-sec rows** (CandleFlowEngine, HybridBracketGold) — those pathways need the same audit.
  - [P1] Even with the fix, the 2026-04-08 cluster shows the original behaviour: `max_consec_losses=3` and `max_positions=3` (post-Apr-29 risk caps) would have arrested the storm at trade ~10 of 61. **Without the audit-tightened risk caps, the engine fix alone is not sufficient.** Confirmed in `omega_config.ini:101-104` head note. P1 because the cap is in the INI now but is hot-reloadable — anyone widening it without re-validating the engine fix re-opens the wound.
  - [P1] Engine `enabled` flag and the `[mode] mode=SHADOW` global are layered. If global is flipped to `LIVE` while MacroCrash is still investigating an issue, only the engine-local `kShadowDefault` shadow_mode pin (in `engine_init.hpp:209-303`) protects the system. That's a fine setup but very implicit; a single flag flip in the wrong file produces an immediate live MacroCrash run.
  - [P2] DOLLAR_STOP path is the only safety net on this engine — the engine's own SL is checked but DOLLAR_STOP is a separate gate. Documented at `on_tick.hpp:907-947` with helpful comments.
**Quick-win improvements:**
  - [1h] Add a runtime assertion in `MacroCrashEngine::force_close`: `assert(now_ms >= 1e12 && now_ms < 4e12)` (post-2001, pre-2096 in ms). Catches future regressions where seconds get passed in.
  - [1h] Add a startup log line that prints all four MacroCrash safeguards (engine-SL, DOLLAR_STOP, max_positions, max_consec_losses) with their current values, so operators see the layered stack.
**Defer:**
  - Re-evaluation of MacroCrash signal model itself (separate exercise).

---

# RSI / EMA engines

## RSIReversalEngine
**Status:** active.
**Config source:** `engine_init.hpp:310-340`, `include/RSIReversalEngine.hpp`.
**Trades in shadow tape:** No rows under `RSIReversal` label.
**Risk read:** Tick-level RSI(14) reversal on extremes + DOM filters (L2 imbalance, vacuum). Tight risk (SL 0.6×ATR, BE at 0.4×ATR, trail 0.4×ATR). Cooldown 60 s, max hold 10 min.
**Issues flagged:** None at engine logic level.
**Quick-win improvements:** None.
**Defer:** None.

## RSIExtremeTurnEngine
**Status:** active. S12 `prev_bar_rsi` cold-start fix confirmed; S54 `BE_OFFSET_PTS=2.5` round-trip fix present (audit-fixes-35).
**Config source:** `include/RSIExtremeTurnEngine.hpp`.
**Trades in shadow tape:** No rows under `RSIExtremeTurn` label.
**Issues flagged:** None.
**Quick-win improvements:** None.
**Defer:** None.

## EMACrossEngine
**Status:** soft-culled (S18, 2026-04-24). `engine_init.hpp:25` forces shadow + `ECE_CULLED=true`. Engine still ticks, logs `[ECE-CULLED-SIGNAL]`, manages out existing positions.
**Trades in shadow tape:** `EMACross` 18 trades, **WR 22.2%**, net -$107.29 — confirms the cull rationale. (Below 35% threshold.)
**Issues flagged:** None new (cull decision ratified).
**Defer:**
  - Re-evaluate cull after a 2-week paper validation with ≥30 trades, WR ≥35%, net positive after costs.

---

# Ledger / supervisor

## OmegaTradeLedger
**Status:** central. All engines route TradeRecord through it. Dedup guard (symbol+entryTs+engine+exitReason).
**Config source:** `include/OmegaTradeLedger.hpp` L25-259; `hold_sec` actually computed at `trade_lifecycle.hpp:468-469` as `(exit_ts > entry_ts) ? exit_ts - entry_ts : 1`.
**Issues flagged:**
  - [P0 origin point] The `(exit_ts > entry_ts) ? ... : 1` guard at `trade_lifecycle.hpp:468-469` was added (S43 era? confirm) but is bypassed when the engine itself writes `tr.exitTs` directly into the TradeRecord before submission. The 48 negative-hold-sec rows in the tape suggest the guard runs in a path that's not always upstream of the CSV writer. Trace which writer produced the rows that have `hold_sec = -1774312273`. If the ledger trusts engine-computed `exitTs`, no guard at the lifecycle level can save it.
  - [P2] No assertion that `entryTs > 0` either; a similar root cause on the entry side would silently produce huge positive hold_sec.
**Quick-win improvements:**
  - [1h] Add an assertion or sanitizer at the ledger entry point: `if (tr.exitTs < tr.entryTs) { tr.exitTs = tr.entryTs; tr.note += "[FIXED-EXIT-TS]"; }` so the bad row is at least flagged in the log even if the math is rescued.
**Defer:**
  - Audit who-writes-tr.exitTs across all engine close paths.

## MacroRegimeDetector
**Status:** active. Fed from quote/tick loops via `g_macroDetector`.
**Config source:** `include/MacroRegimeDetector.hpp`.
**Issues flagged:**
  - [P2] **RISK_OFF is sticky once VIX≥35.** No time-decay fallback. Prolonged events keep the gate on. Not a bug, but worth surfacing because `block_on_risk_off=true` is set on every Tier-1 portfolio — sticky RISK_OFF + Tier-1 ship date overlap means the audit can't distinguish "engines didn't fire because no signal" from "engines were RISK_OFF-blocked".
  - [P2] When VIX feed is stale > 300 s, regime falls to NEUTRAL — safe behaviour but no operator log line says "regime: stale VIX, fallback NEUTRAL".
  - **4 RISK_OFF leaks in the shadow tape** (4 trades opened with `regime=="RISK_OFF"` despite `block_on_risk_off`). Cross-engine, low frequency. Likely a tick-arrival race on the regime variable: the regime updates between the gate check and the order send. Investigate.
**Quick-win improvements:**
  - [1h] Add a post-entry sanity log: if a trade's recorded `regime=="RISK_OFF"` and the engine has `block_on_risk_off=true`, write a `[RISK_OFF_LEAK]` log line with engine + ts so operators see the race.
  - [1 session] Trace the 4 leak rows (engine, ts) and check whether they share a symbol or session.
**Defer:**
  - Optional time-decay on RISK_OFF (auto-flip to NEUTRAL after N hours if VIX hasn't ticked).

---

# Top 5 quick wins (effort × impact)

Ranked by ratio of expected-impact-on-correctness to estimated-effort.

1. **Port the dollar-stop ms-not-sec fix to CandleFlowEngine and HybridBracketGold close paths** (P0 of P0s).
   *Effort:* 1 session.
   *Impact:* Eliminates 6 of the 48 negative-`hold_sec` rows in the tape and prevents an in-the-wild repeat of the 2026-04-15 phantom-fire mode on those engines.
   *Where:* Trace `tick_gold.hpp` and `on_tick.hpp:948-1000` for any `force_close(...)` call passing seconds where ms is expected. Mirror the `MacroCrash` fix at `on_tick.hpp:929-932`.

2. **Fix `TrendPullback::DAILY_LOSS_CAP` to gate on `realised_pnl_today + open_position_max_loss`, not closed `daily_pnl_` only** (P0).
   *Effort:* 1h.
   *Impact:* The intended $150/day gold cap currently fires after the cap has already been broken. Restoring its intent is one helper function.
   *Where:* `CrossAssetEngines.hpp:2000-2009`.

3. **Wire the dead `[minimal_h4]` and `[minimal_h4_us30]` INI sections** (P0).
   *Effort:* 1h each.
   *Impact:* Operators can hot-reload-tune two of the most-traded post-cut survivors without recompile. Currently every key in those sections is dead.
   *Where:* Add `apply_engine_config(MinimalH4Breakout&)` and `apply_engine_config(MinimalH4US30Breakout&)` overloads in `engine_config.hpp`.

4. **Drop `TrendRider::max_lot_cap` from 0.50 → 0.20 until first 50 closed shadow trades validate the backtest** (P0/P1 mitigation).
   *Effort:* 1h (INI edit + restart).
   *Impact:* Caps worst-case 6-cell-simultaneous-SL exposure from the engine-comment's optimistic 6% to the realistic ~10% — manageable while we wait for live data on the highest-risk Tier-1 engine. Reversible.
   *Where:* `omega_config.ini:806`.

5. **Fix OpeningRangeBreakout per-symbol UTC open times before any of the 4 disabled instances re-enables** (P0).
   *Effort:* 1h.
   *Impact:* Today the engines are dormant; the moment someone flips `g_orb_ger30.enabled=true` they fire ORB at NY open on European cash equities. Wrong market.
   *Where:* `CrossAssetEngines.hpp:981-982` plus per-symbol initialisation in `engine_init.hpp:1561-1564`. Move open hour/minute to INI keys.

---

# Appendix A — Per-engine shadow-tape stats (all observed labels)

The table below covers every distinct `engine` label in `audit_input/omega_shadow.csv` (900 rows, 2025-01-24 → 2026-04-08). Labels in **bold** are in the handoff catalog; the rest are surfaced for cross-engine signal (negative-hold-sec, RISK_OFF leaks) only.

| Engine | N | WR% | Net pnl USD | Mean USD | Worst | Best | Mean hold s | Neg-hold | Top exit |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| AsianRange | 2 | 50.0 | 9.52 | 4.76 | 3.60 | 6.82 | 491 | 0 | SL_HIT |
| BBMeanRev | 5 | 40.0 | 5.70 | 1.14 | -20.80 | 40.90 | 103 | 0 | SL |
| **CandleFlowEngine** | 203 | 40.4 | -621.20 | -3.06 | -169.60 | 106.96 | 224 | **5** | SL_HIT |
| CompBreakout | 16 | 43.8 | -13.25 | -0.83 | -32.09 | 26.77 | 142 | 0 | TIMEOUT |
| DomPersistEngine | 21 | 71.4 | 72.53 | 3.45 | -49.05 | 41.81 | 120 | 0 | TRAIL_HIT |
| DXYDivergence | 11 | 54.5 | 16.27 | 1.48 | -7.92 | 6.08 | 139 | 0 | SL_HIT |
| DynamicRange | 1 | 100.0 | 0.72 | 0.72 | 0.72 | 0.72 | 164 | 0 | SL_HIT |
| **EMACross** | 18 | 22.2 | -107.29 | -5.96 | -32.80 | 25.66 | 129 | 0 | SL |
| GoldFlowEngine | 1 | 0.0 | -26.76 | -26.76 | -26.76 | -26.76 | 73 | 0 | SL_HIT |
| GoldSilverLeadLag | 1 | 0.0 | -66.49 | -66.49 | -66.49 | -66.49 | 4090 | 0 | FORCE_CLOSE |
| **HybridBracketGold** | 10 | 50.0 | 20.75 | 2.08 | -32.48 | 59.50 | 38 | **1** | TRAIL_HIT |
| HybridBracketIndex | 41 | 43.9 | -184.14 | -4.49 | -26.08 | 103.20 | 566 | 0 | SL_HIT |
| **IndexFlow** | 17 | 29.4 | -54.54 | -3.21 | -31.36 | 5.40 | 428 | 0 | SL_HIT |
| IntradaySeasonality | 3 | 0.0 | -11.36 | -3.79 | -5.85 | -2.53 | 602 | 0 | TIMEOUT |
| LondonFixMomentum | 2 | 50.0 | 5.35 | 2.68 | -5.28 | 10.63 | 234 | 0 | TP_HIT |
| **MacroCrash** | 45 | 8.9 | -2 707.94 | -60.18 | -259.30 | 133.60 | 266 | **42** | DOLLAR_STOP |
| MeanReversion | 1 | 100.0 | 4.32 | 4.32 | 4.32 | 4.32 | 418 | 0 | SL_HIT |
| PYRAMID | 13 | 30.8 | -32.13 | -2.47 | -24.86 | 4.66 | 136 | 0 | SL_HIT |
| SessionMomentum | 3 | 33.3 | -37.26 | -12.42 | -31.64 | 0.72 | 142 | 0 | SL_HIT |
| TickScalpEngine | 107 | 33.6 | -42.66 | -0.40 | -2.10 | 3.25 | 38 | 0 | SL_HIT |
| **TrendPullback** | 2 | 50.0 | 3 157.42 | 1 578.71 | 3.14 | 3 157.42 | 12 131 | 0 | FORCE_CLOSE |
| TurtleTick | 4 | 25.0 | -36.96 | -9.24 | -15.84 | 16.65 | 200 | 0 | SL_HIT |
| **VWAPReversion** | 10 | 40.0 | 513.09 | 51.31 | -21.08 | 575.17 | 10 191 | 0 | FORCE_CLOSE |
| VWAPStretchReversion | 14 | 21.4 | -75.57 | -5.40 | -15.84 | 9.83 | 278 | 0 | SL_HIT |
| WickRejection | 2 | 0.0 | -8.37 | -4.19 | -5.85 | -2.52 | 192 | 0 | FORCE_CLOSE |
| **XAUUSD_BRACKET** | 28 | 39.3 | -94.79 | -3.39 | -22.27 | 7.94 | 1 534 | 0 | BE_HIT |

**Sanity totals:** 900 trades · 25 distinct engine labels · 48 negative-`hold_sec` rows · 4 RISK_OFF leaks · date range 2025-01-24 → 2026-04-08.

**Top 10 worst single trades:** 9 of 10 are `MacroCrash` `DOLLAR_STOP` rows clustered 2026-04-08 14:32-33 UTC (the cluster that pre-dates the Apr-21 ms-not-sec fix). 10th is a `CandleFlowEngine` -$169.60 `FORCE_CLOSE` on 2026-03-15 22:19 — possibly the same unit-mismatch defect on a different engine's close path; flagged in the P0 doc.

---

# Appendix B — Items the audit could not verify

These are open questions left for S16 because they need either a fresh shadow tape or cross-file tracing the audit didn't have time to do:

- **Tier-1 live-vs-backtest WR comparison.** Shadow tape pre-dates Tier-1 ship; cannot ground-truth Tsmom / Donchian / EmaPullback / TrendRider / C1Retuned WR yet. Requires fresh CSV covering ≥ 2026-04-30.
- **Same-direction reentries within `min_entry_gap_sec`.** Not present in the visible tape excerpts but only because the audit could not run a full pairwise check across 900 rows without bash. Re-run when bash is restored.
- **`MAE > 1.5 × SL` intrabar logic anomalies.** Tape lacks an `sl` column; the heuristic check the handoff requested is not computable from this CSV.
- **`MAX_TRADES_PER_CYCLE` enforcement** (BreakoutEngine). Not visible in the engine header; lives upstream in `trade_loop.hpp` / `order_exec.hpp` — out of scope for this pass.
- **CandleFlowEngine and HybridBracketGold ms-not-sec fix.** Their dollar-stop close paths weren't traced to the same depth as MacroCrash's. The 6 negative-hold-sec rows are circumstantial evidence; confirmation requires `tick_gold.hpp` + the GoldStack sub-engine close paths.
- **`CompressionBreakout` / `ImpulseEntry` engine-name vs class-name mismatch.** Either the handoff catalog is wrong or the source has been renamed; can't tell from the audit window alone.
- **Whether the 4 RISK_OFF leaks share a symbol/session.** Leak rows visible in tape stats but not enumerated; left for follow-up.

---

# Appendix C — Engines wired but not in handoff catalog (for the next audit)

Per scope decision the audit did not produce sections for these, but flag them so they are not forgotten:

`HTFSwingEngines`, `LatencyEdgeEngines` (in `[latency_edge]`), `CandleFlowEngine`, `SweepableEngines{,CRTP}`, `EurusdLondonOpenEngine`, `UsdjpyAsianOpenEngine`, `GbpusdLondonOpenEngine`, `AudusdSydneyOpenEngine`, `NzdusdAsianOpenEngine`, `GoldMidScalperEngine`, `XauusdFvgEngine`, `OHLCBarEngine`, `CellEngine`, `EngineHeartbeat`, `EngineRegistry` / `EngineLastRegistry`, plus the `audit_results/S17_AUDIT.md` work that already exists on disk and overlaps with this audit's territory. A future S16+ should reconcile S15 + S17 catalogues into one.
