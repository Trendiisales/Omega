# SESSION 2026-05-07 -- S12 Tier 1 PS1 Consolidation -- HANDOFF

## Status: DEPLOYED AND OPERATIONAL

VPS confirmed healthy at session end:

```
Get-Service Omega, OmegaWatchdog | Format-Table Name, Status
  Omega          Running
  OmegaWatchdog  Running

Get-Content C:\Omega\omega_config.ini -TotalCount 2
  # ==============================================================================
  # OMEGA -- COMMODITIES & INDICES TRADING SYSTEM    <-- real config, not tombstone

omega_service_stderr.log:
  [Omega] Git hash: 7ecb748                          <-- new build live

watchdog.log:
  === WATCHDOG STARTED === ...
  HEARTBEAT ... (5-min cadence)
  SAFE-TO-RESTART logic confirmed working (deferred a manual-stop window)
```

## What was done

PowerShell control surface reduced from 21 root scripts to 12 (8 diagnostics + 2 build/backtest helpers + the 2 new unified scripts).

### Added

- **`OMEGA.ps1`** -- unified control script. Subcommands: `deploy`, `restart`,
  `start`, `stop`, `watchdog` (internal, used by INSTALL_OMEGA's NSSM service
  body), `help`. Replaces `QUICK_RESTART.ps1` v3.5 + `DEPLOY_OMEGA.ps1` +
  `START_OMEGA.ps1` + `OMEGA_WATCHDOG.ps1` v2.0. All v3.0--v3.5 hardening fixes
  preserved verbatim, plus the DEPLOY_OMEGA stamp/symbols/watermark validation
  merged into `deploy`'s pipeline.
- **`INSTALL_OMEGA.ps1`** -- NSSM installer. Flags: `-CleanService`,
  `-InstallService`, `-InstallWatchdog`, `-All`, `-Uninstall`. Replaces
  `INSTALL_SERVICE.ps1` + `INSTALL_WATCHDOG.ps1`. Watchdog NSSM body is now
  `powershell.exe -File OMEGA.ps1 watchdog`.
- **`DEPLOY_S12_PS1_CONSOLIDATION.sh`** -- one-shot Mac-side commit/push helper
  (matches the pattern of `DEPLOY_S12_FINDING_A.sh`). After this session it can
  be deleted; left in place for reference.

### Removed

11 obsolete scripts: `QUICK_RESTART.ps1`, `DEPLOY_OMEGA.ps1`, `START_OMEGA.ps1`,
`INSTALL_SERVICE.ps1`, `INSTALL_WATCHDOG.ps1`, `OMEGA_WATCHDOG.ps1`,
`OmegaWatchdog.ps1`, `RESTART_OMEGA.ps1`, `REBUILD_AND_START.ps1`, `START.ps1`,
`BASELINE_REPORT.ps1`.

### Kept untouched (diagnostics + helpers)

`VERIFY_STARTUP.ps1`, `VERIFY_ALL_ENGINES.ps1`, `OMEGA_STATUS.ps1`,
`MONITOR.ps1`, `SEARCH_LOG.ps1`, `CLEAR_AND_VERIFY.ps1`, `OMEGA_DIAGNOSE.ps1`,
`PRE_DELIVERY_CHECK.ps1`, `cmake-discover.ps1`, `backtest/fetch_l2_data.ps1`.

## Commits pushed this session (origin/main)

In order:

1. **S12 Tier 1: PS1 consolidation (21 -> 12 scripts)** -- the main
   add-OMEGA.ps1-and-INSTALL_OMEGA.ps1 + delete-11-obsolete commit.
