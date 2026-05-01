# Session 2026-05-01d — Handoff (Step 5 complete)

Branch: `omega-terminal`. HEAD = `<step-5-commit>` (filled in after the
commit lands; parent is `3baa20f` Step-4). This session continues
directly from `docs/SESSION_2026-05-01c_HANDOFF.md` and implements
`docs/omega_terminal/STEP5_OPENER.md`.

## TL;DR — how to actually run this

There is **one** verification path. Push the branch, deploy on the VPS
via the existing `QUICK_RESTART.ps1`, open Chrome at
`http://VPS_IP:7781/`. The rebuilt `Omega.exe` serves both the UI
(from `omega-terminal/dist/`) and the API (from `/api/v1/omega/*`)
on the same port — same model as Step 2 commit `bdb844d`.

```bash
# Mac:
cd /Users/jo/omega_repo
git add \
  src/api/OpenBbProxy.hpp \
  src/api/OpenBbProxy.cpp \
  src/api/OmegaApiServer.cpp \
  CMakeLists.txt \
  omega-terminal/package.json \
  omega-terminal/package-lock.json \
  omega-terminal/src/api/types.ts \
  omega-terminal/src/api/omega.ts \
  omega-terminal/src/panels/IntelPanel.tsx \
  omega-terminal/src/panels/CurvPanel.tsx \
  omega-terminal/src/panels/WeiPanel.tsx \
  omega-terminal/src/panels/MovPanel.tsx \
  omega-terminal/src/panels/PanelHost.tsx \
  docs/SESSION_2026-05-01d_HANDOFF.md \
  docs/omega_terminal/STEP6_OPENER.md
git commit -m "Omega Terminal step 5: INTEL / CURV / WEI / MOV panels + libcurl OpenBbProxy"
git push origin omega-terminal
```

```powershell
# VPS, FIRST TIME ONLY (one-time vcpkg install of libcurl):
C:\vcpkg\vcpkg\vcpkg.exe install curl:x64-windows

# VPS, set the OpenBB env var at the machine level so the Omega service
# inherits it (pick mock OR live — either is fine):
[System.Environment]::SetEnvironmentVariable("OMEGA_OPENBB_MOCK","1","Machine")
# OR for real OpenBB data:
[System.Environment]::SetEnvironmentVariable("OMEGA_OPENBB_TOKEN","<your-hub-token>","Machine")

# VPS, every deploy from here on:
.\QUICK_RESTART.ps1 -Branch omega-terminal
```

Then open `http://VPS_IP:7781/` in Chrome and type `INTEL`, `CURV US`,
`WEI US`, `MOV active` in the command bar. **No `npm run dev`, no
separate process, no second port. The Omega binary is the only
thing that needs to be running.**

`npm run dev` (Vite at `:5173`) is purely for live-reload iteration
on the Mac during development — it is NOT part of the deploy or
verification path and should be ignored unless you're actively
editing the React code locally.

## What shipped this session (1 commit on omega-terminal)

| SHA              | Subject                                                                                                      |
|------------------|--------------------------------------------------------------------------------------------------------------|
| `<step-5-commit>`| Omega Terminal step 5: INTEL / CURV / WEI / MOV panels + libcurl-based OpenBbProxy in OmegaApiServer        |

The commit covers both Part A (UI) and Part B (engine-side C++) of
the Step-5 plan in `docs/omega_terminal/STEP5_OPENER.md`. Architecture
choice (a) — server-side libcurl-direct OpenBB proxy, no Python
sidecar — was approved by the user and implemented as designed.

## Architecture decision

OpenBB Hub is reached via a **server-side proxy in `Omega.exe`**, not
direct browser → OpenBB calls:

- `OMEGA_OPENBB_TOKEN` env var on the host running `Omega.exe`. The
  token never enters the React bundle, never leaves the engine box,
  rotates without a UI redeploy, and stays out of git. This is the
  shape we'll need at the Step-7 cutover when omega-terminal becomes
  the production GUI.
