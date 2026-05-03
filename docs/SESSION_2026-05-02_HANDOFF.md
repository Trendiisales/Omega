# Omega Session Handoff — 2026-05-02 (Omega Terminal Step 2)

**Final State:** Omega Terminal Step 2 complete and committed on branch
`omega-terminal` at `0ed1dec` (parent `6d7256a`). New
`OmegaApiServer` HTTP read-API on `127.0.0.1:7781`; new typed fetch
wrappers on the UI side; Vite dev proxy wired. Production VPS still
running its prior commit unchanged — Step 2 is deploy-pending until
the engine side rebuilds on Windows MSVC.

**Parent commit:** `6d7256a omega-terminal step 1: shell, command bar,
workspace tabs, 24-code router` (the Step 1 deliverable on the same
branch).

---

## What shipped this session

### Engine-side C++ (Omega.exe)

Pure additive change. **No engine trade behaviour change.** No
existing engine logic was modified; only registration metadata was
added so the new HTTP API can read engine state.

**New files**

| File | Lines | Purpose |
|---|---:|---|
| `src/api/OmegaApiServer.hpp` | 70 | HTTP/1.1 server class, mirrors `OmegaTelemetryServer.hpp` shape minus WebSocket |
| `src/api/OmegaApiServer.cpp` | 473 | Hand-rolled HTTP/1.1 accept loop; 4 GET routes; inline JSON serializers; query-string parser; ISO-8601→unix-ms helper. Whole TU gated `#ifndef OMEGA_BACKTEST` |
| `include/EngineRegistry.hpp` | 84 | `EngineSnapshot` struct + `EngineRegistry` class with mutex-guarded `register_engine`/`snapshot_all`. `extern omega::EngineRegistry g_engines;` for cross-TU linkage |

**Modified files** (additive only)

| File | Δ lines | What changed |
|---|---:|---|
| `include/globals.hpp` | +10 | Includes `EngineRegistry.hpp`; defines the single `omega::EngineRegistry g_engines;` instance at file scope |
| `include/engine_init.hpp` | +94 | At end of `init_engines()`: 14 `g_engines.register_engine(...)` calls — one each for HBG, HBI×4, MCE, CFE, Tsmom V1, Tsmom V2, Donchian, EmaPullback, TrendRider, RSI Reversal, RSI Extreme. Snapshot lambdas read only `.enabled` and `.shadow_mode` — no locks taken |
| `src/main.cpp` | +29 | Static-init auto-start of `g_omega_api_server.start(7781)` alongside the existing telemetry server. Gated `#ifndef OMEGA_BACKTEST`. Header banner updated |
| `CMakeLists.txt` | +1 | Adds `src/api/OmegaApiServer.cpp` to the `Omega` target SOURCES; reuses existing ws2_32/OpenSSL link deps |

**Routes served** (`127.0.0.1:7781`, GET only):

```
/api/v1/omega/engines    -> Engine[]      (live from g_engines)
/api/v1/omega/positions  -> Position[]    ([] until Step 3)
/api/v1/omega/ledger     -> LedgerEntry[] (from g_omegaLedger; from/to/engine/limit query filters)
/api/v1/omega/equity     -> EquityPoint[] ([] until Step 3)
```

Anything else returns `404 {"error":"not found", ...}`. Non-GET
methods return `405`.

### UI-side TypeScript (omega-terminal/)

| File | Lines | Purpose |
|---|---:|---|
| `omega-terminal/src/api/types.ts` | 226 | TS interfaces `Engine`, `Position`, `LedgerEntry`, `EquityPoint`, `LedgerQuery`, `EquityQuery`; `OmegaApiError` class with `status`/`url`/`body`/`aborted` fields. Field names byte-identical to the JSON keys |
| `omega-terminal/src/api/omega.ts` | 264 | Typed fetch wrappers `getEngines`, `getPositions`, `getLedger`, `getEquity`. `AbortController` with 5 s default timeout (overridable). HTTP non-2xx, network failures, JSON parse failures and aborts all throw typed `OmegaApiError` |
| `omega-terminal/vite.config.ts` | 26 → 60 | `server.proxy['/api/v1/omega']` → `http://127.0.0.1:7781`, `changeOrigin: true` |

### Docs

| File | Status |
|---|---|
| `docs/SESSION_2026-05-01_HANDOFF.md` | Reconstructed from README + STEP2_OPENER + 04-30c handoff (was missing despite both files referencing it) |
| `docs/SESSION_2026-05-02_HANDOFF.md` | **This file** |

---

## Why this is structured this way

