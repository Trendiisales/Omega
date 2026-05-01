# Omega Terminal — Step 2 Opener

**Paste the section below into a fresh chat to begin Step 2.** Everything
above the horizontal rule is context for you (the human reader); everything
below is the prompt to copy.

State at the boundary of step 1 → step 2:

- Branch: `omega-terminal`. Forked from `68e2a07`. Currently has uncommitted
  step-1 work in `omega-terminal/` and `docs/omega_terminal/` (this file).
- Local `main` has been fast-forwarded to `161d992` (RSI disables + handoff).
- `src/gui/` is untouched and stays untouched until the step-7 cutover.
- Step 1 deliverable: React/Vite shell with 24 registered function codes;
  HOME and HELP live; everything else routes to `ComingSoonPanel`. Builds
  clean (`tsc -b` green; `vite build` produces a 156 KB JS / 12 KB CSS
  bundle on first run after `rm -rf dist`).
- Mac shell hardened: `scripts/setup-mac-shell.sh` adds
  `setopt interactive_comments`, `git_unlock`/`gunlock`/`git-safe`. Run once
  with `bash scripts/setup-mac-shell.sh && source ~/.zshrc`.
- Pending engine-side work outside the GUI critical path: VPS pull/restart
  for `161d992`, HBG S52 A/B verdict, Phase 2a 5-day shadow window. None
  block step 2.

---

## Prompt to paste in the next chat

Read these files first to catch up:

1. `docs/SESSION_2026-05-01_HANDOFF.md` — canonical session handoff and
   7-step build plan.
2. `omega-terminal/README.md` — step-1 deliverable spec, file layout, full
   24-code function-code surface.
3. `omega-terminal/src/router/functionCodes.ts` — `PANEL_REGISTRY` is the
   single source of truth for code → panel routing.
4. `docs/omega_terminal/STEP2_OPENER.md` — this file.

Then start **Omega Terminal step 2: Omega C++ JSON endpoints + UI fetch
layer + Vite proxy.**

### Step 2 scope (from HANDOFF, expanded)

**A. Engine-side, C++ (requires explicit user OK before any edits — the user
has a standing rule "never modify core code unless instructed clearly"):**

1. **New file `src/api/OmegaApiServer.{hpp,cpp}`** — minimal HTTP/1.1
   server bound to `127.0.0.1:7779`. No external deps; same lib choice the
   existing `src/gui/OmegaTelemetryServer` uses (asio standalone, or
   whatever is already linked). Routes:
   - `GET /api/v1/omega/engines` → JSON array of registered engines with
     `name`, `enabled`, `mode` (LIVE/SHADOW), `state` (RUNNING/IDLE/ERR),
     `last_signal_ts`, `last_pnl`.
   - `GET /api/v1/omega/positions` → JSON array of open positions with
     `symbol`, `side`, `size`, `entry`, `current`, `unrealized_pnl`, `mfe`,
     `mae`, `engine`.
   - `GET /api/v1/omega/ledger?from=<iso>&to=<iso>&engine=<name>&limit=<n>`
     → JSON array of closed trades with `id`, `engine`, `symbol`, `side`,
     `entry_ts`, `exit_ts`, `entry`, `exit`, `pnl`, `reason`.
   - `GET /api/v1/omega/equity?from=<iso>&to=<iso>&interval=<1m|1h|1d>`
     → JSON time-series of equity points.

2. **Modify `include/globals.hpp`** — add a thread-safe registry:

   ```cpp
   struct EngineSnapshot {
       std::string name;
       bool enabled;
       std::string mode;          // "LIVE" | "SHADOW"
       std::string state;         // "RUNNING" | "IDLE" | "ERR"
       int64_t last_signal_ts;    // unix ms; 0 = never
       double last_pnl;
   };

   class EngineRegistry {
   public:
       void register_engine(std::string name, std::function<EngineSnapshot()> snapshot);
       std::vector<EngineSnapshot> snapshot_all() const;
   private:
       mutable std::mutex mu_;
       std::vector<std::pair<std::string, std::function<EngineSnapshot()>>> engines_;
   };
   inline EngineRegistry g_engines;
   ```

3. **Modify each engine init site** (HBI, HBG, MCE, CFE, Tsmom V1+V2,
   Donchian, EmaPullback, TrendRider, RSI variants) — add a one-line
   `g_engines.register_engine("HybridGold", []{...})` at engine
   construction, returning the engine's current snapshot. The lambda
   reads from existing per-engine globals; no new state is added.

4. **Modify `src/main.cpp`** — instantiate and start `OmegaApiServer` on a
   dedicated thread alongside the existing `OmegaTelemetryServer`. Both run
   in parallel until step 7's cutover.

5. **Modify `CMakeLists.txt`** — add the new translation unit; link the
   same HTTP/JSON deps as `src/gui/OmegaTelemetryServer`.

**B. UI-side, inside `omega-terminal/`:**

1. **Update `vite.config.ts`** — add proxy for `/api/v1/omega/*` to
   `http://127.0.0.1:7779`. Keep `host: '127.0.0.1'`, `strictPort: true`.
2. **New file `src/api/omega.ts`** — typed fetch wrappers (`getEngines`,
   `getPositions`, `getLedger`, `getEquity`). Use AbortController for
   cancellation; 5s timeout default; surface HTTP errors as typed
   `OmegaApiError`.
3. **New file `src/api/types.ts`** — TypeScript interfaces mirroring the
   JSON shapes (`Engine`, `Position`, `LedgerEntry`, `EquityPoint`). Keep
   field names byte-identical to the JSON keys.
4. **No panel changes yet** — wiring CC, ENG, POS to these endpoints is
   step 3. Step 2's exit criterion is: `curl
   http://127.0.0.1:5173/api/v1/omega/engines` from the host returns the
   live engine list, proxied through Vite.

### Constraints

- Branch: `omega-terminal`. Do not touch `main`. Do not touch `src/gui/`
  (the existing C++ GUI stays live until step 7's cutover).
- Engine-side C++ edits require explicit user approval before any file is
  modified. Surface a plan first; wait for OK; then edit.
- The new `OmegaApiServer` must coexist with the existing
  `OmegaTelemetryServer`. Different port. Different routes. No shared
  state beyond `g_engines` and the existing position/ledger globals.
- Use `kShadowDefault = (g_cfg.mode != "LIVE")` semantics for the engine
  `mode` field — read it, do not redefine.
- Backtest harness gate `-DOMEGA_BACKTEST` must still compile clean — the
  `OmegaApiServer` should be excluded from backtest builds (gate with
  `#ifndef OMEGA_BACKTEST`).

### Verification gates

- `omega-terminal/`: `npm run typecheck` green; `npm run build` green;
  `curl http://127.0.0.1:5173/api/v1/omega/engines` returns the engine
  list when both Omega.exe and `vite dev` are running.
- C++ side: `cmake --build build` green on Mac and Windows; existing
  smoke-test in `QUICK_RESTART.ps1` still passes; `OmegaTelemetryServer`
  endpoints unchanged.
- No new GUI panels appear yet — step 3 lights them up.

### Don't forget

- The user prefs require **full files only** (no diffs/snippets) when
  showing C++ or TS code.
- Warn at 70% chat usage and before any time-management block.
- Update `docs/SESSION_2026-05-02_HANDOFF.md` (or the next-day filename)
  at end of session, mirroring the structure of the 2026-05-01 handoff.
- After step 2 ends, the step-3 opener goes in
  `docs/omega_terminal/STEP3_OPENER.md`.
