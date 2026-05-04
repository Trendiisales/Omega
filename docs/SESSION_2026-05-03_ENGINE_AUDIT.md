# SESSION 2026-05-03 — Engine Audit (Phase 1, Installment 1)

**Author:** Jo (Trendiisales/Omega) + Claude session continuation
**Scope (per Jo selection):** Live + shadow-pending engines, audited against `ENGINE_AUDIT_CHECKLIST.md` sections A-G
**Mode:** Read-only — no engine code modified
**Status:** Partial. Verifications + critical findings done. Per-engine deep audits to continue next session.
**Sister docs:** `KNOWN_BUGS.md`, `ENGINE_AUDIT_CHECKLIST.md`, `PRE_LIVE_CHECKLIST.md`

---

## TL;DR — what changed since the last checklist refresh (2026-04-07)

1. **All three documented bug fixes are present in the current source.** MCE phantom-fire fix, HBG 100× P&L race fix, and the supporting audit-fixes-4 work all match `KNOWN_BUGS.md` line refs. Verifications below.
2. **The IndexMacroCrash status in `ENGINE_AUDIT_CHECKLIST.md` is stale and wrong in a worse direction.** The checklist says "shadow returns before ledger." The engine class itself is now correctly written (no early-return on shadow_mode). But the engines `g_imacro_sp` and `g_imacro_nq` are **declared in `globals.hpp` and never receive `.on_tick()` from any caller**. Entirely dormant. There is also no `g_imacro_nas` or `g_imacro_us30` instance — only 2 of the 4 index symbols documented in the checklist exist.
3. **The `gold_post_impulse_block` gate documented in checklist B2 is permanently off** (`tick_gold.hpp:78`). The comment says "GoldStack + supervisor handle regime transitions directly." Checklist B2 needs updating to reflect this — the gate no longer exists as a runtime concern; engines that should fire at the impulse moment (RSIReversal, MCE) aren't blocked.
4. **`gold_any_open` gate composition has changed** since the 2026-04-07 checklist refresh. Three engines added/removed (FVG added; LatencyEdge culled at S13; CFE re-enabled at audit 2026-04-29). Documented below.
5. **Bug #3 (NAS100 whipsaw / `index_any_open` gate) is still pending.** No instance of `index_any_open` exists anywhere in the source tree. The fix to `tick_indices.hpp` per `KNOWN_BUGS.md` lines 116-126 has not landed.

Severity-ranked action items at the end of this document.

---

## Bug-fix verification (KNOWN_BUGS.md cross-reference)

### Bug #1 — MacroCrash Apr-15 phantom-fire burst — **FIXED in current code**

`KNOWN_BUGS.md` says fix landed in commit `675f063f` and required `m_consec_sl` to count both `SL_HIT` and `DOLLAR_STOP`. Verification:

- `include/MacroCrashEngine.hpp:998` — `if (++m_consec_sl >= 3)` increments unconditionally on the unified loss path
- `include/MacroCrashEngine.hpp:948` — `count_loss_for_kill = (reason_str == "SL_HIT" || reason_str == "DOLLAR_STOP")` — both reasons feed the kill counter
- `include/MacroCrashEngine.hpp:992-996` — explicit AUDIT 2026-04-29 C-1 comment confirms the change
- `include/MacroCrashEngine.hpp:1008-1015` — 4hr DOLLAR_STOP direction-block exists (the second prong of the fix)

**Result: PASS.** Bug cannot recur on the current binary.

### Bug #2 — HybridBracketGold Apr-7 100× P&L race — **FIXED in current code**

`KNOWN_BUGS.md` says fix required serializing `_close()` with a mutex and adding a sanity check on emitted PnL. Verification:

- `include/GoldHybridBracketEngine.hpp:715` — `mutable std::mutex m_close_mtx;`
- `include/GoldHybridBracketEngine.hpp:717-722` — `_close()` opens with `std::lock_guard<std::mutex> _lk(m_close_mtx);`
- `include/GoldHybridBracketEngine.hpp:804` — `tr.pnl = pnl_to_emit;  // sanity-checked, raw pts*size` confirms the magnitude check is wired
- `include/GoldHybridBracketEngine.hpp:708-715` — explicit AUDIT 2026-04-29 comment block documents the fix rationale

**Result: PASS.** Both close paths (manage→TP/SL hit and force_close) traverse the locked path.

### Bug #3 — NAS100 IndexFlow ↔ HybridBracketIndex whipsaw — **PENDING (no fix in code)**

`KNOWN_BUGS.md` says the fix is to add an `index_any_open` gate to `tick_indices.hpp` analogous to gold's `gold_any_open`. Verification:

- Grep across the entire source tree for `index_any_open` returns **zero matches**.
- `gold_any_open` exists at `include/tick_gold.hpp:36` and is used at line 119 in the symbol_gate composition.
- The index tick handler has no equivalent.

**Result: PENDING — no action since `KNOWN_BUGS.md` was written.** The cross-engine whipsaw can still fire on indices.

**Remediation effort estimate:** ~30 lines in `tick_indices.hpp` per `KNOWN_BUGS.md` Bug #3 specification. Should be its own commit + shadow validation, not bundled with other work.

---

## Critical findings (NEW, not yet in `ENGINE_AUDIT_CHECKLIST.md`)

### F1. IndexMacroCrash engines are dormant — declared but never ticked

The `ENGINE_AUDIT_CHECKLIST.md` "INDEX ENGINES" table marks `IndexMacroCrash SP/NQ/NAS/US30` with `❌ BROKEN` and the description "Same bug as MCE: shadow returns before ledger." That is no longer accurate.

**Engine class state:** `IndexFlowEngine.hpp:814-985` defines the `IndexMacroCrashEngine` class. The class:
- Declares `bool shadow_mode = true;` at line 816 with the "NEVER change without authorization" comment
- Opens position correctly in BOTH shadow and live (line 869: `// Do NOT return here -- position must be tracked to generate a TradeRecord on close.`)
- Tracks position lifecycle (BE lock at 1× ATR, bracket floor at 2× ATR, velocity trail at 3× ATR / 2× ATR behind MFE)
- Emits TradeRecord via `if (on_close) on_close(tr);` at line 983 — same pattern as gold MCE

So the engine code itself is structurally correct. The "shadow returns before ledger" bug from the checklist has been silently fixed.

**Wiring state — the new bug:** `globals.hpp:284-285` declares:

```cpp
static omega::idx::IndexMacroCrashEngine g_imacro_sp("US500.F"); // shadow_mode=true always
static omega::idx::IndexMacroCrashEngine g_imacro_nq("USTEC.F"); // shadow_mode=true always
```

A grep across the entire source tree (`include/`, `src/`, `backtest/`) for `g_imacro_sp.` or `g_imacro_nq.` returns no `.on_tick(...)` invocation. The instances exist but never receive bars or process trades. They are deadweight in the binary — RAM allocation and a static constructor only.

**Additional gap:** `g_imacro_nas` (NAS100) and `g_imacro_us30` (DJ30) referenced in the checklist are **not declared anywhere**. Only 2 of the 4 index symbols have an `IndexMacroCrash` instance even on paper.

**Severity: HIGH.** This is the same outstanding issue #1 from `ENGINE_AUDIT_CHECKLIST.md` but with an upgraded diagnosis. The fix path is different from what the checklist implies — it's not a shadow-return-before-ledger bug, it's a missing wiring. Either:
- Wire `g_imacro_sp.on_tick(...)` and `g_imacro_nq.on_tick(...)` into `tick_indices.hpp`'s SP and NQ handlers with `on_close = handle_closed_trade`, plus declare and wire `g_imacro_nas` / `g_imacro_us30` for parity, OR
- Explicitly retire the declarations in `globals.hpp` if IndexMacroCrash is no longer wanted on indices.

The current state — declared, dormant, in production binary — is the worst of all worlds. It uses memory and audit attention without ever firing.

### F2. `gold_post_impulse_block` is permanently disabled

`ENGINE_AUDIT_CHECKLIST.md` section B2 says: "`gold_post_impulse_block` (20s after moves) does NOT gate engines that should fire AT the impulse moment (RSI extremes, crash entries)."

Current state at `include/tick_gold.hpp:74-78`:

```cpp
static bool s_was_impulse = false;
// post-impulse block is permanently off; GoldStack + supervisor
// handle regime transitions directly.
(void)s_was_impulse;
const bool gold_post_impulse_block = false;
```

The variable is hardcoded to `false`. The `crash_impulse_bypass` logic at lines 106-110 still computes correctly but cannot affect anything because `gold_post_impulse_block` is always false at the gate composition (line 119-120: `&& (!gold_post_impulse_block || crash_impulse_bypass)`).

**Severity: LOW (informational).** This isn't a bug — the architectural decision to delegate regime transitions to GoldStack + supervisor is valid. But the checklist B2 item should be updated to read "VERIFY gold_post_impulse_block remains permanently false" rather than flagging the bypass condition, because the bypass condition is now dead code.

### F3. `gold_any_open` gate composition drift

