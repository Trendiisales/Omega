# Omega Terminal — Step 4 Opener

**Paste the section below into a fresh chat to begin Step 4.** Everything
above the horizontal rule is context for you (the human reader);
everything below is the prompt to copy.

State at the boundary of step 3 → step 4:

- Branch: `omega-terminal`. HEAD = `a1287ce` (Step 3 commit). Step 2
  commit `0ed1dec` and Step 1 commit `6d7256a` are still in the
  ancestry. `main` is unchanged at `161d992`.
- `src/gui/` is untouched and stays untouched until the Step 7 cutover.
- Step 3 deliverables shipped:
  - `omega-terminal/src/panels/{CCPanel,EngPanel,PosPanel}.tsx`
    with shared `omega-terminal/src/hooks/usePanelData.ts`.
  - Args parsing: `POS HBG`, `CC EURUSD`, `ENG Tsmom` carry args
    through to the active panel.
  - Engine-side: `EngineLastRegistry` (per-engine last_signal_ts /
    last_pnl) + `OpenPositionRegistry` (HBG-only source for now);
    `OmegaApiServer` `/positions` and `/equity` routes are real.
- Pending follow-ups outside the Step-4 critical path: register the
  remaining open-position sources (Tsmom/Donchian/EmaPullback/
  TrendRider/HBI), differentiate the HBI four-pack's `tr.engine` by
  symbol so the four UI rows split, and the prior session's HBG S52
  verdict / OMEGA_WATCHDOG -Branch flag carry-forwards.

---

## Prompt to paste in the next chat

Read these files first to catch up:

1. `docs/SESSION_2026-05-01b_HANDOFF.md` — Step 3 → Step 4 boundary
   handoff, commit `a1287ce`.
2. `omega-terminal/src/panels/{CCPanel,EngPanel,PosPanel}.tsx` and
   `omega-terminal/src/hooks/usePanelData.ts` — current panel
   patterns; Step 4 panels should pattern-match (header / table /
   sortable columns / `usePanelData` polling / red retry banner).
3. `omega-terminal/src/router/functionCodes.ts` — args-parsing
   `resolveCode("LDG HBG")` already works; LDG/TRADE/CELL just need
   to consume `args` from PanelHost.
4. `omega-terminal/src/api/{omega,types}.ts` — `getLedger(query)`
   already accepts `{from, to, engine, limit}` and returns
   `LedgerEntry[]`. No new TS types needed for LDG. TRADE drill-down
   may need a new `getTrade(id)` wrapper + a `Trade` interface
   carrying tick-level fields if the engine grows the route.
5. `src/api/OmegaApiServer.cpp` — current routes; `/ledger` is real
   already (`build_ledger_json`). Step 4 may add `/cells` (per-cell
   summary) and possibly `/trade/<id>` for TRADE drill-down.
6. `include/EngineLastRegistry.hpp` + `include/OpenPositionRegistry.hpp`
   — registry pattern to follow if Step 4 needs a new central
   accessor (e.g. CellSummaryRegistry for the CELL panel).

Then start **Omega Terminal step 4: light up LDG / TRADE / CELL with
live data.**

### Step 4 scope

**A. UI-side, inside `omega-terminal/`:**

1. **Replace `ComingSoonPanel` for `LDG`, `TRADE`, `CELL` in
   `src/panels/PanelHost.tsx`** with three new live panels:
   - `src/panels/LdgPanel.tsx` — Trade Ledger: paginated table over
     `getLedger(query)`, with engine + symbol + date filters and
     CSV-style copy-to-clipboard. Args: `LDG HBG` filters by engine,
     `LDG XAUUSD` filters by symbol (overload first arg by exact
     match: if it's in the registered engines list, treat as engine,
     else treat as symbol).
   - `src/panels/TradePanel.tsx` — Trade Drill-Down: takes a trade id
     in args (`TRADE 12345`), fetches the trade detail
     (`getTrade(id)` if added; otherwise pull the row from the
     ledger snapshot client-side as a fallback). Shows entry/exit
     timeline, MFE/MAE, exit reason, signal context.
   - `src/panels/CellPanel.tsx` — Cell Grid: per-cell drill across
     Tsmom / Donchian / EmaPullback / TrendRider. Args:
     `CELL Tsmom_H1_long` focuses one cell; bare `CELL` shows the
     grid summary.
