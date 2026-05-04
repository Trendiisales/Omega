# SESSION 2026-05-03 — Engine Audit (Phase 1, Installment 2)
**Author:** Jo (Trendiisales/Omega) + Claude session continuation
**Scope (continued from Installment 1):** GoldFlow S19 verification, `idx_session_ok` fix verification, IndexFlow / IndexHybridBracket / TrendPullback / VWAPReversion / NBM gold London / XauusdFvg deep-walks, plus engine-state drift between `ENGINE_AUDIT_CHECKLIST.md` and `engine_init.hpp`.
**Mode:** Read-only — no engine code modified.
**Status:** Continues. Installment 1 HIGH action items #1 and #3 are now resolved or upgraded; Bug #3 (`index_any_open`) still pending. GoldStack 20 sub-engines and HTFSwing v2 deferred to Installment 3.
**Sister docs:** `KNOWN_BUGS.md`, `ENGINE_AUDIT_CHECKLIST.md`, `PRE_LIVE_CHECKLIST.md`, `SESSION_2026-05-03_ENGINE_AUDIT.md` (Installment 1).
**Audit target:** Branch `feature/usdjpy-asian-open` at HEAD `df12012`. Local checkout is what Installment 1 audited.
**Working environment note:** The interactive Linux sandbox was unavailable this installment ("no space left on device" on useradd), so a PAT-driven `git fetch origin` to confirm the local checkout matches `origin/feature/usdjpy-asian-open` could not be performed in this session. All findings below were verified against the working tree directly (which is the same source Installment 1 used). Action item Verify-A at the end of this document captures the deferred fetch.

---

## TL;DR — what changed since Installment 1
1. **Installment 1 HIGH action item #3 is RESOLVED.** `GoldFlowEngine` is genuinely retired — the file does not exist anywhere in the source tree, the production tick path has zero live references, and the only mentions are tombstone comments. The audit checklist listing GoldFlow as LIVE is wrong by omission and must be updated. `OmegaBacktest.cpp:70` and `tick_gold.hpp:2355` confirm S19 Stage 1B removal in inline comments.
2. **The `idx_session_ok` declaration scope fix `d6d00c6` is present in all four index tick handlers** — `on_tick_us500`, `on_tick_ustec`, `on_tick_dj30`, `on_tick_nas100`. NAS100 has been further tightened to slot 3-4 only (NY core), per a separate `fix(hbi)` commit visible in the reflog.
3. **Six new drift findings** between `ENGINE_AUDIT_CHECKLIST.md` and the actual `engine_init.hpp` enable/shadow state. Most consequential: `VWAPReversion SP/NQ` are listed as "LIVE NY only" in the checklist but are explicitly disabled at `engine_init.hpp:370` and `:373`. Several other engines are similarly drifted.
4. **`TrendPullback gold` is permanently disabled** with a detailed tombstone (`engine_init.hpp:451-464`) — "do not re-enable without fundamentally new logic." Net/T negative across every cohort, gate, session, and year per the S44 v6 backtest harness. The audit checklist still appears to treat it as a LIVE gold engine.
5. **The Installment 1 F1 finding (IndexMacroCrash dormant)** is reaffirmed with stronger evidence: the gate composition in `tick_indices.hpp` SP/NQ/US30/NAS does not even reference `g_imacro_*` as a `has_open_position()` exclusion. The instances are not just untrained — they are completely absent from the gate algebra.
6. **No new mutex bug.** `IndexHybridBracketEngine` lacks the close-path mutex that `GoldHybridBracketEngine` and `XauusdFvgEngine` both carry, but it has only a single close path (through `manage()`), so the gold Bug #2 race pattern does not apply to it.
7. **Bug #3 (`index_any_open`) remains outstanding.** No code changes since Installment 1. Status unchanged.

Severity-ranked action items at the end of this document, refreshed.

---

## Bug-fix and outstanding-item verification (Installment 1 carry-overs)

### Action item #3 (HIGH) — GoldFlowEngine S19 removal verification — **RESOLVED**
Installment 1 flagged this as the most urgent Phase 3 blocker because `OmegaBacktest.cpp:70` claimed `GoldFlowEngine.hpp` was removed at S19 Stage 1B while `ENGINE_AUDIT_CHECKLIST.md` still listed `GoldFlow` as a LIVE gold engine. Verification this installment:

- `Glob **/GoldFlow*` across the repo returns zero matches. The header file `include/GoldFlowEngine.hpp` does not exist on disk on `feature/usdjpy-asian-open` HEAD `df12012`.
- `include/globals.hpp` has zero references to `GoldFlow`, `g_gold_flow`, or `GoldFlowEngine`.
- `include/engine_init.hpp` has zero references.
- `include/tick_gold.hpp` retains six tombstone-style comments at lines 2343, 2345, 2349, 2350, 2352, 2355, 2356 documenting the removal — `// (GoldFlow-related code removed S19 Stage 1B — engine culled)` — and the `cfe_gf_atr` field that previously consumed GoldFlow's EWM ATR is also gone, replaced by `max(2.0pt floor, M1 bar ATR14)`.
- `backtest/OmegaBacktest.cpp:70` carries the explicit comment `// (GoldFlowEngine.hpp removed at S19 Stage 1B — engine culled.)` as a placeholder where the include used to be (between `GoldEngineStack.hpp` and `LatencyEdgeEngines.hpp`), and `OmegaBacktest.cpp:557-559` explains that the backtest feeds `flow_live=false` and `flow_be_locked=false` to engines that previously consumed GoldFlow output, because there is no flow engine in the backtest.

**Outcome.** GoldFlow is fully retired. `ENGINE_AUDIT_CHECKLIST.md` "GOLD ENGINES" status table must remove the GoldFlow row entirely (or move it to a "RETIRED" section with a forward link to the S19 cull doc). Phase 3 baseline backtest can proceed without ambiguity on this point.

### Outstanding issue #1 (HIGH) from `ENGINE_AUDIT_CHECKLIST.md` — IndexMacroCrash dormancy — **REAFFIRMED, EVIDENCE STRENGTHENED**
Installment 1 F1 said: engine class is correct, but the two `g_imacro_*` instances declared at `globals.hpp:284-285` are never `.on_tick(...)` called from anywhere, and `g_imacro_nas` / `g_imacro_us30` are not declared at all. Stronger evidence this installment:

- A targeted grep `g_imacro_\w+\.on_tick` across the entire repo returns zero call sites in production code. The only match is the Installment 1 doc itself recommending the wiring be added.
- The four index tick handlers (`on_tick_us500`, `on_tick_ustec`, `on_tick_dj30`, `on_tick_nas100`) compose their `hybrid_*_can_enter` exclusion gates from active engine `has_open_position()` checks — for example `tick_indices.hpp:266-272` for SP excludes `g_eng_sp`, `g_bracket_sp`, `g_orb_us`, `g_vwap_rev_sp`, `g_trend_pb_sp`, `g_nbm_sp`, `g_iflow_sp`. **`g_imacro_sp` is not in the exclusion list** because it never opens a position. The instances are absent from the runtime gate algebra entirely.
- `engine_init.hpp` does not call `set_shadow_mode(...)` or any other initializer on `g_imacro_sp` / `g_imacro_nq`. Even the per-engine config block that exists for every other index engine is missing for IndexMacroCrash.

**Outcome.** Status unchanged from Installment 1: HIGH severity. The two declared instances are deadweight in the binary. Either wire them with `on_close = handle_closed_trade` on the four-symbol parity (and add NAS/US30 declarations) or retire the declarations from `globals.hpp:284-285`. Carrying the declarations without ticks is the worst of all worlds.

### `idx_session_ok` declaration scope fix `d6d00c6` — **PRESENT IN ALL FOUR HANDLERS**
Installment 1 hand-off asked Installment 2 to confirm the declaration scope fix landed in each of `on_tick_sp`, `on_tick_ustec`, `on_tick_us30`, `on_tick_nas100`. The actual function names are `on_tick_us500` (line 31), `on_tick_ustec` (line 351), `on_tick_dj30` (line 605), and `on_tick_nas100` (line 829). All four declare `const bool idx_session_ok` at the local block scope inside the entry-gate composition:

- `on_tick_us500`: `tick_indices.hpp:261-262` — `g_macro_ctx.session_slot >= 1 && session_slot <= 5`. Used at line 265 inside `hybrid_sp_can_enter`.
- `on_tick_ustec`: `tick_indices.hpp:539-540` — same slot 1-5 window. Used at line 543 inside `hybrid_nq_can_enter`.
- `on_tick_dj30`: `tick_indices.hpp:662-663` — same slot 1-5 window. Used at line 666 inside `hybrid_us30_can_enter`.
- `on_tick_nas100`: `tick_indices.hpp:913-914` — **TIGHTENED to slot 3-4** (NY core only). Used at line 917 inside `hybrid_nas_can_enter`. The block comment at lines 906-911 explains: "NAS100 is a US tech index; meaningful liquidity is overlap + NY open only. The slot=1 retag (05-07 UTC -> 'London open' per on_tick.hpp:278) was intended for instruments with London-pre-open momentum; NAS100 has none." This narrowing is the visible end-state of the `fix(hbi): extend NAS100 NY-noise gate` commit visible in the reflog at index 267.

