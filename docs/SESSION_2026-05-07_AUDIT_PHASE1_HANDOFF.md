# SESSION HANDOFF — 2026-05-07 (Audit Phase 1) — to next session

## Headline

Engine-audit Phase 1 (inventory verification) done. Phase 4 (zombie removal) **partial only** — 4 dead globals removed, 21 deferred after discovery that "dispatch-dead" doesn't mean "plumbing-dead."

Two earlier threads in the same session also closed: (a) feasibility memo for 3 new indicators (media sentiment / volume profile / volume exhaustion) — produced, then dropped at user's request after honest pushback that engine audit is higher-leverage; (b) per-trade triage of 3 reported losers (07:19 EUR SHORT, 08:01 XAUUSD LONG, 08:25 EUR LONG) — verdict noise, no code change warranted.

## What landed (committed-ready on this branch)

| Change | Description |
|---|---|
| `include/globals.hpp:320-326` | 4 VWAPAtrTrail dead-global declarations replaced with removal-marker comment block. Truly clean — zero non-declaration refs anywhere. |
| `docs/INVENTORY_VERIFICATION_2026-05-07.md` | New: full Phase 1 inventory of ~68 engine instances declared in `globals.hpp`, classified LIVE / SHADOW / DISABLED / DEAD with file:line evidence. |

Suggested commit (run after parse-check passes):

```
git add include/globals.hpp docs/INVENTORY_VERIFICATION_2026-05-07.md docs/SESSION_2026-05-07_AUDIT_PHASE1_HANDOFF.md
git commit -m "audit: phase 1 inventory + phase 4 partial (VWAPAtrTrail removal)

Phase 1: docs/INVENTORY_VERIFICATION_2026-05-07.md classifies all ~68
engine globals declared in globals.hpp. Headline: ~21 SHADOW, ~18
DISABLED, ~25 'dispatch-dead' (no on_tick call), 2 manage-only.

Phase 4 partial: removed g_vwap_atr_trail_sp/nq/nas/us30 (only candidates
with zero non-declaration references). Other 21 dispatch-dead globals
deferred — extensive plumbing across engine_init/quote_loop/
trade_lifecycle/on_tick/order_exec/config/tick_indices needs per-family
removal sessions with parse-check between each."
git push origin main
```

## Files modified this session

```
include/globals.hpp                          -- VWAPAtrTrail block removed
docs/INVENTORY_VERIFICATION_2026-05-07.md    -- new (Phase 1 + revised §10)
docs/SESSION_2026-05-07_AUDIT_PHASE1_HANDOFF.md  -- this file
```

## Three threads worked this session

### 1. Indicators feasibility memo — DROPPED

User asked to evaluate implementing media sentiment, Volume Profile Composite (HVN/LVN/POC), and volume-spike/exhaustion indicators from a YouTube walkthrough. Produced a feasibility memo (saved to outputs folder, not the repo). User added that VIX is in the data feed, which collapsed the sentiment indicator from "hard data sourcing" to "math on existing data." Memo revised.

User then asked the right question — "is there real value in doing this or not really" — and the honest answer was: marginal. VP Composite is a visualisation tool that's largely redundant for algorithmic systems; volume exhaustion has low expected alpha; VIX is genuinely useful but as a regime-tag column on `omega_shadow.csv` rather than a new alpha source. The bigger leverage is in understanding which of the existing ~70 engines actually work.

User dropped the thread. The feasibility memo exists in agent outputs but was not promoted to the repo. **No follow-up needed unless user asks.**

### 2. Per-trade triage of 3 reported losers — VERDICT NOISE

Trades reported by user (all SHADOW, no real account impact):
- 07:19 UTC EURUSD SHORT, 19m35s, ✗SL, gross −$9.50, costs −$1.60
- 08:01 UTC XAUUSD LONG MidScalperGold, 9m16s, ✗SL, gross −$6.58, costs −$0.22
- 08:25 UTC EURUSD LONG, 1m6s, ✗SL, gross −$13.00, costs −$7.00

Verdict: trades 1+2 are normal-distribution losses for a 66.6% WR strategy (33% loss rate is by design). Trade 3 is the only ambiguous one — 1m6s + $7 cost suggests heavy fill-spread slippage, which is exactly what S59's `FILL_SPREAD_REJECT` was designed to catch. Cannot determine without VPS deploy timestamps whether trade 3 was pre- or post-S59 deploy.

