# Omega Terminal — Step 5 Opener

**Paste the section below into a fresh chat to begin Step 5.** Everything
above the horizontal rule is context for you (the human reader);
everything below is the prompt to copy.

State at the boundary of step 4 → step 5:

- Branch: `omega-terminal`. HEAD = `<step-4-commit>` (Step 4 commit).
  Step 3 commit `a1287ce`, Step 2 commit `0ed1dec`, and Step 1 commit
  `6d7256a` are still in the ancestry. `main` is unchanged at `161d992`.
- `src/gui/` is untouched and stays untouched until the Step 7 cutover.
- Step 4 deliverables shipped:
  - `omega-terminal/src/panels/{LdgPanel,TradePanel,CellPanel}.tsx`,
    pattern-matched off the Step-3 panels (`usePanelData` polling,
    sortable headers, red retry banner, args parsing).
  - ENG / POS row click navigates to `LDG <engine>`; LDG row click
    navigates to `TRADE <id>`. `TradePanel` engine cell back-links to
    `LDG <engine>`.
  - `App.tsx` and `PanelHost.tsx` widened `onNavigate` /
    `navigate` from `(code: FunctionCode) => void` to
    `(target: string) => void` so panel-internal navigation can carry
    args through the same router contract the command bar uses.
  - `CellPanel` is forward-compatible chrome only — no engine-side
    `/cells` route ships in Step 4 (deferred per the standing
    "engine-side requires explicit approval" rule).
- Pending follow-ups outside the Step-5 critical path: register the
  remaining open-position sources (Tsmom / Donchian / EmaPullback /
  TrendRider / HBI), differentiate the HBI four-pack's `tr.engine` by
  symbol, the engine-side `/cells` route + `CellSummaryRegistry`,
  optional `/trade/<id>` route, MFE / MAE / signal context on
  `LedgerEntry`, OMEGA_WATCHDOG `-Branch` flag, the v3.5 npm/vite
  stderr-mangling fix in `QUICK_RESTART.ps1`. None block Step 5.

---

## Prompt to paste in the next chat

Read these files first to catch up:

1. `docs/SESSION_2026-05-01c_HANDOFF.md` — Step 4 → Step 5 boundary
   handoff, commit `<step-4-commit>`.
2. `omega-terminal/src/panels/{LdgPanel,TradePanel,CellPanel}.tsx` and
   `omega-terminal/src/hooks/usePanelData.ts` — current panel
   patterns; Step-5 panels should pattern-match (header / table /
   sortable columns / `usePanelData` polling / red retry banner /
   args parser overload).
3. `omega-terminal/src/router/functionCodes.ts` — `INTEL`, `CURV`,
   `WEI`, `MOV` are already in `PANEL_REGISTRY` at `step: 5`. No
   router additions needed; the panels just need to be wired in
   `PanelHost.tsx` the way Step-4 wired LDG/TRADE/CELL.
4. `omega-terminal/src/api/{omega,types}.ts` — current typed fetch
   wrappers and JSON shapes (engine-side endpoints only). Step 5
   introduces the first non-engine data source — see "Architecture
   choice" below.
5. `src/api/OmegaApiServer.cpp` — current routes. If Step 5 chooses
   option (a) below, this file grows the OpenBB proxy endpoints.

Then start **Omega Terminal step 5: light up INTEL / CURV / WEI / MOV
with OpenBB-derived market data.**

### Architecture choice (decide first)

Step 5 is the first time the UI needs data the engine binary doesn't
already produce. Two paths, with trade-offs to surface and let the
user pick:

**(a) Server-side OpenBB proxy** — extend `OmegaApiServer.cpp` with
`/api/v1/openbb/<route>` endpoints that call the OpenBB Python SDK
(or a small Python sidecar) and pass JSON through. Pros: single
origin, no CORS, browser doesn't need network access; the engine
binary owns all data sources. Cons: engine binary becomes
multi-language; brings Python into the build / deploy story; the
OpenBB SDK install on the Windows VPS adds setup friction.

**(b) Browser-direct OpenBB Hub calls** — UI calls
`https://api.openbb.co/...` directly from the React panels using a
`Bearer` token loaded from a Vite env var. Pros: keeps the engine
binary single-language; deploy story unchanged. Cons: tokens live in
the bundle (rotate frequently); CORS on every endpoint must work;
browser network egress is required.