`include/tick_gold.hpp:36-50` shows the current `gold_any_open` predicate:

```cpp
const bool gold_any_open =
    (gs_open && !gs_winning)                ||  // GoldStack blocks unless profitable trail
    g_bracket_gold.has_open_position()      ||
    g_trend_pb_gold.has_open_position()     ||
    g_nbm_gold_london.has_open_position()   ||  // London NBM also blocks
    g_h1_swing_gold.has_open_position()     ||
    g_h4_regime_gold.has_open_position()    ||
    g_candle_flow.has_open_position()       ||  // CFE re-enabled 2026-04-29
    g_ema_cross.has_open_position()         ||
    g_xauusd_fvg.has_open_position()             ;  // FVG added 2026-05-02
```

Differences from the engines listed in `ENGINE_AUDIT_CHECKLIST.md` "GOLD ENGINES" status table:

- `g_h1_swing_gold` and `g_h4_regime_gold` participate in the gate but are not in the checklist status table at all. They are HTF swing engines from `HTFSwingEngines.hpp`. They need a row in the table.
- `g_candle_flow` (CandleFlowEngine) participates — recent re-enable per audit 2026-04-29 noted in the inline comment. Not in the checklist status table.
- `g_ema_cross` participates — also missing from the status table.
- `g_xauusd_fvg` (XauusdFvgEngine) added 2026-05-02 (the "shipped" engine per the prior session handoff). Missing from the status table.
- `LatencyEdgeEngines` removed at S13 Finding B (`tick_gold.hpp:38` comment). The checklist status table doesn't list LatencyEdge separately, but `BACKTEST.md` and `ENGINE_AUDIT_CHECKLIST.md` reference latency engines elsewhere — probably worth a confirmatory note.

**Severity: MEDIUM.** Not a bug; the gate composition is correct for current production. But the checklist status table is missing four active engines (H1 swing, H4 regime, CandleFlow, EMA cross) and the recently-shipped FVG. An auditor reading the checklist today would think those engines aren't even in the system.

### F4. RSIReversal telemetry is in tick handler, not engine — checklist d688b1e fix interpretation

The 2026-04-07 checklist describes bug fix `d688b1e` as "RSIReversal telemetry inside !shadow_mode block" → fixed. Verification:

- `include/RSIReversalEngine.hpp` itself contains no `g_telemetry.UpdateLastSignal()` calls and no `if (!shadow_mode)` guard blocks. The engine class has `bool shadow_mode = true;` (line 82) and `tr.shadow = shadow_mode;` (line 628), but does not interact with telemetry directly.
- `include/tick_gold.hpp:1466-1704` is the wiring layer for RSIReversal. Lines 1689-1704 show `g_rsi_reversal.force_close(...)`, `patch_size(...)`, and the entry telemetry call sites.
- Calls like `g_telemetry.UpdateLastSignal("XAUUSD", ...)` for RSIReversal entries are at lines 889 and 910 — both unconditional (no shadow gate around them).

**Result: PASS.** The checklist intent is satisfied — telemetry fires regardless of shadow mode. The architecture is "engines emit signals; tick handler wires telemetry + close ledger" which is cleaner than embedding telemetry inside engine classes. Worth noting in a future checklist update so an auditor doesn't grep `RSIReversalEngine.hpp` looking for telemetry that lives in `tick_gold.hpp`.

### F5. `XauusdFvgEngine` is shipped and gated, but absent from the audit checklist

The prior session handoff (`SESSION_2026-05-03_FVG_VERIFIER_PASS.md`) reports the FVG engine passed verification. The engine is now wired into `gold_any_open` (`tick_gold.hpp:50`) and respects the cohort gate per its own design comments (`include/XauusdFvgEngine.hpp:58, 840`).

`ENGINE_AUDIT_CHECKLIST.md` does not have a row for `XauusdFvgEngine`. As the most recently-shipped engine, it should be the freshest entry in the status table.

**Severity: MEDIUM.** Recommend adding a row covering A1-A6, B1-B6, C1-C6, D1-D4, E1-E4, F1-F4 against the FVG engine in the next checklist refresh.

---

## Per-engine review status

This is a partial pass. I've classified each engine in scope as one of:
- **VERIFIED** — checklist items explicitly cross-checked against current source; result documented above.
- **SPOT-CHECKED** — read selected code paths; nothing alarming surfaced; full A-G walk-through deferred.
- **NOT YET** — hand off to next session.