**Recommendation made: do not change engine source based on 3 trades.** The 2-week shadow watch from S61 is the right next step; reactive tuning off small samples is the road to overfitting.

### 3. Engine audit Phase 1 — COMPLETED

Cross-referenced `globals.hpp` declarations with `engine_init.hpp` flag settings and tick-path dispatch wiring. Definitive classification of every engine instance. See `docs/INVENTORY_VERIFICATION_2026-05-07.md` for the full breakdown.

Headline counts:
- LIVE-intended: ~7 (4 IndexFlow, 2 TrendPullback SP/NQ, NbmGoldLondon)
- SHADOW: ~21 (compression cohort + most XAUUSD HTF/portfolio engines)
- DISABLED (`enabled=false`): ~18
- MANAGE-ONLY: 2 (g_bracket_gold, g_candle_flow)
- DEAD (no `.on_tick()` call): ~25 — but see below

### 4. Engine audit Phase 4 — STOPPED EARLY

Began removal pass on the §10 candidates ("do 10 now" instruction). After grep, discovered the "dead" classification was correct *for dispatch* but wrong as a removal-readiness signal. The 25 dispatch-dead engines have extensive supporting plumbing across 6–8 files each:

- `engine_init.hpp` — `apply_engine_config()`, `apply_bracket()`, `wire_bracket()`, `apply_be()`, parameter setters, `.ENTRY_SIZE`, `.ACCOUNT_EQUITY` sync
- `quote_loop.hpp` — `stale_beng()`, `stale_bracket()`, `fc_snap()`, `fc_bracket()`, `sbk()`, `fc_bracket_snap()`, diagnostic prints
- `trade_lifecycle.hpp` — `be_pnl()` PnL accounting, `.pos.active` coverage counts, `.vwap()` reads
- `on_tick.hpp` — `.seed()`, `.phase` macro context, `.pos.active` coverage gates, `push_live_trade()` UI feeds, `chk()` checks
- `tick_indices.hpp` — `sup_decision(g_sup_*, g_eng_*, ...)` per-symbol supervisors, `.pos.active` block-other-engine gates
- `config.hpp` — `mid_beng()`, `mid_bracket()` mid-tick refresh hooks
- `order_exec.hpp` — `.on_reject()`, `fill_bracket()` order routing
- `VERIFY_ALL_ENGINES.ps1` — diagnostic regex strings (script, not source — but also needs cleanup)

Removing them properly = 100+ edits across 8 files, and the bash sandbox is broken (S61 carry-forward) so I couldn't run `g++ -fsyntax-only` to validate. Stopped after the only truly clean cluster (4 VWAPAtrTrail) and surfaced the finding instead of pushing a blind refactor.

## Key correction to Phase 1 framing

Phase 1's "DEAD" classification was correct for the question "does this engine fire?" but misleading as a removal signal. A more useful next-session classification:

- **Dispatch-dead, plumbing-dead** — single-edit removal. Only VWAPAtrTrail (4) qualified.
- **Dispatch-dead, plumbing-live** — multi-file refactor needed. Most of the remaining "DEAD" 21.
- **Dispatch-dead, plumbing-load-bearing** — `.pos.active` queries that gate other engines' behavior. `tick_indices.hpp:69` index-coverage gate is the clearest example. Removal needs behavioral analysis, not just compile-check.

## Open follow-ups (priority order)

### 1. Engine audit Phase 2 — pull VPS shadow CSV (highest leverage)

The whole point of this audit is to flag underused / net-negative engines. Cannot proceed locally — needs VPS access to `C:\Omega\logs\shadow\omega_shadow.csv` (or wherever the per-engine ledger lives). For each engine in the inventory, compute over last 30 days: fire count, sum PnL, WR, average trade. Flag `<5 fires/month` or net-negative.

This is where the real $ impact lives. Phase 4 cleanup is hygiene; Phase 2 is decision-making.

### 2. Engine audit Phase 4 — per-family removal sessions (deferred from this session)

