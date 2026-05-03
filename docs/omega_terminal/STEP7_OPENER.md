# Omega Terminal — Step 7 Opener (replace OpenBB with direct free providers)

Paste the section below into a fresh chat to begin Step 7. Everything
above the horizontal rule is context for you (the human reader);
everything below is the prompt to copy.

State at the boundary of step 6 → step 7:

* Branch: `omega-terminal`. HEAD = `<step-6-commit + Step-6 hot-fix>`.
  Step 5 commit `09441de`, Step 5 hot-fix `76b5a21`, Step 4 commit
  `3baa20f`, Step 3 commit `a1287ce`, Step 2 commit `0ed1dec`, Step 1
  commit `6d7256a` are still in the ancestry. `main` is unchanged at
  `161d992`. `src/gui/` is untouched and stays untouched until the
  Step-7 cutover (renamed in this opener — see below).
* Step 6 deliverables shipped (per `docs/SESSION_2026-05-01e_HANDOFF.md`):
   * 13 new BB-style panels live in the UI bundle (OMON / FA / KEY /
     DVD / EE / NI / GP / QR / HP / DES / FXC / CRYPTO / WATCH). Eager
     bundle delta verified at +56.25 kB raw / +9.43 kB gzipped vs the
     Step-5 baseline (within the +60 / +15 cap).
   * `src/api/OpenBbProxy.{hpp,cpp}` — libcurl-based HTTPS client with
     per-route LRU cache + mock-mode fallback. Built and deployed.
   * `src/api/OmegaApiServer.cpp` — 17 route builders (Step 5 + Step 6)
     forwarding to OpenBbProxy. The merged ones (FA / KEY / EE) stitch
     2-3 OpenBB calls into one composite envelope via the in-file
     `ob_*` JSON helpers.
   * `src/api/WatchScheduler.{hpp,cpp}` — cron-style nightly scan over
     S&P 500 + NDX, persisted to an in-process registry, surfaced via
     `/api/v1/omega/watch`.
   * `CMakeLists.txt` hot-fixes:
     - `if(NOT CMAKE_TOOLCHAIN_FILE)` (truthy check) so a cached-empty
       value doesn't short-circuit autodetect.
     - Manual probe of `<vcpkg>/installed/x64-windows/` if
       `find_package(CURL)` fails after the toolchain loads.
     - POST_BUILD `copy_directory` of vcpkg `installed/x64-windows/bin`
       next to `Omega.exe` so the runtime DLLs (libcurl, zlib, openssl,
       nghttp2, libssh2) ship alongside the binary.
   * Engine + service confirmed up on the VPS in shadow mode. 14
     trading engines registered, FIX feed live, ticks flowing.
* **Critical architectural finding closed at end of Step 6:**
  `https://api.openbb.co/` and `https://api.openbb.co/api/v1/<route>`
  return HTTP 404. **OpenBB does not host a public REST endpoint.**
  OpenBB themselves state: *"OpenBB does not host/serve market data —
  ODP uses provider connectors and your API keys."* The OpenBB
  Platform is a Python library that runs locally as a FastAPI sidecar;
  the OpenBB Hub (`my.openbb.co`) is a web app for managing your
  account and saved provider API keys, not a data endpoint. Our
  Step-5 architecture decision rejected the sidecar approach, so the
  current `OpenBbProxy` points at a URL that 404s. **Mock mode works
  because it never actually fetches.** Real OpenBB calls have never
  flowed through this engine and never will at the configured base URL.
* User decision at end of Step 6: **rip OpenBB out, replace with
  direct free-provider calls.** Engine stays on libcurl. No Python
  sidecar. No new vcpkg ports. The omega-terminal UI side does NOT
  change — envelope JSON shapes are preserved so the panels keep
  rendering unchanged.

Pending follow-ups outside the Step-7 critical path (carried forward
from Step 6):
* Register the remaining open-position sources (Tsmom / Donchian /
  EmaPullback / TrendRider / HBI).
* Differentiate the HBI four-pack's `tr.engine` by symbol.
* Engine-side `/cells` route + `CellSummaryRegistry`.
* Optional `/trade/<id>` route.
* MFE / MAE / signal context on `LedgerEntry`.
* `OMEGA_WATCHDOG -Branch` flag.
* The v3.5 npm/vite stderr-mangling fix in `QUICK_RESTART.ps1`.
* Recurring `0xC0000005 / 0xC0000374` engine crash pattern in the
  Windows Event Log (predates Step 6, ntdll.dll+0x103e89 heap
  corruption — needs PageHeap run, separate session).
* The Step-6 commit + CMakeLists.txt hot-fix may still be unpushed at
  the moment Step 7 opens; verify with `git log --oneline -5` first.
* Mac dev parity for `Omega.exe` (winsock2.h `#ifdef _WIN32`).
None of these block Step 7.