- Single-origin (`http://VPS_IP:7781`) — no CORS roulette.
- libcurl directly to `https://api.openbb.co/api/v1/`. No Python
  sidecar, no OpenBB Platform install on the VPS, no `pip` step in
  `QUICK_RESTART.ps1`. The four Step-5 endpoints are simple enough
  that the SDK's value (breadth across hundreds of routes) doesn't
  outweigh the deploy cost. If Step-6+ explodes the endpoint count,
  the `/api/v1/omega/openbb/*` route shape is reversible — we can
  swap libcurl for a Python sidecar later without changing the UI
  contract.
- Per-route TTL LRU cache inside `OpenBbProxy` so that the MOV
  panel's 1 s polling does not translate to 1 OpenBB call per second
  per UI tab. Cadences (panel poll → cache TTL): MOV 1 s → 750 ms,
  WEI 5 s → 4 s, INTEL 30 s → 25 s, CURV 60 s → 50 s.

## Engine-side C++ (Omega.exe)

- `src/api/OpenBbProxy.{hpp,cpp}` (new) — libcurl-based HTTPS client
  for OpenBB Hub. Singleton `OpenBbProxy::instance()`. `get(route,
  query, ttl_ms)` returns `{status, body, from_cache}`. Process-wide
  `curl_global_init` via `std::call_once`; one `curl_easy` handle per
  request inside an internal mutex. Per-URL LRU cache bounded at 256
  entries.
  - **Token strategy:** reads `OMEGA_OPENBB_TOKEN` at first use. If
    unset, every `get()` returns HTTP 503 with body
    `{"error":"OPENBB_TOKEN_NOT_SET","detail":"Set the
    OMEGA_OPENBB_TOKEN environment variable on the host running
    Omega.exe, or set OMEGA_OPENBB_MOCK=1 for synthetic data."}`.
  - **Mock mode:** `OMEGA_OPENBB_MOCK=1` bypasses the network
    entirely and returns canned synthetic JSON shaped like an OpenBB
    OBBject (`{"results":[...], "provider":"mock", "extra":
    {"mock":true}}`). This is **additive to the original Step-5 plan**;
    surfacing it here so it's not a surprise. Lets the user verify
    the full pipeline (UI → /api → proxy → mock data → UI render)
    without a paid OpenBB account. Mock data covers the four Step-5
    routes (`news/world`, `treasury_rates`, `equity/price/quote`,
    `equity/discovery/{active|gainers|losers}`); other routes return
    `{"results":[]}`. Provider badges in every Step-5 panel render
    amber **MOCK** when this mode is active so it's obvious at a
    glance.
  - Gated under `#ifndef OMEGA_BACKTEST` — the backtester targets
    don't need market data and stay free of the libcurl dep.
- `src/api/OmegaApiServer.cpp` — added four new routes:
  - `GET /api/v1/omega/intel?screen=<id>&limit=<n>` →
    `OpenBbProxy::get("news/world", "limit=N", 25 000 ms TTL)`. The
    `screen` param is parsed but currently maps every value to the
    default OpenBB news call (Step 6 will branch into sector / macro
    screens by switching on screen-id).
  - `GET /api/v1/omega/curv?region=US|EU|JP` → US calls
    `OpenBbProxy::get("fixedincome/government/treasury_rates",
    "provider=federal_reserve", 50 000 ms TTL)`. EU/JP return a 200
    with empty `results` and a structured `warnings` entry — Step 6
    wires those providers (ECB, BoJ).
  - `GET /api/v1/omega/wei?region=US|EU|ASIA|WORLD|<symbol-list>` →
    Region preset selects a curated index-ETF symbol list (US:
    SPY/QQQ/DIA/IWM/VTI; EU: VGK/EZU/FEZ/EWG/EWU; ASIA:
    EWJ/FXI/EWY/EWT/EWA; WORLD: VT/ACWI/VEA/VWO/EFA). Anything else
    is forwarded as a literal comma-separated list, so power users
    can type `WEI AAPL,MSFT,GOOGL`. Calls
    `OpenBbProxy::get("equity/price/quote",
    "symbol=...&provider=yfinance", 4 000 ms TTL)`.
  - `GET /api/v1/omega/mov?universe=active|gainers|losers` → Calls
    `OpenBbProxy::get("equity/discovery/<universe>",
    "provider=yfinance", 750 ms TTL)`. Universe is coerced to
    `active` on any unrecognised value.
  - All four routes pass the OpenBB OBBject envelope through verbatim
    — no JSON parsing in C++. The UI dereferences `.results` to get
    the row array. `provider` and `warnings` from the envelope are
    surfaced in a corner badge on each panel.
  - `?provider=<id>` query param on `/wei` and `/mov` overrides the
    default OpenBB provider, so users with paid OpenBB providers can
    swap `yfinance` for `fmp` etc. without code changes.
  - HTTP status text mapping extended for 503 → "Service
    Unavailable" and 504 → "Gateway Timeout" so the engine binary
    surfaces the proxy errors with the correct HTTP semantics.
