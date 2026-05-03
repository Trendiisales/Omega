# SESSION 2026-05-01g HANDOFF

## TL;DR

Step 7 GUI is functionally complete and deployed at commit `17933cd` on `origin/omega-terminal`. Three frontend polish edits + one C++ one-line fix are written in the working tree but NOT yet committed. The session ended on an open trading diagnosis: only one trade has fired all day in SHADOW mode (an `AsianRange SL` from 07:00:34 UTC, persisted from a prior session). The user wants HybridGold-style small scalp trades and is seeing none; root cause not yet pinned.

## State at end of session

### Committed + deployed on `origin/omega-terminal`

- `49ab109` Step 7b/7c — OmegaApiServer.cpp rewrite onto MarketDataProxy; OpenBb compat shim deleted (prior session)
- `95353b2` Step 7a follow-up — MarketDataProxy.{hpp,cpp} + CMakeLists SOURCES swap + WatchScheduler reroute (prior session, recovery)
- `15dddaf` Step 7 GUI — per-tab back/forward history (Alt+←/→ + Cmd/Ctrl+[/]) + live engine-link pill (replaces hardcoded "pending")
- `17933cd` Step 7 GUI completion — `/cells` + `/trade/<id>` engine routes + CellPanel + TradePanel rewrites

VPS last deployed at 09:42:52 UTC, banner `COMMIT : 17933cd`, service Running.

### Written locally on Mac, NOT committed

```
M omega-terminal/src/App.tsx             # persistent back bar between tabs and panel
M omega-terminal/src/panels/HomePanel.tsx # live status strip + un-dim all tiles + "Yahoo + FRED" subtitle
M src/api/MarketDataProxy.cpp             # Yahoo screener count 25 -> 100 (1-line bump)
```

All three pass typecheck + Vite build (293.76 kB main / 75.50 kB gz) + g++ -fsyntax-only -std=c++20 -Wall -Wextra (0/0).

