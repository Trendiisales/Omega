# Session 2026-05-01b — Handoff (Step 3 complete)

Branch: `omega-terminal`. HEAD = `a1287ce`. All commits below are pushed to GitHub.

This session continues directly from `docs/SESSION_2026-05-01_HANDOFF.md`
(commit `67ab845`) which was the Step-2 → Step-3 boundary handoff.

## What shipped this session (1 commit on omega-terminal)

| SHA       | Subject                                                                         |
|-----------|---------------------------------------------------------------------------------|
| `a1287ce` | Step 3: live CC/ENG/POS panels + last_signal/last_pnl + HBG positions + equity route |

The commit covers Part A (UI) and Part B (engine accessors) of the
Step-3 plan in `docs/omega_terminal/STEP3_OPENER.md`. Part B was scoped
down to (B.1 full + B.2 HBG-only + B.3 full) per the user decision in
chat — "(b2-ii) HBG-only this session" was selected to avoid spreading
engine-side surgery across 10+ headers in a single commit.

### UI (omega-terminal/) — Part A

- `omega-terminal/src/panels/CCPanel.tsx` (new) — Command Center: equity
  strip, engines table, positions summary footer. 2 s polling on
  `/engines` and `/positions`, 30 s on `/equity`. Args first token
  filters the positions summary by symbol.
- `omega-terminal/src/panels/EngPanel.tsx` (new) — full sortable engines
  table with substring filter (e.g. `ENG HBG` filters by name). Row
  click highlights and logs a stub for the Step-4 LDG drill-down.
- `omega-terminal/src/panels/PosPanel.tsx` (new) — sortable positions
  table with engine OR symbol filter (e.g. `POS HBG`, `POS XAUUSD`).
  Skeleton-rows loading state, red retry banner on `OmegaApiError`.
- `omega-terminal/src/hooks/usePanelData.ts` (new) — generic
  `usePanelData<T>(fetcher, deps, opts)` hook owning AbortController
  lifecycle, polling cadence (paused while document.hidden), discriminated-
  union state (`loading | ok | err`), and a manual `refetch()`. Keeps
  previous data on refetch by default so polls don't flicker.
- `omega-terminal/src/router/functionCodes.ts` — `resolveCode` now
  parses positional args ("POS HBG" → `args: ['HBG']`) and Step-3 panels
  sort live before unlive in autocomplete.
- `omega-terminal/src/types.ts` — `Workspace.args: string[]` + `RouteResult.args`.
- `omega-terminal/src/components/CommandBar.tsx` — onDispatch now passes
  the full `RouteResult` (so args travel through). Tab/Enter on a
  highlighted suggestion preserves any args the user has typed.
- `omega-terminal/src/App.tsx` — workspace state carries args; PanelHost
  receives them. Footer shows the active args when present. Header
  bumps "step 3".
- `omega-terminal/src/panels/PanelHost.tsx` — wires CC/ENG/POS to the
  new live panels, accepts and forwards args.

### Engine side — Part B

- `include/EngineLastRegistry.hpp` (new) — per-engine "last close"
  side-table (B.1). Records `(last_ts_ms, last_pnl)` per `tr.engine`
  string, queried by pattern (literal or trailing-`_` prefix). Threading:
  `mutex`-guarded read/write; called off the tick hot path.
- `include/OpenPositionRegistry.hpp` (new) — open-position snapshotter
  registry (B.2). `register_source(label, fn)` where `fn` returns a
  vector of `PositionSnapshot` whose fields mirror the TS `Position`
  interface byte-for-byte.
- `include/globals.hpp` — defines `g_engine_last` and `g_open_positions`
  globals next to `g_engines` (Step-2 pattern). External linkage so
  OmegaApiServer.cpp's externs resolve.
- `include/trade_lifecycle.hpp` — `g_engine_last.record(tr.engine, exitTs*1000, net_pnl)`
  in three places: shadow guard return (line ~204), PARTIAL_1R fast path
  (line ~262), live close path (line ~330). Shadow trades count as
  "signals" for UI purposes even though they don't book to the ledger.
- `include/engine_init.hpp` — `reg` lambda signature changes from
  `(name, enabled, shadow_mode)` to
  `(name, enabled, shadow_mode, std::initializer_list<const char*> trade_engine_patterns)`
  and now reads `g_engine_last.get_latest_for_any(...)`. The 14 existing
  `g_engines.register_engine(...)` calls each got an explicit pattern
  list (e.g. HybridGold → `{"HybridBracketGold"}`, Tsmom → `{"Tsmom_"}`,
  HBI four-pack → `{"HybridBracketIndex"}` — see HBI caveat below).
  HBG open-position snapshotter registered. `set_equity_anchor(g_cfg.account_equity)`
  called so /equity walks anchored at the live account size.
