# Session 2026-05-01e — Handoff (Step 6 complete)

Branch: `omega-terminal`. HEAD = `<step-6-commit>` (filled in after the
commit lands; parent is `76b5a21` Step-5 hot-fix). This session continues
directly from `docs/SESSION_2026-05-01d_HANDOFF.md` and implements
`docs/omega_terminal/STEP6_OPENER.md`.

## TL;DR — how to actually run this

Same shape as Step 5 — push the branch, deploy on the VPS via the existing
`QUICK_RESTART.ps1`, open Chrome at `http://VPS_IP:7781/`. The rebuilt
`Omega.exe` serves both the UI (from `omega-terminal/dist/`) and the API
(from `/api/v1/omega/*`) on the same port.

```bash
# Mac:
cd /Users/jo/omega_repo
git add \
  src/api/OmegaApiServer.cpp \
  src/api/OpenBbProxy.cpp \
  src/api/WatchScheduler.hpp \
  src/api/WatchScheduler.cpp \
  CMakeLists.txt \
  omega-terminal/src/api/types.ts \
  omega-terminal/src/api/omega.ts \
  omega-terminal/src/panels/_shared/PanelChrome.tsx \
  omega-terminal/src/panels/OmonPanel.tsx \
  omega-terminal/src/panels/FaPanel.tsx \
  omega-terminal/src/panels/KeyPanel.tsx \
  omega-terminal/src/panels/DvdPanel.tsx \
  omega-terminal/src/panels/EePanel.tsx \
  omega-terminal/src/panels/NiPanel.tsx \
  omega-terminal/src/panels/GpPanel.tsx \
  omega-terminal/src/panels/QrPanel.tsx \
  omega-terminal/src/panels/HpPanel.tsx \
  omega-terminal/src/panels/DesPanel.tsx \
  omega-terminal/src/panels/FxcPanel.tsx \
  omega-terminal/src/panels/CryptoPanel.tsx \
  omega-terminal/src/panels/WatchPanel.tsx \
  omega-terminal/src/panels/PanelHost.tsx \
  docs/SESSION_2026-05-01e_HANDOFF.md
git commit -m "Omega Terminal step 6: BB function suite (13 panels) + WatchScheduler"
git push origin omega-terminal
```

```powershell
# VPS (no new vcpkg deps; the Step-5 vcpkg curl install is still in place):
.\QUICK_RESTART.ps1 -Branch omega-terminal
```

Then open `http://VPS_IP:7781/` in Chrome and type any of the new codes
(`OMON AAPL`, `FA AAPL`, `KEY AAPL`, `DVD AAPL`, `EE AAPL`, `NI AAPL`,
`GP AAPL 1d`, `QR AAPL,MSFT,GOOG`, `HP AAPL 1h`, `DES AAPL`,
`FXC MAJORS`, `CRYPTO MAJORS`, `WATCH SP500`) in the command bar.

`OMEGA_OPENBB_MOCK=1` continues to short-circuit every route to synthetic
data, including all 13 new endpoints. The provider badge in each panel's
top-right shows amber `MOCK` when this mode is active so verification
builds are obvious at a glance.

## What shipped this session (1 commit on omega-terminal)

| SHA              | Subject                                                                                                  |
|------------------|----------------------------------------------------------------------------------------------------------|
| `<step-6-commit>`| Omega Terminal step 6: BB function suite (13 panels) + WatchScheduler nightly screener                  |

Architecture decisions (locked in via the AskUserQuestion gate at the top
of the session):

- **WATCH** uses a cron-style nightly job inside `Omega.exe`
  (`WatchScheduler` singleton with its own thread + condition_variable).
  The route just snapshots an in-process registry — fast, predictable.
- **OpenBB endpoint mappings** match the table in `STEP6_OPENER.md`
  exactly; no overrides applied.
- **GP / HP cache sharing**: both routes call
  `OpenBbProxy::get("equity/price/historical", "symbol=…&interval=…", 25000)`
  with byte-identical query strings, so the existing URL-keyed LRU cache
  collapses them onto one slot. No `chart=true` distinguisher; chart
  rendering is purely client-side.
- **C++ approval shape**: plan-first, then approved in one batch; this
  handoff captures the post-approval final state.

## Engine-side C++ (Omega.exe)

### `src/api/OmegaApiServer.cpp` (modified, +~700 lines)

