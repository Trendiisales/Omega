# NEXT_SESSION -- S11 Priority Brief

Date checkpoint: 2026-05-07
Previous session: S10 (2026-05-07)

## How to start S11

1. First action: request folder access to `/Users/jo/omega_repo` via `request_cowork_directory`. The bash sandbox in S9 and S10 was broken ("no space left on device" -- useradd in /etc/passwd fails). If it persists, do all work via Read/Write/Edit/Grep/Glob (those reach the host directly, not the sandbox).
2. Read this file end-to-end before doing anything else.
3. Verify VPS state: confirm `[Omega] Git hash:` in latest log matches whatever `origin/main` is at on session start. If S10 P3a deploy hadn't completed by end-of-S10, re-verify using the verification grep below before starting S11 priority 1.

## What S10 actually shipped

S10 priority 1 (verify VPS post-deploy of S9): COMPLETE.
- Local + origin/main at `1089873d` confirmed via direct `.git/refs` reads
- Live VPS log (provided by user mid-session) confirmed:
  - `[Omega] Git hash: 1089873` -- right binary running
  - All 6 TrendRider cells `ARMED (shadow_mode=true, ...)` -- the S9 hard-pin held
  - `[TREND-PB] ... thresh=Y.YY` field visible per user confirmation -- the S9 ATR-scaled IMM-REV is live
  - `[GOLD-L2-LIVE] imb=...` continuously updating -- S8 L2 fix still working

S10 priority 3 phase A (HBG/HBI dispatch cull): COMPLETE.
- Commit `ba5f0e9ea809b20cca10ddde26ca5b2208266168` on origin/main
- 5 dispatch blocks removed (~340 lines):
  - `include/tick_gold.hpp` lines ~2097-2202 -- GoldHybridBracketEngine dispatch
  - `include/tick_indices.hpp` lines ~246-310 -- HBI[US500.F]
  - `include/tick_indices.hpp` lines ~562-614 -- HBI[USTEC.F]
  - `include/tick_indices.hpp` lines ~712-762 -- HBI[DJ30.F]
  - `include/tick_indices.hpp` lines ~976-1048 -- HBI[NAS100]