| Engine | Status | Notes |
|---|---|---|
| MacroCrashEngine (gold) | **VERIFIED** | Bug #1 fix present (F1 above). `on_trade_record` wired in `engine_init.hpp:226`. Comments at `MacroCrashEngine.hpp:131-180` confirm shadow → ledger pipeline. |
| GoldHybridBracketEngine | **VERIFIED** | Bug #2 fix present (F2 above). `_close` mutex-serialised. |
| RSIReversalEngine | **VERIFIED** | d688b1e fix interpretation confirmed (F4 above). Telemetry lives in `tick_gold.hpp` wiring layer. |
| IndexMacroCrashEngine | **VERIFIED** | Critical finding F1 above — engine code correct, instances dormant. |
| `gold_any_open` gate composition | **VERIFIED** | F3 above — composition healthy, checklist out of date by ≥4 engines. |
| `gold_post_impulse_block` | **VERIFIED** | F2 above — permanently off. |
| `index_any_open` gate | **VERIFIED ABSENT** | F3 (Bug #3) above — never added. |
| GoldFlowEngine | **NOT YET** | `BACKTEST.md` says GoldFlowEngine.hpp removed at S19 Stage 1B (`OmegaBacktest.cpp:70`). Checklist still lists it as LIVE. **This is a discrepancy worth verifying urgently next session.** |
| GoldEngineStack (20 sub-engines) | **NOT YET** | Largest engine family. Needs a dedicated session — the 20 sub-engines each warrant a row in the status table. |
| TrendPullback (gold/sp/nq) | **NOT YET** | In `CrossAssetEngines.hpp`. Spot-checked references — telemetry wiring at `tick_indices.hpp:230, 496, 642` etc. |
| NBM gold London | **NOT YET** | Status: LIVE per checklist. NBM indices DISABLED. Both need walk-through. |
| IndexFlowEngine (SP/NQ/NAS/US30) | **SPOT-CHECKED** | Lines 1-100 read; engine class structure looks healthy; `set_shadow_mode` setter exists at `IndexFlowEngine.hpp:523` for per-engine control. Full A-G walk pending. |
| HybridBracketIndex (SP/NQ/NAS/US30) | **NOT YET** | Carries the whipsaw bug #3 weight on the wiring side. Checklist says fixed `idx_session_ok` declaration scope (`d6d00c6`) — verify. |
| VWAPReversion (SP/NQ) | **NOT YET** | Status: LIVE NY only. Anchor reset on NY open per checklist G4 — verify. |
| XauusdFvgEngine | **NOT YET** | F5 above — recommended for fresh row in checklist. The verifier passed per prior session, but the audit checklist hasn't ingested it. |

---

## Outstanding issues from `ENGINE_AUDIT_CHECKLIST.md` — current status

| # | Issue from checklist | Original priority | Current status |
|---|---|---|---|
| 1 | IndexMacroCrash shadow returns before opening position | HIGH | **DIAGNOSIS UPDATED** — engines dormant (F1 above), not "shadow returns" bug. Severity remains HIGH. |
| 2 | NBM indices disabled "live data insufficient" — no shadow validation | MEDIUM | **NOT YET RE-VERIFIED** this session. Carry over. |
| 3 | ORB indices disabled with no documented reason | MEDIUM | **NOT YET RE-VERIFIED** this session. Carry over. |
| 4 | WickRejTick shelved "pending revalidation" — Sharpe=3.79 | MEDIUM | **NOT YET RE-VERIFIED** this session. Carry over. Highest-Sharpe shelved engine — worth specific attention in Phase 3 backtest. |
| 5 | MeanReversionEngine regime-gated — misses RSI extremes in non-MR regime | LOW | **NOT YET RE-VERIFIED** this session. Carry over. The checklist notes RSIReversal "partially addresses" this — that's still true given F4 above. |

---

## Severity-ranked action items

### HIGH — block on Phase 3 backtest until resolved

1. **(F1) IndexMacroCrash wiring decision.** The two `g_imacro_*` declarations are dormant. Either wire them properly into `tick_indices.hpp` SP and NQ handlers with `on_close = handle_closed_trade`, plus declare and wire NAS/US30 instances for parity per the checklist intent — or retire the declarations from `globals.hpp` if IndexMacroCrash is no longer wanted on indices. Current state inflates the engine count without producing trades.

2. **(Bug #3) `index_any_open` gate.** Add per `KNOWN_BUGS.md` Bug #3 spec — single commit, shadow validate, then fold whipsaw exclusions into post-fix metric reads. Estimated effort ~30 lines.

3. **(GoldFlowEngine status discrepancy)** `OmegaBacktest.cpp:70` says `GoldFlowEngine.hpp` was removed at S19 Stage 1B. The audit checklist still lists `GoldFlow` as a LIVE gold engine. Either the removal was incomplete (and the engine still ships), or the checklist is wrong. **Verify which is true before any backtest run** — Phase 3 baseline numbers will be misinterpreted if GoldFlow is in the checklist but removed from build.

### MEDIUM — should be addressed before next checklist refresh commit

4. **(F3) Refresh `gold_any_open` engine list in `ENGINE_AUDIT_CHECKLIST.md`.** Add rows for `g_h1_swing_gold`, `g_h4_regime_gold`, `g_candle_flow`, `g_ema_cross`, `g_xauusd_fvg`. Confirm `LatencyEdge` engines are removed throughout the doc.

5. **(F5) Add `XauusdFvgEngine` row** to `ENGINE_AUDIT_CHECKLIST.md` "GOLD ENGINES" table. Run it through A-G fresh.

6. **(F2) Update checklist B2 wording.** `gold_post_impulse_block` is no longer a runtime concern. Item B2 should read: "Verify `gold_post_impulse_block` remains permanently false (architectural decision per session 2026-XX-XX); regime transitions delegated to GoldStack + supervisor."

7. **Walk through GoldStack 20 sub-engines.** Largest pending audit footprint. Each should occupy its own row in the status table (currently aggregated under "GoldStack").

### LOW — informational / next-session research

8. **NBM indices and ORB indices: re-verify the disabled rationale.** "Live data insufficient" and "no documented reason" are not durable explanations for engines that show ~54% WR on backtest (per the checklist outstanding issues note). Phase 3 backtest can produce the data needed to make a deploy/shelve decision.

9. **WickRejTick (Sharpe 3.79 shelved).** Re-include in Phase 3 backtest to confirm or refute the historic number. Highest-Sharpe shelved engine.

10. **PRE_LIVE_CHECKLIST.md item #1: `session_watermark_pct=0.0` in `omega_config.ini`.** Not an engine concern, but flagged in the same audit envelope. Required value `0.27` before live mode flip. Shadow-mode runs unaffected; live deploy blocked until restored.

---

## What was NOT covered this session (Phase 1 hand-off)

The following are queued for the next session's Phase 1 continuation:

- Per-engine A-G walk-through for: GoldEngineStack (20 sub-engines), GoldFlowEngine (pending S19 removal verification), TrendPullback gold + sp + nq, NBM gold London, IndexFlow SP/NQ/NAS/US30 (full walk; only spot-checked here), HybridBracketIndex SP/NQ/NAS/US30, VWAPReversion SP/NQ, XauusdFvgEngine (fresh entry).
- Confirm `idx_session_ok` declaration scope fix `d6d00c6` is present in each of the four index tick functions (`on_tick_sp`, `on_tick_ustec`, `on_tick_us30`, `on_tick_nas100`).
- Sizing audit (checklist section D) — `risk_per_trade_usd` formula, `lot_min`/`lot_max` clamps, `patch_size` invocations.
- Cost-realism audit (checklist section E) — `apply_realistic_costs()` call sites and `tr.spreadAtEntry` population.
- IndexFlow-specific section G — full walk-through per checklist.

Estimated next-session scope: 5-7 engines deeply audited + checklist drift items above. That should close Phase 1.

---

## Phase 2 / Phase 3 implications from this installment

- **Phase 2 (sim_lib defense parity)** is unaffected by these findings. Continue planning Stage 2 as agreed.
- **Phase 3 (OmegaBacktest)** is impacted. Action item #3 (GoldFlowEngine status) must be resolved before the baseline backtest run, otherwise the per-engine attribution table will be ambiguous. Action item #1 (IndexMacroCrash wiring) does not block Phase 3 if we agree the engines stay dormant for now — they'll just produce zero trades in the backtest, which is consistent with their dormant production state.
- Action items #4/#5/#6 are documentation hygiene — they don't gate the backtest but should be cleared before the audit deliverable is closed.

---

## Files touched this session

- **Read-only (no modification):** `include/MacroCrashEngine.hpp`, `include/GoldHybridBracketEngine.hpp`, `include/RSIReversalEngine.hpp`, `include/IndexFlowEngine.hpp`, `include/tick_gold.hpp` (sections), `include/globals.hpp` (sections), `backtest/OmegaBacktest.cpp` (header section), and `ENGINE_AUDIT_CHECKLIST.md` / `KNOWN_BUGS.md` / `BACKTEST.md` / `README.md` / `SYMBOLS.md` / `PRE_LIVE_CHECKLIST.md`.
- **Created:** This file (`docs/SESSION_2026-05-03_ENGINE_AUDIT.md`).

— END INSTALLMENT 1 —
