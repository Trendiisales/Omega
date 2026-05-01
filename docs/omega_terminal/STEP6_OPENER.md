# Omega Terminal — Step 6 Opener (BB function suite)

Paste the section below into a fresh chat to begin Step 6. Everything
above the horizontal rule is context for you (the human reader);
everything below is the prompt to copy.

State at the boundary of step 5 → step 6:

* Branch: `omega-terminal`. HEAD = `<step-5-commit>`. Step 4 commit
  `3baa20f`, Step 3 commit `a1287ce`, Step 2 commit `0ed1dec`, and Step
  1 commit `6d7256a` are still in the ancestry. `main` is unchanged at
  `161d992`.
* `src/gui/` is untouched and stays untouched until the Step 7 cutover.
* Step 5 deliverables shipped:
   * `src/api/OpenBbProxy.{hpp,cpp}` — libcurl-based HTTPS client for
     OpenBB Hub. Reads `OMEGA_OPENBB_TOKEN` from env. Per-route TTL
     LRU cache. `OMEGA_OPENBB_MOCK=1` short-circuits to canned
     synthetic JSON for local dev.
   * `OmegaApiServer.cpp` gained four routes:
     `/api/v1/omega/{intel,curv,wei,mov}`. Each wraps an OpenBB Hub
     call and returns the OBBject envelope verbatim — no JSON parsing
     in C++.
   * `CMakeLists.txt` — `find_package(CURL REQUIRED)` + linkage on the
     `Omega` target only. Backtest targets stay free of libcurl.
   * `omega-terminal/src/panels/{IntelPanel,CurvPanel,WeiPanel,MovPanel}.tsx`
     — pattern-matched off CCPanel (multi-card, INTEL) and LdgPanel
     (sortable table, WEI/MOV); CURV is the first chart panel and
     uses Recharts via `React.lazy` so the chart bundle is split out.
   * `omega-terminal/src/api/{types,omega}.ts` — `OpenBbEnvelope<T>`
     generic + four row types + four `getX` typed wrappers.
   * `omega-terminal/src/panels/PanelHost.tsx` — INTEL/WEI/MOV imported
     eagerly; CURV via `React.lazy` + `<Suspense>` to keep the eager
     bundle under the +60 kB raw / +15 kB gzipped Step-5 budget.
   * Eager-bundle delta: +21.84 kB raw / +4.43 kB gzipped vs Step 4.
     CurvPanel chunk (~389 kB raw / ~107 kB gzipped) loads only on
     first navigation to CURV.
* Pending follow-ups outside the Step-6 critical path: register the
  remaining open-position sources (Tsmom / Donchian / EmaPullback /
  TrendRider / HBI), differentiate the HBI four-pack's `tr.engine` by
  symbol, the engine-side `/cells` route + `CellSummaryRegistry`,
  optional `/trade/<id>` route, MFE / MAE / signal context on
  `LedgerEntry`, OMEGA_WATCHDOG `-Branch` flag, the v3.5 npm/vite
  stderr-mangling fix in `QUICK_RESTART.ps1`, EU/JP curves on CURV,
  INTEL screen-id branching, generic `/api/v1/omega/openbb/<route>`
  passthrough, provider-override UX, OpenBB rate-limit surfacing.
  None block Step 6.

Prompt to paste in the next chat:

Read these files first to catch up:

1. `docs/SESSION_2026-05-01d_HANDOFF.md` — Step 5 → Step 6 boundary
   handoff, commit `<step-5-commit>`.
2. `omega-terminal/src/panels/{IntelPanel,CurvPanel,WeiPanel,MovPanel}.tsx`
   and `omega-terminal/src/api/{types,omega}.ts` — Step-5 panel
   patterns; Step-6 panels should pattern-match (header / sortable
   table or multi-card or chart / `usePanelData` polling / red retry
   banner / args parser overload / provider-or-MOCK badge in the
   top-right).
3. `omega-terminal/src/router/functionCodes.ts` — `OMON`, `FA`, `KEY`,
   `DVD`, `EE`, `NI`, `GP`, `QR`, `HP`, `DES`, `FXC`, `CRYPTO`, `WATCH`
   are already in `PANEL_REGISTRY` at `step: 6`. No router additions
   needed; the panels just need to be wired in `PanelHost.tsx`.
4. `src/api/OpenBbProxy.{hpp,cpp}` — the existing libcurl client + LRU
   cache. New Step-6 endpoints just call `OpenBbProxy::get(route,
   query, ttl_ms)`; the engine-side work is one new builder per panel
   in `OmegaApiServer.cpp`.
5. `src/api/OmegaApiServer.cpp` — the existing `build_intel_json /
   build_curv_json / build_wei_json / build_mov_json` block is the
   exact pattern to copy for the 13 new Step-6 routes.