**Outcome.** PASS for all four handlers. Each declaration is properly block-scoped — no shadowing or leak between handlers. Installment 1's hand-off line item is closed.

### Bug #3 (`index_any_open`) — **STILL OUTSTANDING**
A re-grep this installment confirms: zero matches for `index_any_open` anywhere in the repo. No code changes since Installment 1. The fix from `KNOWN_BUGS.md` Bug #3 has not landed. Severity unchanged: blocks Phase 3 cross-engine NAS100 whipsaw analysis.

---

## New drift findings — `ENGINE_AUDIT_CHECKLIST.md` vs. `engine_init.hpp` reality

Installment 1 found that the `gold_any_open` gate composition had drifted from the checklist by ≥4 engines (F3). This installment surfaces a parallel class of drift on the `engine_init.hpp` side: many engines the checklist labels as "LIVE" or "LIVE NY only" are actually disabled at init time, or run only in shadow mode regardless of `g_cfg.mode`. These are not bugs — the disables and shadow pins are deliberate — but the audit checklist is the canonical reference auditors read first, and it materially mis-describes the runtime configuration. List below, all line refs are to `include/engine_init.hpp` unless stated.

### H1. VWAPReversion SP/NQ are explicitly disabled at init
`g_vwap_rev_sp.enabled = false` at line 370. `g_vwap_rev_nq.enabled = false` at line 373. `EXTENSION_THRESH_PCT` and `COOLDOWN_SEC` are still tuned (0.35/0.40 and 300s respectively), implying the disable is recoverable on a single line flip — but until that flip, both engines log nothing and trade nothing. The `ENGINE_AUDIT_CHECKLIST.md` "INDEX ENGINES" status table lists `VWAPReversion (SP/NQ)` as "LIVE NY only".

The supporting infrastructure is healthy:
- `reset_ewm_vwap(anchor)` is called on NY open at `tick_indices.hpp:157` (SP, with `s_sp_ny_open`) and `:446` (NQ, with `s_nq_ny_open`). Checklist item G4 ("anchor reset on NY open") technically PASSES on the wiring side — the anchor gets reset every NY session even though the engine is disabled.
- Force-close hooks are wired in `quote_loop.hpp:814-815` (shutdown) and `:913-914` (panic), and `:1082` (shutdown_cb), so if the enable flag flips, the lifecycle hooks fire correctly.

**Severity: MEDIUM.** Documentation drift, not a bug. But for a Phase 3 backtest, attributing zero VWAPReversion trades to "we disabled it" vs "the checklist said it was on" matters for the per-engine attribution table.

### H2. TrendPullback gold is permanently tombstoned
`engine_init.hpp:451-464` carries a detailed disable rationale block — too long to inline, but the headline numbers from the comment:

- S44 v6 backtest harness, HEAD `aa6624b0` on `s44-bt-validation` branch: net/T = -$0.84 baseline, -$0.70 best-gate (G3: `atr>=3.0 AND spread<=0.85`).
- Zero of 24 net-positive hours under G3. No whitelist exists.
- 2025 net/T = -$0.69, 2026 net/T = -$0.71. No 2026 edge.
- Tightest cohort (mae<1pt, 99.6% WR, gross +$0.037/T) still net -$0.78/T because spread cost > strategy alpha across every cohort, gate, session, year.
- Concluding line: "Tombstone: do not re-enable without fundamentally new logic."

`g_trend_pb_gold.enabled = false` at line 464. The engine is still wired (`tick_gold.hpp:1934, 1999`) and configured upstream (`PULLBACK_BAND_PCT`, `H4_GATE_ENABLED`, `ATR_SL_MULT`, etc.) but those settings are irrelevant while `enabled=false`.

The audit checklist appears to treat TrendPullback gold as a LIVE engine. It is not, and per the tombstone, will not be without fresh strategy work.

**Severity: MEDIUM.** Same class as H1 — documentation drift. But the tombstone is more emphatic than H1's disable, so the checklist correction here is about retiring the engine row, not just flipping a status column.

