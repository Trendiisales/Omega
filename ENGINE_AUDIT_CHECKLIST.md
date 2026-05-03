# Omega Engine Audit Checklist
# Updated: 2026-05-03 (Installment 2 refresh + KNOWN_BUGS.md Bugs #3-#5 cross-reference)
# Maintained every session — never let this slip.

## THE RULE
Shadow mode = simulate exactly as if live. Only difference: no FIX order sent.
Every trade must appear in GUI Recent Trades with correct costs.

---

## CHECKLIST — Run against EVERY engine before shipping

### A. SHADOW SIMULATION PIPELINE
- [ ] A1. Close callback reaches `handle_closed_trade(tr)` — check that `on_close` or
       equivalent is NOT gated by `if (!shadow_mode)` or `if (mode == LIVE)`
- [ ] A2. `TradeRecord` fields all populated: symbol, side, engine, entryPrice, exitPrice,
       sl, size, pnl, mfe, entryTs, exitTs, exitReason, spreadAtEntry
- [ ] A3. Shadow fill simulation exists for stop-order engines (bracket/pending orders):
       `if (shadow_mode) { if (ask >= hi) confirm_fill(true,...) }` — price must cross
       the level to simulate fill, not just arm
- [ ] A4. `write_trade_open_log()` called on entry (shadow + live) — appears in CSV audit trail
- [ ] A5. `g_telemetry.UpdateLastSignal()` called BEFORE shadow check — GUI signal panel
       shows the signal regardless of shadow mode
- [ ] A6. `g_telemetry.UpdateLastEntryTs()` called on live entry only (inside `!shadow_mode`)

### B. ENTRY GATES
- [ ] B1. Engine does NOT use `gold_can_enter` or `base_can_*` as its outer gate if it
       is designed to bypass the regime system (e.g. RSIReversal, MCE, RSI-extreme engines)
- [ ] B2. ~~`gold_post_impulse_block` (20s after moves)~~ — **DEAD CODE** as of S19 era.
       `tick_gold.hpp:78` hard-pins `gold_post_impulse_block = false`. The bypass condition
       previously documented here is irrelevant; left in-place for future reference if the
       block is ever re-enabled with a real condition. (F2 / Installment 1 — refresh 2026-05-03.)
- [ ] B3. Session gate is correct: gold 24h (dead zone 05-07 UTC only), indices slots 1-5
       (07:00-22:00 UTC), no index entries in Asia (slot 6).
       NAS100-specific tightening: slot 3-4 NY-core only — see `tick_indices.hpp:906-911`
       block comment for rationale.
- [ ] B4. `bars_ready` / `m1_ready` checked before using RSI/ATR/EMA values
- [ ] B5. Cooldown after close is reasonable — not so long it misses the next oscillation
       (RSIReversal: 60s, bracket: 120s, MCE: 300s)
- [ ] B6. No duplicate gate: engine's own gate should not re-check conditions already
       checked by the outer wiring in tick_gold.hpp / tick_indices.hpp

### C. POSITION MANAGEMENT
- [ ] C1. `has_open_position()` management path runs UNCONDITIONALLY every tick —
       not inside the entry gate block. SL/trail/BE must fire even when `can_enter=false`
- [ ] C2. BE lock threshold is reachable given the engine's typical move size:
       BE_ATR_MULT × ATR must be less than typical profit move (not 1.0× ATR for
       a $2pt RSI oscillation engine)
- [ ] C3. Trail distance does not exceed BE threshold (trail must be tighter than BE)
- [ ] C4. SL is ATR-proportional, not fixed ticks — fixed ticks break on volatility changes
- [ ] C5. Max hold time is appropriate for the strategy frequency (RSI oscillation: 10min,
       bracket: 20min, trend: 60min, crash: no limit while trailing)
