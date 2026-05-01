# Omega Session Handoff — 2026-05-01 (Omega Terminal Step 1)

> **Reconstruction note (2026-05-01).** This file was missing from the repo
> at the start of the 2026-05-01 follow-on session even though
> `omega-terminal/README.md` and `docs/omega_terminal/STEP2_OPENER.md` both
> reference it. The carry-forward summary at session start named it as a
> required read. This document is reconstructed from those two files plus
> `docs/SESSION_2026-04-30c_PHASE1_HANDOFF.md` so the document trail is
> consistent before Step 2 begins.

**Final State:** Omega Terminal Step 1 complete and committed on branch
`omega-terminal` at `6d7256a` — React/Vite shell with command bar,
workspace tabs, function-code router, 24 registered codes, two live
panels (HOME, HELP), and `ComingSoonPanel` placeholders for everything
else. Production VPS is unrelated to this branch and continues to run
the prior C++ engine commit; nothing in Step 1 reaches the engine.

**Parent commit:** `68e2a07` (`S52: HybridGold trail fix per
STAGE1A_FINAL audit (MIN_TRAIL_ARM_PTS 1.5->3.0, MFE_TRAIL_FRAC
0.20->0.40)`). The Step 1 commit `6d7256a` sits directly on top of it.

---

## What shipped this session

### Omega Terminal — Step 1 (UI shell, no engine integration)

Pure additive change in a brand-new tree at repo root: `omega-terminal/`.
**No engine behaviour change. No `src/gui/` change. No `main` change.**

**New tree: `omega-terminal/`**

Vite + React 18 + TypeScript 5.6 + Tailwind 3.4. Amber-on-black is the
only theme until Step 7's cutover. Layout:

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

**Function-code surface (24 codes, locked):**

| Code   | Title                  | Step | Group  | Live in step 1? |
|--------|------------------------|:----:|--------|:---:|
| HOME   | Home                   |  1   | shell  | ✅ |
| HELP   | Help                   |  1   | help   | ✅ |
| CC     | Command Center         |  3   | omega  | ComingSoon |
| ENG    | Engines                |  3   | omega  | ComingSoon |
| POS    | Positions              |  3   | omega  | ComingSoon |
| LDG    | Trade Ledger           |  4   | omega  | ComingSoon |
| TRADE  | Trade Drill-Down       |  4   | omega  | ComingSoon |
| CELL   | Cell Grid              |  4   | omega  | ComingSoon |
| INTEL  | Intelligence Screener  |  5   | market | ComingSoon |
| CURV   | Curves                 |  5   | market | ComingSoon |
| WEI    | World Equity Indices   |  5   | market | ComingSoon |
| MOV    | Movers                 |  5   | market | ComingSoon |
| OMON   | Options Monitor        |  6   | market | ComingSoon |
| FA     | Financial Analysis     |  6   | market | ComingSoon |
| KEY    | Key Stats              |  6   | market | ComingSoon |
| DVD    | Dividends              |  6   | market | ComingSoon |
| EE     | Earnings Estimates     |  6   | market | ComingSoon |
| NI     | News                   |  6   | market | ComingSoon |
| GP     | Graph Price            |  6   | market | ComingSoon |
| QR     | Quote Recap            |  6   | market | ComingSoon |
| HP     | Historical Price       |  6   | market | ComingSoon |
| DES    | Description            |  6   | market | ComingSoon |
| FXC    | FX Cross               |  6   | market | ComingSoon |
| CRYPTO | Crypto                 |  6   | market | ComingSoon |
| WATCH  | Watch List             |  6   | market | ComingSoon |

Aliases are resolved case-insensitively (`?`/`H` → HELP,
`POSITIONS`/`PORT`/`PORTFOLIO` → POS, `LEDGER`/`TRADES` → LDG,
`CHART`/`GRAPH` → GP, `FX`/`FOREX` → FXC, `BTC`/`COIN` → CRYPTO, …).

**Mac shell hardening:** `scripts/setup-mac-shell.sh` (idempotent)
adds `setopt interactive_comments` plus `git_unlock` / `gunlock` /
`git-safe` to `~/.zshrc`. Run once with
`bash scripts/setup-mac-shell.sh && source ~/.zshrc`.

---

## Why this is structured this way

The Step 1 cut intentionally ships zero engine wiring:

- The function-code surface is locked up front (all 24 codes registered)
  so autocomplete and Help are accurate from day 1, even though only HOME
  and HELP have live panels. This avoids the trap where the registry
  grows alongside the panels and stays partial for weeks.
- `ComingSoonPanel` is intentionally generic — every not-yet-shipped code
  routes through it and prints which step lights it up. That keeps the
  router pure and lets Steps 3-6 each delete one `case` from the registry
  branch table rather than rewrite logic.
- The new tree lives entirely under `omega-terminal/`. The existing C++
  GUI under `src/gui/` is left alone and stays live until the Step 7
  cutover. This is the explicit "do not touch `main` or `src/gui/`" rule.

---

## Diff totals (Step 1)

```
 omega-terminal/                         | new tree
   index.html                            | new file
   package.json                          | new file
   package-lock.json                     | new file (npm install output)
   postcss.config.js                     | new file
   tailwind.config.js                    | new file
   tsconfig.json                         | new file
   tsconfig.app.json                     | new file
   tsconfig.node.json                    | new file
   vite.config.ts                        | new file
   src/main.tsx                          | new file
   src/App.tsx                           | new file
   src/index.css                         | new file
   src/types.ts                          | new file
   src/components/CommandBar.tsx         | new file
   src/components/WorkspaceTabs.tsx      | new file
   src/panels/PanelHost.tsx              | new file
   src/panels/HomePanel.tsx              | new file
   src/panels/HelpPanel.tsx              | new file
   src/panels/ComingSoonPanel.tsx        | new file
   src/router/functionCodes.ts           | new file
   src/vite-env.d.ts                     | new file
 scripts/setup-mac-shell.sh              | new file
 docs/omega_terminal/                    | new dir
   README.md                             | new file
   STEP2_OPENER.md                       | new file
 omega-terminal/README.md                | new file
```

No file outside the new tree, `scripts/`, and `docs/omega_terminal/` was
modified. `src/gui/` is unchanged. `main` is unchanged.

---

## Verification at this point

- `tsc -b` (typecheck): **PASS**
- `vite build`: **PASS** — first run after `rm -rf dist` produces ~156 KB
  JS / ~12 KB CSS bundle.
- `vite dev` on `127.0.0.1:5173`: shell renders, `Ctrl+K` focuses command
  bar, autocomplete shows the full 24-code surface, HOME and HELP route
  to live panels, every other code routes to `ComingSoonPanel`.
- No engine IPC tests — Step 1 has none to run. That is Step 2's gate.

---

## Verification still required (user action)

1. **Push branch to origin:** `git push origin omega-terminal` (the
   branch is local-only at session end).
2. **Run the Mac shell hardener once:**
   `bash scripts/setup-mac-shell.sh && source ~/.zshrc`.
3. **VPS is unaffected** — no deploy needed for Step 1.

---

## What Step 1 explicitly does NOT do

- No engine IPC, no websockets, no fetch calls.
- No persistence — tabs reset on reload.
- No charts, tables, or real-data widgets in panels.
- No theme switcher (amber-on-black is the only theme until Step 7).
- No CLI args parsing (`CC EURUSD`) — that lives in Step 2's router
  upgrade.
- No change to `src/gui/`, `include/`, `src/` (engine), `CMakeLists.txt`,
  or anything the C++ build sees.

If runtime engine behaviour shifts in the next 24h, **it is not Step 1**
— Step 1 cannot reach the runtime.

---

## Deferred / not done this session

Carrying forward from prior handoffs:

| Item | Status |
|---|---|
| VPS deploy `161d992` (RSI disables) | Still pending — user action on VPS. |
| HBG S52 A/B verdict | Still in flight — Mac backtest. |
| Phase 2a 5-day ledger window (CellEngine V1 vs V2) | Still in flight — Day 5 ≈ 2026-05-06. |
| Omega Terminal Step 1 | **Done this session** (commit `6d7256a`). |

---

## Open / known issues flagged

### Missing handoff doc (this very file)
At the start of the 2026-05-01 follow-on session, this file did not
exist in the repo even though README + STEP2_OPENER both referenced it.
Reconstructed today before Step 2 began. Going forward, every step
deliverable must be paired with a handoff doc landed in the same
commit. The Step 2 commit will land
`docs/SESSION_2026-05-02_HANDOFF.md` (or the appropriate next-day
filename) per `STEP2_OPENER.md` § "Don't forget".

