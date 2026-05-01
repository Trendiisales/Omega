# Omega Terminal — Step 3 Opener

**Paste the section below into a fresh chat to begin Step 3.** Everything
above the horizontal rule is context for you (the human reader); everything
below is the prompt to copy.

State at the boundary of step 2 → step 3:

- Branch: `omega-terminal`. HEAD = `0ed1dec` (Step 2 commit). Step 1 commit
  `6d7256a` is the parent. Local `main` last sat at `161d992` and is
  unchanged.
- `src/gui/` is untouched and stays untouched until the Step 7 cutover.
- Step 2 deliverable: `OmegaApiServer` HTTP/1.1 read-API on
  `127.0.0.1:7781`; 14 engines self-register via `g_engines` in
  `init_engines()`; UI-side typed fetch wrappers in
  `omega-terminal/src/api/{types,omega}.ts`; Vite dev proxy
  `/api/v1/omega/*` → `:7781`. Mac sandbox gates green; MSVC VPS build
  is the user's outstanding action.
- Pending engine-side work outside the GUI critical path: VPS pull/restart
  for `161d992` (RSI disables), VPS rebuild for `0ed1dec` (Step 2), HBG
  S52 A/B verdict, Phase 2a 5-day shadow window (Day 5 ≈ 2026-05-06).
  Step 3 may begin in parallel with the VPS rebuild — none of those
  block UI panel wiring.

---

## Prompt to paste in the next chat

Read these files first to catch up:

1. `docs/SESSION_2026-05-02_HANDOFF.md` — canonical Step 2 handoff,
   commit `0ed1dec`, route surface, port choice, carry-forward items.
2. `omega-terminal/README.md` — Step 1 deliverable spec, file layout,
   full 24-code function-code surface.
3. `omega-terminal/src/router/functionCodes.ts` — `PANEL_REGISTRY` is
   the single source of truth for code → panel routing.
4. `omega-terminal/src/api/types.ts` — TS interfaces (`Engine`,
   `Position`, `LedgerEntry`, `EquityPoint`, `OmegaApiError`) Step 3's
   panels will consume.
5. `omega-terminal/src/api/omega.ts` — typed fetch wrappers
   (`getEngines`, `getPositions`, `getLedger`, `getEquity`); 5 s
   default timeout; `OmegaApiError` on every failure mode.
6. `src/api/OmegaApiServer.cpp` — current endpoint behaviour. Note
   `/positions` and `/equity` are stubs returning `[]`.
7. `include/EngineRegistry.hpp` — `EngineSnapshot` shape; what fields
   are populated today (`enabled`/`mode`/`state`) vs what is `0`/empty
   (`last_signal_ts`/`last_pnl`).
8. `docs/omega_terminal/STEP3_OPENER.md` — this file.

Then start **Omega Terminal step 3: light up the omega-group panels
(CC, ENG, POS) with live data from the Step 2 endpoints.**

### Step 3 scope

**A. UI-side, inside `omega-terminal/`:**

1. **Replace the `ComingSoonPanel` routing for `CC`, `ENG`, `POS` in
   `src/panels/PanelHost.tsx`** with three new live panels:
   - `src/panels/CCPanel.tsx` — Command Center: equity strip across
     the top; engines table (uses `getEngines`); positions summary
     (uses `getPositions`).
   - `src/panels/EngPanel.tsx` — Engines drill-down: tabular view of
     `getEngines` with per-row click to filter ledger by engine.
   - `src/panels/PosPanel.tsx` — Positions: tabular view of
     `getPositions` with sort + filter.
2. **Add CLI argument parsing to the router** (`src/router/functionCodes.ts`).
   `resolveCode("CC EURUSD")` should return the CC descriptor with
   `args: ["EURUSD"]` set on the result. Update `PanelHost.tsx` to
   pass args into panel components. The args parser is shared across
   all panels (Step 3 only consumes it for `POS HBG` engine filter
   and `CC <symbol>` initial focus, but the parsing itself is
   uniform).