- Header comment extended with the 13 new routes.
- Added `#include "WatchScheduler.hpp"`.
- New OpenBB envelope helpers in the anonymous namespace right after
  `json_bool`: `ob_extract_value`, `ob_results_array`, `ob_provider_str`,
  `ob_warnings_value`, `ob_extra_value`, `ob_merge_warnings`,
  `ob_pick_provider`, `ob_pick_extra`, `ob_worst_status`. These are
  string-level scanners with brace/bracket/string-aware depth tracking —
  no third-party JSON lib added.
- 13 new `build_*_json` functions following the Step-5 pattern. Single-
  call routes (OMON / DVD / NI / QR / DES / FXC / CRYPTO) pass the OpenBB
  envelope through verbatim. Merged routes (FA / KEY / EE) make 2-3
  OpenBB calls and stitch the bodies into one composite envelope via the
  `ob_*` helpers above. WATCH reads from
  `WatchScheduler::instance().snapshot(universe)` and serialises a small
  JSON envelope with `last_run_ts`, `next_run_ts`, `scanning`, `universe`,
  `provider`, and the hits array.
- GP and HP share a single builder (`build_gp_or_hp_json`) so the upstream
  cache key is byte-identical when the params match.
- 13 new dispatch entries inserted right below the `/mov` entry in the
  `OmegaApiServer::run` else-if chain.
- `OmegaApiServer::start()` now also calls `WatchScheduler::instance().start()`;
  `OmegaApiServer::stop()` now also calls `WatchScheduler::instance().stop()`.
  Both scheduler calls are idempotent so duplicate start/stop is safe.

### `src/api/OpenBbProxy.cpp` (modified, +~270 lines)

- `fetch_mock` extended with synthetic JSON for 13 new OpenBB upstream
  routes so `OMEGA_OPENBB_MOCK=1` exercises every Step-6 panel end-to-
  end. The merged-call routes synthesise per-call mock data (e.g. the FA
  panel issues `equity/fundamental/{income,balance,cash}` separately and
  the engine merges them via the new `build_fa_json` builder).
- The new mock blocks share the same `R"X(...)X"` raw-string delimiter
  idiom introduced in the Step-5 hot-fix wherever the mock body contains
  literal `()`.

### `src/api/WatchScheduler.{hpp,cpp}` (new, ~95 + ~310 lines)

- Singleton `WatchScheduler::instance()` with `start()`, `stop()`,
  `snapshot(universe)`, `trigger_now()`, `running()` API.
- Worker thread waits on a `std::condition_variable` until the next 00:30
  UTC tick, then runs `run_scan("SP500")`, `run_scan("NDX")`, and
  synthesises an `"ALL"` snapshot from the union of the two.
- v1 screener is a simple momentum + relative-volume rule (|Δ%| ≥ 5 % AND
  volume ≥ 100k) that emits `MOMO-UP` / `MOMO-DN` hits scored by
  `abs(change_percent)`. Easy to swap for a richer composite without
  changing the public surface.
- Constituent universes are baked-in `static const char* const kSP500[]`
  and `kNDX[]` arrays (~150 + ~100 symbols respectively as a deployable
  v1 footprint; expand here without breaking the API contract).
- Mock mode reuses `OpenBbProxy::instance()` — no special mock branch in
  the scheduler itself; if the proxy is in mock mode the scheduler sees
  mock quotes and the WATCH panel renders them with the amber `MOCK`
  badge.

### `CMakeLists.txt` (modified, +1 line)

Single new line in the `SOURCES` list:

```cmake
src/api/WatchScheduler.cpp        # Step 6 Omega Terminal: nightly INTEL screener (S&P 500 + NDX)
```

No new `find_package`, no new vcpkg port, no new linker arg. The existing
`CURL::libcurl` linkage covers WatchScheduler via the existing
`OpenBbProxy::instance()`. The Step-5 vcpkg toolchain fallback (CMakeLists
lines 23-48) still applies; if the VPS still has `curl:x64-windows`
installed from the Step-5 deploy, configure should succeed without any
new vcpkg work.

## UI (omega-terminal/) — Part A

### `omega-terminal/src/api/types.ts` (modified, +~340 lines)

Added Step-6 row types and query types:
- `OptionsRow` + `OmonQuery`
- `IncomeStatement`, `BalanceSheet`, `CashFlow`, `FaEnvelope`, `FaQuery`
- `KeyMetricsRow`, `MultiplesRow`, `KeyEnvelope`, `KeyQuery`
- `Dividend`, `DvdQuery`
- `EpsConsensus`, `EpsSurprise`, `EeEnvelope`, `EeQuery`
- `NiQuery` (reuses `IntelArticle` from Step 5)
- `BarInterval`, `HistoricalBar`, `GpQuery`, `HpQuery`
- `QuoteRow`, `QrQuery`
- `CompanyProfile`, `DesQuery`
- `FxQuote`, `FxcQuery`
- `CryptoQuote`, `CryptoQuery`
- `WatchHit`, `WatchEnvelope`, `WatchQuery`