If the cleanup is wanted, suggested per-family sequence (each = own session, parse-check, commit, VPS deploy validation):

| Session | Removes | Files touched |
|---|---|---|
| A | FX Tier-0 (5: `g_eng_eurusd/gbpusd/audusd/nzdusd/usdjpy`) | `globals.hpp`, `engine_init.hpp` |
| B | FX brackets (5: `g_bracket_eurusd/...`) | `globals.hpp`, `engine_init.hpp`, `order_exec.hpp`, `quote_loop.hpp`, `on_tick.hpp`, `config.hpp`, `trade_lifecycle.hpp` |
| C | US index Tier-0 (4: `g_eng_sp/nq/us30/nas100`) | Most-plumbed cluster. `globals.hpp`, `engine_init.hpp`, `trade_lifecycle.hpp`, `quote_loop.hpp`, `on_tick.hpp`, `tick_indices.hpp`, `config.hpp` |
| D | Index/EU brackets + commodity Tier-0 + ca_fx_cascade/carry_unwind | Cleanup tail |

Each session must end with `g++ -std=c++17 -I include -DOMEGA_BACKTEST -fsyntax-only scripts/s61_parsecheck.cpp` clean before commit.

### 3. 2-week shadow watch (passive, from S61) — UNCHANGED

Watch for: `RANGE_HIST_LOAD ok` count vs `COLD_START_BLOCK` count, `FILL_SPREAD_REJECT` count vs total fires (target <5%), per-engine WR holding above 60% OOS for the compression cohort.

### 4. Tsmom_H1_long backtest re-validation (offline, from S61) — UNCHANGED

Layer `MAE_EXIT_ATR=2.0` into `phase1/signal_discovery/post_cut_revalidate_all.py::sim_c` and re-run.

### 5. Open audit questions still unanswered

- `kShadowDefault` runtime value on VPS — needs `g_cfg.mode` confirmation. Determines whether tsmom/donchian/ema_pullback/gold_stack/pdhl_rev are LIVE or SHADOW.
- EU index Tier-0 dispatch (`g_eng_ger30/uk100/estx50`) — strongly suspect dead by analogy to US Tier-0, but need final read of EU dispatch block in `tick_indices.hpp`.
- Oil/Brent Tier-0 (`g_eng_cl`, `g_eng_brent`) — comment in `globals.hpp:597` says "Oil uses g_eng_cl" but no `.on_tick()` call found. Final read of `tick_oil.hpp` needed.

## Closed this session (no follow-up needed)

- **VWAPAtrTrail removal** — 4 truly dead globals retired.
- **Indicators feasibility thread** — answered (marginal value), then dropped per user.
- **Trade triage thread** — answered (3 trades is noise on a 66% WR strategy), no code change.

## Sandbox issue (CARRY-FORWARD — S61 → still broken)

`mcp__workspace__bash` continues throwing `useradd: /etc/passwd: No space left on device`. This blocked parse-checks during the Phase 4 attempt. **Cowork restart strongly recommended before Phase 4 continuation**, otherwise Mac-side parse-check is the only validation path:

```
g++ -std=c++17 -I include -DOMEGA_BACKTEST -fsyntax-only scripts/s61_parsecheck.cpp
```

## Repo info

- Mac path: `~/omega_repo`
- VPS path: `C:\Omega`
- GitHub: https://github.com/Trendiisales/Omega
- Default branch: `main` (S60 OMEGA.ps1 branch guard hard-fails any non-main deploy)

## Critical reminders for next session (from S61, still apply)

1. **PowerShell uses `;` not `&&`** for statement separation:
   ```powershell
   git checkout main; git pull
   .\OMEGA.ps1 deploy
   ```
2. **Don't `git pull` from a non-main branch.** S60 catches the deploy symptom but not the cause. Always `git checkout main` first.

## Persistent shadow WARNs (by design, unchanged from S61)

- VIX Level cold-start (clears on first VIX.F tick)
- RSI Reversal SHADOW (clears when going LIVE; currently `enabled=false` so this never resolves until re-enable)

## Context budget note

This session ran heavy on file reads (multiple 1000+ line engine headers + globals.hpp + engine_init.hpp grep results). Next session should start fresh for the Phase 2 VPS pull — that work alone will fill a session.