- Each replaced by a 6-8 line marker comment: `// HBG-* culled S10 P3a (2026-05-07): ...`
- `g_hybrid_gold/sp/nq/us30/nas100.on_tick(...)` -- 0 occurrences in codebase post-edit
- `[HYBRID-*] ORDERS SENT` printf strings -- 0 occurrences in codebase post-edit
- Engines stay CONSTRUCTED in `globals.hpp` + `engine_init.hpp` (Phase B's job to remove)
- `g_hybrid_*.has_open_position()` and `.phase` queries elsewhere left intact (return safe defaults on dormant engines: `false` / `Phase::IDLE`)

## S10 deferred / blocked

- S10 P2 (HBI documented sweep): BLOCKED on (a) no HBI backtester, (b) no NAS100/USTEC tick captures in repo, (c) bash sandbox broken so cannot compile/run. Carries to S11+.
- S10 P3 phases B-E: deferred to S11+ per don't-bundle rule (each phase = separate commit + deploy + verify).
- S10 P4 (cTrader/cBot dead-code cull): not started.

## S11 audit findings carried forward (from VPS log read mid-S10)

These are flagged for awareness, not assigned to a specific S11 priority:

A. cTrader DOM L2 watchdog DEAD on ctid=43014358:
```
[L2-WATCHDOG] *** DEAD *** cTrader depth from ctid=43014358 dead for 120s
[L2-WATCHDOG] L2 engines GATED. ACTION REQUIRED: restart Omega or check ctid=43014358
[SYSTEM-ALERT] L2_DEAD >120s -- running on FIX prices (no auto-restart)
```
Concurrent with `[REAL-DOM] Disconnected -- reconnecting in 3s` repeating loop.
- Gold L2 still works via FIX 264=0 (the S8 fix). `[GOLD-L2-LIVE] imb=...` keeps printing.
- BUT L2-using INDEX engines are being gated. Affects: any engine reading `g_macro_ctx.sp_l2_imbalance` / `nq_l2_imbalance` / `us30_l2_imbalance` / `nas_l2_imbalance` (i.e. IFlowSP/NQ/US30/NAS).
- Suspect: cBot DOM streamer subscription on cTrader side (`localhost:8765`) is dropping its source.
- Possible S11 task: investigate ctid=43014358 token expiry, or refactor to FIX-only L2 across all symbols (the S8 pattern that already works for gold).

B. Estx50 silent at startup-self-test:
```
[STARTUP-FAIL] Estx50 never pulsed in 78s post-init (live_required=true, session=07-22 UTC)
[STARTUP-SELFTEST] live_required total=42, pulsed=41, silent=1
[STARTUP-SELFTEST] silent engines: Estx50
```
- Likely benign -- startup happened at 00:28 UTC, Estx50 session is 07-22 UTC, so it was out of session at startup. The `live_required=true` flag is misapplied for session-gated engines.
- Possible S11 task: gate the heartbeat-init / startup-selftest by current session_slot to suppress false alarms.

C. Three FIX securities unmatched at logon:
```
[OMEGA-SECURITY] UNMATCHED: 'UK100.F' id=4461
[OMEGA-SECURITY] UNMATCHED: 'UKBRENT.F' id=2634
[OMEGA-SECURITY] UNMATCHED: 'ESTC' id=3229
```
- Pre-existing condition. UK100, BRENT, ESTX50 already match cleanly to other ext IDs.
- Possible S11 task: add the .F-suffix variants and `ESTC` to the symbol learning map, OR ignore them with a comment.

D. CRITICAL FINDING from S9 audit (still open): MidScalperGold, MinimalH4, C1Retuned were claimed to be "live-trading" in chat but engine_init.hpp + log say `shadow=true`. Confirmed in S10 log. Don't read their PnL as live until you have flipped shadow_mode for each one and re-verified.

## S11 priorities (in order)

1. Verify S10 P3a clean on VPS (cheap, mandatory). Confirm:
   - `Get-Content C:\Omega\logs\latest.log -Tail 200` shows `Git hash: ba5f0e9` (or whatever origin/main is now)
   - `Select-String -Path C:\Omega\logs\latest.log -Pattern "HYBRID-(GOLD|SP|NQ|US30|NAS100)\] ORDERS SENT"` returns zero matches
   - All other engines (Tsmom, Donchian, EmaPullback, TrendRider, TrendPullback, GoldStack) pulse normally

2. P3 Phase B: delete HBG/HBI globals + init blocks.
   File targets per S10 audit:
   - `include/globals.hpp` -- delete the `g_hybrid_gold` / `g_hybrid_sp` / `g_hybrid_nq` / `g_hybrid_us30` / `g_hybrid_nas100` declarations. Search anchor: `g_hybrid_gold`, `g_hybrid_sp` (both unique).
   - `include/engine_init.hpp` -- delete the init blocks that call `.init()` / `.warmup_*()` / set risk params for the 5 hybrid engines. Search anchor: `g_hybrid_gold.init`, `g_hybrid_sp.init` (or however the init pattern reads -- read first to confirm).
   - `include/EMACrossEngine.hpp` -- has g_hybrid_* refs per S10 grep. Verify whether they're heartbeat registrations or behavioural; if behavioural, you may need a Phase B.5 to cut those before B can compile.
   - `include/on_tick.hpp` and `include/quote_loop.hpp` -- both showed g_hybrid_* refs in S10 grep. Inspect each before deleting -- they may be just type forward-decls or includes that need to come out.
   - Heartbeat registration in `engine_init.hpp` -- search for `HEARTBEAT-INIT] HybridGold registered` style strings; delete those registrations too or the live-required self-test will fail with 5 silent engines.
   Build locally if possible (bash sandbox check at S11 start). If sandbox still broken, ship + deploy and let VPS catch any compile errors.

3. P3 Phase C: delete the engine source files entirely.
   - `include/GoldHybridBracketEngine.hpp` (full file delete + remove `#include` references in tick_gold.hpp / globals.hpp / engine_init.hpp)
   - `include/IndexHybridBracketEngine.hpp` (same pattern + remove from tick_indices.hpp / globals.hpp / engine_init.hpp)

4. P3 Phase D: clean up backtest harnesses (per S10 brief unchanged):
   - `backtest/hbg_duka_bt.cpp`
   - `backtest/hbg_ab.sh`
   - `backtest/IndexBacktest.cpp` (review -- may share code with non-HBI tests)
   - `backtest/OmegaBacktest.cpp` (review)
   - `backtest/omega_bt.cpp` (review)
   - `scripts/hbg_full_sweep.py`

5. P4: cTrader/cBot dead-code cull (~2500 lines, multi-file). Per S10 brief: gut `omega_main.hpp` first, then helper headers, then file deletions, then comment cleanup. Same phased deploy pattern.

6. P2 carryover (HBI documented sweep): only attempt if bash sandbox is functional AND user has pulled NAS100/USTEC tick captures from VPS. Otherwise defer further.

7. Investigate one of the S11 carry-forward findings (A / B / C above) if time permits.

8. Refresh this file at end of S11 with S11 outcomes + S12 priorities.

## Standing context for S11 agent

User preferences:
- Always give full code with context. No snippets. Provide full file. (For Cowork-mode workflow where edits go directly to disk: prefer the Edit tool for surgical changes and offer to dump the full file on request -- don't blindly dump 2000+ line files just because there's a 5-line edit. Confirm with user if uncertain.)
- Warn at 70% context with summary.
- Warn before time-management/session-usage block hits.
- Never modify core code unless instructed clearly. (Pre-confirm any edit that touches more than one file or goes beyond a single localized change.)

Repo paths:
- Mac: `/Users/jo/omega_repo` (working repo -- make changes here, push to GitHub)
- GitHub: https://github.com/Trendiisales/Omega
- VPS: `C:\Omega` (auto-pulls from GitHub via DEPLOY_OMEGA.ps1)

Deploy command (canonical):
```
cd C:\Omega
.\DEPLOY_OMEGA.ps1
```
- Does pull, build, validate, launch in one shot. No `nssm`, no manual `cmake --build`. Omega.exe runs as a process under `OmegaWatchdog.ps1`, not as a Windows service.
- If deploy says "Another deploy is already running":
```
Get-WmiObject Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -like '*DEPLOY_OMEGA*' -and $_.ProcessId -ne $PID } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
.\DEPLOY_OMEGA.ps1
```

Tail logs:
```
Get-Content C:\Omega\logs\latest.log -Tail 200 -Wait
```

One-shot grep for a pattern (PowerShell):
```
Select-String -Path C:\Omega\logs\latest.log -Pattern "HYBRID-(GOLD|SP|NQ|US30|NAS100)\] ORDERS SENT"
```

## Bash sandbox status

BROKEN as of S9 + S10. Error: `useradd: /etc/passwd.NNNNNN: No space left on device`. Affects all `mcp__workspace__bash` calls (resume, create, re-resume all fail). Falls back to file-tool-only workflow which is sufficient for editing/grepping/reading but blocks any local compile/test/script execution.

If bash works at S11 start: P2 (HBI sweep) becomes unblocked once tick captures are pulled.
If bash still broken: same fallback as S9/S10 -- ship to VPS, let VPS compile, verify via log.

## What you should NOT touch in S11

- TsmomEngine trail logic (backtested -35%, disabled, settled).
- TrendRider risk parameters (4%/trade, 0.50 cap) -- intentional Kelly-fraction sizing per the comment block at engine_init.hpp:736-747. The shadow pin at engine_init.hpp:759 must remain `true` until the promotion gate (PF >= 1.3, >= 30 trades, beats Tsmom expectancy) is cleared via paper data.
- Gold L2 wiring (S8 fix, working in production via FIX 264=0).
- TrendPullback IMM-REV ATR scaling (S9 ship, working in production).
- `has_open_position()` / `.phase` reads of g_hybrid_* anywhere in tick_gold.hpp / tick_indices.hpp / etc -- these are still valid C++ after S10 P3a (engines are dormant but constructed). They become invalid only AFTER P3 Phase B deletes the globals; remove them as part of B, not before.

## S10 -> S11 commit chain

```
1089873d  S9 partial: TrendPullback ATR-scale IMM-REV + TrendRider shadow-pin
ba5f0e9e  S10 P3a: cull HBG+HBI dispatch sites in tick_gold/tick_indices
[next]    S11 P3b: delete HBG/HBI globals + init blocks  (planned)
[next]    S11 P3c: delete HBG/HBI engine source files     (planned)
[next]    S11 P3d: clean up HBG/HBI backtest harnesses    (planned)
```