- `CMakeLists.txt` — added `find_package(CURL REQUIRED)` and
  `CURL::libcurl` to the Omega target's `target_link_libraries`. New
  `src/api/OpenBbProxy.cpp` source. Status banner at the bottom of
  the configure log shows `libcurl: <version> (Step 5 OpenBB
  proxy)`. Backtest targets explicitly noted as "no libcurl" in the
  banner so it's clear the dep doesn't cross-contaminate.
  - **One-time install required on each build host:**
    - macOS: `brew install curl` (system libcurl already works for
      `find_package(CURL)` too, so this is usually a no-op)
    - Linux: `apt-get install libcurl4-openssl-dev`
    - **Windows VPS (the production deploy target):**
      `C:\vcpkg\vcpkg\vcpkg.exe install curl:x64-windows` — without
      this, `cmake -B build` fails with `Could NOT find CURL
      (missing: CURL_LIBRARY CURL_INCLUDE_DIR)`. This was hit on the
      first Step-5 deploy attempt today; documented here so the next
      VPS rebuild after a fresh checkout doesn't surprise the next
      operator. `QUICK_RESTART.ps1` correctly recovered to the
      previous binary on that failed configure, so live trading was
      never down.

## UI (omega-terminal/) — Part A

- `omega-terminal/src/api/types.ts` — added `OpenBbEnvelope<T>`
  generic wrapper plus the four row types: `IntelArticle`,
  `CurvPoint`, `WeiQuote`, `MovRow`. Added query types: `IntelQuery`,
  `CurvQuery`, `WeiQuery`, `MovQuery`. The envelope mirrors the
  OpenBB OBBject shape (`results`, `provider`, `warnings`, `chart`,
  `extra`) so the panels can both parse OpenBB responses verbatim
  and read the `provider` / `extra.mock` fields for the mock-mode
  badge.
- `omega-terminal/src/api/omega.ts` — added `getIntel`, `getCurv`,
  `getWei`, `getMov` typed wrappers. Per-call timeout bumped to 30 s
  for OpenBB routes (matches the libcurl timeout server-side, since
  OpenBB Hub free-tier endpoints can take 5–10 s on cold starts).
- `omega-terminal/src/panels/IntelPanel.tsx` (new, 297 lines) —
  Intelligence Screener. Multi-card top summary (article count,
  latest article time, provider, mode), then a scrollable feed of
  article rows. 30 s polling. Args: `INTEL <screen-id>`. Article rows
  show title (linked when `url` is present), source, symbols (first
  6 with overflow count), and a 280-char excerpt from `text`. Red
  retry banner on `OmegaApiError`. Provider/MOCK badge in the top-
  right.
- `omega-terminal/src/panels/CurvPanel.tsx` (new, 296 lines) — Yield
  Curve. Recharts `<LineChart>` with maturity (years) on the X axis
  and rate (%) on the Y axis. Reduces multi-row OpenBB responses
  into one chart point per maturity at the most-recent date.
  Maturity parser handles `month_<n>` / `year_<n>` (federal_reserve
  provider) plus loose `<n>m` / `<n>y` (forward-compat for other
  providers). 60 s polling. Args: `CURV US|EU|JP`. EU/JP currently
  surface the engine's "not yet wired" warning in the warnings
  banner. Provider/MOCK badge in the top-right.