### H3. TrendPullback ger40 is disabled — "not live-validated"
`g_trend_pb_ger40.enabled = false` at line 729 with the inline comment "DISABLED: not live-validated". TrendPullback nq and sp are re-enabled at lines 724 and 727 with detailed S14 2026-04-24 rationale: "Apr 2 post-mortem +$200 strategy-only ($540 FC loss was connectivity, since patched by Hard Stop arch). Mar 27 +$1,183 on 8 paired trades. DAILY_LOSS_CAP=$80 active." So three of four TrendPullback symbols are split: gold/ger40 disabled, nq/sp enabled.

The checklist appears to list TrendPullback as a single status row covering all symbols. It needs to be split per-symbol.

**Severity: LOW.** Documentation drift. The disabled state is intentional per the inline rationale.

### H4. NBM SP/NQ/NAS/US30 all disabled — confirms checklist outstanding issue #2
`g_nbm_sp.enabled = false` line 1456, `g_nbm_nq.enabled = false` line 1457, `g_nbm_nas.enabled = false` line 1458, `g_nbm_us30.enabled = false` line 1459. This matches the `ENGINE_AUDIT_CHECKLIST.md` outstanding issue #2 ("NBM indices disabled — live data insufficient — no shadow validation"). The disable is confirmed in source.

**Severity: LOW (confirmation, not new).** No new action required beyond what's already in the checklist outstanding issues list.

### H5. All ORB instances disabled — confirms checklist outstanding issue #3
`g_orb_us.enabled = false` line 1466, `g_orb_ger30.enabled = false` line 1467, `g_orb_uk100.enabled = false` line 1468, `g_orb_estx50.enabled = false` line 1469. Matches checklist outstanding issue #3 ("ORB indices disabled with no documented reason"). The disable is confirmed in source — and yes, no rationale block precedes lines 1466-1469. The four lines stand alone with no comment.

**Severity: LOW (confirmation), but the rationale gap is real.** Phase 3 backtest can produce numbers to make a deploy/shelve decision. The disable lines themselves should carry a 2-3 line tombstone explaining the rationale once that decision is made.

### H6. NBM gold London is enabled and live-wired (and not in the H4 disable list)
`g_nbm_gold_london` does not appear in the `engine_init.hpp` disable block at lines 1456-1459. The default `enabled = true` from the class declaration carries through. The engine is `.on_tick(...)`-called at `tick_gold.hpp:2259`. This matches the checklist's "LIVE (London only)" claim for NBM gold London — checklist is correct here.

**Severity: NONE — informational.** Including this for completeness so the checklist refresh doesn't accidentally cull NBM gold London thinking the H4 disable list applies to it too. It does not.

---

## Per-engine notes

### IndexFlowEngine SP/NQ/NAS/US30 — production wiring verified

Class lives at `include/IndexFlowEngine.hpp:282` (containing `IndexFlowEngine` itself, plus `IndexMacroCrashEngine` at 808 and `IndexSwingEngine` at 1129 in the same translation unit). Verifications:

**Wiring (A1-A2).** All four instances declared at `globals.hpp:280-283` and ticked from `tick_indices.hpp` at:
- SP: lines 307 (open-position branch) and 316 (entry branch, `is_signal_path=true`).
- NQ: lines 582 / 590.
- US30: lines 703 / 709.
- NAS: lines 954 / 960.
The `bool` 6th argument distinguishes warmup-only vs. signal-emitting paths — see line 316 `const auto isig = g_iflow_sp.on_tick(sym, bid, ask, sp_l2_imb, ca_on_close, true);`.

**Shadow mode handling (A3-A4).** `set_shadow_mode(bool)` proxy setter at line 523 lets `engine_init.hpp:37-40` flip per-instance shadow state to `kShadowDefault` (where `kShadowDefault = (g_cfg.mode != "LIVE")` — see line 17). Close path at lines 480-482 emits TradeRecord with `tr.shadow = shadow_mode` then `if (on_close) on_close(tr)` — TradeRecord emits regardless of shadow. Same pattern at lines 557, 982-983, 1334-1341 for the other engine classes in the same file.

**Force-close (A5).** Each engine has `force_close(bid, ask, on_close)` (`include/IndexFlowEngine.hpp:526` for IndexFlow proper, plus the IndexMacroCrash and IndexSwing variants in the same file).

**Sizing (A6, D1-D4).** `patch_size(double lot)` at line 516 with active+positive guard at the position level (line 457). Risk computation lives in the per-symbol `IndexSymbolCfg` block at line 113+ — values are in price points, dollar risk computed as `risk_pts * lot * usd_per_pt`. US500.F=$50/pt, USTEC.F=$20/pt, NAS100=$1/pt, DJ30.F=$5/pt per the comment at lines 12-15.