### `omega-terminal/src/api/omega.ts` (modified, +~250 lines)

Added 13 typed fetchers: `getOmon`, `getFa`, `getKey`, `getDvd`, `getEe`,
`getNi`, `getGp`, `getQr`, `getHp`, `getDes`, `getFxc`, `getCrypto`,
`getWatch`. Default timeout for OpenBB-backed routes stays at 30 s; WATCH
uses the engine-internal 5 s default since `/watch` answers from a local
registry without an OpenBB round-trip.

### `omega-terminal/src/panels/_shared/PanelChrome.tsx` (new, ~280 lines)

New shared module exporting deduplicated chrome bits used by all 13 Step-6
panels: `ProviderBadge`, `SummaryCard`, `SortHeader`, `SkeletonRows`,
`SkeletonArticles`, `FetchStatusBar`, `PanelHeader`, `WarningsBanner`,
`MissingSymbolPrompt`, `detectMock`, plus formatting helpers (`formatNum`,
`formatVolume`, `formatLargeNum`, `formatPct`, `dirClass`, `parseDateMs`,
`formatUtc`, `formatDate`, `truncate`). Step-5 panels are NOT refactored
to use this module — they keep their inline copies untouched per the
"never modify core code unless instructed clearly" preference. The dedup
is what keeps the eager bundle inside the Step-6 budget cap.

### 13 new panels under `omega-terminal/src/panels/`

| Panel       | Lines | Layout                               | Args                       | Polling    |
|-------------|-------|--------------------------------------|----------------------------|------------|
| OmonPanel   | ~290  | Sortable chain table + summary cards | `OMON <sym> [<expiry>]`    | 5 s        |
| FaPanel     | ~260  | 3-tab table (income/balance/cash)    | `FA <sym>`                 | 5 min      |
| KeyPanel    | ~135  | Multi-card grid (4 sections)         | `KEY <sym>`                | 5 min      |
| DvdPanel    | ~190  | Sortable history table + TTM card    | `DVD <sym>`                | 5 min      |
| EePanel     | ~190  | Multi-card consensus + sortable surprise | `EE <sym>`             | 5 min      |
| NiPanel     | ~165  | Article feed (like INTEL)            | `NI <sym>`                 | 30 s       |
| GpPanel     | ~205  | Recharts `<LineChart>` (LAZY)        | `GP <sym> [<interval>]`    | 30 s       |
| QrPanel     | ~190  | Sortable quote table                 | `QR <sym-list>`            | 5 s        |
| HpPanel     | ~215  | Sortable OHLCV table + interval pills| `HP <sym> [<interval>]`    | 30 s       |
| DesPanel    | ~140  | Single-symbol detail card grid       | `DES <sym>`                | 5 min      |
| FxcPanel    | ~180  | Sortable FX table                    | `FXC <pair-or-region>`     | 5 s        |
| CryptoPanel | ~190  | Sortable crypto table                | `CRYPTO <sym-or-region>`   | 5 s        |
| WatchPanel  | ~210  | Sortable hits table + universe pills | `WATCH <universe>`         | 60 s       |

All 13 follow the Step-5 pattern: `usePanelData` for the fetch+poll
lifecycle, red retry banner on `OmegaApiError`, provider/MOCK badge in
the top-right, args parser overload (where the panel exposes a pill row
for in-panel switching).

### `omega-terminal/src/panels/PanelHost.tsx` (modified, +~50 lines)

- 12 new eager imports (one per Step-6 panel except GP).
- GP via `React.lazy` + `<Suspense>` — same rule as CURV. CURV and GP
  share Vite's auto-emitted `LineChart-<hash>.js` vendor chunk so adding
  GP doesn't double the chart cost.
- 13 new `if (code === 'X') return <XPanel …>` entries below the Step-5
  block.

## Bundle budget — within the +60 kB raw / +15 kB gzipped cap

Step-5 baseline: 222.84 kB raw / 62.43 kB gzipped (eager).

Step-6 with code-split (verified via `npx vite build --outDir dist-step6`):