- `omega-terminal/src/panels/WeiPanel.tsx` (new, 271 lines) — World
  Equity Indices. Sortable table over `WeiQuote[]`. Columns: Symbol,
  Name, Last, Δ, Δ%, Volume. Default sort: Δ% desc. Summary cards:
  symbol count, average Δ%, up/down counters. 5 s polling. Args:
  `WEI <region>`. Volume formatter renders K/M/B suffixes.
  Provider/MOCK badge in the top-right.
- `omega-terminal/src/panels/MovPanel.tsx` (new, 327 lines) — Movers.
  Sortable table over `MovRow[]`. Columns: Symbol, Name, Price, Δ,
  Δ%, Volume. Three-button universe pill row (ACTIVE / GAINERS /
  LOSERS) lets users flip universes without re-typing in the
  command bar. Default sort orientation flips on initial universe
  (asc for losers, desc otherwise). 1 s polling — most-frequent
  poll in the Step-5 set; the engine-side proxy's 750 ms cache TTL
  keeps the OpenBB call rate at ~1/s regardless of UI tab count.
  Provider/MOCK badge in the top-right.
- `omega-terminal/src/panels/PanelHost.tsx` — wired INTEL / CURV /
  WEI / MOV to the new panels, replacing `ComingSoonPanel`. CURV is
  loaded via `React.lazy` + `<Suspense>` so the Recharts dependency
  is split into a separate chunk and only fetched when the user
  navigates to CURV (see Bundle budget below). New
  `LazyChartFallback` component renders a small amber-on-black
  "Loading chart bundle…" placeholder during the chunk fetch.
- `omega-terminal/package.json` — added `recharts ^2.13.3` (npm
  resolved to `^2.15.4` during install — semver-minor, API-
  compatible).

No changes to `omega-terminal/src/router/functionCodes.ts` — INTEL /
CURV / WEI / MOV were already in `PANEL_REGISTRY` at `step: 5` since
Step 1.

No changes to `omega-terminal/src/types.ts`, `App.tsx`, `CommandBar`,
`WorkspaceTabs`, `usePanelData`, or the existing Step-3/4 panels —
the wire layer (`omega.ts` + `usePanelData`) was already shaped to
absorb new endpoints without touching the shell.

## Bundle budget — within the +60 kB raw / +15 kB gzipped cap

Step-4 baseline: 201 kB raw / 58 kB gzipped (single bundle).

Step-5 with code-split (verified twice — once locally via `npx vite
build --outDir dist-verify`, once via the actual VPS deploy log):

| Chunk                          | Raw       | Gzipped   | Loaded when                   |
|--------------------------------|-----------|-----------|-------------------------------|
| `index-<hash>.js` (eager)      | 222.84 kB | 62.43 kB  | Page load                     |
| `index-<hash>.css`             |  15.53 kB |  4.13 kB  | Page load                     |
| `CurvPanel-<hash>.js` (lazy)   | 388.72 kB | 107.47 kB | First navigation to CURV      |

Eager-bundle delta vs Step-4: **+21.84 kB raw / +4.43 kB gzipped** —
both well under the +60 kB raw / +15 kB gzipped budget. The Recharts-
heavy CurvPanel chunk is paid only on demand, so the cold-load
experience for CC / ENG / POS / LDG / TRADE / CELL / INTEL / WEI / MOV
is unchanged from Step 4.

## Verification gates