- `src/api/OmegaApiServer.hpp` — declares `set_equity_anchor(double)`.
- `src/api/OmegaApiServer.cpp` — real `build_positions_json()` (walks
  `g_open_positions.snapshot_all()`); real `build_equity_json(q)` (walks
  `g_omegaLedger.snapshot()`, buckets at 1m/1h/1d, equity =
  `s_equity_anchor + cumulative_net_pnl(<= bucket_end)`). Includes
  `OpenPositionRegistry.hpp`. Atomic anchor `s_equity_anchor{10000.0}`
  default with thread-safe setter.

### Known engine-side limitation (documented, deferred)

The HBI four-pack (HybridSP / HybridNQ / HybridUS30 / HybridNAS100) all
stamp `tr.engine = "HybridBracketIndex"` today, so all four registry
entries currently show the SAME `last_signal_ts` / `last_pnl` value.
Documented inline in `EngineLastRegistry.hpp` and the `reg` lambda
header comment. Fix is engine-side: differentiate `tr.engine` by symbol
(e.g. `"HybridBracketIndex_SP"`). Deferred — not regressing anything
today, just sharing values across four rows.

## End state on the VPS (185.167.119.59)

- Service `Omega` is RUNNING on prior commit `fe924ea` until the user
  pulls + restarts. After deploy it will be on `a1287ce`.
- Old GUI: `http://185.167.119.59:7779` — unchanged.
- New UI: `http://185.167.119.59:7781` — Step-3 panels live after deploy.
- API: `http://185.167.119.59:7781/api/v1/omega/{engines,positions,ledger,equity}`
  — JSON, real engine data. Positions returns one row when HBG has an
  open bracket; otherwise empty array. Engines now show non-zero
  `last_signal_ts` / `last_pnl` for engines that have closed ≥1 trade
  since process start (after deploy).
- Mode: SHADOW. RSI Reversal + RSI Extreme remain disabled (from prior
  session).

## Verification done in this session

- `omega-terminal`: `npm run typecheck` ✅ + `npm run build` ✅
  (44 modules transformed, 183 kB raw / 56 kB gzipped).
- New C++ headers compile-checked standalone with `g++ -std=c++17 -Wall
  -Wextra` and round-trip behaviour tests (prefix match returns latest
  by `last_ts_ms`; HBG snapshotter round-trip preserves all 9 fields).
- MSVC Windows build is the user's outstanding deploy step.

## Carry-forward (not done this session)

The numbering picks up from the prior 2026-05-01 handoff for continuity.

1. **VPS deploy of `a1287ce`** — pull + `.\QUICK_RESTART.ps1 -Branch
   omega-terminal`, then hard-refresh `:7781`. Click CC / ENG / POS,
   then `curl http://localhost:7781/api/v1/omega/engines | jq` to
   confirm non-zero last_signal_ts/last_pnl on at least one engine.
2. **Other-engine open-position sources** — register snapshotters for
   Tsmom (per cell), Donchian, EmaPullback, TrendRider, and the HBI
   four-pack so the POS panel covers everything not just HBG. Purely
   additive in `init_engines()`. Some engines have private
   `positions_` so a small public `snapshot_open_positions()` const
   method per engine class will be needed; surface plan first per the
   engine-code rule.
3. **HBI four-pack `tr.engine` differentiation by symbol** — change
   `IndexHybridBracketEngine`'s trade emission so `tr.engine` carries
   the symbol (e.g. `"HybridBracketIndex_SP"`). Then update the four
   `register_engine` lines in `engine_init.hpp` to point at the
   per-symbol literal. Until this is done the four HBI rows in the UI
   show the same last_signal/last_pnl. Surface plan first.
4. **Step 4 — LDG / TRADE / CELL panels** — `docs/omega_terminal/STEP4_OPENER.md`
   has the next-step plan. ENG row click already logs a stub
   `[ENG] row clicked: <name> (drill-down lands in step 4)`; replace
   that with `LDG <engine>` navigation. Likely engine-side: a per-trade
   timeline accessor and a cell-grid summary route.
5. **HBG S52 verdict** — pre-seed full-window cache from existing
   baseline data, then run `./backtest/hbg_ab.sh` for the recent-window
   verdict (~30 min). Strategic flag stands: pre-S52 baseline is
   -$345 over 26 months / 32.8% WR — losing strategy regardless of S52.
6. **OMEGA_WATCHDOG.ps1 `-Branch` flag** — surgical fix; same shape as
   the v3.4 `QUICK_RESTART.ps1` patch. Surface plan first per the
   engine-code rule.
7. **`Bar State Load [FAIL]`** in startup verifier — pre-existing,
   `load_indicators()` reportedly never called.
