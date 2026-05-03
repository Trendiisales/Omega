# Session 2026-05-01 — Handoff

Branch: `omega-terminal`. All commits below are pushed to GitHub.

## What shipped this session (5 commits on omega-terminal)

| SHA | Subject |
|-----|---------|
| `fe924ea` | omega-terminal: replace amber-on-black with Omega palette |
| `f73d4ad` | OmegaApiServer: bind 0.0.0.0 so UI reachable from browser |
| `bdb844d` | OmegaApiServer serves omega-terminal/dist/; QUICK_RESTART v3.5 builds UI |
| `532bec8` | HBG backtest: --from/--to window flags + cached-baseline A/B wrapper |
| `3fa971d` | QUICK_RESTART.ps1 v3.4: add -Branch param so feature branches can deploy |

Step-2 ancestors `45e3ef0` (docs) and `0ed1dec` (initial step 2) were already on the remote at the start of the session.

## End state on the VPS (185.167.119.59)

- Service `Omega` is RUNNING on `fe924ea` (or whatever HEAD is after final pull/restart).
- Old GUI: `http://185.167.119.59:7779` — unchanged, still live.
- New UI:  `http://185.167.119.59:7781` — omega-terminal React app served directly from `Omega.exe` via `OmegaApiServer`, with Omega-palette colours.
- API:     `http://185.167.119.59:7781/api/v1/omega/{engines,positions,ledger,equity}` — JSON, returning real engine data.
- Firewall rule `Omega API 7781` is in place.
- Mode: SHADOW. RSI Reversal + RSI Extreme remain disabled (from prior session's `161d992`).

## Carry-forward (not done this session)

1. **Final colour redeploy** — pull `fe924ea` to VPS, `.\QUICK_RESTART.ps1 -Branch omega-terminal`, hard-refresh browser to confirm gold/blue-gray instead of amber.
2. **Step 3 panel wiring** — `docs/omega_terminal/STEP3_OPENER.md`. Replace `ComingSoonPanel` for `CC` / `ENG` / `POS` with live panels using `getEngines` / `getPositions` / `getLedger` from `omega-terminal/src/api/omega.ts`. The new `text-up` / `text-down` Tailwind classes and `.up` / `.down` / `.bid` / `.ask` CSS classes are ready for the bid/ask/P&L cells. Engine-side: `last_signal_ts` / `last_pnl` accessors needed (currently 0). Plan in opener.
3. **HBG S52 verdict** — pre-seed full-window cache from existing baseline data, then run `./backtest/hbg_ab.sh` for the recent-window verdict (~30 min). Seeding commands are in earlier chat. Wrapper details in `backtest/hbg_ab.sh`. Strategic flag: pre-S52 baseline is -$345 over 26 months, 32.8% WR — losing strategy regardless of S52.
4. **`OMEGA_WATCHDOG.ps1` -Branch flag** — currently calls `QUICK_RESTART.ps1` with no `-Branch`, so an auto-restart while VPS is on `omega-terminal` silently reverts to `main`. Surgical fix needed (same shape as the v3.4 patch). Surface plan first per the engine-code rule.
5. **`Bar State Load` [FAIL]** in startup verifier — pre-existing, not introduced this session. Worth investigating: `load_indicators()` reportedly never called.
6. **Phase 2a 5-day shadow window** — Day 5 ≈ 2026-05-06.
7. **`symbols.ini` drift on VPS** — uncommitted local changes detected during pulls. Run `git diff symbols.ini` on VPS to see what's pending; commit or stash before any operation that might wipe it.
8. **Engine link status pill** — top-right reads `ENGINE LINK: PENDING`. Likely intentional (status-pill polling is Step 3 work per `App.tsx` comment), but worth verifying via Network tab whether `/api/v1/omega/engines` is being polled at all.

## Symbols (where they appear)

- omega-terminal HOME: just function-code tiles, no symbols.
- omega-terminal panels CC / POS / ENG / LDG / TRADE / GP: symbols + bid/ask/P&L will appear here once Step 3 panels are built. Type the code in command bar (Ctrl+K to focus) — e.g. `POS HBG`, `CC XAUUSD`, `LDG`.
- Existing :7779 GUI: header ticker shows symbols with bid (green) / ask (red); market panel shows symbol rows by category (gold / primary / FX / Asia / EU).

## Standing rules to remind the next session of

- Branch is `omega-terminal`. Do not modify `main` or `src/gui/`.
- Engine-side C++ edits (anything in `src/`, `include/`) require explicit user approval — surface a plan first.
- `OmegaApiServer.{cpp,hpp}` are touchable per session pattern but still need approval before edit.
- Backtest harnesses in `backtest/` are NOT engine code; treat as ordinary tooling.
- User preferences: always provide full files (no diffs/snippets); warn at ~70% context; warn before time/session usage block.

## Files written/modified by Cowork agent

All changes pushed via GitHub API to `omega-terminal`:

- `QUICK_RESTART.ps1` (v3.4 → v3.5)
- `backtest/hbg_duka_bt.cpp` (added --from/--to window flags)
- `backtest/hbg_ab.sh` (new file, A/B harness with cached baseline)
- `src/api/OmegaApiServer.cpp` (static file serving + INADDR_ANY bind)
- `src/api/OmegaApiServer.hpp` (Bind comment updated)
- `omega-terminal/tailwind.config.js` (amber palette → Omega palette + up/down semantics)
- `omega-terminal/src/index.css` (Omega CSS variables + .up/.down/.bid/.ask classes)

## Quick-start for next session — paste this as the opening prompt

> Read these files first to catch up on where we are on the omega-terminal branch (last commit `fe924ea`):
> 1. `docs/SESSION_2026-05-01_HANDOFF.md` (this file in workspace, also pushed to repo)
> 2. `docs/omega_terminal/STEP3_OPENER.md` (step 3 plan from prior session)
> 3. `omega-terminal/tailwind.config.js` + `src/index.css` (new palette + up/down conventions)
> 4. `omega-terminal/src/api/omega.ts` + `src/api/types.ts` (typed fetch wrappers panels will use)
> 5. `src/api/OmegaApiServer.cpp` (current endpoint behaviour; positions/equity are stubs)
>
> Verify the colour redeploy: `git pull origin omega-terminal && .\QUICK_RESTART.ps1 -Branch omega-terminal` on VPS, then hard-refresh `http://185.167.119.59:7781`. Confirm off-white text + gold accents (no amber). Then begin Step 3 panel wiring per STEP3_OPENER.md, OR run the HBG S52 verdict, OR fix OMEGA_WATCHDOG.ps1 — user's choice.