Throwaway dist directories on disk (sandbox can't unlink, host UID needs to remove):

```
omega-terminal/dist-step7-back/
omega-terminal/dist-step7-home/
omega-terminal/dist-step7-final/   (carry-over from earlier in session)
omega-terminal/dist-step7-verify/  (carry-over)
omega-terminal/dist-verify/        (carry-over from prior session)
```

### Recovery — first thing next session

```bash
cd ~/omega_repo

# Clear sandbox-left lock if present
rm -f .git/index.lock

# Confirm only the 3 expected modifications appear
git status

# Stage + commit + push
git add omega-terminal/src/App.tsx omega-terminal/src/panels/HomePanel.tsx src/api/MarketDataProxy.cpp
git status
git commit -m "Step 7 GUI: persistent back bar + Home live strip + Yahoo screener count 25 to 100"
git push origin omega-terminal

# Sandbox-locked verify-build artifacts; sudo because sandbox UID owns them
sudo rm -rf omega-terminal/dist-step7-back omega-terminal/dist-step7-home omega-terminal/dist-step7-final omega-terminal/dist-step7-verify omega-terminal/dist-verify
```

VPS:

```powershell
cd C:\Omega
.\QUICK_RESTART.ps1 -Branch omega-terminal
```

## What landed in the GUI this session

### `15dddaf` Step 7 GUI (back/forward + engine pill)

- `omega-terminal/src/types.ts` — Workspace gained `history: HistoryEntry[]` + `historyIdx: number`. New `EngineLinkStatus = 'connected' | 'pending' | 'down'`.
- `omega-terminal/src/hooks/useEngineHealth.ts` (new) — Polls `/api/v1/omega/engines` every 5 s with 4 s per-request timeout. At-most-one in-flight, abortable, mount-safe cleanup. Returns `{status, lastOkAt, lastError}`.
- `omega-terminal/src/App.tsx` — Per-workspace back/forward stack (max 50, dedupes consecutive identical, trims forward-tail). Header chevrons (◂ ▸) with disabled state. Keyboard: Alt+← / Alt+→ (Chrome/FF style) and Cmd/Ctrl+[ / Cmd/Ctrl+] (Safari style). Hardcoded "engine link: pending" replaced by live pill (green/amber/red) with hover tooltip.

### `17933cd` Step 7 GUI completion (real /cells + /trade routes)

- `src/api/OmegaApiServer.cpp` — added `build_trade_json(id, status)` and `build_cells_json(q)` plus dispatch:
  - `/api/v1/omega/trade/<id>` walks `g_omegaLedger.snapshot()` for matching id; returns full TradeRecord shape (id, engine, symbol, side, ts, prices, MFE, MAE, regime, slippage, commission, L2 imbalance, ATR at entry, shadow flag). 400 on empty/non-numeric id, 404 on miss.
  - `/api/v1/omega/cells` derives a (engine, symbol)-grouped cell view from the same ledger — trades, wins, win-rate, total P&L, avg MFE/MAE, last-signal-ts (proxied off most recent exit_ts).
  - Both touch zero engine-side code; read what's already there.
- `omega-terminal/src/api/types.ts` — `TradeDetail` (full superset of LedgerEntry) and `CellRow` + `CellsQuery`.
- `omega-terminal/src/api/omega.ts` — `getTrade(id, opts)` + `getCells(query, opts)`. `getTrade` rejects empty ids client-side; URL-encodes id segment.
- `omega-terminal/src/panels/TradePanel.tsx` — Primary path `getTrade(id)`. On 404, falls back to scanning recent ledger window. Header tag shows "live engine detail" (green) on primary path, "ledger fallback" (amber) on fallback. Timeline now renders real MFE/MAE/regime/L2/ATR. Separate Costs card on full-detail path.
- `omega-terminal/src/panels/CellPanel.tsx` — "Endpoint pending" banner gone. Fetches `getCells({engine?, symbol?})` with args→filters mapping (`CELL`, `CELL HybridGold`, `CELL HybridGold XAUUSD`). Sortable table over all 8 columns. Click row → `LDG <engine>`.

### Uncommitted (this branch's recovery commit covers these)

- `omega-terminal/src/App.tsx` — Persistent back/forward breadcrumb bar between WorkspaceTabs and the panel area. Always renders when active tab has any history depth. Chunky **◀ Back to <prev panel title>** button on the left, **<next panel> Forward ▶** on the right, history depth indicator in the middle. Visible amber strip — addresses the user complaint that the existing chevrons were too subtle.
- `omega-terminal/src/panels/HomePanel.tsx` — 4-card live status strip above the tile grid: Engines online ("8/12 — 12 registered"), Open positions, Closed trades ("last 200"), Last activity. Polls `/engines`, `/positions`, `/ledger?limit=200` every 10 s. Cards never block grid; render "—" while loading or on error. All tile dimming + "step N" badges removed (every panel is live in Step 7). MARKET subtitle changed from "OpenBB-derived data + screening" to "Live market data via Yahoo + FRED" to match MarketDataProxy substrate.
- `src/api/MarketDataProxy.cpp` — `yahoo_screener` URL bumped from `count=25` to `count=100`. Yahoo's free predefined-screener tier supports up to 100. 4× more rows for MOV active/gainers/losers panel (was "very limited" per user).

## Open trading diagnosis (UNRESOLVED — next session priority)

User complaint: "we should still be trading and there is nothing… gold was trading and now there is nothing… many small 20-40 dollar trades that we know are available… not 1 trade [all day]"

### What we know

Mode is SHADOW (intentional, user confirmed). No engine has been auto-disabled, no loss-streak guard tripped, no engine_culled. Confirmed via:

```powershell
Select-String -Path C:\Omega\logs\latest.log -Pattern "shadow_mode|MODE|engine_disabled|fast_loss_streak|engine_culled|auto.disable" | Select-Object -Last 30
```

…which returned only init-time SHADOW config lines plus one reconnect message. No kill switches fired.

### What the engine state shows

GUI screenshot at 10:25 UTC (FIX session UP 00:01:31 = 91 s of uptime since most recent restart):

- All engines `FLAT`
- One closed trade in Recent Trades: 07:00:34 XAUUSD LONG entry 4596.61 exit 4591.61 4m18s reason xSL engine `AsianRange SL` net -$5.61 (PERSISTED from prior session, not this run)
- Daily P&L +$0.00 (session-scoped, reset on restart)
- All engine attribution cards +$0.00 (BREAKOUT, BRACKET, MEAN REV, GOLD STACK, CROSS-ASSET, LATENCY, HTF SWING, H1 SWING, H4 REGIME)
- Last Signals: "Waiting for first signal…"

### What the bracket diagnostics say

```powershell
Select-String -Path C:\Omega\logs\latest.log -Pattern "TRADE-COST|SHADOW-CLOSE|PARTIAL-CLOSE|HBG-CLOSE|GOLD.*CLOSE|TP_HIT|SL_HIT|FORCE_CLOSE" | Select-Object -Last 30
Select-String -Path C:\Omega\logs\latest.log -Pattern "TRADE-OPEN|HBG-OPEN|GOLD.*OPEN|\[ENTRY\]|impulse_block|spread.*wide|GATE.*BLOCK" | Select-Object -Last 30
Get-Content C:\Omega\logs\latest.log | Measure-Object -Line
```

returned only 4 `GOLD-BRK-DIAG` lines (10:23:48 → 10:24:19) and the daily-log header. Total log size: 154 lines. All 4 GOLD-BRK-DIAG lines show:

```
phase=IDLE can_enter=1 spread_ok=1 freq_ok=1 in_dead_zone=0 london_noise=0
in_asia=0 asia_ok=1 drift={0.90,1.69,1.61,1.53} l2_imb=0.500 can_arm=1
trend_bias=0 trend_blocked=0 pyramid_ok=0 impulse_block=0 cd_bypass=0
pyr_sl_age_s=never brk_hi=0.00 brk_lo=0.00 range=0.00 session_slot=2
```

Bracket engine is willing (`can_arm=1`) but has nothing to arm against (`range=0.00`). Per the startup verifier, `STRUCTURE_LOOKBACK=600` ticks @ ~195/min = ~185 s to fill. Engine was 91 s old at screenshot time → window not yet filled, normal post-restart behavior.

But: the user pointed out (correctly) this only explains the last 90 s, not the whole day. The wider question — why HybridGold (the small-trade scalper) hasn't fired all day even across multiple restarts — is **not yet answered**.

### What I confirmed about HybridGold before chat ran out

- Engine binding: `g_hybrid_gold` is a `GoldHybridBracketEngine` (declared `include/globals.hpp:320`, configured `include/engine_init.hpp:42`).
- Registry binding: registered with `g_engines` as `"HybridGold"` at `engine_init.hpp:1815-1818` and with `g_open_positions` at `engine_init.hpp:1894`.
- Default mode: `g_hybrid_gold.shadow_mode = kShadowDefault` (line 42).
- Log prefix: NO grep hit for `HBG-DIAG`, `[HBG]`, or `HybridGold` in the current log. The tail-search patterns I tried (`HBG`, `HybridGold`, `GOLD-DIAG`, `GOLD-SIG`) need to be re-run after fresh tick flow + structure window is filled.
- HBG-DIAG counters exist in code: `include/SweepableEngines.hpp:1028` (trail-arm and trail-fire counters).
- HBG-FIX-1 lineage: TRAIL_FRAC fixed at 0.25 (`include/SweepableEnginesCRTP.hpp:73, 736, 745`).

### Things I read that DON'T match the user's worry

- The OmegaTradeLedger.hpp comment "shadow short-circuits to audit-only logging so they never pollute g_omegaLedger" appears stale or refers to a different code path.
- The actual code at `include/trade_lifecycle.hpp:329` calls `g_omegaLedger.record(tr)` UNCONDITIONALLY at end of LIVE-close branch — no `if (!tr.shadow)` gate.
- `include/trade_lifecycle.hpp:257` (PARTIAL_1R branch) ALSO calls `g_omegaLedger.record(tr)` unconditionally.
- So shadow trades that close should go into g_omegaLedger and show in LDG/CELL/TRADE panels.
- The V2 CellShadowLedger CSV split (`logs/shadow/{tsmom_v2,donchian_v2,epb_v2,trend_v2}.csv`) IS a separate path, but only for the v2 portfolios.

### Diagnostic queue for next session

**Step 1 — confirm the engine has any signs of life at all today:**

```powershell
# Total log size — should be many thousands if engine has been running all day
Get-Content C:\Omega\logs\latest.log | Measure-Object -Line

# Any trade-close events at all this session
Select-String -Path C:\Omega\logs\latest.log -Pattern "TRADE-COST|SHADOW-CLOSE|PARTIAL-CLOSE" | Select-Object -Last 20

# HybridGold-specific logging
Select-String -Path C:\Omega\logs\latest.log -Pattern "HBG|HybridGold|hybrid_gold" | Select-Object -Last 30

# Other gold engines
Select-String -Path C:\Omega\logs\latest.log -Pattern "AsianRange|GoldHybrid|MeanRev|GOLD-STACK" | Select-Object -Last 20

# Are any rotated archive logs around? May contain the rest of today's trading
Get-ChildItem C:\Omega\logs\archive\*.log | Sort LastWriteTime -Descending | Select -First 10
```

**Step 2 — if HybridGold is producing zero diagnostics, find out why:**

The engine is in `g_engines` (it WILL show up on `/engines` and CC panel) but if its `update()` isn't being driven by the tick stream, or its entry conditions are gated by something the diag isn't surfacing, we won't see HBG output. Read:

- `include/SweepableEngines.hpp` around line 1028 (HBG-DIAG counters)
- `include/SweepableEnginesCRTP.hpp` around line 736 (HBG-FIX-1 trail logic)
- `include/engine_init.hpp:42` and the surrounding init block — what other parameters get set on `g_hybrid_gold` at startup
- The actual GoldHybridBracketEngine class definition (find via `grep -rn "class GoldHybridBracketEngine"`)

**Step 3 — once HBG is firing diagnostics, walk the gates:**

- Spread gate
- Frequency gate
- Dead zone
- London noise window
- Drift threshold
- L2 imbalance threshold

`GOLD-BRK-DIAG` already surfaces these for the bracket engine. Look for the equivalent HBG-DIAG line and walk each `=0` field to find the blocker.

### Two quality-of-life fixes for the post-restart bracket window blackout

**Problem:** Every QUICK_RESTART loses ~3 minutes of bracket signals because `STRUCTURE_LOOKBACK=600` ticks needs ~185 s to refill. Across 5 restarts/hour (heavy testing) that's 15 min/hour of dead time.

Two non-conflicting fixes:

1. **Persist the structure ring buffer to disk.** The engine already persists bar state on a 10-minute cadence (`[BAR-SAVE]` lines). Add `bars_gold_brk_structure.dat` (or similar) to the same save loop. On startup, load → bracket can arm immediately.
2. **Reduce STRUCTURE_LOOKBACK** from 600 to ~200 ticks (~60 s to fill). Risk: smaller window = noisier brackets. Make it configurable.

Either is engine-side work. Do these AFTER pinning the actual "no HBG trades all day" root cause — not before, in case they mask a real bug.

## Carry-over issues (still open from prior sessions)

1. **`QUICK_RESTART.ps1` stderr-mangling** — `npm ci` and `npm run build` blocks (~lines 627/636) use raw `2>&1 | ForEach-Object { Write-Host ... }`, missing the `Format-NativeOutputLine` wrap that v3.3 added for the cmake block (~line 670). When npm fails, operator sees `[System.Management].Automation.RemoteException` instead of the real `npm ERR!` body. Mechanical fix, ~10 lines. Hit twice this session.

2. **`QUICK_RESTART.ps1` default `-Branch` is `"main"`.** Every manual run needs `-Branch omega-terminal` or it silently resets to a pre-Step-5 main and wrecks the deploy. Three permanent fixes: (a) merge `omega-terminal` → `main` once Step 7 verifies live; (b) flip script default to `"omega-terminal"` until merge; (c) patch `OMEGA_WATCHDOG.ps1` to pass `-Branch omega-terminal`.

3. **Banner `GUI` line hardcodes `:7779`.** Banner final line says `GUI : http://185.167.119.59:7779` — that's the OLD telemetry GUI. Terminal UI is on `:7781`. Either change to `:7781`, or add a second `TERMINAL UI` line.

4. **`OMEGA_FRED_KEY` env var unset on VPS service.** CURV panel returns 503. MarketDataProxy log at startup confirms: `[MarketDataProxy] live mode -- Yahoo Finance + FRED (no FRED key; /curv will return 503)`. Set the env var on the service to enable yields.

5. **`omega-terminal/dist-verify/` and the various `dist-step7-*/` directories** — sandbox-locked verify-build artifacts on Mac. From Mac terminal: `sudo rm -rf omega-terminal/dist-step7-back omega-terminal/dist-step7-home omega-terminal/dist-step7-final omega-terminal/dist-step7-verify omega-terminal/dist-verify`. Either gitignore the pattern `dist-*` or move under `build/`. Doesn't block deploy.

6. **`docs/SESSION_2026-05-01f_HANDOFF.md` still untracked.** Previous session's handoff doc. If you want a permanent record of that session, `git add docs/SESSION_2026-05-01f_HANDOFF.md`.

## GUI status (post-recovery commit)

After the recovery commit lands and VPS redeploys:

- **Title bar**: green engine-link pill, ◂ ▸ chevrons (kept as secondary surface).
- **Persistent back bar**: chunky amber strip directly under the workspace tabs on every screen with history. Big visible "◀ Back to <PrevPanel>" button + history depth indicator + "<NextPanel> Forward ▶" when applicable. This addresses the "no way to return to where you were" complaint that the chevrons alone didn't solve.
- **Home page**: 4-card live status strip (engines online, open positions, closed trades, last activity), all 24 panel tiles in bright amber (no dimming, no step badges), MARKET subtitle reads "Live market data via Yahoo + FRED".
- **All 24 panels live**: HOME, HELP, CC, ENG, POS, LDG, TRADE, CELL, INTEL, CURV (CURV will 503 until OMEGA_FRED_KEY is set), WEI, MOV (now up to 100 rows), OMON, FA, KEY, DVD, EE, NI, GP, QR, HP, DES, FXC, CRYPTO, WATCH.
- **Keyboard**: Ctrl/Cmd+K focus command bar, Ctrl/Cmd+T new tab, Ctrl/Cmd+W close tab, Alt+← / Alt+→ back/forward, Cmd/Ctrl+[ / Cmd/Ctrl+] back/forward, Tab in command bar accepts highlighted suggestion, Enter dispatches.
- **Args carry-through**: `LDG HybridGold` filters ledger; `CELL HybridGold XAUUSD` filters cells to one (engine, symbol); `TRADE 12345` opens single-trade detail.

## NOT done this session (declined / scope)

- HelpPanel rewrite with full Step-7 reference (back/forward shortcuts, args syntax, live-vs-mocked routes)
- `?` keyboard overlay
- Tab persistence to localStorage
- Tab labels showing args (e.g. distinguishing two TRADE tabs by id)
- Empty-state copy polish (POS / LDG / CELL when zero data)
- Favicon + browser tab title
- Status-card sparklines
- Engine-side `CellSummaryRegistry` (the "real" /cells path)
- Engine-side real `last_signal_ts` (currently exit_ts proxy)
- Real `/news/world` provider for INTEL panel
- Persisting bracket structure window across restarts
- Reducing STRUCTURE_LOOKBACK
- Diagnosing why HybridGold has fired zero trades all day (URGENT, top of next session)

## Verified this session

- TypeScript `npm run typecheck`: clean across all edits
- Vite `npm run build`: clean — final size 293.76 kB main / 75.50 kB gz, lazy chunks (CurvPanel 5.77 kB, GpPanel 4.04 kB) split correctly
- C++ `g++ -fsyntax-only -std=c++20 -Wall -Wextra -DOMEGA_BACKTEST=0 -I include -I src/api src/api/OmegaApiServer.cpp src/api/MarketDataProxy.cpp`: 0 errors, 0 warnings
- Live deploy of `17933cd` confirmed: banner COMMIT match, EXE timestamp match, log hash match, service Running, all 7 .cpp compiled (main, SymbolConfig, OmegaTelemetryServer, OmegaApiServer, MarketDataProxy, WatchScheduler, gold_coordinator), vcpkg DLLs deployed

## NOT verified this session

- The recovery commit (back bar + Home strip + screener bump) running live on the VPS — written but not pushed
- Any panel except HOME viewed live (user only screenshotted Home + the legacy :7779 telemetry dashboard)
- HybridGold's actual entry-gating logic — only confirmed it's registered and shadow_mode-defaulted
- Whether the structure window has ever filled in any session today (the only logs I saw were ≤91 s post-restart)

## Files touched this session

```
M  omega-terminal/src/types.ts                         # 15dddaf — Workspace history + EngineLinkStatus
A  omega-terminal/src/hooks/useEngineHealth.ts          # 15dddaf — engine-link health poller
M  omega-terminal/src/App.tsx                           # 15dddaf, then again (uncommitted) — back bar
M  src/api/OmegaApiServer.cpp                           # 17933cd — /trade/<id> + /cells routes
M  omega-terminal/src/api/types.ts                      # 17933cd — TradeDetail + CellRow + CellsQuery
M  omega-terminal/src/api/omega.ts                      # 17933cd — getTrade + getCells
M  omega-terminal/src/panels/TradePanel.tsx             # 17933cd — full rewrite onto /trade/<id>
M  omega-terminal/src/panels/CellPanel.tsx              # 17933cd — full rewrite onto /cells
M  omega-terminal/src/panels/HomePanel.tsx              # uncommitted — live status strip + un-dim
M  src/api/MarketDataProxy.cpp                          # uncommitted — screener count 25 -> 100
A  docs/SESSION_2026-05-01g_HANDOFF.md                  # this file
```

## Session lessons

1. **Don't claim a screen is "complete" before the user clicks through every panel.** I twice told the user the GUI was complete; the user disagreed both times and was right both times (dimmed tiles, missing back-bar). The ground truth is the user's screen, not mine.
2. **The initial back/forward chevrons were too small (12 px Unicode triangles in a dim disabled state).** Persistent visible affordances beat hidden keyboard shortcuts every time on a screen the user is just learning. Lesson: when adding navigation, make the surface OBVIOUS first, optimize density second.
3. **Sandbox can't unlink files in host-mounted directories.** Every git lock or stale verify-dir requires the user to clean up from their Mac terminal. Plan for this: don't run `vite build` against the real `dist/` from the sandbox; use a throwaway outDir AND warn the user up-front it can't be cleaned by me.
4. **Don't paste shell blocks with apostrophes inside `#` comment lines.** zsh on Mac drops into `quote>` continuation mode on the unclosed apostrophe even when the line "should" be a comment. Either omit the comment or strip apostrophes from comment text.
5. **Diagnose from real data, not from comments.** The `OmegaTradeLedger.hpp` comment said "shadow short-circuits to audit-only logging so they never pollute g_omegaLedger" — that comment is stale and contradicted by `trade_lifecycle.hpp:329`. Always read the code, not just the doc-comment.
6. **Trading complaints are usually multi-cause.** The "no trades since restart" question has two layers: (a) bracket structure window post-restart blackout, (b) why HybridGold hasn't fired all day across many restarts. Layer (a) is real but small; layer (b) is the actual user concern. Address the larger layer first or you sound dismissive.

## Open task list for next session

* [first thing] Recovery commit (commands above)
* [first thing] VPS redeploy + confirm Home live status strip + bright tiles + back bar + MOV row count
* [URGENT, top priority after deploy] Diagnose HybridGold zero-trade situation — start with the 5 greps in "Diagnostic queue" above
* [follow-up to HBG diagnosis] Walk the engine code to find the HBG entry gate that's failing
* [optional, after HBG is fixed] Persist bracket structure_lookback across restarts
* [optional] Reduce STRUCTURE_LOOKBACK to ~200 to shrink restart blackout
* [optional, mechanical] Patch QUICK_RESTART.ps1 stderr-mangling
* [optional, mechanical] Decide on permanent branch-default fix
* [optional, mechanical] Fix banner :7779 line
* [optional] Set OMEGA_FRED_KEY env var on VPS service for CURV panel
* [optional] Commit docs/SESSION_2026-05-01f_HANDOFF.md and this file
* [optional] gitignore `omega-terminal/dist-*` pattern