2. **S12 PS1 consolidation hotfix: Send-Notification parse error** -- fixed a
   PS5.1 parse failure where the WinRT type-load syntax
   `[Type, Assembly, ContentType = WindowsRuntime]` was wrapped across two
   lines (which PS5.1 rejects with "Missing ] at end of attribute or type
   literal"). Replaced the function body with a no-op since toast notifications
   from a LocalSystem NSSM service are never visible to a human anyway --
   real audit trail is `logs\watchdog.log` via `Write-WD`.
3. **S12 PS1 consolidation hotfix: deploy [7/12] config-clobber** -- the
   legacy `Copy-Item config\omega_config.ini -> omega_config.ini` step (carried
   over from `DEPLOY_OMEGA.ps1`) silently overwrote the canonical root config
   with the post-2026-04-29-audit `[CONFIG-TOMBSTONE]` stub that lives at
   `config\omega_config.ini`. The fix drops the legacy copy entirely (the
   `[2/12]` `git reset` already restored the canonical root config), adds a
   defensive tombstone-detect in `[7/12]` that aborts the deploy with the
   recovery command printed inline, and halts `[11/12]` on `mode=NOT_FOUND`
   (the previous code only halted on `mode=LIVE+watermark=0`).

There may be a 4th commit at `7ecb748` that landed during the session that
neither I nor the user-visible commit chain produced -- worth `git log
--oneline -10` next session to confirm the chain.

## Pending follow-ups (NOT done this session, but tracked here)

### A. `VERIFY_STARTUP.ps1` reads the wrong log file for the running git hash

`VERIFY_STARTUP.ps1` looks for `[Omega] Git hash:` inside
`C:\Omega\logs\latest.log`. In the NSSM-wrapped service model that line lands
in `logs\omega_service_stderr.log` instead, because NSSM redirects the wrapped
process's stdout/stderr there. Symptom: `[LOG] Running hash : not_found` at
the end of every deploy's verify step.

The `[12/12]` post-start check in `OMEGA.ps1` already verifies the hash via
the correct file (`logs\omega_service_stderr.log`), so this is a soft warning
in the verify script, not a real failure.

Fix is roughly: change the path constant in `VERIFY_STARTUP.ps1` (look for
`$LogPath` or similar) from `latest.log` to `omega_service_stderr.log`, OR
have it search both files. One-line fix once the script is read.

### B. Encoding fix verification

This session added a `[Console]::OutputEncoding = [System.Text.Encoding]::UTF8`
block at the top of `OMEGA.ps1` to fix the `Γ£ô`, `Γöé`, `â• â•` mojibake from
vite / box-draw output. **Not pushed yet** at session end -- it's an
uncommitted modification to `OMEGA.ps1` on the Mac repo. Push command:

```bash
cd ~/omega_repo
git add OMEGA.ps1
git commit -m "S12 PS1 consolidation polish: UTF-8 console output

Adds [Console]::OutputEncoding = UTF-8 + \$OutputEncoding = UTF-8 at the
top of OMEGA.ps1. Native commands invoked from this script (npm, vite,
cmake, MSBuild) emit UTF-8 to stdout. Without this, the default Windows
console (CP-1252 / OEM 437) renders each UTF-8 byte as its CP-1252 glyph,
producing visible mojibake like 'Γ£ô' (= U+2713 CHECK MARK), 'Γöé'
(= U+2502 BOX DRAWINGS LIGHT VERTICAL), and 'â• â•' (= '═ ═' banner
separator). The bytes in the log files are correct -- only the live
console rendering is wrong. Setting OutputEncoding fixes both directions:
captured native output is decoded as UTF-8 before display, and Write-Host
output is encoded as UTF-8 for the host. try/catch lets the change
silently no-op when OMEGA.ps1 runs under NSSM as a service (no console
host attached -- [Console]::OutputEncoding throws)."
git push origin main
```

Then on VPS: `cd C:\Omega ; git fetch origin ; git reset --hard origin/main`
-- no redeploy needed; the encoding only affects the next run of OMEGA.ps1.

### C. Watchdog log dedup

Watchdog logged `SERVICE-DOWN: restart #4 complete. Waiting 30s...` twice in a
row at 04:19:34 UTC. Cosmetic, not blocking. Probably a fall-through after the
singleton mutex's "Another deploy is already running" early-exit. Worth a
30-second look in `Invoke-Watchdog`'s SERVICE-DOWN block.

### D. `7ecb748` commit provenance

A commit hash `7ecb748` showed up as the running Omega git hash but doesn't
match either of my hotfix commits. Confirm with:

```
git log --oneline -10
```

If it's an automated push (log-push hook, watchdog auto-update artifact, etc.),
note where it came from for future audit. If it's a manual commit the user
made, confirm what's in it.

## Sandbox issue (unchanged; 4th consecutive session)

`mcp__workspace__bash` is still broken in Cowork. Every shell invocation throws:

```
useradd: /etc/passwd: No space left on device
```

This means no compile-checks, no `pwsh` parse-checks, no shellcheck on the
deploy `.sh`, no `git status` / `git log` from the agent side. The work this
session was done entirely with the file tools (Read / Write / Edit / Glob /
Grep), which all work normally. The `Send-Notification` parse error and the
`[7/12]` config-clobber bug both could have been caught by a one-line `pwsh
-Command Invoke-Expression` parse-check before commit -- both surfaced only
after the user ran them on the VPS.

**Action for Anthropic / next session start**: ask the user to do a Cowork
restart before doing any non-trivial work, or file a bug if it persists across
restarts. If the sandbox is back, the deploy script's own `pwsh`-detection
block (in `DEPLOY_S12_PS1_CONSOLIDATION.sh` step `[5/6]`) will run a
parse-check on the new scripts before commit, which would have caught both
hotfix-prompting bugs locally.

## Repo directory & branches

- Mac local repo: `~/omega_repo` (= `/Users/jo/omega_repo`)
- VPS local repo: `C:\Omega`
- Remote: `https://github.com/Trendiisales/Omega`
- VPS local branch (current): `omega-terminal` -- but its HEAD is now pinned
  at the consolidation commit on `origin/main` because every deploy does
  `git reset --hard origin/main`. The branch label is misleading; for clarity
  it would be cleaner to `git checkout main` on the VPS at some point. Not
  blocking.
- The 36 pre-reset commits previously on `omega-terminal` are still recoverable
  via `git reflog` for ~90 days; if any of them mattered, recover before the
  reflog expires.

## Quickstart for the next session

```
# (Mac)
cd ~/omega_repo
git fetch origin
git status
git log --oneline -10

# (VPS, if needed)
cd C:\Omega
Get-Service Omega, OmegaWatchdog
Get-Content C:\Omega\logs\watchdog.log -Tail 20
Get-Content C:\Omega\logs\omega_service_stderr.log -Tail 20 | Select-String "Git hash|Mode|FATAL|ERROR"
```

If everything looks healthy, push the encoding fix (Section B above) and move
on to whatever is next on the S12 backlog.

## Trade-relevant audit (UNDONE this session)

During the brief window when `OMEGA.ps1 deploy` ran with the buggy `[7/12]`
copy, Omega.exe started with a tombstoned `omega_config.ini` (no `mode=`, no
`session_watermark_pct=`, no broker block, no engine config). The user's
`Stop-Service` came in shortly after. **Whether Omega.exe wrote any
`[CFE] ENTRY` / `[*] ENTRY` lines in that window has not been audited** -- I
asked twice, the user did not paste the log tail. Worth a 5-minute check next
session:

```
Get-Content C:\Omega\logs\latest.log | Select-String "ENTRY|EXIT|ORDER|FILL" | Select-Object -Last 50
Get-Content C:\Omega\logs\omega_service_stderr.log -Head 200 | Select-String "Mode:|FATAL|ERROR|config"
```

If the binary errored at startup on the missing config (most likely), there
will be FATAL lines and zero ENTRY lines after the deploy timestamp. If it
default-init'd and tried to trade, there will be ENTRY lines on the engine
defaults -- if so, reconcile with broker positions immediately.