Then start Omega Terminal step 6: light up the BB function suite.

## Step 6 scope

13 new panels, all OpenBB-backed via the existing `OpenBbProxy`. Most
are trivial sortable tables or multi-card layouts; a few (OMON, GP,
HP, WATCH) are richer.

### Panel-by-panel proposed mapping

The proposed OpenBB endpoints below are starting points; surface them
to the user before coding so they can override any that don't suit.

| Panel  | Args                | Proposed OpenBB route                            | Layout                    | Cadence |
|--------|---------------------|--------------------------------------------------|---------------------------|---------|
| OMON   | `<symbol> [<expiry>]` | `/derivatives/options/chains?symbol=<sym>`       | Chain table + IV summary  | 5 s     |
| FA     | `<symbol>`          | `/equity/fundamental/{income,balance,cash}`      | 3-tab table               | 5 min   |
| KEY    | `<symbol>`          | `/equity/fundamental/{key_metrics,multiples}`    | Multi-card                | 5 min   |
| DVD    | `<symbol>`          | `/equity/fundamental/dividends`                  | Sortable table            | 5 min   |
| EE     | `<symbol>`          | `/equity/estimates/{consensus,surprise}`         | Multi-card + table        | 5 min   |
| NI     | `<symbol>`          | `/news/company?symbol=<sym>`                     | Article feed (like INTEL) | 30 s    |
| GP     | `<symbol> [<intvl>]` | `/equity/price/historical?symbol=<sym>&interval=<i>` | Recharts <LineChart> | 30 s    |
| QR     | `<symbol-list>`     | `/equity/price/quote?symbol=<list>`              | Sortable table (like WEI) | 5 s     |
| HP     | `<symbol> [<intvl>]` | `/equity/price/historical?symbol=<sym>&interval=<i>` | Sortable OHLCV table | 30 s    |
| DES    | `<symbol>`          | `/equity/profile?symbol=<sym>`                   | Single-symbol detail card | 5 min   |
| FXC    | `<base>/<quote>` or `<region>` | `/currency/price/quote?symbol=<pair>`  | Sortable table            | 5 s     |
| CRYPTO | `<symbol>`          | `/crypto/price/quote?symbol=<sym>`               | Sortable table            | 5 s     |
| WATCH  | `<universe>`        | n/a — engine-side scheduler (see below)          | Sortable table            | 60 s    |

### A. UI-side, inside `omega-terminal/`:

1. Replace `ComingSoonPanel` for the 13 step-6 codes in
   `src/panels/PanelHost.tsx` with the new panels. Pattern-match off
   the four Step-5 panels for layout choices (multi-card vs sortable
   table vs Recharts vs feed). All chart panels (GP, plus any future
   chart) MUST be lazy-loaded via `React.lazy` + `<Suspense>` so the
   Recharts dependency stays in the on-demand chunk it already lives
   in for CURV.
2. Args overload per panel mirrors the table above.
3. New TS types in `omega-terminal/src/api/types.ts`:
   `OptionsChain`, `OptionsRow`, `IncomeStatement`,
   `BalanceSheet`, `CashFlow`, `KeyMetrics`, `Dividend`,
   `EpsEstimate`, `EpsSurprise`, `HistoricalBar`, `CompanyProfile`,
   `FxQuote`, `CryptoQuote`, `WatchHit`. Plus matching `XQuery` types
   for any panel that takes args.
4. New typed fetchers in `omega-terminal/src/api/omega.ts`:
   `getOmon`, `getFa`, `getKey`, `getDvd`, `getEe`, `getNi`, `getGp`,
   `getQr`, `getHp`, `getDes`, `getFxc`, `getCrypto`, `getWatch`.
   Default timeout stays at 30 s for OpenBB-backed routes.
5. Reuse `usePanelData` exactly as in Step 5. Per-panel cadences
   listed in the table above.

### B. Engine-side, C++ (requires explicit user OK before any edit):

1. 13 new builder functions in `OmegaApiServer.cpp`, one per panel,
   each taking `(query map, status&)` and returning a `std::string`
   body. Each calls `OpenBbProxy::instance().get(route, query, ttl_ms)`
   and either passes the body through verbatim (most cases) or
   composes multiple OpenBB calls into one merged envelope (FA needs
   income + balance + cash; KEY needs key_metrics + multiples; EE
   needs consensus + surprise).
2. 13 new dispatch entries in the `OmegaApiServer::run` else-if chain.
3. Per-route cache TTLs sized just below each panel's poll cadence:
   OMON 4 s, QR 4 s, FXC 4 s, CRYPTO 4 s, NI 25 s, GP 25 s, HP 25 s,
   FA 250 s, KEY 250 s, DVD 250 s, EE 250 s, DES 250 s, WATCH 50 s.