3. **Per-panel state model:** introduce a small `usePanelData<T>(fetch,
   deps)` hook that handles the AbortController lifecycle, loading
   state, error state, and a manual refetch. Used by all three
   panels. Polling cadence (2 s for /engines + /positions; on-demand
   for /ledger) lives here too.
4. **Loading + error UI**: amber-on-black skeleton rows for loading;
   red-tinted banner with `OmegaApiError.message` for failures, with a
   one-click retry.

**B. Engine-side, C++ (requires explicit user OK before any edits — the
user's standing rule "never modify core code unless instructed clearly"
still applies):**

1. **Per-engine `last_signal_ts` and `last_pnl` accessors.** Today the
   snapshot lambdas in `engine_init.hpp` return `0` for both. Step 3
   needs real values so the UI table doesn't show all-zero "Last
   signal" columns. Two options to surface in the plan and let the
   user pick:
   - (a) Each engine exposes a `last_signal_ts()` and `last_pnl()`
     getter on its existing class. Snapshot lambda calls them. ~2
     small members per engine (atomic int64 + atomic double),
     touched on entry/close paths. Cheapest at runtime.
   - (b) A side table `g_engine_last` keyed by engine name, written
     by the central trade-close path (`handle_closed_trade`) and the
     central signal-emit path. No per-engine member changes.
2. **Real `/api/v1/omega/positions` route** in `OmegaApiServer.cpp`.
   Today it returns `[]`. Needs to walk a unified open-positions view
   across engines. Not all engines expose open positions uniformly
   today (some hold per-engine state, some go through bracket
   structures); surface the enumeration of "what counts as an open
   position" and let the user OK the canonical list before I write
   the JSON serializer.
3. **Real `/api/v1/omega/equity` route** in `OmegaApiServer.cpp`.
   Today it returns `[]`. Server-side aggregation across `1m`/`1h`/
   `1d` from `g_omegaLedger` snapshots. The aggregation logic is
   simple but needs a stable equity-anchor (start equity + cumulative
   net_pnl walk). Surface the choice (per-engine equity vs portfolio
   equity) in the plan.

### Constraints

- Branch: `omega-terminal`. Do not touch `main`. Do not touch
  `src/gui/`.
- Engine-side C++ edits require explicit user approval before any file
  is modified. Surface a plan first; wait for OK; then edit.
- Do not alter the `omega-terminal/src/api/types.ts` field names
  without simultaneously updating the C++ JSON serializer in
  `OmegaApiServer.cpp`. The contract is byte-identical and that
  parity is the only thing keeping the two sides in sync without a
  schema generator.
- Backtest harness gate `-DOMEGA_BACKTEST` must still compile clean —
  any new C++ files that talk to the API server should remain gated
  with `#ifndef OMEGA_BACKTEST` like the existing `OmegaApiServer`
  bodies.
- Polling cadence on the UI side must respect AbortController — no
  zombie fetches that outlive the panel mount.

### Verification gates

- `omega-terminal/`: `npm run typecheck` green; `npm run build` green.
- `npm run dev`: with both `Omega.exe` and `vite dev` running, the
  CC/ENG/POS panels render the live engine list. `Ctrl+W` closes a
  panel and the polling stops cleanly (verify with `chrome://inspect`
  network tab — no requests after unmount).
- C++ side: `cmake --build build` green on Mac and Windows; existing
  smoke-test in `QUICK_RESTART.ps1` still passes;
  `OmegaTelemetryServer` endpoints still unchanged.
- Curl-level: `/api/v1/omega/engines` shows non-zero
  `last_signal_ts`/`last_pnl` for engines that have fired at least
  once; `/positions` shows non-empty array when at least one
  position is open; `/equity?interval=1h` returns a non-empty
  series.

### Don't forget

- The user's prefs require **full files only** (no diffs/snippets)
  when showing C++ or TS code.
- Warn at 70% chat usage and before any time-management block.
- Update `docs/SESSION_2026-05-03_HANDOFF.md` (or the next-day
  filename) at end of session, mirroring the structure of the
  2026-05-02 handoff.
- After step 3 ends, the step-4 opener goes in
  `docs/omega_terminal/STEP4_OPENER.md`.
