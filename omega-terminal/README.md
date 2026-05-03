# Omega Terminal

Amber-on-black React/Vite shell for the Omega trading platform.

This is **Step 1** of the 7-step build plan from
`docs/SESSION_2026-05-01_HANDOFF.md`. It delivers the visual shell, command
input, workspace model, full function-code registry, and routing primitives.
Only HOME and HELP have live panels in step 1; every other code routes to a
`ComingSoonPanel` that names its target step.

## Status

| Concern                       | State                                 |
|-------------------------------|---------------------------------------|
| Vite + React + TypeScript     | Done                                  |
| Tailwind amber-on-black       | Done                                  |
| Command bar + autocomplete    | Done (Ctrl+K)                         |
| WorkspaceTabs                 | Done (Ctrl+T new, Ctrl+W close)       |
| Function-code router          | Done (24 codes registered)            |
| HOME panel                    | Live (clickable code grid)            |
| HELP panel                    | Live (full cheat-sheet)               |
| All other panels              | ComingSoon placeholders               |
| Engine IPC / live data        | Pending — Step 2                      |

## Branch rules

- Lives on branch `omega-terminal`. Do **not** modify `main` or `src/gui/`.
- The new tree lives entirely under `omega-terminal/` at repo root.
- No build artifacts from the C++ engine touch this directory.

## Getting started

```bash
cd omega-terminal
npm install
npm run dev          # http://127.0.0.1:5173
```

To produce a production bundle:

```bash
npm run build        # tsc -b && vite build  (output: dist/)
npm run preview      # serve dist/ for sanity check
npm run typecheck    # tsc -b --noEmit
```

## Project layout

```
omega-terminal/
  index.html
  package.json
  postcss.config.js
  tailwind.config.js
  tsconfig.json
  tsconfig.app.json
  tsconfig.node.json
  vite.config.ts
  src/
    main.tsx                 # React entry
    App.tsx                  # Top-level shell
    index.css                # Tailwind + base styles
    types.ts                 # FunctionCode, BuildStep, PanelGroup, ...
    components/
      CommandBar.tsx         # Top input + autocomplete dropdown
      WorkspaceTabs.tsx      # Tab strip, +, X
    panels/
      PanelHost.tsx          # Code -> panel (HOME, HELP, default)
      HomePanel.tsx          # Landing: grouped clickable code grid
      HelpPanel.tsx          # Cheat-sheet of codes + keyboard
      ComingSoonPanel.tsx    # Generic placeholder for step >1 codes
    router/
      functionCodes.ts       # PANEL_REGISTRY + resolveCode + suggestCodes
    vite-env.d.ts
```

## Function codes (full surface)

| Code   | Title                  | Step | Group  |
|--------|------------------------|:----:|--------|
| HOME   | Home                   |  1   | shell  |
| HELP   | Help                   |  1   | help   |
| CC     | Command Center         |  3   | omega  |
| ENG    | Engines                |  3   | omega  |
| POS    | Positions              |  3   | omega  |
| LDG    | Trade Ledger           |  4   | omega  |
| TRADE  | Trade Drill-Down       |  4   | omega  |
| CELL   | Cell Grid              |  4   | omega  |
| INTEL  | Intelligence Screener  |  5   | market |
| CURV   | Curves                 |  5   | market |
| WEI    | World Equity Indices   |  5   | market |
| MOV    | Movers                 |  5   | market |
| OMON   | Options Monitor        |  6   | market |
| FA     | Financial Analysis     |  6   | market |
| KEY    | Key Stats              |  6   | market |
| DVD    | Dividends              |  6   | market |
| EE     | Earnings Estimates     |  6   | market |
| NI     | News                   |  6   | market |
| GP     | Graph Price            |  6   | market |
| QR     | Quote Recap            |  6   | market |
| HP     | Historical Price       |  6   | market |
| DES    | Description            |  6   | market |
| FXC    | FX Cross               |  6   | market |
| CRYPTO | Crypto                 |  6   | market |
| WATCH  | Watch List             |  6   | market |

Aliases are resolved case-insensitively. Notable ones: `?`/`H` → HELP,
`POSITIONS`/`PORT`/`PORTFOLIO` → POS, `LEDGER`/`TRADES` → LDG,
`CHART`/`GRAPH` → GP, `FX`/`FOREX` → FXC, `BTC`/`COIN` → CRYPTO.

## Keyboard

| Key            | Action                                    |
|----------------|-------------------------------------------|
| `Ctrl+K`       | Focus command bar                         |
| `↑` / `↓`      | Move autocomplete highlight               |
| `Tab`          | Accept highlighted suggestion (no fire)   |
| `Enter`        | Dispatch (resolves via router)            |
| `Esc`          | Close autocomplete                        |
| `Ctrl+T`       | New workspace tab                         |
| `Ctrl+W`       | Close current workspace tab               |

## What Step 1 deliberately leaves out

- No engine IPC, no websockets, no fetch calls.
- No persistence — tabs reset on reload.
- No charts, tables, or real-data widgets in panels.
- No theme switcher (amber-on-black is the only theme until step 7).
- No CLI args parsing (`CC EURUSD`) — that's part of step 2's router upgrade.

## Deprecated files (slated for deletion)

These exist as 4-line tombstones and will be removed in a follow-up commit:

```
src/panels/CCPanel.tsx
src/panels/TradePanel.tsx
src/panels/PortfolioPanel.tsx
src/panels/NewsPanel.tsx
src/panels/PlaceholderPanel.tsx
```

Cleanup command (run from `omega-terminal/`):

```bash
rm src/panels/CCPanel.tsx \
   src/panels/TradePanel.tsx \
   src/panels/PortfolioPanel.tsx \
   src/panels/NewsPanel.tsx \
   src/panels/PlaceholderPanel.tsx
```

## Build plan reference

See `../docs/SESSION_2026-05-01_HANDOFF.md` § "Omega Terminal — Build Plan"
for the canonical step-by-step plan.