Prompt to paste in the next chat:

Read these files first to catch up:

1. `docs/SESSION_2026-05-01e_HANDOFF.md` — Step 6 → Step 7 boundary
   handoff. Contains the OpenBB removal decision plus all the Step-5 +
   Step-6 hot-fix recovery notes (vcpkg toolchain, runtime DLL deploy,
   service-tracking gotchas under NSSM). The "Step 7 carry-over"
   section at the bottom is the most important part.
2. `src/api/OpenBbProxy.{hpp,cpp}` — the existing libcurl client +
   per-route LRU cache + mock-mode glue. Step 7 either renames this to
   `MarketDataProxy` and rewrites the upstream URLs and per-route
   provider mapping, OR keeps the file name and just rewrites the
   internals. The cache + mock substrate are reusable as-is.
3. `src/api/OmegaApiServer.cpp` — the 17 route builders that produced
   Step 5 + Step 6's envelope shapes. Step 7 rewrites the `build_*_json`
   bodies to (a) call the new direct providers via the rewritten proxy
   and (b) reshape provider-specific JSON into our existing envelope
   shapes. The route URLs (`/api/v1/omega/<code>`) and the JSON
   envelope shapes do NOT change — so the UI side stays intact.
4. `src/api/WatchScheduler.cpp` — the cron-driven nightly scanner that
   currently calls `OpenBbProxy::instance().get("equity/price/quote",
   ...)`. Replace with the new proxy's quote method.
5. `omega-terminal/src/api/types.ts` — the wire envelope shapes. **Do
   not change these.** They define the contract the panels read.
6. `omega-terminal/src/api/omega.ts` — typed fetchers. **Do not change
   these either.** The Step-7 work is engine-side only.

Then start Omega Terminal step 7: replace OpenBB with direct free
providers.

## Step 7 scope

Engine-side only. Zero UI changes. The 17 routes and the envelope
shapes are preserved verbatim; only the source of the data changes.

### Route-by-route mapping (proposed; surface to user before coding)

| Code   | Engine route                | Step 6 source (broken)                         | Step 7 source (proposed)                            | Auth          |
|--------|-----------------------------|------------------------------------------------|-----------------------------------------------------|---------------|
| INTEL  | /intel                      | OpenBB /news/world                             | Yahoo Finance trending news (or RSS aggregator)     | none          |
| CURV   | /curv                       | OpenBB /fixedincome/government/treasury_rates  | FRED `api.stlouisfed.org/fred/series/observations`  | free API key  |
| WEI    | /wei                        | OpenBB /equity/price/quote                     | Yahoo Finance `query1.finance.yahoo.com/v7/finance/quote` | none |
| MOV    | /mov                        | OpenBB /equity/discovery/{active,gainers,losers} | Yahoo Finance trending tickers + sort logic       | none          |
| OMON   | /omon                       | OpenBB /derivatives/options/chains             | Yahoo Finance `query2.finance.yahoo.com/v7/finance/options/<sym>` | none |
| FA     | /fa                         | OpenBB /equity/fundamental/{income,balance,cash} | Yahoo Finance `quoteSummary` w/ financialsTemplate | none         |
| KEY    | /key                        | OpenBB /equity/fundamental/{key_metrics,multiples} | Yahoo Finance `quoteSummary` w/ summaryDetail / defaultKeyStatistics | none |
| DVD    | /dvd                        | OpenBB /equity/fundamental/dividends           | Yahoo Finance `chart` w/ events=div                 | none          |
| EE     | /ee                         | OpenBB /equity/estimates/{consensus,surprise}  | Yahoo Finance `quoteSummary` w/ earningsHistory + earningsTrend (best free option; coverage is partial) | none |
| NI     | /ni                         | OpenBB /news/company                           | Yahoo Finance `query1.finance.yahoo.com/v1/finance/search?q=<sym>` (returns news in payload) | none |
| GP     | /gp                         | OpenBB /equity/price/historical                | Yahoo Finance `chart` (`query1.../v8/finance/chart/<sym>`) | none    |
| QR     | /qr                         | OpenBB /equity/price/quote                     | Yahoo Finance `quote` (multi-symbol)                | none          |
| HP     | /hp                         | OpenBB /equity/price/historical                | Yahoo Finance `chart` (same as GP, shared cache slot) | none        |
| DES    | /des                        | OpenBB /equity/profile                         | Yahoo Finance `quoteSummary` w/ assetProfile        | none          |
| FXC    | /fxc                        | OpenBB /currency/price/quote                   | Yahoo Finance `quote` for `<pair>=X` symbol form    | none          |
| CRYPTO | /crypto                     | OpenBB /crypto/price/quote                     | Yahoo Finance `quote` for `<sym>-USD` symbol form   | none          |
| WATCH  | /watch                      | engine-side WatchScheduler (uses OpenBbProxy)  | engine-side WatchScheduler (uses MarketDataProxy)   | n/a           |