| Chunk                          | Raw       | Gzipped   | Loaded when                                |
|--------------------------------|-----------|-----------|--------------------------------------------|
| `index-<hash>.js` (eager)      | 279.09 kB |  71.86 kB | Page load                                  |
| `index-<hash>.css`             |  15.67 kB |   4.17 kB | Page load                                  |
| `CurvPanel-<hash>.js` (lazy)   |   5.77 kB |   2.34 kB | First navigation to CURV                   |
| `GpPanel-<hash>.js` (lazy)     |   4.04 kB |   1.90 kB | First navigation to GP                     |
| `LineChart-<hash>.js` (vendor) | 383.12 kB | 105.41 kB | First navigation to CURV or GP (shared)    |

Eager-bundle delta vs Step-5: **+56.25 kB raw / +9.43 kB gzipped** —
both inside the +60 kB raw / +15 kB gzipped cap. The Recharts vendor
chunk is shared between CURV and GP, so adding GP didn't double the
chart cost.

## Verification gates

| Gate                                                | Status |
|-----------------------------------------------------|--------|
| `omega-terminal/`: `npm run typecheck`              | ✅ green |
| `omega-terminal/`: `npm run build` (Mac dev sandbox)| ✅ green (delta +56.25 kB raw / +9.43 kB gz) |
| C++ side: `g++ -std=c++20 -fsyntax-only` on `OpenBbProxy.cpp`, `OmegaApiServer.cpp`, `WatchScheduler.cpp` (with curl stub) | ✅ all three pass clean |
| C++ side: `cmake -B build` + `cmake --build build --target Omega` (Win VPS) | ⏳ pending the deploy run; no new external deps so the Step-5 hot-fix's vcpkg toolchain fallback should keep configure green |
| `OMEGA_OPENBB_MOCK=1` smoke (VPS)                   | ⏳ pending |
| Live OpenBB smoke (token set, VPS)                  | ⏳ pending |
| AbortController unmount cleanup on Step-6 panels    | ⏳ pending visual check on the live VPS UI |

Mac `cmake --build build --target Omega` remains N/A by design (Gotcha #3
from the Step-5 handoff: `src/main.cpp:10` unconditional `<winsock2.h>`).
The Mac path stops at `cmake -B build` configure-green and the syntax
check above.

## VPS build pre-flight (carry-over from Step 5)

The Step-5 hot-fix bits are still intact at HEAD:
- `CMakeLists.txt` lines 23-48: vcpkg toolchain fallback (`VCPKG_ROOT` →
  `C:/vcpkg/vcpkg/...` → `C:/vcpkg/...` → `C:/dev/vcpkg/...`).
- `src/api/OpenBbProxy.cpp` lines 36-39: `<winsock2.h>` + `<ws2tcpip.h>`
  before `<curl/curl.h>`.
- `src/api/OpenBbProxy.cpp` `R"X(...)X"` delimiter on every mock block
  containing literal `()`.

Step 6 introduces:
- One new C++ source file (`src/api/WatchScheduler.cpp`) — added to
  `SOURCES`; no new `find_package`, no new vcpkg port.
- Zero new external libraries. WatchScheduler reuses `OpenBbProxy` for
  upstream calls; the existing `CURL::libcurl` linkage covers it.

If `cmake -B build` fails on the VPS with `Could NOT find CURL`, run the
Step-5 one-time install:

```powershell
C:\vcpkg\vcpkg\vcpkg.exe install curl:x64-windows
```

Then re-run `.\QUICK_RESTART.ps1 -Branch omega-terminal`. The recovery
path keeps the previous binary running until the next configure
succeeds.

## Pending follow-ups (not blocking Step 7)

Carried forward from Step 5:
- Register the remaining open-position sources (Tsmom / Donchian /
  EmaPullback / TrendRider / HBI).
- Differentiate the HBI four-pack's `tr.engine` by symbol.
- Engine-side `/cells` route + `CellSummaryRegistry`.
- Optional `/trade/<id>` route.
- MFE / MAE / signal context on `LedgerEntry`.
- `OMEGA_WATCHDOG -Branch` flag.
- The v3.5 npm/vite stderr-mangling fix in `QUICK_RESTART.ps1`.
- EU and JP yield curves on CURV.
- INTEL screen-id branching.
- `/api/v1/omega/openbb/<route>` generic passthrough.
- Provider override UX.
- OpenBB rate-limit surfacing.
- `QUICK_RESTART.ps1` vcpkg pre-flight.
- Mac dev parity for Omega.exe (`#ifdef _WIN32` around `winsock2.h`).