2. **Wire ENG row click** in `src/panels/EngPanel.tsx` to dispatch
   the active workspace to `LDG <engine_name>`. Current code logs a
   stub via `console.log` -- replace with `onNavigate` plumbed through
   from PanelHost the same way HomePanel does it.
3. **POS row click** in `src/panels/PosPanel.tsx` should navigate to
   `LDG <engine>` filtering by the position's engine. Same plumbing
   as ENG.
4. **Reuse `usePanelData`** for all three panels.

**B. Engine-side, C++ (requires explicit user OK before any edits):**

1. **`/api/v1/omega/cells` route** — per-cell summary across
   Tsmom/Donchian/EmaPullback/TrendRider. Each cell already exposes
   `cell_id`, `n_open()`, and (for some) WR/PnL. A small
   `CellSummaryRegistry` mirroring `OpenPositionRegistry` would let
   each engine register a snapshotter that returns its cells. Surface
   the design before any edit.
2. **(Optional) `/api/v1/omega/trade/<id>` route** — pulls a single
   `LedgerEntry` by id. Today `OmegaTradeLedger::snapshot()` returns
   the full vector; a thin lookup is trivial. Only worth doing if the
   client-side fallback (find in the ledger snapshot) proves slow at
   ledger size.
3. **Carry over the deferred B.2 work from Step 3** if not already
   done in a separate session: register the missing open-position
   sources so the POS panel covers all engines, and differentiate
   the HBI four-pack's `tr.engine` by symbol.

### Constraints

- Branch: `omega-terminal`. Do not touch `main` or `src/gui/`.
- Engine-side C++ edits require explicit user approval; surface a
  plan first; wait for OK; then edit.
- API contract field names in `omega-terminal/src/api/types.ts` are
  byte-identical to `OmegaApiServer.cpp` JSON keys -- both sides
  change together.
- Backtest harness gate `-DOMEGA_BACKTEST` must still compile; new
  C++ files that talk to the API server stay gated under
  `#ifndef OMEGA_BACKTEST` like `OmegaApiServer.cpp`.
- AbortController unmount cleanup is non-negotiable -- continue using
  `usePanelData`.

### Verification gates

- `omega-terminal/`: `npm run typecheck` green; `npm run build` green.
- `npm run dev`: with `Omega.exe` running, the LDG/TRADE/CELL panels
  render real data; `Ctrl+W` closes a panel and the polling stops
  cleanly (verify no requests after unmount in DevTools network tab).
- C++ side: `cmake --build build` green on Mac and Windows; existing
  smoke-test in `QUICK_RESTART.ps1` still passes; `OmegaTelemetryServer`
  endpoints still unchanged.
- Curl-level: `/api/v1/omega/ledger?engine=HybridBracketGold` returns
  the gold trades only; if `/cells` lands, returns a non-empty array.

### Don't forget

- The user's prefs require **full files only** (no diffs/snippets)
  when showing C++ or TS code.
- Warn at 70% chat usage and before any time-management block.
- Update the next-day `docs/SESSION_<YYYY-MM-DD>_HANDOFF.md` at end
  of session, mirroring the structure of
  `docs/SESSION_2026-05-01b_HANDOFF.md`.
- After Step 4 ends, the Step-5 opener goes in
  `docs/omega_terminal/STEP5_OPENER.md`. Step 5 is the OpenBB market-
  data panels (INTEL / CURV / WEI / MOV).
