# Omega Engine Audit Checklist
# Updated: 2026-04-07 | Maintained every session — never let this slip

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
- [ ] B2. `gold_post_impulse_block` (20s after moves) does NOT gate engines that should
       fire AT the impulse moment (RSI extremes, crash entries)
- [ ] B3. Session gate is correct: gold 24h (dead zone 05-07 UTC only), indices slots 1-5
       (07:00-22:00 UTC), no index entries in Asia (slot 6)
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
- [ ] G1. `idx_session_ok` declared locally in EACH function (on_tick_sp, on_tick_ustec,
       on_tick_us30, on_tick_nas100) — NOT shared across functions (scope bug)
- [ ] G2. L2 imbalance (`sp_l2_imb`, `nq_l2_imb` etc) wired from `g_macro_ctx` to engine
- [ ] G3. `IndexMacroCrash` shadow_mode=true entries MUST still reach `handle_closed_trade`
       via a `TradeRecordCallback` (same bug as MCE gold — currently broken)
- [ ] G4. VWAP anchor reset on NY open (13:30 UTC) and 30min settle gate enforced
- [ ] G5. No index engine fires in Asia (slot 6) or dead zone (slot 0)

---

## CURRENT ENGINE STATUS

### GOLD ENGINES
| Engine | Shadow | A1 | B1 | B2 | C1 | D1 | Status |
|--------|--------|----|----|----|----|-----|--------|
| GoldFlow | via g_cfg.mode | ✅ | ✅ | ✅ | ✅ | ✅ | LIVE |
| GoldHybridBracket | own flag | ✅ | ✅ | ✅ | ✅ | ✅ | LIVE (shadow if not LIVE mode) |
| MacroCrashEngine | own flag=true | ✅ FIXED 972ec3e | ✅ | ⚠️ post_impulse | ✅ | ✅ | SHADOW — flip after validation |
| RSIReversalEngine | own flag=true | ✅ | ✅ FIXED d688b1e | ✅ FIXED d688b1e | ✅ | ✅ | SHADOW — flip after validation |
| TrendPullback gold | via g_cfg.mode | ✅ | ✅ | ✅ | ✅ | ✅ | LIVE |
| NBM gold London | enabled=true | ✅ | ✅ | ✅ | ✅ | ✅ | LIVE |
| GoldStack (20 engines) | via g_cfg.mode | ✅ | ✅ | ✅ | ✅ | ✅ | LIVE (regime-gated) |

### INDEX ENGINES
| Engine | Shadow | A1 | B1 | G1 | G3 | Status | Reason disabled |
|--------|--------|----|----|----|----|--------|----------------|
| SP/NQ/NAS/US30 HybridBracket | own flag | ✅ | ✅ | ✅ FIXED d6d00c6 | n/a | LIVE (shadow) | Session-gated correctly |
| IndexFlowEngine SP/NQ/NAS/US30 | n/a | ✅ | ✅ | ✅ | n/a | LIVE | Active, wired correctly |
| IndexMacroCrash SP/NQ/NAS/US30 | own flag=true | ❌ BROKEN | ✅ | ✅ | ❌ BROKEN | SHADOW broken | Same bug as MCE: shadow returns before ledger |
| VWAPReversion SP/NQ | n/a | ✅ | ✅ | n/a | n/a | LIVE (NY only) | London/NY only by design |
| NBM SP/NQ/NAS/US30 | enabled=false | — | — | — | — | DISABLED | "live data insufficient" — needs shadow first |
| ORB US/GER/UK | enabled=false | — | — | — | — | DISABLED | No documented reason — needs investigation |
| TrendPullback SP/NQ | n/a | ✅ | ✅ | n/a | n/a | LIVE | Active |
| ESNQ Divergence | enabled=false | — | — | — | — | DISABLED | Shadow not validated |

---

## KNOWN BUGS — FIXED THIS SESSION
| # | Bug | Impact | Fixed in |
|---|-----|--------|---------|
| 1 | MCE on_close had `if shadow_mode return` | MCE shadow trades never in GUI | 972ec3e |
| 2 | RSIReversal gated by gold_can_enter (post_impulse_block) | Never fired at RSI extreme | d688b1e |
| 3 | RSIReversal telemetry inside !shadow_mode block | GUI showed "Waiting for signal" | d688b1e |
| 4 | idx_session_ok declared in SP block, used in NQ/US30/NAS100 functions | Compile error | d6d00c6 |
| 5 | NAS100 hybrid bracket no Asia session gate | Asia drift noise trade -$24.78 | b1e2413 |
| 6 | GoldHybridBracket window starved when can_enter=false | range stayed 0.00 forever | f9c5cea |

## OUTSTANDING ISSUES — NOT YET FIXED
| # | Issue | Engine | Impact | Priority |
|---|-------|--------|--------|---------|
| 1 | IndexMacroCrash shadow returns before opening position — no TradeRecord | IMACRO SP/NQ/NAS/US30 | Shadow signals invisible | HIGH |
| 2 | NBM indices disabled "live data insufficient" — no shadow validation done | NBM SP/NQ/NAS/US30 | Missing momentum trades on indices | MEDIUM |
| 3 | ORB disabled with no documented reason | ORB US/GER/UK | Missing NY open range trades (54% WR) | MEDIUM |
| 4 | WickRejTick shelved "pending revalidation" — Sharpe=3.79 | WickRejTick gold | Highest Sharpe shelved engine | MEDIUM |
| 5 | MeanReversionEngine regime-gated — misses RSI extremes in non-MR regime | MeanReversion | Already partially addressed by RSIReversal | LOW |

---

## HOW TO USE THIS CHECKLIST
1. Before shipping any new engine: run through sections A-F line by line
2. Before enabling a disabled engine: run through sections A-F + relevant section (G for indices)
3. After any bug fix: update the KNOWN BUGS table and re-verify affected checklist items
4. This file lives in the repo root — update it in the same commit as the code change