8. **Phase 2a 5-day shadow window** — Day 5 ≈ 2026-05-06.
9. **`symbols.ini` drift on VPS** — uncommitted local changes; `git
   diff symbols.ini` on VPS, then commit or stash.
10. **Engine link status pill** — top-right reads `ENGINE LINK: PENDING`.
    Now that real /engines polling is happening at 2 s, drive the pill
    state from `engines.state.status` (loading / ok / err). Small
    addition in App.tsx — UI-only.

## Symbols (where they appear)

- omega-terminal HOME: just function-code tiles, no symbols.
- **CC panel**: equity strip + engines table (no per-symbol cells).
  Positions summary shows totals only.
- **ENG panel**: engine name / mode / state / last signal / last P&L.
- **POS panel**: full position rows including symbol; for now just
  XAUUSD when HBG is in a bracket.
- **Existing :7779 GUI**: unchanged — header ticker, market panel
  rows by category.

## Standing rules to remind the next session of

- Branch is `omega-terminal`. Do not modify `main` or `src/gui/`.
- Engine-side C++ edits (anything in `src/`, `include/`) require
  explicit user approval — surface a plan first.
- `OmegaApiServer.{cpp,hpp}` are touchable per session pattern but
  still need approval before edit.
- Backtest harnesses in `backtest/` are NOT engine code; treat as
  ordinary tooling.
- API contract: field names in `omega-terminal/src/api/types.ts` are
  byte-identical to the JSON keys produced by `OmegaApiServer.cpp`. Do
  not rename a field on one side without doing it on the other in the
  same commit.
- User preferences: always provide full files (no diffs/snippets);
  warn at ~70% context; warn before time/session usage block.

## Files written/modified by Cowork agent (commit a1287ce)

All changes pushed via `git push` to `omega-terminal`:

New files:
- `include/EngineLastRegistry.hpp`
- `include/OpenPositionRegistry.hpp`
- `omega-terminal/src/hooks/usePanelData.ts`
- `omega-terminal/src/panels/CCPanel.tsx`
- `omega-terminal/src/panels/EngPanel.tsx`
- `omega-terminal/src/panels/PosPanel.tsx`

Modified files:
- `include/globals.hpp` (2 globals + comments)
- `include/trade_lifecycle.hpp` (3 record-call sites)
- `include/engine_init.hpp` (reg lambda + 14 register_engine calls + HBG position source + equity anchor)
- `src/api/OmegaApiServer.hpp` (set_equity_anchor decl)
- `src/api/OmegaApiServer.cpp` (positions + equity routes + anchor state)
- `omega-terminal/src/types.ts` (Workspace.args, RouteResult.args)
- `omega-terminal/src/router/functionCodes.ts` (args parser, suggestCodes scoring)
- `omega-terminal/src/components/CommandBar.tsx` (full RouteResult dispatch)
- `omega-terminal/src/App.tsx` (workspace.args, footer, "step 3" label)
- `omega-terminal/src/panels/PanelHost.tsx` (CC/ENG/POS routes, args fwd)

## Quick-start for next session — paste this as the opening prompt

> Read these files first to catch up on where we are on the
> omega-terminal branch (last commit `a1287ce`):
> 1. `docs/SESSION_2026-05-01b_HANDOFF.md` (this file, also pushed
>    to repo)
> 2. `docs/omega_terminal/STEP4_OPENER.md` (Step 4 plan, paste the
>    prompt at the bottom)
> 3. `omega-terminal/src/panels/{CCPanel,EngPanel,PosPanel}.tsx` +
>    `omega-terminal/src/hooks/usePanelData.ts` — current panel
>    implementations the Step-4 panels (LDG/TRADE/CELL) should
>    pattern-match.
> 4. `include/{EngineLastRegistry,OpenPositionRegistry}.hpp` —
>    Step-3 engine-side registries; new Step-4 sources (other open
>    positions, per-trade timeline) should follow the same shape.
> 5. `src/api/OmegaApiServer.cpp` — current routes; Step 4 will add
>    `/trade/<id>` and `/cells` endpoints.
>
> First action: confirm the VPS is on `a1287ce`. If not: `git pull
> origin omega-terminal && .\QUICK_RESTART.ps1 -Branch omega-terminal`,
> hard-refresh `http://185.167.119.59:7781`, click CC / ENG / POS, and
> spot-check `/api/v1/omega/engines` for non-zero last_signal_ts.
> Then user picks one of: Step 4 panel wiring, register the missing
> open-position snapshotters (carry-forward #2), HBI tr.engine
> differentiation (#3), HBG S52 verdict (#5), or OMEGA_WATCHDOG.ps1
> -Branch flag (#6).