A hybrid is also possible: cache static reference data (sector maps,
currency lists) in the engine binary and proxy those, but call live
prices direct from the browser.

### Step 5 scope (after architecture is picked)

**A. UI-side, inside `omega-terminal/`:**

1. **Replace `ComingSoonPanel` for `INTEL`, `CURV`, `WEI`, `MOV` in
   `src/panels/PanelHost.tsx`** with four new live panels. Pattern-
   match off `LdgPanel.tsx` (sortable table) for `WEI` and `MOV`,
   off `CCPanel.tsx` (multiple summary cards) for `INTEL`, and off
   nothing yet (curve chart) for `CURV` — Step 5 is the first panel
   that needs real charting, so adopt one of the Recharts /
   Chart.js / d3 options Vite already pulls.
2. **Args overload** for each panel matches the Step-3/4 surface:
   `INTEL <screen-id>`, `CURV US|EU|JP`, `WEI <region>`, `MOV
   <universe>`.
3. **New TS module for OpenBB types** — `omega-terminal/src/api/openbb.ts`
   if option (b) is picked, or extend `omega-terminal/src/api/types.ts`
   with new interfaces if option (a). The naming convention is the
   same as for the engine API: byte-identical JSON keys, one type per
   endpoint.
4. **Reuse `usePanelData`** — its polling cadence + AbortController
   contract is exactly what live market data needs. Pick cadences
   per panel (1 s for MOV, 30 s for INTEL, 5 s for WEI, 1 m for CURV).

**B. Engine-side, C++ (option (a) only — requires explicit user OK):**

1. **`/api/v1/openbb/<route>` proxy block in `OmegaApiServer.cpp`**.
   Either spawn a Python sidecar that owns the OpenBB SDK and talk
   to it over a local socket, or ship a thin libcurl client that
   calls OpenBB Hub directly and returns the JSON unchanged. Surface
   design first.
2. **Build / deploy story for the Python sidecar** — if the sidecar
   route is chosen, add a `requirements.txt` next to it and update
   `QUICK_RESTART.ps1` to install Python deps the same way it
   installs npm deps for the UI build.

### Constraints

- Branch: `omega-terminal`. Do not touch `main` or `src/gui/`.
- Engine-side C++ edits require explicit user approval; surface a
  plan first; wait for OK; then edit.
- API contract field names mirror the JSON keys byte-identical to
  whichever side ships the wire layer — same rule as the engine API.
- Backtest harness gate `-DOMEGA_BACKTEST` must still compile; new
  C++ files that talk to the API server stay gated under
  `#ifndef OMEGA_BACKTEST` like `OmegaApiServer.cpp`.
- AbortController unmount cleanup is non-negotiable — continue using
  `usePanelData`.
- Bundle size: the existing 201 kB / 58 kB-gzipped baseline at Step 4
  is the budget marker. New chart libraries are the biggest risk;
  prefer Recharts (already in scope) over d3 unless a feature
  genuinely requires d3.

### Verification gates

- `omega-terminal/`: `npm run typecheck` green; `npm run build` green.
- `npm run dev`: with `Omega.exe` running, the INTEL / CURV / WEI / MOV
  panels render real OpenBB data; `Ctrl+W` closes a panel and the
  polling stops cleanly (verify no requests after unmount in DevTools
  network tab).
- C++ side (option (a) only): `cmake --build build` green on Mac and
  Windows; `QUICK_RESTART.ps1` smoke test still passes;
  `OmegaTelemetryServer` endpoints still unchanged.
- Curl-level (option (a) only): `/api/v1/openbb/<route>` returns the
  expected JSON shape end-to-end.
- Bundle: total size growth ≤ +60 kB raw / +15 kB gzipped vs Step 4.

### Don't forget

- The user's prefs require **full files only** (no diffs/snippets)
  when showing C++ or TS code.
- Warn at 70% chat usage and before any time-management block.
- Update the next-day `docs/SESSION_<YYYY-MM-DD>_HANDOFF.md` at end
  of session, mirroring the structure of
  `docs/SESSION_2026-05-01c_HANDOFF.md`.
- After Step 5 ends, the Step-6 opener goes in
  `docs/omega_terminal/STEP6_OPENER.md`. Step 6 is the BB function
  suite (OMON / FA / KEY / DVD / EE / NI / GP / QR / HP / DES / FXC /
  CRYPTO / WATCH).