The Step 2 cut intentionally lands **only** the wire. No panel changes,
no per-engine accessors beyond the cheap-read `.enabled`/`.shadow_mode`
fields that already exist. The reasons:

- The function-code surface is locked from Step 1; Step 3 is the first
  step that lights up panels, and at that point we will need richer
  per-engine accessors (for `last_signal_ts` and `last_pnl`). Adding
  them now without a consumer would mean designing the contract twice.
- Splitting the registry types into `include/EngineRegistry.hpp` was
  necessary so `OmegaApiServer.cpp` can compile as an independent
  translation unit. `globals.hpp` is a single-TU include that drags in
  the entire engine universe (SymbolEngines, GoldEngineStack, the
  cross-asset stack, etc.) and depends on prior includes in main.cpp.
  Keeping the registry types isolated lets the API server's TU compile
  in seconds against just `<functional>`/`<mutex>`/etc.
- `OmegaApiServer` runs on `:7781` instead of the original
  STEP2_OPENER `:7779` because the existing `OmegaTelemetryServer`
  already binds `:7779` (HTTP) + `:7780` (WebSocket). Both servers run
  in parallel until the Step 7 cutover.

---

## Diff totals

```
 CMakeLists.txt                     |   +1
 docs/SESSION_2026-05-01_HANDOFF.md | +306  (new)
 docs/SESSION_2026-05-02_HANDOFF.md |  new  (this file)
 include/EngineRegistry.hpp         |  +84  (new)
 include/engine_init.hpp            |  +94
 include/globals.hpp                |  +10
 omega-terminal/src/api/omega.ts    | +264  (new)
 omega-terminal/src/api/types.ts    | +226  (new)
 omega-terminal/vite.config.ts      |  +37 / -4
 src/api/OmegaApiServer.cpp         | +473  (new)
 src/api/OmegaApiServer.hpp         |  +70  (new)
 src/main.cpp                       |  +29
```

11 files in commit `0ed1dec`. 1590 insertions, 4 deletions.

---

## Verification at this point

Mac sandbox gates run during this session — all passed:

- `npm run typecheck` (`omega-terminal/`): clean exit.
- `npm run build` (`omega-terminal/`): 38 modules transformed; JS 161 kB
  (was 156 kB at Step 1; 5 kB growth = the new `api/` files); CSS
  12 kB unchanged; build time 923 ms.
- `g++ -std=c++20 -fsyntax-only -Wall -Wextra -Wpedantic -I include -I
  src` on `src/api/OmegaApiServer.{hpp,cpp}`: clean (no warnings, no
  errors).
- Real link test against a minimal main.cpp-mimic TU
  (`g++ ... main_mimic.cpp src/api/OmegaApiServer.cpp -o smoke`):
  clean compile + link. `nm` confirms `omega::OmegaApiServer::{ctor,
  dtor, start, stop, run}` symbols emitted; `g_engines` defined
  exactly once.
- Runtime smoke on `127.0.0.1:17781` with two registered fakes:
  - `GET /api/v1/omega/engines` returns
    `[{"name":"EngineA","enabled":true,"mode":"SHADOW","state":"RUNNING","last_signal_ts":1714579200000,"last_pnl":12.5},{"name":"EngineB","enabled":false,"mode":"LIVE","state":"IDLE","last_signal_ts":0,"last_pnl":0}]`.
    Field names match `omega-terminal/src/api/types.ts` byte-for-byte.
  - `/positions` → `[]`, `/ledger?limit=5` → `[]` (stubs), `/nope` →
    `404`.
- MSVC build verification: **NOT RUN** — the Mac sandbox cannot
  replicate the toolchain. Must run on the Windows VPS via
  `QUICK_RESTART.ps1` before this commit can be considered
  fully-validated.

---

## Verification still required (user action)

1. **Push branch to origin:** `git push origin omega-terminal` (the
   branch is local-only at session end; remote is unaware of this
   commit).
2. **Windows MSVC build on VPS:** `QUICK_RESTART.ps1` — must compile
   clean. The g++ syntax check on Mac is necessary but not sufficient.
3. **Step 2 exit gate (live):** with both `Omega.exe` and `vite dev`
   running, `curl http://127.0.0.1:5173/api/v1/omega/engines` from
   the host returns the live 14-engine list, proxied through Vite at
   :5173 to OmegaApiServer at :7781.
4. **Smoke-confirm OmegaTelemetryServer is unaffected:** the existing
   GUI on :7779 should still work normally — Step 2 added a sibling
   server, did not replace one.

---

## What Step 2 explicitly does NOT do

- Does not wire any panel to the new endpoints. CC, ENG, POS still
  show `ComingSoonPanel`. Wiring is Step 3.
