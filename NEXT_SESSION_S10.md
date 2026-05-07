# NEXT_SESSION — S10 Priority Brief

Date checkpoint: 2026-05-07
Previous session: S9 (2026-05-07)

## How to start S10

1. First action: request folder access to `/Users/jo/omega_repo` via `request_cowork_directory`. The bash sandbox in S9 was broken ("no space left on device") — if it persists, do all work via Read/Write/Edit/Grep/Glob (those reach the host directly, not the sandbox).
2. Read this file end-to-end before doing anything else.
3. Verify deploy state on VPS: confirm last commit on `origin/main` matches what was pushed at end of S9 (`S9: TrendPullback ATR-scaled IMM-REV + TrendRider shadow-pin`). If not deployed yet, prompt the user to run `.\DEPLOY_OMEGA.ps1` on the VPS (NOT the old `nssm` / `cmake --build` recipe — that's wrong for this repo).

## What S9 actually shipped

Two surgical code edits, both pushed and (assuming user successfully redeployed) live:

1. `include/CrossAssetEngines.hpp:1640-1679` — TrendPullback IMM-REVERSAL guard now scales with ATR. Was `if (adverse > 3.0)`. Now `imm_rev_thresh = max(3.0, atr * 0.3)`. Behaviour: no-op for gold (ATR ~1-3 → threshold pinned at 3pt floor); widens guard for high-ATR indices (NQ/SP/GER40 in news regimes). Affects `g_trend_pb_ger40/nq/sp` engines (gold variant has `enabled=false` so unaffected).

2. `include/engine_init.hpp:759` — `g_trend_rider.shadow_mode = true` (hard-pinned, was `kShadowDefault`). **CRITICAL FINDING**: TrendRider was firing LIVE on production since 2026-04-30 (7 days), contrary to chat/NEXT_SESSION claims of "validating in shadow". Risk parameters: 4% per trade (8x Tsmom), 0.50 lot cap (10x Tsmom), 6 cells, no TP, no time exit. Now genuinely in shadow until promotion gate (PF >= 1.3, >= 30 trades, beats Tsmom expectancy) is cleared via paper data.

After deploy, verify on VPS log:
- Zero new `[OMEGA-PERF] TrendRider_*` live trade lines. Shadow CSV writes only.
- New `[TREND-PB] ... IMM-REVERSAL adverse=X.XX thresh=Y.YY` log lines now show `thresh=` field (the diagnostic addition for the new code).
- `[GOLD-L2-LIVE] imb=` continues to show varying values — already confirmed live in S9 (`imb=0.228 age_ms=121` observed).
- `[OMEGA-PERF] Tsmom`, `[OMEGA-PERF] GoldStack` continue normally.

## S9 audit findings (no code action taken)

These are flagged for user awareness, no action this session:

A. **Live engine inventory cross-check vs S8 chat claims:**
   - User chat said MidScalperGold, MinimalH4, C1Retuned were "earning their keep (live-trading)"
   - Code in `engine_init.hpp` says all three are hard-pinned `shadow_mode = true`
   - Verify on VPS whether these are really live or shadow before reading their PnL as live

B. **Long-trade protection audit (S9 task 2):**
   - Live untrailed exposure is small. Of currently-live engines: Tsmom (intentionally trail-less per S8 backtest), GoldStack (stack-managed), TrendRider (now pinned shadow). All other engines without trails are shadow-only.
   - When promoting MinimalH4, C1Retuned, EmaPullback, or DonchianPortfolio out of shadow: ADD a stage-trail and re-validate via `phase1/signal_discovery/post_cut_revalidate.py` before live promotion. The "give-back-open-profit" pattern Tsmom hit is structural for any no-trail multi-day engine.
   - Highest theoretical-give-back risk in current shadow universe: C1Retuned D1 cells (30-day hold), MinimalH4 (4-day hold), EmaPullback H4/H6 (5-7.5 day hold).

## S10 priorities (in order)

1. **Verify VPS post-deploy state** (cheap, mandatory). Confirm S9 commit deployed, TrendRider not firing live, IMM-REV thresh logging visible on indices.

2. **HybridBracketIndex documented sweep** (S9 task 4 — was deferred). Three blockers from S9:
   - No HBI backtester. Port `backtest/hbg_duka_bt.cpp` → `backtest/hbi_duka_bt.cpp` (~few hundred lines, structurally similar). Parameterize symbol via constexpr / template / config flag.
   - No NAS100/USTEC tick captures in repo. Pull from VPS: `l2_ticks_NAS100_*.csv`, `l2_ticks_USTEC_*.csv` from `C:\Omega\logs\` to `/Users/jo/omega_repo/data/` or similar. Wait ~1 week of accumulation if data is sparse.
   - Bash sandbox needs to be functional to compile/run the backtester. Verify at session start.

3. **HBG/HBI full code deletion** (S9 task 5 — was deferred). 287 references across 44 files; 71 in `tick_indices.hpp` alone. Phased deploy approach mandatory:
   - Phase A: comment-only — replace HBG/HBI call sites in `tick_gold.hpp`/`tick_indices.hpp` with `// HBG/HBI culled S8 — call removed`. Engines still constructed, never called. Build, deploy, verify.
   - Phase B: remove globals from `globals.hpp` + init blocks from `engine_init.hpp`. Build, deploy, verify.
   - Phase C: delete `GoldHybridBracketEngine.hpp` and `IndexHybridBracketEngine.hpp` files. Build, deploy, verify.
   - Phase D (optional): clean up backtest harnesses (`hbg_duka_bt.cpp`, `hbg_ab.sh`, `IndexBacktest.cpp`, `OmegaBacktest.cpp`, `omega_bt.cpp`, `scripts/hbg_full_sweep.py`).
   - Phase E (skip): documentation/incident files stay as historical record.
   - Each phase = separate commit + VPS deploy + log verification cycle. DO NOT bundle.

4. **cTrader/cBot dead-code cull** (S9 task 6 — was deferred). ~2500 lines, multi-file, hot-path implications. Same phased deploy pattern as HBG/HBI deletion. File:line list is in the original `NEXT_SESSION.md` (the pre-S8 version, items 7-9). Order suggested there: gut `omega_main.hpp` first, then helper headers, then file deletions, then comment cleanup.

5. **Refresh `NEXT_SESSION.md`** — the on-disk pre-S8 version is now two sessions stale. After S10 ships, replace it with this S10 brief + S10 outcomes.

## Standing context for S10 agent

User preferences (from S9):
- Always give full code with context. No snippets. Provide full file. (For Cowork-mode workflow where edits go directly to disk, this means: prefer the Edit tool for surgical changes, then offer to dump the full file on request — don't blindly dump 2000+ line files just because there's a 5-line edit. Confirm with user if uncertain.)
- Warn at 70% context with summary.
- Warn before time-management/session-usage block hits.
- Never modify core code unless instructed clearly. (Pre-confirm any edit that touches more than one file or goes beyond a single localized change.)

Repo paths:
- Mac: `/Users/jo/omega_repo` (the working repo — make changes here, push to GitHub)
- GitHub: https://github.com/Trendiisales/Omega
- VPS: `C:\Omega` (auto-pulls from GitHub via DEPLOY_OMEGA.ps1)

Deploy command (canonical, the only one to use):
- VPS: `cd C:\Omega; .\DEPLOY_OMEGA.ps1` — does pull, build, validate, launch in one shot. No `nssm`, no manual `cmake --build`. Omega.exe runs as a process under `OmegaWatchdog.ps1`, not as a Windows service.
- If deploy says "Another deploy is already running" — kill the stuck PS process: `Get-WmiObject Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -like '*DEPLOY_OMEGA*' -and $_.ProcessId -ne $PID } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }` then re-run.

Tail logs:
- `Get-Content C:\Omega\logs\latest.log -Tail 200 -Wait`

## What you should NOT touch in S10

- TsmomEngine trail logic (backtested -35%, disabled, settled).
- HBG/HBI engines (early-return culled — don't try to revive without architectural change).
- TrendRider risk parameters (4%/trade, 0.50 cap) — these are intentional Kelly-fraction sizing per the comment block at engine_init.hpp:736-747. Just keep the shadow pin until promotion.