New from Step 6:
- **WATCH richer screener** — v1 ships a deliberately-simple momentum +
  relative-volume rule. Step 7+ can wire the INTEL panel's full
  composite screener once it lands engine-side.
- **WATCH constituent lists from config** — currently baked into
  `WatchScheduler.cpp`. A future `config/sp500.txt` / `config/ndx.txt`
  read at start() would let ops update the universe without a rebuild.
- **WATCH `/trigger_now` route** — the scheduler exposes
  `trigger_now()` already; a small route handler in OmegaApiServer.cpp
  would let ops kick a scan from the command line. Out of scope this
  commit because that's a write-API surface that wants a small auth
  story first.
- **OMON server-side expiry filter** — currently the engine returns the
  full chain and OmonPanel filters client-side. Step 7+ can add server-
  side filtering once the chain-provider field-name divergence is
  audited (cboe / intrinio / tradier).
- **FA / KEY / EE reuse `ob_*` helpers** — the merged-envelope helpers
  now exist in OmegaApiServer.cpp; they're a candidate for promotion to
  a shared `JsonExtract.hpp` if a future engine-side feature needs them
  outside this TU.
- **GP chart range / overlay** — v1 GpPanel renders a single close-line.
  Volume bars, range selector, multi-symbol comparison, and indicators
  are explicit Step-7+ candidates.
- **HP date-range filtering** — the HpQuery type already exposes
  `start_date` / `end_date` and the engine forwards them; HpPanel just
  doesn't surface a UI for them yet.

## Files changed summary

```
src/api/OmegaApiServer.cpp                          (modified, +~700 lines)
src/api/OpenBbProxy.cpp                             (modified, +~270 lines)
src/api/WatchScheduler.hpp                          (new, ~95 lines)
src/api/WatchScheduler.cpp                          (new, ~310 lines)
CMakeLists.txt                                      (modified, +1 line)
omega-terminal/src/api/types.ts                     (modified, +~340 lines)
omega-terminal/src/api/omega.ts                     (modified, +~250 lines)
omega-terminal/src/panels/_shared/PanelChrome.tsx   (new, ~280 lines)
omega-terminal/src/panels/OmonPanel.tsx             (new, ~290 lines)
omega-terminal/src/panels/FaPanel.tsx               (new, ~260 lines)
omega-terminal/src/panels/KeyPanel.tsx              (new, ~135 lines)
omega-terminal/src/panels/DvdPanel.tsx              (new, ~190 lines)
omega-terminal/src/panels/EePanel.tsx               (new, ~190 lines)
omega-terminal/src/panels/NiPanel.tsx               (new, ~165 lines)
omega-terminal/src/panels/GpPanel.tsx               (new, ~205 lines)
omega-terminal/src/panels/QrPanel.tsx               (new, ~190 lines)
omega-terminal/src/panels/HpPanel.tsx               (new, ~215 lines)
omega-terminal/src/panels/DesPanel.tsx              (new, ~140 lines)
omega-terminal/src/panels/FxcPanel.tsx              (new, ~180 lines)
omega-terminal/src/panels/CryptoPanel.tsx           (new, ~190 lines)
omega-terminal/src/panels/WatchPanel.tsx            (new, ~210 lines)
omega-terminal/src/panels/PanelHost.tsx             (modified, +~50 lines)
docs/SESSION_2026-05-01e_HANDOFF.md                 (new, this doc)
```

Mac-side cleanup post-verification (same shape as the Step-5 dist-verify
note):

```bash
rm -rf omega-terminal/dist-step6
```

## Push and deploy checklist

1. **Mac:** stage and commit (explicit paths from the TL;DR).
2. **Mac:** push to origin/omega-terminal.
3. **VPS:** confirm vcpkg `curl:x64-windows` is still installed (Step-5
   one-time install). If it isn't,
   `C:\vcpkg\vcpkg\vcpkg.exe install curl:x64-windows`.
4. **VPS:** `.\QUICK_RESTART.ps1 -Branch omega-terminal`.
5. **Browser:** open `http://VPS_IP:7781/`. Type any of the 13 new codes
   and confirm each panel renders with a `provider:` corner badge (mock
   or real). Open DevTools network tab, hit `Ctrl+W` to close a panel,
   confirm no further `/api/v1/omega/*` requests fire after the close.
6. **WATCH dry run:** open `http://VPS_IP:7781/api/v1/omega/watch?universe=SP500`
   directly in the browser. Initial response will show
   `last_run_ts: 0` until the first 00:30 UTC tick (or until the next
   `trigger_now` is wired). Confirm `next_run_ts` is the next 00:30 UTC.