- [ ] C6. Force-close path exists and calls `on_close` (needed for disconnect/restart)
- [ ] C7. Time helpers (`entry_ts`, `idx_now_ms`, `idx_now_sec`) prefer tick-time over
       wall-clock. Backtest replay must inject tick-time via `set_idx_test_clock_ms()`
       (index engines) or the `now_ms_at_fill` parameter (`IndexHybridBracketEngine::confirm_fill`).
       Production: tick-time ≈ wall-clock so override is a no-op. Replay: required.
       (Bug #4 / KNOWN_BUGS.md — `audit-fixes-33`. Production unaffected; replay was broken pre-fix.)
- [ ] C8. Grace timers (`m_pending_blocked_since` and equivalents) MUST be reset by
       `reset_to_idle()` — not only by the steady-state `else` branch on the next clean tick.
       Repeated ARM/CANCEL cycles otherwise read stale block timestamps.
       (Bug #5 / KNOWN_BUGS.md — `audit-fixes-34`.)

### D. SIZING
- [ ] D1. Lot size uses `risk_per_trade_usd / (sl_dist_pts × tick_value)` — not hardcoded 0.01
- [ ] D2. Result is clamped: `max(lot_min, min(lot_max, calculated))`
- [ ] D3. `patch_size()` called from tick_gold/tick_indices after entry with the correctly
       computed lot (engine's internal size placeholder = 0.01 until patched)
- [ ] D4. Commission + spread cost covered by minimum TP: at Asia ATR=3pt, SL=0.6×ATR=$1.80,
       need at least $1.00 gross minimum (commission $0.60 + spread $0.40)

### E. COSTS IN TRADE RECORD
- [ ] E1. `tr.spreadAtEntry` populated with `ask - bid` at entry time
- [ ] E2. `handle_closed_trade` applies realistic costs via `apply_realistic_costs()`:
       commission, slippage, spread — these reduce `tr.pnl` to `tr.net_pnl`
- [ ] E3. `tr.pnl` is raw price points × size (before costs)
- [ ] E4. `tr.net_pnl` is what shows in GUI — commission + spread already deducted

### F. TELEMETRY AND GUI
- [ ] F1. `g_telemetry.UpdateLastSignal()` called on every entry (unconditional)
- [ ] F2. `g_omegaLedger.record(tr)` called via `handle_closed_trade` — appears in
       GUI Recent Trades table
- [ ] F3. Engine attribution (`tr.engine`) matches what GUI ENGINE ATTRIBUTION panel shows
- [ ] F4. Shadow trades show in Recent Trades with ✓/✗ result and net cost

### G. INDEXFLOW-SPECIFIC (indices only)
- [ ] G1. `idx_session_ok` declared locally in EACH function (on_tick_us500, on_tick_ustec,
       on_tick_dj30, on_tick_nas100) — NOT shared across functions (scope bug, FIXED `d6d00c6`)
- [ ] G2. L2 imbalance (`sp_l2_imb`, `nq_l2_imb` etc) wired from `g_macro_ctx` to engine
- [ ] G3. `IndexMacroCrash` shadow_mode=true entries reach `handle_closed_trade` via
       `TradeRecordCallback` — FIXED `audit-fixes-32`. Four-symbol parity wired
       (`g_imacro_sp/nq/nas/us30` declared in `globals.hpp:286-289`; ticked from
       `tick_indices.hpp:347/642/782/1054`; per-symbol vol_ratio driven by slow-EWM ATR
       baseline per handler).
- [ ] G4. VWAP anchor reset on NY open (13:30 UTC) and 30min settle gate enforced
- [ ] G5. No index engine fires in Asia (slot 6) or dead zone (slot 0)
- [ ] G6. Cross-engine block: any open position on `g_iflow_*` / `g_hybrid_*` /
       `g_minimal_h4_us30` blocks all four index symbols via
       `omega::idx::index_any_open()` (`globals.hpp:401`). Post-close gap of
       `g_index_min_entry_gap_sec` seconds (default 120) blocks via
       `omega::idx::idx_recent_close_block()`. Hooked from `ca_on_close()` in
       `trade_lifecycle.hpp` for any close on US500.F / USTEC.F / NAS100 / DJ30.F.
       (Bug #3 fix, `audit-fixes-32`.)

---

## GATE COMPOSITION REFERENCE
**Source of truth — refresh this section whenever a new engine is added or culled.
Most-drifted section per Installment 1+2 audits; refreshing here costs less than
reverse-engineering it from the source on every audit pass.**

### `gold_any_open` (`include/tick_gold.hpp:36-50`)
Engines that mutually block each other on XAUUSD (one-at-a-time):
- `g_gold_stack` (with profitable-trail exception — line 37: `gs_open && !gs_winning`)
- `g_bracket_gold` (GoldHybridBracket)
- `g_trend_pb_gold` (currently `enabled=false` — see RETIRED/TOMBSTONED below; included
   in the gate for future re-enable safety)
- `g_nbm_gold_london`
- `g_h1_swing_gold` (HTFSwing v2)
- `g_h4_regime_gold` (HTFSwing v2)
- `g_candle_flow` (CFE — re-enabled 2026-04-29 audit)
- `g_ema_cross` (ECE)
- `g_xauusd_fvg` (FVG — added 2026-05-02 per design doc §7.3 + open Q §11.6)

Removed at S19 Stage 1B: `g_le_stack` (LatencyEdge stack culled; comment retained line 38).
Removed at S49 X5: `g_pullback_cont`, `g_pullback_prem` (engine culled; comment retained line 45).

### `index_any_open` + post-close gap (`include/globals.hpp:291-401`)
Engines covered by the cross-engine block (Bug #3 fix, `audit-fixes-32`):
- `g_iflow_sp`, `g_iflow_nq`, `g_iflow_nas`, `g_iflow_us30`
- `g_hybrid_sp`, `g_hybrid_nq`, `g_hybrid_us30`, `g_hybrid_nas100`
- `g_minimal_h4_us30`

`g_imacro_*` instances are NOT in `index_any_open` (they hold no broker-side position;
shadow-only engine). They are still included in the gate algebra via their own
`has_open_position()` checks where relevant.

Post-close gap default: `omega::idx::g_index_min_entry_gap_sec = 120` seconds. Tunable from
`engine_init.hpp` or via config (not yet wired to `omega_config.ini`).
`record_index_close()` is called from `ca_on_close()` in `trade_lifecycle.hpp` for any
close on US500.F / USTEC.F / NAS100 / DJ30.F.

### `idx_session_ok` per-handler (`include/tick_indices.hpp`)
- `on_tick_us500`: lines 261-262 — slot 1-5 (London + NY)
- `on_tick_ustec`: lines 539-540 — slot 1-5
- `on_tick_dj30`: lines 662-663 — slot 1-5
- `on_tick_nas100`: lines 913-914 — **slot 3-4 only** (NY core; see block comment 906-911
   explaining the `fix(hbi): extend NAS100 NY-noise gate` rationale — NAS100 has no
   London-pre-open momentum, so the slot=1 retag was inappropriate)

---

## CURRENT ENGINE STATUS

### GOLD ENGINES
| Engine | Shadow | A1 | B1 | B2 | C1 | D1 | Status |
|--------|--------|----|----|----|----|-----|--------|
| GoldHybridBracket | own flag | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | LIVE (shadow if not LIVE mode) |
| MacroCrashEngine | own flag=true | ✅ FIXED 972ec3e | ✅ | n/a (B2 dead) | ✅ | ✅ | SHADOW — flip after validation |
| RSIReversalEngine | own flag=true | ✅ | ✅ FIXED d688b1e | ✅ FIXED d688b1e | ✅ | ✅ | SHADOW — flip after validation |
| NBM gold London | enabled=true (default) | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | LIVE (London 07:00-13:30 UTC, see globals.hpp:90 comment) |
| GoldStack (20 sub-engines) | via g_cfg.mode | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | LIVE (regime-gated). 20 sub-engines deferred to dedicated audit installment. |
| HTFSwing v2 — h1_swing_gold | shadow=true pinned | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | SHADOW — engine_init.hpp:471 |
| HTFSwing v2 — h4_regime_gold | shadow=true pinned | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | SHADOW — engine_init.hpp:475 |
| CandleFlow (`g_candle_flow`) | via g_cfg.mode | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | LIVE (re-enabled 2026-04-29 audit) |
| EMACross (`g_ema_cross`) | via g_cfg.mode | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | LIVE |
| XauusdFvg (`g_xauusd_fvg`) | shadow=true pinned (engine_init.hpp:86) | ✅ | ✅ | n/a (B2 dead) | ✅ | ✅ | SHADOW — pinned, cannot flip via g_cfg.mode (Installment 2 F5 inaugural row) |

(Removed 2026-05-03 refresh: `GoldFlow` row — S19 Stage 1B culled, see RETIRED below.
Moved 2026-05-03 refresh: `TrendPullback gold` row — TOMBSTONED, see RETIRED below.)

### INDEX ENGINES
| Engine | Shadow | A1 | B1 | G1 | G3 | G6 | Status | Reason / Line ref |
|--------|--------|----|----|----|----|----|--------|----------------|
| HybridBracket SP/NQ/NAS/US30 (`g_hybrid_*`) | own flag | ✅ | ✅ | ✅ FIXED d6d00c6 | n/a | ✅ FIXED audit-fixes-32 | LIVE (shadow per kShadowDefault) | Session-gated correctly; NAS100 narrowed to slot 3-4 |
| IndexFlow SP/NQ/NAS/US30 (`g_iflow_*`) | n/a | ✅ | ✅ | ✅ | n/a | ✅ FIXED audit-fixes-32 | LIVE | Active, wired correctly (tick_indices.hpp:307/582/703/954) |
| IndexMacroCrash SP/NQ/NAS/US30 (`g_imacro_*`) | shadow=true pinned (class default) | ✅ FIXED audit-fixes-32 | ✅ | ✅ | ✅ FIXED audit-fixes-32 | ✅ | SHADOW (4-symbol wired) | Per-symbol vol_ratio wired in tick_indices.hpp:347/642/782/1054 |
| VWAPReversion SP/NQ (`g_vwap_rev_sp/nq`) | n/a | ✅ (anchor reset wired despite disable) | ✅ | n/a | n/a | n/a | DISABLED | engine_init.hpp:370,373 — flip `enabled=true` to activate. Anchor reset healthy at tick_indices.hpp:157,446. |
| NBM SP/NQ/NAS/US30 (`g_nbm_*`) | enabled=false | — | — | — | — | — | DISABLED | engine_init.hpp:1456-1459 — "live data insufficient", needs shadow first |
| ORB US/GER30/UK100/ESTX50 (`g_orb_*`) | enabled=false | — | — | — | — | — | DISABLED | engine_init.hpp:1466-1469 — **rationale gap, no inline comment**. See Outstanding #3. |
| TrendPullback SP/NQ (`g_trend_pb_sp/nq`) | n/a | ✅ | ✅ | n/a | n/a | n/a | LIVE | engine_init.hpp:724,727 — RE-ENABLED S14 2026-04-24, DAILY_LOSS_CAP=$80 |
| TrendPullback GER40 (`g_trend_pb_ger40`) | n/a | ✅ | ✅ | n/a | n/a | n/a | DISABLED | engine_init.hpp:729 — "not live-validated" |
| ESNQ Divergence | enabled=false | — | — | — | — | — | DISABLED | Shadow not validated |

---

## RETIRED / TOMBSTONED ENGINES
**Engines that previously appeared in the LIVE list but have been removed from source
or hard-disabled with a tombstone comment. Do not re-introduce without explicit
re-validation against the cited rationale.**

| Engine | Status | Where | Why |
|--------|--------|-------|-----|
| GoldFlow | CULLED — header file deleted | S19 Stage 1B (2026 — pre-audit) | Engine source file `include/GoldFlowEngine.hpp` does not exist. Tombstone comments at `tick_gold.hpp:2343-2356` and `OmegaBacktest.cpp:70`. EWM ATR consumption replaced by `max(2.0pt floor, M1 bar ATR14)`. Verified by Installment 2 grep. |
| TrendPullback gold (`g_trend_pb_gold`) | TOMBSTONED — `enabled=false` | engine_init.hpp:451-464 | S44 v6 backtest verdict (148M ticks, 3933 trades): net/T = -$0.84 baseline, -$0.70 best-gate (G3: `atr>=3.0 AND spread<=0.85`); 0/24 net-positive hours; no whitelist exists. Tightest cohort still net -$0.78/T (spread cost > strategy alpha across every cohort, gate, session, year). Tombstone wording: "do not re-enable without fundamentally new logic." Source: `bt_trades.csv` (S44 v6, HEAD aa6624b0 on s44-bt-validation). |
| LatencyEdge stack (`g_le_stack`) | CULLED | S13 Finding B 2026-04-24 | Engine culled; comment line at `tick_gold.hpp:38`. |
| PullbackCont / PullbackPrem (`g_pullback_cont/prem`) | CULLED | S49 X5 | Engine culled; comment line at `tick_gold.hpp:45`. |

---

## KNOWN BUGS — FIXED THIS AUDIT (engine-checklist findings, audit-internal numbering)
| # | Bug | Impact | Fixed in |
|---|-----|--------|---------|
| 1 | MCE on_close had `if shadow_mode return` | MCE shadow trades never in GUI | 972ec3e |
| 2 | RSIReversal gated by gold_can_enter (post_impulse_block) | Never fired at RSI extreme | d688b1e |
| 3 | RSIReversal telemetry inside !shadow_mode block | GUI showed "Waiting for signal" | d688b1e |
| 4 | idx_session_ok declared in SP block, used in NQ/US30/NAS100 functions | Compile error | d6d00c6 |
| 5 | NAS100 hybrid bracket no Asia session gate | Asia drift noise trade -$24.78 | b1e2413 |
| 6 | GoldHybridBracket window starved when can_enter=false | range stayed 0.00 forever | f9c5cea |

### Cross-reference: `KNOWN_BUGS.md` (production-CSV trade-record bugs, separate numbering)
Status as of 2026-05-03:

| KNOWN_BUGS.md # | Bug | Status |
|---|-----|--------|
| 1 | MacroCrash Apr-15 phantom-fire burst (61 trades, −$9,907) | FIXED `675f063f` (audit-fixes-4): consec-loss kill counts DOLLAR_STOP; force_close updates MAE before delegating |
| 2 | HybridBracketGold Apr-7 100× P&L race (1 trade, −$3,008) | FIXED `675f063f`: `m_close_mtx` covers full close path; sanity check on `tr.pnl` magnitude |
| 3 | NAS100 cross-engine whipsaw IFLOW ↔ HBI (59 trades, −$56) | FIXED `audit-fixes-32`: `index_any_open()` predicate + post-close `idx_recent_close_block()` gap (default 120s). VPS shadow validation pending. |
| 4 | Index engines wall-clock vs tick-time (backtest replay only) | FIXED `audit-fixes-33`: `set_idx_test_clock_ms()` override + `now_ms_at_fill` parameter on `IndexHybridBracketEngine::confirm_fill`. Production unaffected. |
| 5 | `IndexHybridBracketEngine::m_pending_blocked_since` not reset by `reset_to_idle()` | FIXED `audit-fixes-34`: one-line addition to reset list. Production low-impact; backtest high. |

## OUTSTANDING ISSUES — NOT YET FIXED
| # | Issue | Engine | Impact | Priority |
|---|-------|--------|--------|---------|
| 1 | ~~IndexMacroCrash shadow returns before opening position~~ — **RESOLVED `audit-fixes-32`**. Four-symbol parity wired (globals.hpp:286-289 + tick_indices.hpp:347/642/782/1054). Shadow ledger flow validated in IndexBacktest harness. | IMACRO SP/NQ/NAS/US30 | — | RESOLVED |
| 2 | NBM indices disabled "live data insufficient" — no shadow validation done | NBM SP/NQ/NAS/US30 | Missing momentum trades on indices | MEDIUM |
| 3 | ORB disabled with no inline rationale (engine_init.hpp:1466-1469) | ORB US/GER30/UK100/ESTX50 | Missing NY open range trades (54% WR claimed). Add 2-3 line tombstone or re-validate. | MEDIUM |
| 4 | WickRejTick shelved "pending revalidation" — Sharpe=3.79 | WickRejTick gold | Highest-Sharpe shelved engine | MEDIUM |
| 5 | MeanReversionEngine regime-gated — misses RSI extremes in non-MR regime | MeanReversion (in GoldEngineStack) | Already partially addressed by RSIReversal | LOW |
| 6 | VPS shadow live-fire validation of Bug #3 fix pending | IndexFlow ↔ HBI cross-engine | KNOWN_BUGS.md target was 59 NAS100 whipsaws → 0; Mac IndexBacktest harness can't reproduce IFLOW (no L2 in HistData). Re-run python whipsaw filter on VPS-collected post-fix CSV after 24-48h. | HIGH |
| 7 | GoldEngineStack 20 sub-engines audit pending | GoldStack family | Largest deferred Phase 1 footprint — recommend dedicated installment | MEDIUM |
| 8 | HTFSwing v2 deep param walk (`make_h1_gold_params()`, `make_h4_gold_params()`) pending | HTFSwing h1/h4 gold | Architecturally complete and emitting shadow signals; no live-mode validation pass yet. | LOW |
| 9 | Sizing audit (section D) walk across all live engines pending | All engines | `risk_per_trade_usd`, `lot_min/max` clamps, `patch_size` invocations | LOW |
| 10 | Cost-realism audit (section E) walk across all live engines pending | All engines | `apply_realistic_costs()` call sites + `tr.spreadAtEntry` population | LOW |

---

## HOW TO USE THIS CHECKLIST
1. Before shipping any new engine: run through sections A-G line by line.
2. Before enabling a disabled engine: run through sections A-G + relevant section (G for indices).
3. After any bug fix: update the KNOWN BUGS table and re-verify affected checklist items.
4. This file lives in the repo root — update it in the same commit as the code change.
5. **GATE COMPOSITION REFERENCE** must be refreshed whenever an engine is added to / removed
   from `gold_any_open` or `index_any_open`. This is the most-drifted section per Installment
   1+2 audits — refreshing it costs less than reverse-engineering it from the source on
   every audit pass.
6. **Cross-link discipline.** Engine-checklist bug numbering (1-6 above) is INDEPENDENT
   of `KNOWN_BUGS.md` numbering (1-5). Always cite the source document explicitly when
   referencing a bug ID across docs.