**No mutex on close.** Single close path per position; no concurrent `_close()` + `force_close()` race.

**Status:** PASS. Engine architecture is healthy. Production wiring matches design. Shadow/live transition is config-driven via `kShadowDefault`.

### IndexHybridBracketEngine SP/NQ/NAS/US30 — single-path close, no mutex needed

Class lives at `include/IndexHybridBracketEngine.hpp:246`. Carries the cross-engine whipsaw weight from Bug #3 on the wiring side. Verifications:

**Wiring (A1-A2).** `g_hybrid_sp/nq/us30/nas100` (the four instances; the previous pattern referenced `g_bracket_sp/nq/us30/nas100` which is the older `BracketEngine` family) — both pairs are wired into `tick_indices.hpp` and both appear in the `hybrid_*_can_enter` exclusion gates. The dual-bracket presence is intentional, with each engine type taking a separate slot in the same `idx_session_ok` window.

**Shadow handling and close path.** `shadow_mode = true` default at line 250. Shadow-fill simulation at line 334 — `if (shadow_mode) { /* self-fill against bracket levels */ }` — this is the analog of the pattern Installment 1 was concerned about for IndexMacroCrash, but reading the surrounding code (lines 325-339), the pattern is correctly the SHADOW SIMULATION fill path, not a "shadow returns before ledger" bug. In live mode the engine waits for broker fill confirmation; in shadow mode it self-fills when price crosses bracket levels and emits TradeRecord through the same `manage()`/exit path. The TradeRecord is emitted at lines 670-694 with `tr.shadow = shadow_mode` and `if (on_close) on_close(tr)` regardless of shadow.

**Single close path.** The engine has only `manage()` (line 570) for close — no separate `_close()` or `force_close()` that could race. The Bug #2 100× P&L pattern from gold cannot occur here. No mutex is required.

**MFE/MAE tracking (B/C).** Line 575-580: `move > pos.mfe → pos.mfe = move`; `move < pos.mae → pos.mae = move`. MAE convention matches gold HBG (`pos.mae` stays ≤ 0, represents worst against-position move). Tracked across the position lifecycle, written to the TradeRecord at lines 670-680 — `tr.mfe = pos.mfe * pos.size`, `tr.mae = pos.mae * pos.size`, `tr.spreadAtEntry = pos.spread_at_entry`.

**S51 1A.1.a fix (line 678).** "was hardcoded 0.0; now real spread captured at fill" — this is the spread-at-entry fix that lets `apply_realistic_costs()` populate `slippage_entry`/`slippage_exit` correctly. Applied at line 678 with the inline AUDIT comment.

**S23 trail-arm guards (line 586-589).** Ported from gold S20 commit `70bc25b6`. BE lock + every subsequent trail step require BOTH `pos.mfe >= cfg_.min_trail_arm_pts` AND `(now_s - pos.entry_ts) >= cfg_.min_trail_arm_secs`. Cross-engine consistency with gold HBG.

**Status:** PASS. Single-path close eliminates Bug #2-style race. Shadow handling is correct. Bug #3 (`index_any_open`) cross-engine whipsaw remains the outstanding wiring issue, not this engine itself.

### TrendPullbackEngine gold/sp/nq/ger40 — split status

Class at `include/CrossAssetEngines.hpp:1471`. Per H2 and H3 above, the runtime status splits per symbol:

| Symbol | enabled | shadow_mode | Notes |
|---|---|---|---|
| gold | false | kShadowDefault (irrelevant while disabled) | Tombstoned (engine_init.hpp:451-464). Wired at tick_gold.hpp:1934, 1999 but never fires while disabled. |
| sp | true | kShadowDefault | RE-ENABLED S14 2026-04-24 (line 727). DAILY_LOSS_CAP=$80 active. |
| nq | true | kShadowDefault | RE-ENABLED S14 2026-04-24 (line 724). DAILY_LOSS_CAP=$80 active. |
| ger40 | false | kShadowDefault (irrelevant while disabled) | "not live-validated" (line 729). |

Close path at lines 1665/1668, 1701/1704, 1735/1738, 1776/1826 emits TradeRecord with `tr.shadow = shadow_mode` and `on_close(tr)` regardless of shadow. Force-close handles available per the broader CrossAssetEngines pattern.