- Does not add `last_signal_ts` or `last_pnl` accessors to any engine.
  Both fields are returned as `0` for every engine in this step. The
  per-engine accessors land with Step 3 alongside the panel wiring.
- Does not implement `positions` or `equity` route bodies. Both
  return `[]`. Step 3 wires them as part of CC/POS.
- Does not retire `OmegaTelemetryServer`. The legacy GUI server stays
  on `:7779`/`:7780` until the Step 7 cutover.
- Does not touch `main`, does not touch `src/gui/`.

If runtime trade behaviour shifts in the next 24h, **it is not
Step 2** — Step 2 is read-only against existing globals.

---

## Deferred / not done this session

Carrying forward:

| Item | Status |
|---|---|
| VPS deploy `161d992` (RSI disables) | Still pending — user action on VPS. |
| HBG S52 A/B verdict | Still in flight — Mac backtest. |
| Phase 2a 5-day ledger window (CellEngine V1 vs V2) | Still in flight — Day 5 ≈ 2026-05-06. |
| Omega Terminal Step 2 | **Done this session** (commit `0ed1dec`). |
| Per-engine `last_signal_ts` / `last_pnl` accessors | Pending — Step 3 work. |
| OmegaApiServer `/positions` and `/equity` endpoints | Pending — Step 3 work. |
| MSVC build verification of `0ed1dec` | Pending — VPS step. |

---

## Open / known issues flagged

### One scope deviation worth recording
STEP2_OPENER's file list was 6 files (the new `OmegaApiServer.{hpp,cpp}`
+ 4 modifications). Step 2 actually shipped 7 — the extra one is
`include/EngineRegistry.hpp`. Reason: extracting the registry types
out of `globals.hpp` was the only way to keep `OmegaApiServer.cpp` a
truly independent translation unit. This is a refactor for TU
isolation, not a behaviour change.

### `OmegaTelemetryServer` is not retired
The legacy server keeps running on `:7779` + `:7780`. The Step 7
cutover is when it retires; until then both servers coexist.

### Auto-start static-init pattern
`g_omega_api_server` starts via a namespace-anonymous static-init
struct in `src/main.cpp`. This means the accept loop is up before
`init_engines()` registers any engines — pre-registration `GET
/engines` requests return `[]`. The window is ~milliseconds and the
behaviour is correct (the registry simply hasn't been populated
yet); flagged for awareness rather than concern.

---

## Watch in next session

- Branch `omega-terminal` advances cleanly on top of `0ed1dec` with
  the Step 3 deliverable. No fast-forward of `main` until Step 7.
- `src/gui/` remains untouched. Anything modified there during Step 3
  is a constraint violation.
- The Step 3 verification gate is: CC, ENG, POS panels render live
  data fetched through `omega-terminal/src/api/omega.ts` from
  `OmegaApiServer` at `:7781`, proxied via Vite at `:5173`.

---

## Next session agenda (queued)

Order matters — VPS rebuild first to validate Step 2 before Step 3
extends it.

1. **Resolve VPS rebuild for `0ed1dec`** (mandatory before Step 3 lands).
2. **Step 3: wire CC, ENG, POS panels** to the Step 2 endpoints. Add
   per-engine `last_signal_ts` / `last_pnl` accessors on the C++ side
   (engine-side approval still required). Implement real
   `/positions` route. Add the CLI args parser (`CC EURUSD`, `POS
   HBG`) into the omega-terminal router.
3. Step 4 (LDG / TRADE / CELL panels) is the session after Step 3.

The Step 3 opener template is `docs/omega_terminal/STEP3_OPENER.md`
(landed this session). Paste the section below the horizontal rule
in that file into the next chat to begin Step 3.

---

## How this session built on Step 1

Step 1 (commit `6d7256a`) shipped the React/Vite shell + 24-code
router. Step 2 wires the read API that Step 3's panels will consume.
The two are independent: Step 1 has no engine dependency, Step 2 has
no UI panel dependency, and the `omega-terminal/src/api/` layer is the
only contract surface between them.

Either commit can roll back without affecting the other:

- Roll back Step 1 only: requires also rolling back Step 2 (Step 2
  modified files Step 1 introduced like `vite.config.ts`).
- Roll back Step 2 only: clean — Step 2's engine-side files are all
  new or strictly additive. Reverting `0ed1dec` removes
  `OmegaApiServer`, the `g_engines` registry, the registration calls,
  the proxy entry, and the `api/` TS files. `omega-terminal/` Step 1
  features (HOME/HELP/router/tabs) still work — they don't depend on
  `api/`.