### `OmegaTelemetryServer` already on :7779
Step 2 originally specified `127.0.0.1:7779` for the new
`OmegaApiServer`, but `src/gui/OmegaTelemetryServer` is already bound to
HTTP :7779 + WebSocket :7780. Two listeners cannot bind the same port.
**Resolution: `OmegaApiServer` will bind `127.0.0.1:7781` instead** —
both servers run in parallel until the Step 7 cutover, neither
overlaps. This is documented here so STEP2_OPENER's `:7779` reference
is superseded. The next-session handoff will note this in its diff
totals.

---

## Watch in next session

- Branch `omega-terminal` advances cleanly on top of `6d7256a` with the
  Step 2 deliverable. No fast-forward of `main` until Step 7.
- `src/gui/` remains untouched. Anything modified there during Step 2 is
  a constraint violation.
- The Step 2 verification gate is `curl
  http://127.0.0.1:5173/api/v1/omega/engines` returning the live engine
  list, proxied through Vite at port :5173 to the new
  `OmegaApiServer` at :7781.

---

## 7-step Omega Terminal build plan (canonical)

This is the source of truth that README and STEP2_OPENER reference.

### Step 1 — Shell (DONE this session, commit `6d7256a`)
React/Vite + Tailwind amber-on-black; command bar with autocomplete;
workspace tabs (`Ctrl+T`/`Ctrl+W`); function-code router with all 24
codes registered; HOME and HELP live; everything else routes to
`ComingSoonPanel`.

### Step 2 — Omega C++ JSON endpoints + UI fetch layer + Vite proxy
Engine-side: new `src/api/OmegaApiServer.{hpp,cpp}` HTTP/1.1 server on
`127.0.0.1:7781` (revised from the original `:7779` due to port
collision); add `EngineSnapshot` + `EngineRegistry g_engines` in
`include/globals.hpp`; per-engine `g_engines.register_engine(...)` at
each init site; spawn server thread in `src/main.cpp`; update
`CMakeLists.txt`. UI-side: Vite proxy `/api/v1/omega/*` →
`http://127.0.0.1:7781`; new `src/api/types.ts` + `src/api/omega.ts`
(typed fetch wrappers with `AbortController`, 5 s timeout, typed
`OmegaApiError`). No panel changes. Exit: `curl
http://127.0.0.1:5173/api/v1/omega/engines` returns live engines.
**Engine-side edits require explicit user approval before any C++ file
is modified.**

### Step 3 — Omega panels (CC, ENG, POS) live
Wire Command Center, Engines, Positions to the Step 2 endpoints. Adds
the CLI-args parser (`CC EURUSD`, `POS HBG`) into the router.

### Step 4 — Trade ledger panels (LDG, TRADE, CELL)
Filterable trade ledger across engines and symbols; per-trade
drill-down with timeline / MFE / MAE / exit reason; per-cell grid
across Tsmom, Donchian, EmaPullback, TrendRider.

### Step 5 — Market panels group A (INTEL, CURV, WEI, MOV)
Composite intelligence screener; yield curves & term structure; world
equity indices dashboard; movers / unusual-volume scanner.

### Step 6 — Market panels group B (OMON, FA, KEY, DVD, EE, NI, GP, QR, HP, DES, FXC, CRYPTO, WATCH)
Options chain & IV surface; financial analysis; key stats; dividends;
earnings estimates; news; price chart; quote recap; historical price;
description; FX cross; crypto; nightly INTEL-rule watch list.

### Step 7 — Cutover & theme
Replace `src/gui/` clients with `omega-terminal/` build artifact; add
the theme switcher (amber-on-black stays default); retire the legacy
GUI server.

---

## How this session built on the prior 2026-04-30c ship

Prior session shipped CellEngine refactor Phase 1 (extract shared
primitives + structural-sanity asserts), pure additive, header-only.
This session is independent of that — Phase 1 ships in
`include/CellPrimitives.hpp` and the four engine headers; Step 1 ships
in the brand-new `omega-terminal/` tree and `docs/omega_terminal/`. Either
deliverable can roll back without affecting the other. Phase 2a's
five-day ledger window (V1 vs V2 byte-identical comparison) is
unrelated to anything in this UI work and continues running on the
engine side independently.