Yahoo Finance covers 16 of 17 routes with no API key. **CURV is the
only route that needs an API key** (FRED's free tier is generous —
120 reqs/min — and the key is one form-fill at
`fredaccount.stlouisfed.org/apikeys`). EE coverage is partial on
free Yahoo Finance and may need to fall back to mock for some symbols.

### Why Yahoo Finance specifically

* No account or API key required.
* Coverage spans equities, ETFs, indices, FX, crypto, options chains,
  fundamentals, news, and historical bars — i.e. nearly all the BB
  function suite.
* Stable JSON over HTTPS at well-known hosts (`query1.finance.yahoo.com`,
  `query2.finance.yahoo.com`). libcurl already handles this; no
  additional engine-side dependencies.
* Field set is rich enough to populate the existing envelope shapes
  with minor reshape glue.
* Caveat: it's an undocumented public-but-not-guaranteed API. Free
  providers come and go. Plan B for any individual route is to drop
  back to mock mode for that route specifically — the existing
  per-route `fetch_mock` block in OpenBbProxy is reusable verbatim.

### A. Engine-side, C++ (requires explicit user OK before any edit):

1. Rename / rewrite `src/api/OpenBbProxy.{hpp,cpp}` →
   `src/api/MarketDataProxy.{hpp,cpp}`. Keep the singleton,
   per-URL TTL LRU cache, mock substrate, and libcurl substrate.
   Replace the upstream URL builder (`kBaseUrl + route + query`) with
   a small per-method dispatch that knows how to talk to each free
   provider. The mock-mode env var becomes `OMEGA_MARKETDATA_MOCK`
   (with `OMEGA_OPENBB_MOCK` kept as a recognised alias for one
   release cycle so existing VPS service env-vars don't break).
2. Rewrite the 17 `build_*_json` builders in
   `src/api/OmegaApiServer.cpp` to:
   - Call the new proxy's per-source method (e.g.
     `MarketDataProxy::yahoo_quote(symbols)` or
     `MarketDataProxy::fred_series(series_id)`)
   - Reshape the provider-specific JSON into our existing envelope
     shapes via the same hand-rolled `ob_*` JSON helpers (rename to
     `md_*` if desired) we already have in this TU.
   - Preserve the `provider` field in the envelope so the UI's MOCK /
     LIVE badge still works (e.g. `provider: "yahoo"` or
     `provider: "fred"`).
3. `src/api/WatchScheduler.cpp` — replace the
   `OpenBbProxy::instance().get("equity/price/quote", ...)` call in
   `run_scan()` with the new proxy's batched-quote method. The
   constituent universes and the screener logic don't change.
4. `CMakeLists.txt` — rename the SOURCES line for the renamed proxy
   file. No new `find_package`. No new vcpkg ports. The Step-6 hot-fix
   for vcpkg toolchain detection + runtime DLL POST_BUILD copy stays
   intact and is exactly what a Yahoo-Finance-backed build needs (HTTPS
   to query1/query2 hosts uses the same libcurl + openssl set).
5. **OMON requires JSON parsing — same as Step 6.** Yahoo Finance's
   options-chain payload is even denser than OpenBB's. Per Step-6
   convention, parse client-side in OmonPanel; engine just shapes the
   chain into the OptionsRow envelope.
6. **GP / HP cache slot sharing** — preserved. Yahoo's `chart` call
   takes (`symbol`, `interval`, `range`) and we make the same call for
   both routes when the params match.
7. **WATCH backwards-compat** — `WatchScheduler::run_scan` keeps the
   same shape (iterate batches of 100 symbols, parse the array, apply
   the |Δ%| ≥ 5 % screen). Only the upstream changes.

### B. UI-side, inside `omega-terminal/`:

**Zero changes.** The 13 panels, `types.ts`, `omega.ts`, `PanelHost`,
the shared chrome module — all stay exactly as Step 6 left them. The
contract the engine exposes (`/api/v1/omega/<code>` returning the
declared envelope shape) is the boundary. Step 7 only changes what
sits behind that boundary.

### Constraints

* Branch: `omega-terminal`. Do not touch `main`, `src/gui/`, or any
  trading-engine code. The engine + FIX session must stay running
  through the Step-7 deploy with no behavioural change.
* Engine-side C++ edits require explicit user approval; surface a
  plan first; wait for OK; then edit. Same shape as Step 6.
* API contract field names mirror the JSON keys byte-identical to
  whichever side ships the wire layer — same rule as Step 6. Do NOT
  break existing field names (`results`, `provider`, `warnings`,
  `extra`, `income`, `balance`, `cash`, `key_metrics`, `multiples`,
  `consensus`, `surprise`, `hits`, `last_run_ts`, `next_run_ts`,
  `scanning`, `universe` are all on the wire today and the panels
  expect them).