| Gate                                            | Status                                                                       |
|-------------------------------------------------|------------------------------------------------------------------------------|
| `omega-terminal/`: `npm run typecheck`          | ✅ green (Mac dev sandbox)                                                   |
| `omega-terminal/`: `npm run build` (Mac)        | ✅ green (Mac dev sandbox)                                                   |
| `omega-terminal/`: VPS production build         | ✅ green ("[OK] UI built in 31.3s -> omega-terminal\dist\")                  |
| Bundle delta ≤ +60 kB / +15 kB                  | ✅ +21.84 / +4.43 (verified Mac and VPS, byte-identical)                     |
| C++ side: `cmake -B build` (Mac)                | ✅ configure green, libcurl 8.7.1 found from system                         |
| C++ side: `cmake --build build` (Mac, target=Omega) | ❌ N/A by design — `src/main.cpp:10` unconditionally `#include <winsock2.h>`. Omega.exe is Windows-only. Mac compiles only the backtest targets. |
| C++ side: `cmake -B build` (Win VPS)            | ❌ failed first attempt: `Could NOT find CURL (missing: CURL_LIBRARY CURL_INCLUDE_DIR)`. Fix: one-time `vcpkg install curl:x64-windows`, then green. |
| C++ side: `cmake --build build` (Win VPS)       | ⏳ pending the vcpkg install above                                          |
| `OMEGA_OPENBB_MOCK=1` smoke (VPS)               | ⏳ pending the build                                                         |
| Live OpenBB smoke (token set, VPS)              | ⏳ pending the build                                                         |
| AbortController unmount cleanup                 | ⏳ visual check pending — `Ctrl+W` close + DevTools network tab on the live VPS UI |
| Service recovery on failed configure            | ✅ verified — `QUICK_RESTART.ps1` correctly fell back to the previous Omega.exe when the missing-curl configure aborted; live trading not interrupted |

## Hot-fix after first VPS build attempt

After the vcpkg curl install (Gotcha #1 below), the second deploy
attempt got further but failed mid-compile of `OpenBbProxy.cpp` with
a wall of `curl.h` errors plus a syntax error at line 341. Two real
bugs in my Step-5 code, both fixed and committed in this same handoff:

### Bug A: Windows include order — `<winsock2.h>` must precede `<curl/curl.h>`

Symptoms (truncated):
```
curl.h(141,16): error C2146: syntax error: missing ';' before identifier 'curl_socket_t'
curl.h(434,19): error C2079: 'curl_sockaddr::addr' uses undefined struct 'sockaddr'
multi.h(115,17): error C3646: 'fd': unknown override specifier
multi.h(159,40): error C2061: syntax error: identifier 'fd_set'
```

Root cause: libcurl's `curl.h` references `curl_socket_t`, `sockaddr`,
and `fd_set` — all typedef'd by `winsock2.h` on Windows. My Step-5
`OpenBbProxy.cpp` only had `#include <curl/curl.h>` cold; on Windows
that meant the typedefs were missing and curl.h was the unhappy
party reporting downstream errors. (The repo-level CMake defines
`_WINSOCKAPI_` globally, which prevents `<windows.h>` from pulling
in the older `winsock.h`, so we have a clean field for `winsock2.h`
in this TU but it has to be included explicitly.)

Fix in `src/api/OpenBbProxy.cpp`:
```cpp
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <curl/curl.h>
```

This is the same idiom `OmegaApiServer.cpp` already uses for the
plain BSD-sockets includes; I just forgot it for the curl side.

### Bug B: Raw-string delimiter collision — `(mock)"` prematurely terminated `R"(...)"`

Symptom (truncated):
```
OpenBbProxy.cpp(341,74): error C2143: syntax error: missing ')' before ':'
warning C4473: 'snprintf' : not enough arguments passed for format string
the missing variadic argument 2 is required by format string '%s'
```

Root cause: my `equity/price/quote` mock-data `snprintf` used:
```cpp
std::snprintf(buf, sizeof(buf),
    R"({"symbol":"%s","name":"%s ETF (mock)","last_price":%.2f,)"
    R"("change":%.2f,...)"
    ...);
```
The default raw-string form `R"(...)"` terminates at the FIRST `)"`
it encounters — and `(mock)"` contains exactly that sequence. So the
literal terminated mid-string and the rest of the line parsed as
broken C++ — hence the "missing ')'", "newline in constant", and
"variadic argument" warnings.

Fix: use a custom delimiter `R"X(...)X"` for the offending strings
only. The other three mock blocks (news/world, treasury_rates,
equity/discovery) don't contain `()` and keep the simpler form.
Documented inline in the file with a "Raw-string delimiter note"
header above the `fetch_mock` definition.

### Push the fix

```bash
cd /Users/jo/omega_repo
git add src/api/OpenBbProxy.cpp docs/SESSION_2026-05-01d_HANDOFF.md
git commit -m "Step 5 hot-fix: winsock2 include order + raw-string delimiter in OpenBbProxy mock"
git push origin omega-terminal
```

Then on the VPS: `.\QUICK_RESTART.ps1 -Branch omega-terminal`. The
vcpkg curl install from the prior step is still in place; this is
purely a source-side fix.

## Known gotchas (hit during this session)

### 1. Windows VPS: vcpkg curl is a one-time install

Symptom on first Step-5 deploy:
```
CMake Error ... FindCURL.cmake:203 (find_package_handle_standard_args):
  Could NOT find CURL (missing: CURL_LIBRARY CURL_INCLUDE_DIR)
```

Fix:
```powershell
C:\vcpkg\vcpkg\vcpkg.exe install curl:x64-windows
```

After this runs once, every subsequent `cmake -B build` finds curl
through the vcpkg toolchain (already loaded by `CMakeLists.txt` lines
9–12) and the configure stage prints `libcurl: 8.x.x (Step 5 OpenBB
proxy)`. The recovery path in `QUICK_RESTART.ps1` already handles
this gracefully — the previous binary keeps running until the next
deploy succeeds.

### 2. Mac: `npm install recharts` rewrote the lockfile and lost the rollup native binary

Symptom on first `npm run dev` after the recharts install:
```
Error: Cannot find module @rollup/rollup-darwin-arm64.
npm has a bug related to optional dependencies (...)
```

Fix:
```bash
cd /Users/jo/omega_repo/omega-terminal
rm -rf node_modules package-lock.json
npm install
```

This is a known npm bug (npm/cli#4828) that surfaces whenever a lockfile
is rewritten and the platform-specific optional dep doesn't round-trip
correctly. Already-staged `package-lock.json` will include the corrected
optional-dep entries after the second install — commit it.

### 3. Mac cannot build Omega.exe (and never could)

`src/main.cpp:10` has an unconditional `#include <winsock2.h>`. This
is pre-existing and orthogonal to Step 5; mentioned because the
Step-5 verification flow tempted me to try `cmake --build build` on
Mac, which fails the moment the Omega target is touched. The Mac-
buildable targets are only the backtest binaries (`OmegaBacktest`,
`TsmomCellBacktest`, `OmegaSweepHarness`, `OmegaSweepHarnessCRTP`,
`OmegaEdgeFinderExtract`, `OmegaEdgeFinderTest`) — they avoid
`main.cpp` and stay portable.

If we ever want Mac-side smoke testing of the Step-5 OpenBB proxy,
the smallest possible fix is wrapping that one include under
`#ifdef _WIN32`. That edit was deliberately NOT made this session
because (a) it's core engine code, and (b) other Windows-only
assumptions deeper in the engine would surface the moment the Mac
linker got past that one include. Filed as a Step-7 candidate (same
session as the `src/gui/` retirement, since the GUI is a similar
platform-portability cleanup).

### 4. zsh ate the `OpenBbProxy.*` glob in the original `git add`

The original push command in this session used unquoted wildcards
which zsh tried to expand from `omega-terminal/` and failed because
the C++ files are at repo root. The TL;DR section above uses
explicit paths only — no wildcards.

## Bundle-verify cleanup

`npm run build` cannot delete the existing `omega-terminal/dist/`
inside the dev sandbox because the Step-4 deploy artefacts in there
were written by a different uid. Verification was run via
`npx vite build --outDir dist-verify` instead. On the user's Mac:

```bash
rm -rf omega-terminal/dist omega-terminal/dist-verify
```

The next real `npm run build` (or VPS deploy) will recreate `dist/`
cleanly. The VPS deploy already produced its own clean `dist/` —
this is purely a Mac-side cleanup.

## Pending follow-ups (not blocking Step 6)

Carried forward from Step 4:

- Register the remaining open-position sources (Tsmom / Donchian /
  EmaPullback / TrendRider / HBI).
- Differentiate the HBI four-pack's `tr.engine` by symbol.
- Engine-side `/cells` route + `CellSummaryRegistry`.
- Optional `/trade/<id>` route.
- MFE / MAE / signal context on `LedgerEntry`.
- `OMEGA_WATCHDOG -Branch` flag.
- The v3.5 npm/vite stderr-mangling fix in `QUICK_RESTART.ps1`.

New from Step 5:

- **EU and JP yield curves** — wire OpenBB ECB and BoJ providers in
  `build_curv_json`. The UI side and the warnings banner are already
  in place; only the engine-side branch needs to grow two more
  routes.
- **INTEL screen-id branching** — currently every screen-id maps to
  `/news/world`. Split out sector (`SECTOR <name>`), earnings
  (`EARNINGS`), and macro (`MACRO`) screens by composing different
  OpenBB calls and merging the results.
- **/api/v1/omega/openbb/<route>** generic passthrough — useful for
  prototyping new panels in the UI without an engine-side route
  change. Defer until at least one new panel needs it.
- **Provider override UX** — both `?provider=<id>` query params on
  `/wei` and `/mov` work end-to-end, but there is no UI for setting
  them. Power users can type `WEI US` and get yfinance; a future
  dropdown / slash command can let them switch providers without
  hand-editing the URL.
- **OpenBB rate-limit surfacing** — when OpenBB returns 429, the
  proxy passes that status through verbatim and the UI's red retry
  banner shows "OmegaApi HTTP 429". Adding a small `Retry-After`
  parser server-side and a countdown in the UI banner would make
  this less brittle for free-tier users hitting the cap.
- **`QUICK_RESTART.ps1` vcpkg pre-flight** — add a step that verifies
  `curl:x64-windows` is installed in vcpkg before invoking `cmake
  -B build`. Either auto-install or fail fast with a clearer
  message. Same shape would help future Step-6+ ports (`pugixml`,
  `nlohmann-json` if we ever take the JSON-lib stance back, etc.).
  Out of scope today because that script is core deploy infra and
  edits to it require explicit user approval — flag for the next
  cycle.
- **Mac dev parity for Omega.exe** — see Gotcha #3 above. Wrap
  `winsock2.h` under `#ifdef _WIN32` in `src/main.cpp`. Trivial
  edit; risk is the cascade of other Windows assumptions it
  exposes. Best done at the Step-7 cutover when `src/gui/` retires
  and the engine is in motion anyway.

## Files changed summary

```
src/api/OpenBbProxy.hpp                             (new, 95 lines)
src/api/OpenBbProxy.cpp                             (new, 286 lines)
src/api/OmegaApiServer.cpp                          (modified, +145 lines)
CMakeLists.txt                                      (modified, +24 lines)
omega-terminal/package.json                         (modified, +1 line)
omega-terminal/package-lock.json                    (modified — recharts +
                                                     transitives + corrected
                                                     optional-dep set after
                                                     the lockfile rebuild)
omega-terminal/src/api/types.ts                     (modified, +138 lines)
omega-terminal/src/api/omega.ts                     (modified, +109 lines)
omega-terminal/src/panels/IntelPanel.tsx            (new, 297 lines)
omega-terminal/src/panels/CurvPanel.tsx             (new, 296 lines)
omega-terminal/src/panels/WeiPanel.tsx              (new, 271 lines)
omega-terminal/src/panels/MovPanel.tsx              (new, 327 lines)
omega-terminal/src/panels/PanelHost.tsx             (modified, +28 lines)
docs/SESSION_2026-05-01d_HANDOFF.md                 (new, this doc)
docs/omega_terminal/STEP6_OPENER.md                 (new, BB function suite)
```

## Push and deploy checklist

1. **Mac:** stage and commit (explicit paths from the TL;DR; do NOT
   use shell glob wildcards inside zsh).
2. **Mac:** push to origin/omega-terminal.
3. **VPS one-time:** `vcpkg install curl:x64-windows`.
4. **VPS one-time:** set `OMEGA_OPENBB_MOCK=1` (or
   `OMEGA_OPENBB_TOKEN=<token>`) at the Machine env scope so the
   service inherits it.
5. **VPS:** `.\QUICK_RESTART.ps1 -Branch omega-terminal`.
6. **Browser:** open `http://VPS_IP:7781/`. Type `INTEL`, `CURV US`,
   `WEI US`, `MOV active` in the command bar. Confirm each panel
   renders with a `provider:` corner badge (mock or real). Open
   DevTools network tab, hit `Ctrl+W` to close a panel, confirm no
   further `/api/v1/omega/*` requests fire after the close.