**Pyramid logic.** Configured for gold (`PYRAMID_ENABLED = true`, `PYRAMID_SIZE_MULT = 0.5`, `PYRAMID_MAX_ADDS = 1` at engine_init.hpp:447-449) — but irrelevant given gold is disabled. Re-confirm pyramid configuration on sp/nq instances if those flip out of shadow.

**Status:** Mixed. Architecturally healthy; runtime config drifts from checklist as documented under H2/H3.

### VWAPReversionEngine SP/NQ — disabled but anchor reset wired

Class at `include/CrossAssetEngines.hpp:1139`. Per H1, both instances are disabled at init. Despite the disable:

**Anchor reset wiring is healthy.** `reset_ewm_vwap(anchor)` at line 1164 is called from `tick_indices.hpp:157` (SP, on `s_sp_ny_open`) and `:446` (NQ, on `s_nq_ny_open`). Even with the engine disabled, the anchor is freshly seeded each NY session — so flipping `enabled = true` at any future point starts the engine from a clean per-session anchor with no stale-VWAP risk.

**Time-based EWM decay.** Half-life 7200s (2hr) — `EWM_VWAP_HALF_LIFE_SEC` at line 1161. Decay formula `α = 1 - exp(-dt/T₁)` invariant to tick frequency (line 1186-1208). At dt=1s, α ≈ 0.000139. After 2hr, ~50% decayed.

**Cooldown ladder.** `COOLDOWN_SEC=180` (normal), `MAE_COOLDOWN_SEC=600` (after MAE exit), `CONSEC_FC_BLOCK_SEC=1800` (30min block after 2 consec MAE same-direction), `TP_FLIP_COOLDOWN_SEC=1200` (20min block on opposite direction after TP because TP = price crossed VWAP and trend may continue). This is well-considered but currently dormant.

**Force-close** at line 1406-1407 delegating to `pos_.force_close(bid, ask, on_close)`.

**Status:** Architecturally healthy and well-tuned. Currently disabled per H1. Phase 3 backtest can use `enabled=true` flag flip + 26-month rerun to verify the engine still earns its tuning before any live promotion.

### NoiseBandMomentumEngine — gold London live, indices disabled

Class at `include/CrossAssetEngines.hpp:2212`. Six instances: `g_nbm_sp/nq/nas/us30` (all disabled per H4) plus `g_nbm_gold_london` and `g_nbm_oil_london` (both default-enabled, not overridden in `engine_init.hpp`).

**Gold London wiring.** `.on_tick(sym, bid, ask, ca_on_close)` at `tick_gold.hpp:2259`. Session window per `globals.hpp:90` comment is "London 07:00-13:30 UTC" — meaningful liquidity overlap with London session before NY open.

**Status:** Gold London PASS. Indices currently disabled per H4. Oil London not separately audited this installment.

### XauusdFvgEngine — F5 from Installment 1, fresh row