4. `OpenBbProxy` itself does not need changes. Cache eviction at 256
   entries should comfortably accommodate ~200 active query
   permutations even with multiple tabs open.
5. **WATCH is special.** It is described as "INTEL rules run nightly
   across S&P 500 + NDX components" — this implies an engine-side
   scheduler, not a single OpenBB call. Surface a design plan first
   before coding. Two reasonable shapes:
   - (a) Cron-style nightly job inside `Omega.exe` that iterates
     constituents, runs the INTEL screener for each, persists hits to
     a `g_watch_hits` registry mirrored after `OpenPositionRegistry`,
     and the `/watch` route just snapshots that registry.
   - (b) Lazy on-demand: `/watch` runs the screener serially the
     first time it's called each day, caches results for 24 h. Slower
     first hit; no scheduler infrastructure.
   Pick before coding the WATCH panel.
6. **OMON requires real JSON parsing.** Options chains return
   hundreds of rows with strike / expiry / bid / ask / IV / OI / etc.
   The chain-summary header (ATM IV, IV term structure, put/call
   ratio) needs to read individual rows, which means parsing the
   OpenBB OBBject in C++ or doing it client-side. Recommend
   client-side: pass the whole envelope through and let the OMON
   panel compute the summary from `.results`. Keeps the engine-side
   "no third-party JSON libs" stance intact.
7. **GP and HP share** the underlying `/equity/price/historical`
   route; if both panels are open simultaneously they should share a
   cache hit. Ensure the cache key (URL including all query params)
   actually collides — same URL + same query → same cache slot.
   Audit: GP should pass `chart=true` while HP passes nothing else
   different, so the URLs already differ; pick a query param schema
   that lets them share when the user wants the same data in both
   views, OR explicitly accept that they don't and document it.

### Constraints

* Branch: `omega-terminal`. Do not touch `main` or `src/gui/`.
* Engine-side C++ edits require explicit user approval; surface a
  plan first (especially for WATCH); wait for OK; then edit.
* API contract field names mirror the JSON keys byte-identical to
  whichever side ships the wire layer — same rule as the engine API.
* Backtest harness gate `-DOMEGA_BACKTEST` must still compile; new
  C++ files that talk to the API server stay gated under
  `#ifndef OMEGA_BACKTEST` like `OmegaApiServer.cpp` and
  `OpenBbProxy.cpp`.
* AbortController unmount cleanup is non-negotiable — continue using
  `usePanelData`.
* Bundle size: the existing 222.84 kB raw / 62.43 kB gzipped eager
  baseline at Step 5 is the new budget marker. Step-6 budget: same
  +60 kB raw / +15 kB gzipped cap on the eager bundle. Lazy chunks
  are unbudgeted (CurvPanel pattern). Any new chart panel (GP) MUST
  be lazy-loaded — eager-importing GP would re-introduce the Step-5
  Recharts cost into the eager bundle.
* Prefer composing existing endpoints over adding new ones. FA/KEY/EE
  each need 2-3 OpenBB calls merged — do them server-side so the UI
  sees one envelope per panel.

### Verification gates

* `omega-terminal/`: `npm run typecheck` green; `npm run build` green
  (with code-split).
* `npm run dev`: with `Omega.exe` running, every Step-6 panel
  renders real OpenBB data (or mock data when `OMEGA_OPENBB_MOCK=1`).
  `Ctrl+W` closes a panel and the polling stops cleanly (verify no
  requests after unmount in DevTools network tab).
* C++ side: `cmake --build build` green on Mac and Windows;
  `QUICK_RESTART.ps1` smoke test still passes; `OmegaTelemetryServer`
  endpoints still unchanged.
* Curl-level: each `/api/v1/omega/<step6-route>` returns the expected
  OBBject envelope end-to-end.
* Bundle: eager total size growth ≤ +60 kB raw / +15 kB gzipped vs
  Step 5 baseline (222.84 kB / 62.43 kB).

### Don't forget

* The user's prefs require full files only (no diffs/snippets) when
  showing C++ or TS code.
* Warn at 70% chat usage and before any time-management block.
* Update the next-day `docs/SESSION_<YYYY-MM-DD>_HANDOFF.md` (or
  `_<letter>_HANDOFF.md` to chain off today) at end of session,
  mirroring the structure of `docs/SESSION_2026-05-01d_HANDOFF.md`.
* After Step 6 ends, the Step-7 opener goes in
  `docs/omega_terminal/STEP7_OPENER.md`. Step 7 is the cutover:
  retire `src/gui/OmegaTelemetryServer`, point `:7779/:7780` at
  `:7781`, and delete (or archive) `src/gui/`. The omega-terminal UI
  becomes the production GUI.