* Backtest harness gate `-DOMEGA_BACKTEST` must still compile; new
  `MarketDataProxy.cpp` stays gated under `#ifndef OMEGA_BACKTEST`
  like its predecessor.
* AbortController unmount cleanup is non-negotiable on the UI side —
  `usePanelData` is unchanged.
* Bundle size budget unchanged — but Step 7 is engine-side only, so
  there should be no eager-bundle delta.
* Prefer one new `MarketDataProxy` method per upstream pattern over a
  proliferation of one-shot routes. Yahoo's `quoteSummary` covers
  several panels (FA / KEY / DES / DVD / EE) with different `modules=`
  query params; expose it as one proxy method that takes the modules
  list rather than five copies.
* The `provider` field's value is the master signal the UI uses for
  its top-right badge. Set it to the upstream source name
  (`yahoo` / `fred` / `mock`), NOT the literal `"openbb"` (which is
  no longer accurate). The `MOCK` rendering branch is unchanged.

### Verification gates

* Mock smoke: `OMEGA_MARKETDATA_MOCK=1` (or the legacy
  `OMEGA_OPENBB_MOCK=1`) — every Step-6 panel renders synthetic data
  with the amber `MOCK` badge. Same as Step 6 mock mode but routed
  through the new proxy.
* Live smoke: with no env var set, every Step-6 panel renders real
  data with `provider: yahoo` (or `fred` for CURV) badge. UI shows
  realistic numbers; the `[FIX-FALLBACK]` log lines from the trading
  engine are unaffected.
* `omega-terminal/`: `npm run typecheck` green, `npm run build` green,
  eager bundle delta ≈ 0 (no UI changes).
* C++ side: `cmake --build build` green on the VPS;
  `QUICK_RESTART.ps1 -Branch omega-terminal` deploys a healthy
  service. Configure log still shows `vcpkg: using ...` and
  `vcpkg DLLs: copy from ...` (Step-6 hot-fix lines).
* Curl-level: each `/api/v1/omega/<code>` returns the same envelope
  shape it returned in Step 6 — diff against a Step-6 mock-mode
  response and the only field that should differ is `provider`.
* WATCH: `/api/v1/omega/watch?universe=SP500` continues to surface
  `last_run_ts`, `next_run_ts`, `scanning`, `universe`, and a
  `hits` array. The first nightly scan after Step 7 deploys
  populates real hits via Yahoo Finance instead of mock data.
* Recurring engine crash pattern (separate from Step 7) — keep an eye
  on the Windows Event Log post-deploy. Step 7 should not introduce
  new crash modes; if it does, roll back via the previous-binary
  fallback in `QUICK_RESTART.ps1` and surface the trace.

### Decisions to surface to user before coding

1. **Rename `OpenBbProxy` → `MarketDataProxy`?** Recommended yes;
   the OpenBB name is no longer accurate. Clean break.
2. **News source for INTEL / NI** — Yahoo Finance's `search` endpoint
   includes news in its payload, so it's the path of least resistance.
   Alternative: drop INTEL/NI to permanent mock until a richer free
   news source is wired (out of scope for Step 7 if so).
3. **EE handling** — Yahoo Finance's `earningsHistory` +
   `earningsTrend` modules cover most of EE for major US tickers but
   coverage is patchy for small caps and non-US tickers. Surface
   patchy coverage as `warnings` in the envelope, or drop EE to
   permanent mock?
4. **FRED API key for CURV** — operator generates a free key at
   fredaccount.stlouisfed.org/apikeys and sets `OMEGA_FRED_KEY` at
   Machine scope, same shape as the existing `OMEGA_OPENBB_TOKEN`
   pattern. Confirm the operator is happy to do that one-time setup.
5. **Backwards-compat env-var alias** — keep `OMEGA_OPENBB_MOCK` as a
   recognised alias for one release cycle, or hard-cut to
   `OMEGA_MARKETDATA_MOCK` immediately?

### Don't forget

* The user's prefs require full files only (no diffs/snippets) when
  showing C++ or TS code.
* Warn at 70% chat usage and before any time-management block.
* Update the next-day handoff doc at end of session, mirroring the
  structure of `docs/SESSION_2026-05-01e_HANDOFF.md`.
* After Step 7 ends, the Step-8 opener goes in
  `docs/omega_terminal/STEP8_OPENER.md`. Step 8 is the original
  cutover: retire `src/gui/OmegaTelemetryServer`, point `:7779/:7780`
  at `:7781`, and delete (or archive) `src/gui/`. The omega-terminal
  UI becomes the production GUI.
* Step 7 is engine-only, but commit it on the same `omega-terminal`
  branch as Steps 5 + 6 so the whole BB function suite ships as one
  coherent feature train.