Class at `include/XauusdFvgEngine.hpp:97`. The most recently-shipped engine (per the prior session's `SESSION_2026-05-03_FVG_VERIFIER_PASS.md`). Verifications:

**A — Shadow/ledger.** `shadow_mode = true` at line 160. `tr.shadow = shadow_mode` at line 1163. `on_close(tr)` at line 1196 (unconditional, regardless of shadow). The header comment at line 53 explicitly notes the shadow_mode is "pinned to true (NOT kShadowDefault)" — and `engine_init.hpp:86` confirms with `g_xauusd_fvg.shadow_mode = true;`. So unlike index/cross-asset engines, XauusdFvg cannot be flipped live by changing `g_cfg.mode` alone — it requires explicit code change. This is the safe default for a freshly-shipped engine.

**B — Close-path mutex (Bug #2 prophylactic).** `mutable std::mutex m_close_mtx` at line 443. `std::lock_guard<std::mutex> _lk(m_close_mtx)` at line 1053. Engine adopts the same pattern as `GoldHybridBracketEngine` (the Bug #2 fix template) from the start, even though by current design it has only one close path. This is defensive, future-proof against any later force-close addition.

**C — Cohort gate participation.** Wired into `gold_any_open` at `tick_gold.hpp:50` per Installment 1 F3, and respects the cohort gate per its own design comments at `include/XauusdFvgEngine.hpp:58, 840`. Direction-aware blocks for opposite-side fills in the same cohort.

**D — Sizing.** Cancel hook injected at `engine_init.hpp:87` (`g_xauusd_fvg.cancel_fn = [](const std::string& id) { send_cancel_order(id); }`). Close callback at line 88. So both order-cancel and order-close infrastructure are wired.

**E — Cost realism.** TradeRecord emission via the shared `apply_realistic_costs()` path; `tr.spreadAtEntry` populated via the position-level capture at fill (per the design doc).

**F — Telemetry.** Per-position state surfaceable via `g_xauusd_fvg.has_open_position()` and `g_xauusd_fvg.m_pos` (referenced from `engine_init.hpp:2119, 2121`). Wired into the same telemetry conventions as other gold engines.

**Status:** PASS for the fresh A-F walk. Recommend `ENGINE_AUDIT_CHECKLIST.md` add a "GOLD ENGINES" status row covering A-G with this verification as the inaugural entry.

---

## Severity-ranked action items (refreshed against Installment 1 + new findings)

### HIGH — block on Phase 3 backtest until resolved
1. **(F1, REAFFIRMED) IndexMacroCrash wiring decision.** Two declared instances (`g_imacro_sp`, `g_imacro_nq`), zero `.on_tick` calls, zero participation in entry-gate exclusions, zero `engine_init.hpp` configuration. Either wire the four-symbol parity (declare `g_imacro_nas`, `g_imacro_us30`, then `.on_tick(...)` all four from `tick_indices.hpp`) or retire the two existing declarations from `globals.hpp:284-285`. Decision must precede Phase 3 because the dormant declarations bias the engine count and the per-engine attribution table.
2. **(Bug #3, UNCHANGED) `index_any_open` gate.** Add per `KNOWN_BUGS.md` Bug #3 spec — single commit, shadow validate, then fold whipsaw exclusions into post-fix metric reads. Estimated effort ~30 lines in `tick_indices.hpp`.
3. **(GoldFlow, RESOLVED — documentation only)** Update `ENGINE_AUDIT_CHECKLIST.md` "GOLD ENGINES" table to remove `GoldFlow` row (or move to "RETIRED"). The S19 Stage 1B cull is fully landed in source.

### MEDIUM — should be addressed before next checklist refresh commit
4. **(H1) Update VWAPReversion SP/NQ status in checklist.** Both instances are `enabled=false` at `engine_init.hpp:370, 373`. Checklist's "LIVE NY only" label is wrong. Either re-enable on a tested commit or update the checklist to reflect dormant status.
5. **(H2) Tombstone TrendPullback gold in the checklist.** Engine has a full disable rationale block at `engine_init.hpp:451-464` ending in "do not re-enable without fundamentally new logic." Move to a "RETIRED" or "TOMBSTONED" section in the checklist with link back to the inline rationale.
6. **(H3) Split TrendPullback row per-symbol.** Gold/ger40 disabled, nq/sp re-enabled with rationale. Single-row checklist entry hides the asymmetry.
7. **(F3 from Installment 1) Refresh `gold_any_open` engine list in checklist.** Add `g_h1_swing_gold`, `g_h4_regime_gold`, `g_candle_flow`, `g_ema_cross`, `g_xauusd_fvg`. (Repeated from Installment 1 — still outstanding.)
8. **(F5 from Installment 1) Add `XauusdFvgEngine` row.** Use the A-F verification above as the inaugural entry. (Repeated from Installment 1.)
9. **(F2 from Installment 1) Update checklist B2 wording.** `gold_post_impulse_block` is permanently false; the bypass condition is dead code. (Repeated from Installment 1.)
10. **(H5) Add ORB disable rationale.** `g_orb_us/ger30/uk100/estx50` disable lines at `engine_init.hpp:1466-1469` carry no comment. Add a 2-3 line tombstone or link to the cull doc.

### LOW — informational / next-session research
11. **(H4) NBM indices: re-verify the "live data insufficient" rationale during Phase 3.** Outstanding from Installment 1 carry-over #2; H4 confirms the disable is intentional but the rationale is older than current calibration data.
12. **WickRejTick (Sharpe 3.79, shelved).** Outstanding from Installment 1 carry-over #4. Highest-Sharpe shelved engine. Not visited this installment.
13. **PRE_LIVE_CHECKLIST.md item #1: `session_watermark_pct=0.0` in `omega_config.ini`.** Outstanding from Installment 1. Required value `0.27` before live-mode flip. Shadow runs unaffected.
14. **GoldStack 20 sub-engines.** Largest deferred audit footprint. Each should occupy its own row in the status table. Recommend a dedicated installment.
15. **HTFSwing v2 (`g_h1_swing_gold`, `g_h4_regime_gold`).** Pinned `shadow_mode=true` at `engine_init.hpp:471, 475` with `enabled=true`. Architecturally complete and emitting shadow signals; no live-mode validation pass yet visible. Worth its own audit row in the next checklist refresh.

### Verify-A — deferred from this installment
**Verify the local checkout matches `origin/feature/usdjpy-asian-open`.** The interactive Linux sandbox was unavailable this installment, so a PAT-driven `git fetch origin feature/usdjpy-asian-open` plus a `git rev-parse HEAD` comparison could not be performed. Local HEAD is `df12012`. Origin HEAD per the most recent local FETCH_HEAD record is `16a4d2a`, which would imply the local has commits that have not been fetched back, but FETCH_HEAD is older than the packed-refs entry for `origin/main` (`207323d`), so the divergence read is unreliable without a fresh fetch. When the sandbox returns, run:
- `git -C /Users/jo/omega_repo fetch origin feature/usdjpy-asian-open`
- `git -C /Users/jo/omega_repo rev-parse HEAD origin/feature/usdjpy-asian-open`
If they match, every line ref in this document and Installment 1 is accurate against origin. If they diverge, re-run the bug-fix verifications (#1, #2, F1, F3, F5 from Installment 1; H1-H6 from this installment) against the diverging hunks.

---

## What was NOT covered this session (Phase 1 hand-off, refreshed)
- **GoldEngineStack 20 sub-engines.** Largest pending audit footprint. Each sub-engine should occupy its own status-table row. Recommend a dedicated installment 3.
- **HTFSwing v2 deep walk** — only confirmed wiring + pinned-shadow this installment. Per-instrument param walk-through (`make_h1_gold_params()`, `make_h4_gold_params()`) deferred.
- **`OmegaCostGuard.hpp` and `apply_realistic_costs()` call sites** — section E of the checklist. Spot-checked via the `tr.spreadAtEntry` references this installment but a full E1-E4 walk across all live engines is pending.
- **Sizing audit (section D)** — `risk_per_trade_usd` formula, `lot_min`/`lot_max` clamps, and `patch_size` invocations across engines. Not done this installment.
- **MeanReversionEngine** (`GoldEngineStack.hpp:1737`) — Installment 1 outstanding issue #5 ("regime-gated, misses RSI extremes in non-MR regime"). Not re-verified this installment.
- **WickRejTick / shelved-engine pass.** Carry-over from Installment 1 #4.
- **NBM oil London** — `g_nbm_oil_london` exists alongside gold London but was not specifically walked.

Estimated next-session scope: GoldStack family (which is the bulk of the deferred surface area) + HTFSwing v2 + sizing/cost-realism walk-through. That should close Phase 1 of the audit.

---

## Phase 2 / Phase 3 implications from this installment
- **Phase 2 (sim_lib defense parity)** is unaffected by these findings.
- **Phase 3 (OmegaBacktest)** is partially unblocked. GoldFlow status discrepancy (Installment 1 action item #3) is now resolved on the source side; the checklist still needs the doc update. Action items #1 (IndexMacroCrash wiring) and #2 (Bug #3) remain blockers if the audit deliverable depends on having a complete cross-engine attribution table for index symbols. They do not block a Phase 3 baseline run that simply records "0 trades" for the dormant engines.
- Drift findings H1-H5 are not Phase 3 blockers but are pre-conditions for the Phase 3 attribution table being internally consistent. Specifically: the table must label engines by their actual runtime status (`enabled` AND `shadow_mode`), not their checklist label.
- The XauusdFvgEngine fresh A-F row (F5 / Installment 2 §XauusdFvgEngine) does not block anything but should land in the checklist before Phase 3 attribution writeups, so the freshest production engine has a documented audit baseline.

---

## Files touched this session
- **Read-only (no modification):** `include/MacroCrashEngine.hpp` (sections), `include/GoldHybridBracketEngine.hpp` (sections), `include/IndexFlowEngine.hpp` (sections), `include/IndexHybridBracketEngine.hpp` (sections), `include/CrossAssetEngines.hpp` (sections — VWAPReversion, TrendPullback, OpeningRange, NoiseBandMomentum classes), `include/XauusdFvgEngine.hpp` (sections), `include/tick_indices.hpp` (sections — all four `on_tick_*` handlers), `include/tick_gold.hpp` (sections), `include/globals.hpp` (sections), `include/engine_init.hpp` (sections), `backtest/OmegaBacktest.cpp` (header section), and the `.git/` reflog/refs metadata for branch verification.
- **Created:** This file (`docs/SESSION_2026-05-03_ENGINE_AUDIT_INSTALLMENT_2.md`).

— END INSTALLMENT 2 —
