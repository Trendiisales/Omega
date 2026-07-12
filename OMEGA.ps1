#Requires -Version 5.1
# ==============================================================================
#                     OMEGA  --  UNIFIED CONTROL SCRIPT
#                     S12 PS1 Consolidation, 2026-05-07
# ==============================================================================
#
# Replaces (and supersedes) the following pre-S12 scripts:
#   QUICK_RESTART.ps1   v3.5   --> .\OMEGA.ps1 deploy
#   DEPLOY_OMEGA.ps1            --> .\OMEGA.ps1 deploy   (stamp/symbols/watermark
#                                   validation merged in)
#   START_OMEGA.ps1             --> .\OMEGA.ps1 start
#   OMEGA_WATCHDOG.ps1  v2.0    --> .\OMEGA.ps1 watchdog (used by
#                                   INSTALL_OMEGA.ps1 -InstallWatchdog)
#
# Subcommands:
#   deploy    Full pipeline: stop service, git pull, build (UI + C++), copy
#             assets, write+validate stamp, verify symbols/watermark, start
#             service, run stale-binary checks, optionally run VERIFY_STARTUP.
#   restart   Stop service + start service (no rebuild). Stamp-verifies the
#             on-disk Omega.exe BEFORE starting; hard-blocks on mismatch.
#   start     Start service (or attach if running). Stamp-verifies BEFORE
#             starting; hard-blocks on mismatch. Warns if origin/main has new
#             source commits beyond what is currently stamped.
#   stop      Graceful Stop-Service with -ForceKill fallback. CFE-open warning
#             prompt before stopping a Running service.
#   watchdog  Long-running monitor loop. Checks service status, latest.log
#             staleness, L2 CSV staleness, GitHub HEAD vs running hash, and
#             defers any restart while open positions are detected. Exists as
#             an internal subcommand and is intended to be invoked by NSSM
#             via the OmegaWatchdog service installed by INSTALL_OMEGA.ps1.
#   help      Show usage and exit.
#
# Architecture:
#   Omega.exe runs as an NSSM-wrapped Windows service named "Omega" (auto-start,
#   LocalSystem). NSSM redirects stdout+stderr to logs\omega_service_*.log.
#   This script ALWAYS uses Stop-Service / Start-Service for Omega -- never
#   Start-Process. The watchdog is a separate NSSM service named "OmegaWatchdog"
#   that invokes `OMEGA.ps1 watchdog` and calls `OMEGA.ps1 deploy -SkipVerify`
#   when an auto-update / crash / log-stale restart is needed.
#
# Hardening preserved verbatim from sources:
#   * v3.0 -- service-based stop/start (Stop-Service / Start-Service); no
#             Start-Process for Omega anywhere.
#   * v3.1 -- $LASTEXITCODE guards after every git/cmake call; build failure
#             auto-restarts service with the previous Omega.exe so live trading
#             never sees a silent gap.
#   * v3.2 -- $zombieNames hoisted to script scope; defensive pre-wipe re-kill;
#             both wipe paths (full + per-file) wrapped in 5-attempt
#             exponential backoff (200/400/800/1600/3200 ms) with kill-between.
#   * v3.3 -- Format-NativeOutputLine unwraps ErrorRecord -> RemoteException
#             so MSBuild stderr (C2065/LNK#### etc) is no longer mangled to
#             '[System.Management.Automation.RemoteException]'.
#   * v3.4 -- -Branch parameter (default 'main'); all three Step-2 git
#             references use $Branch / "origin/$Branch".
#   * v3.5 -- omega-terminal UI auto-build (npm ci + npm run build) before
#             the C++ build; UI failures route through the same [RECOVERY]
#             path as a C++ build failure.
#   * SOURCE_HASH walk (skip commits whose only changes are under logs/) so
#     the GUI version badge and the omega_build.stamp agree, even when a
#     CMake POST_BUILD push_log step has moved HEAD past the source commit.
#   * EXE_SHA256 self-validation pass after stamp write; cross-check between
#     stamp source hash and version_generated.hpp OMEGA_GIT_HASH.
#   * symbols.ini per-symbol numeric verification (XAUUSD 6.00/12.00 etc).
#   * Watermark+mode hard block (mode=LIVE with session_watermark_pct=0).
#   * Stale-binary detection: process EXE LastWriteTimeUtc within 10s of the
#     freshly-built Omega.exe AND `[Omega] Git hash:` line in stderr matches.
#   * Watchdog SAFE-TO-RESTART: telemetry probe of /api/telemetry; defer any
#     restart while live_trades/open_positions count > 0 OR is unknown.
#   * Deploy sentinel (deploy_in_progress.flag): watchdog stays in re-attach
#     mode instead of treating a deploy stop as a crash.
#   * NSSM cmake-discover.ps1 dot-sourced (glob-based, version-bump-proof).
#
# Borders are ASCII-only ('=', '-', '*') because this file's encoding may be
# saved as UTF-8 without BOM by tools that don't preserve PS5.1's box-draw
# parsing rules. The semantics are unchanged.
# ==============================================================================

[CmdletBinding(PositionalBinding=$false)]
param(
    # ----- Subcommand dispatch -----
    [Parameter(Position=0)]
    [ValidateSet('deploy','restart','start','stop','watchdog','help','')]
    [string]$Command = '',

    # ----- Common parameters -----
    [string]$OmegaDir       = "C:\Omega",
    [string]$Branch         = "main",
    [string]$GitHubToken    = "",
    [int]   $StopTimeoutSec = 30,
    [int]   $StartupWaitSec = 15,
    [switch]$SkipVerify,
    [switch]$ForceKill,
    # -Fast: incremental build. Preserves build/*.obj/*.pch and SKIPS the force-
    # touch, so MSBuild recompiles ONLY the TUs whose sources changed (after git
    # reset, just the edited files have a new mtime) + relinks -- ~2-4min vs the
    # 7-10min full rebuild. Use for routine code-only deploys you trust. The full
    # safe path (wipe-on-crash always still fires) is the DEFAULT when -Fast is off.
    [switch]$Fast,

    # -Clean: force a from-scratch rebuild (wipe .obj + force-touch all sources).
    # S-2026-06-20: deploys now default to INCREMENTAL (only changed TUs recompile;
    # the 26 vendored TWS .obj never change -> never rebuilt). The old default
    # recompiled all 33 TUs every deploy (~161s). Staleness is covered by the
    # build-time git-hash regen + the post-deploy "hash confirmed in log" verify.
    # Use -Clean on a CMakeLists/toolchain change or any doubt. A prior crashed
    # build (unsuccessfulbuild stamp) STILL force-wipes regardless of -Clean.
    [switch]$Clean,

    # -ColdStop: legacy deploy ordering -- stop the service BEFORE pull+build,
    # leaving Omega down for the entire UI+C++ build (6-10 min observed).
    # DEFAULT (off) is HOT-SWAP: the service keeps running on the previous
    # binary through git pull, warmup-CSV regen, UI build and C++ build; it is
    # stopped only at [7/12] for the exe copy + stamp + restart, so downtime is
    # the stop/copy/start window (~20-40 s). Build failures under hot-swap
    # leave the running service untouched (no recovery restart needed).
    # Added S-2026-06-12e after operator flagged 6-7 min deploy downtime.
    [switch]$ColdStop,

    # -SkipSeed / -ForceSeed (S-2026-06-29 DEPLOY-SPEED):
    # the [2b] warm-seed refresh (rebuild from l2_ticks + IBKR refresh + audit)
    # runs EVERY deploy and can block up to 300s reaching IBKR 4001 -- pure dead
    # time when the on-disk seeds are already fresh. New default: AUTO-SKIP when the
    # cheap freshness audit (tools/seed_freshness_audit.py, reads last-bar age, no
    # IBKR) reports every ENABLED-engine seed fresh on disk AFTER the git reset.
    # Audit stale -> reseed. -ForceSeed always reseeds; -SkipSeed never reseeds.
    # (Stamp-based gating was removed: the deploy's git reset --hard reverts the
    # committed warmup CSVs, so a "recently seeded" stamp does NOT mean the on-disk
    # seeds are fresh -- it caused a 17-stale boot. Measure the files, not a proxy.)
    # -AllowStaleSeed: override the [2c] FAIL-CLOSED gate. By default a deploy ABORTS
    # (keeps the prior live binary via hot-swap) if seeds are still stale after [2b] --
    # so a blind binary can never silently ship. Set this ONLY to deliberately ship
    # code while IBKR 4001 is down (gateway maintenance) and you accept stale seeds.
    [switch]$ForceSeed,
    [switch]$SkipSeed,
    [switch]$AllowStaleSeed,

    # ----- watchdog subcommand parameters -----
    [int]   $StaleThresholdSec      = 60,
    [int]   $L2StaleThresholdSec    = 120,
    [int]   $CheckIntervalSec       = 15,
    [int]   $PostRestartWaitSec     = 30,
    [int]   $GitHubPollIntervalSec  = 300,
    [string]$TelemetryUrl           = "http://localhost:7781/api/v1/omega/positions",  # S-2026-06-25: was :7779/api/telemetry (market prices, no open_positions field) -> telemetry_healthy always False + safe-to-restart blind. This route returns the live open-positions JSON array.
    [int]   $TelemetryTimeoutSec    = 5
)

Set-StrictMode -Off
$ErrorActionPreference = "Continue"

# ==============================================================================
# Console output encoding (fixes vite / box-draw mojibake)
# ------------------------------------------------------------------------------
# Native commands under PS5.1 (npm, vite, cmake, MSBuild) emit UTF-8 to stdout,
# but the default Windows console runs on the legacy OEM/CP-1252 code page,
# which renders each UTF-8 byte as its CP-1252 glyph. The visible symptoms in
# deploy output are 'Γ£ô' (= UTF-8 0xE2 0x9C 0x93 = U+2713 CHECK MARK) and
# 'Γöé' (= UTF-8 0xE2 0x94 0x82 = U+2502 BOX DRAWINGS LIGHT VERTICAL), and
# 'â• â•' for VERIFY_STARTUP's banner separator. The bytes in the log files
# are correct -- only the live console rendering is wrong.
#
# Setting [Console]::OutputEncoding to UTF-8 here makes the host decode native
# stdout as UTF-8 and re-encode it for whatever font/encoding the terminal is
# using. The try/catch lets this silently no-op when OMEGA.ps1 runs under NSSM
# as a service (no console host attached, [Console]::OutputEncoding throws).
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding           = [System.Text.Encoding]::UTF8
} catch { }

# ==============================================================================
# S60 2026-05-07 -- BRANCH GUARD
# ------------------------------------------------------------------------------
# Hard-block any non-help OMEGA.ps1 command unless the working tree at
# $OmegaDir is checked out on the expected branch (default 'main', or
# whatever -Branch was passed). Closes the foot-gun where running
# 'git pull origin main' from a different branch fast-forwards THAT branch
# to origin/main and leaves HEAD on it -- subsequent 'git pull' calls
# (with no args) then sync against the wrong upstream and silently miss
# new pushes to origin/main, so the live VPS runs an old binary.
#
# 2026-05-07 incident: VPS was on 'omega-terminal' for an unknown number
# of pulls; origin/main had moved 2 commits beyond (analyzer script +
# .ps1 wrapper) but neither file ever appeared in the working tree. The
# deploy ran an outdated source and the analyzer command failed with
# 'file not found' until the operator noticed the wrong-branch state.
#
# Behaviour: if HEAD branch != $Branch, print remediation and exit 1.
#   * Skipped for 'help' and the empty default command (so users can
#     always read the help text without the repo being in any state).
#   * Skipped if $OmegaDir is not a git working tree (no .git/), so
#     non-deploy uses of this script don't trip the guard.
#   * Override path: pass -Branch <name> explicitly to run on a non-main
#     branch deliberately (the guard compares HEAD to $Branch, not a
#     hard-coded 'main', so explicit opt-in is honoured).
# ==============================================================================
if ($Command -ne '' -and $Command -ne 'help') {
    if (Test-Path "$OmegaDir\.git") {
        $currentBranch = ""
        try {
            Push-Location $OmegaDir
            $currentBranch = (& git branch --show-current 2>$null | Out-String).Trim()
            Pop-Location
        } catch {
            try { Pop-Location } catch { }
        }
        if ($currentBranch -and $currentBranch -ne $Branch) {
            Write-Host ""
            Write-Host "==========================================================" -ForegroundColor Red
            Write-Host " BRANCH GUARD: refusing to run." -ForegroundColor Red
            Write-Host "==========================================================" -ForegroundColor Red
            Write-Host "  $OmegaDir is on branch '$currentBranch'" -ForegroundColor Yellow
            Write-Host "  but OMEGA.ps1 expects '$Branch'." -ForegroundColor Yellow
            Write-Host ""
            Write-Host "  This usually means 'git pull origin main' was run" -ForegroundColor Yellow
            Write-Host "  while a different branch was checked out, fast-" -ForegroundColor Yellow
            Write-Host "  forwarding the wrong branch and leaving HEAD on it." -ForegroundColor Yellow
            Write-Host ""
            Write-Host "  To fix:" -ForegroundColor Cyan
            Write-Host "    cd $OmegaDir" -ForegroundColor Cyan
            Write-Host "    git checkout $Branch" -ForegroundColor Cyan
            Write-Host "    git pull" -ForegroundColor Cyan
            Write-Host ""
            Write-Host "  If you genuinely intend to run on '$currentBranch'," -ForegroundColor Cyan
            Write-Host "  re-invoke OMEGA.ps1 with -Branch $currentBranch." -ForegroundColor Cyan
            Write-Host "==========================================================" -ForegroundColor Red
            Write-Host ""
            exit 1
        }
    }
}

# ==============================================================================
# CONFIG-DRIFT GUARD (S-2026-06-18) -- pre-build gate
# ------------------------------------------------------------------------------
# Stops the "claimed PF != deployed/faithful config" falsification class (the NAS
# PF2.69-vs-shipped-1.26 case). Hard-blocks the build ONLY when a deploy-line PF
# claim in engine_init.hpp CONTRADICTS the faithful audit in
# backtest/AUDITED_CONFIGS.tsv. Unbacked claims / marginal-shadow enables / owed
# audits are reported as warnings (non-blocking). Python-tolerant: if python is
# absent, warn and continue (never break the Windows build over a missing tool).
# ==============================================================================
# S-2026-06-22: the drift guard MOVED into Invoke-Deploy, AFTER the git reset
# ([2a/12]), so it checks the FRESHLY-PULLED tree -- not the stale working tree.
# Old placement here ran pre-pull => a guard-failing committed state could not be
# fixed by deploying its fix (the deploy aborted before pulling it; on 2026-06-22
# this forced a manual `git reset --hard origin/main` on the VPS). See Invoke-DriftGuard.
function Invoke-DriftGuard {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $py) {
        Write-Host "[DRIFT-GUARD] python not found -- skipping config-drift check (non-fatal)." -ForegroundColor Yellow
        return $true
    }
    Write-Host "[DRIFT-GUARD] checking config-drift (engine_init claims vs faithful audit)..." -ForegroundColor Cyan
    & $py.Source "$OmegaDir\tools\config_drift_guard.py"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "==========================================================" -ForegroundColor Red
        Write-Host " CONFIG-DRIFT GUARD FAILED -- a deploy-line PF claim" -ForegroundColor Red
        Write-Host " contradicts backtest/AUDITED_CONFIGS.tsv. Fix the comment" -ForegroundColor Red
        Write-Host " or re-audit + update the manifest before deploying." -ForegroundColor Red
        Write-Host "==========================================================" -ForegroundColor Red
        return $false
    }
    return $true
}

# ==============================================================================
# Common variables
# ==============================================================================
$ServiceName  = "Omega"
$OmegaExe     = "$OmegaDir\Omega.exe"
$BuildExe     = "$OmegaDir\build\Release\Omega.exe"
$BuildDir     = "$OmegaDir\build"
$ConfigSrc    = "$OmegaDir\omega_config.ini"
$LogStdout    = "$OmegaDir\logs\omega_service_stdout.log"
$LogStderr    = "$OmegaDir\logs\omega_service_stderr.log"
$LatestLog    = "$OmegaDir\logs\latest.log"
$StampFile    = "$OmegaDir\omega_build.stamp"
$WatchdogLog  = "$OmegaDir\logs\watchdog.log"
$DeployFlag   = "$OmegaDir\deploy_in_progress.flag"
$TokenFile    = "$OmegaDir\.github_token"

# Build-process zombie names (hoisted to script scope per v3.2). Single source
# of truth for Step 0, defensive pre-wipe re-kill, and per-attempt re-kill in
# the wipe retry loops.
$zombieNames = @('cl', 'link', 'MSBuild', 'mspdbsrv', 'tracker',
                 'VBCSCompiler', 'cvtres', 'rc', 'cmake', 'cmake-gui',
                 'lib', 'mt', 'ml', 'cvtcil', 'CL')

if ($GitHubToken -eq "" -and (Test-Path $TokenFile)) {
    try { $GitHubToken = (Get-Content $TokenFile -Raw).Trim() } catch { }
}

# ==============================================================================
# Common helpers
# ==============================================================================

function Format-NativeOutputLine {
    # v3.3 audit-fixes-33 -- unwraps an ErrorRecord (raised when PowerShell 5.1
    # marshals native stderr through `2>&1`) into the underlying text. When the
    # wrapped exception is a RemoteException (MSBuild structured output),
    # ToString() flattens to the literal '[System.Management.Automation
    # .RemoteException]' and the actual diagnostic is dropped on the floor.
    param($Obj)
    if ($null -eq $Obj) { return "" }
    if ($Obj -is [System.Management.Automation.ErrorRecord]) {
        if ($Obj.Exception -and -not [string]::IsNullOrEmpty($Obj.Exception.Message)) {
            return $Obj.Exception.Message
        }
        if ($null -ne $Obj.TargetObject) {
            return [string]$Obj.TargetObject
        }
    }
    return $Obj.ToString()
}

$script:lastWdMsg = $null
$script:lastWdMsgTime = [DateTime]::MinValue

function Write-WD {
    # Watchdog log helper. Mirrors line to host AND appends to watchdog.log.
    # Dedup guard (added 2026-05-07): suppress consecutive identical messages
    # within 5s -- prevents the rare double-log race seen at watchdog.log:204/205
    # ("SERVICE-DOWN: restart #10 complete" appearing twice at 04:16:24).
    param([string]$msg)
    $now = Get-Date
    if ($msg -eq $script:lastWdMsg -and ($now - $script:lastWdMsgTime).TotalSeconds -lt 5) { return }
    $script:lastWdMsg     = $msg
    $script:lastWdMsgTime = $now
    $ts = $now.ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")
    $line = "$ts [WATCHDOG] $msg"
    Write-Host $line
    try { Add-Content -Path $WatchdogLog -Value $line -ErrorAction SilentlyContinue } catch { }
}

function Send-Notification {
    # No-op stub. The original implementation (carried over from
    # OmegaWatchdog.ps1) used the WinRT type-load syntax
    #   [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
    # which the PS5.1 parser only accepts on a single line. When wrapped across
    # two lines (as the source file did) the parser hits the comma+newline,
    # treats the construct as an attribute/type literal, and reports
    # 'Missing ] at end of attribute or type literal' -- a parse-time error
    # that prevents the entire script from loading, killing the watchdog NSSM
    # service on start.
    #
    # Even with the syntax fixed, the function is pointless in the deployed
    # environment: when OMEGA.ps1 runs as an NSSM-wrapped LocalSystem service
    # (the only context in which the watchdog calls this), there is no
    # interactive desktop session, so the toast never reaches a human. The
    # real audit trail is logs\watchdog.log via Write-WD, which every caller
    # of Send-Notification also already writes to. Keeping this as a no-op
    # eliminates both the parse fragility and a misleading "notification was
    # sent" code path that does nothing useful.
    param([string]$Title, [string]$Body)
}

function Test-CfeOpen {
    # Returns $true if the tail of the service stdout log shows an unmatched
    # [CFE] ENTRY (i.e. no matching [CFE] EXIT after it). Used as a pre-stop
    # warning in `deploy`/`restart`/`stop` so the operator can choose whether
    # to force-close at current market price.
    if (-not (Test-Path $LogStdout)) { return $false }
    $tail = Get-Content $LogStdout -Tail 1000 -ErrorAction SilentlyContinue
    if (-not $tail) { return $false }
    $lastEntry = ($tail | Select-String "\[CFE\] ENTRY") | Select-Object -Last 1
    $lastExit  = ($tail | Select-String "\[CFE\] EXIT")  | Select-Object -Last 1
    if (-not $lastEntry) { return $false }
    if (-not $lastExit) { return $true }
    return ($lastEntry.LineNumber -gt $lastExit.LineNumber)
}

function Confirm-CfePosition {
    # If CFE position is open, prompt the operator. Returns $true if it is OK
    # to proceed with the stop, $false if the operator aborted.
    if (-not (Test-CfeOpen)) {
        Write-Host "  [OK] No open CandleFlow position detected" -ForegroundColor Green
        return $true
    }
    $tail = Get-Content $LogStdout -Tail 1000 -ErrorAction SilentlyContinue
    $lastEntry = ($tail | Select-String "\[CFE\] ENTRY") | Select-Object -Last 1
    Write-Host ""
    Write-Host "  *********************************************************" -ForegroundColor Red
    Write-Host "  *  WARNING: CandleFlow position is OPEN                 *" -ForegroundColor Red
    Write-Host "  *  Stopping now will FORCE CLOSE at current market price *" -ForegroundColor Red
    Write-Host "  *********************************************************" -ForegroundColor Red
    Write-Host ""
    if ($lastEntry) {
        Write-Host "  Last entry:" -ForegroundColor Yellow
        Write-Host "  $($lastEntry.Line)" -ForegroundColor Yellow
        Write-Host ""
    }
    Write-Host "  Type YES to force-stop anyway (will take the loss): " -ForegroundColor Red -NoNewline
    $userConfirm = Read-Host
    if ($userConfirm -ne "YES") {
        Write-Host ""
        Write-Host "  Stop cancelled. Wait for position to close then retry." -ForegroundColor Green
        Write-Host ""
        return $false
    }
    Write-Host "  Proceeding..." -ForegroundColor Red
    Write-Host ""
    return $true
}

function Read-Stamp {
    # Parses omega_build.stamp into a hashtable. Returns $null if the stamp is
    # missing. Empty/missing fields are returned as empty strings.
    if (-not (Test-Path $StampFile)) { return $null }
    $lines = Get-Content $StampFile -ErrorAction SilentlyContinue
    if (-not $lines) { return $null }
    $h = @{
        GIT_HASH       = (($lines | Where-Object { $_ -match '^GIT_HASH=' })       -replace '^GIT_HASH=',       '').Trim()
        GIT_HASH_SHORT = (($lines | Where-Object { $_ -match '^GIT_HASH_SHORT=' }) -replace '^GIT_HASH_SHORT=', '').Trim()
        HEAD_HASH      = (($lines | Where-Object { $_ -match '^HEAD_HASH=' })      -replace '^HEAD_HASH=',      '').Trim()
        EXE_SHA256     = (($lines | Where-Object { $_ -match '^EXE_SHA256=' })     -replace '^EXE_SHA256=',     '').Trim()
        BUILD_TIME     = (($lines | Where-Object { $_ -match '^BUILD_TIME=' })     -replace '^BUILD_TIME=',     '').Trim()
        EXE_PATH       = (($lines | Where-Object { $_ -match '^EXE_PATH=' })       -replace '^EXE_PATH=',       '').Trim()
    }
    return $h
}

function Test-StampMatchesExe {
    # Returns $true iff omega_build.stamp's EXE_SHA256 matches the SHA256 of
    # $OmegaExe. All other failure modes (missing stamp, missing exe, empty
    # field) return $false. The `start` and `restart` subcommands hard-block
    # when this returns $false.
    if (-not (Test-Path $OmegaExe)) {
        Write-Host "  [STAMP] $OmegaExe not found" -ForegroundColor Red
        return $false
    }
    $stamp = Read-Stamp
    if (-not $stamp) {
        Write-Host "  [STAMP] omega_build.stamp not found -- run OMEGA.ps1 deploy first" -ForegroundColor Red
        return $false
    }
    if ([string]::IsNullOrWhiteSpace($stamp.EXE_SHA256)) {
        Write-Host "  [STAMP] EXE_SHA256 field empty -- corrupt stamp" -ForegroundColor Red
        return $false
    }
    $actual = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash.Trim()
    if ($actual -ne $stamp.EXE_SHA256) {
        Write-Host "  [STAMP] EXE SHA256 mismatch:" -ForegroundColor Red
        Write-Host "          stamp  = $($stamp.EXE_SHA256.Substring(0,16))..." -ForegroundColor Red
        Write-Host "          actual = $($actual.Substring(0,16))..." -ForegroundColor Red
        return $false
    }
    return $true
}

function Stop-OmegaService {
    # Service-based graceful stop. Polls for Stopped state up to $StopTimeoutSec
    # then escalates to taskkill /F /IM Omega.exe /T. Mops up any straggler
    # Omega.exe processes. Returns $true on success, $false on terminal failure.
    param([switch]$Force)

    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $svc) {
        Write-Host "  [FATAL] Omega service does not exist. Run INSTALL_OMEGA.ps1 first." -ForegroundColor Red
        return $false
    }

    Write-Host "  Current status: $($svc.Status)" -ForegroundColor Cyan

    if ($svc.Status -ne 'Running' -and $svc.Status -ne 'StartPending') {
        Write-Host "  [OK] Service already stopped" -ForegroundColor Green
        return $true
    }

    if ($Force) {
        Write-Host "  -ForceKill specified -- killing Omega.exe processes directly" -ForegroundColor Yellow
        taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
        Start-Sleep -Seconds 2
        & sc.exe stop $ServiceName 2>&1 | Out-Null
    } else {
        Write-Host "  Sending graceful stop signal..." -ForegroundColor Cyan
        try {
            Stop-Service -Name $ServiceName -Force -ErrorAction Stop
        } catch {
            Write-Host "  [WARN] Stop-Service threw: $_" -ForegroundColor Yellow
        }
        $stopStart = Get-Date
        $stopped   = $false
        while (((Get-Date) - $stopStart).TotalSeconds -lt $StopTimeoutSec) {
            $s = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            if ($s -and $s.Status -eq 'Stopped') { $stopped = $true; break }
            $elapsedSec = [math]::Floor(((Get-Date) - $stopStart).TotalSeconds)
            Write-Host ("  Waiting for service stop... {0}s / {1}s (status={2})" -f $elapsedSec, $StopTimeoutSec, $s.Status) -ForegroundColor DarkGray
            Start-Sleep -Seconds 3
        }
        if ($stopped) {
            $elapsedStopped = [math]::Round(((Get-Date) - $stopStart).TotalSeconds, 1)
            Write-Host "  [OK] Service stopped cleanly in ${elapsedStopped}s" -ForegroundColor Green
        } else {
            Write-Host "  [WARN] Service did not stop in ${StopTimeoutSec}s. Escalating to taskkill /F." -ForegroundColor Yellow
            taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
            Start-Sleep -Seconds 3
            & sc.exe stop $ServiceName 2>&1 | Out-Null
            Start-Sleep -Seconds 2
        }
    }

    # Mop up stragglers
    for ($i = 0; $i -lt 5; $i++) {
        $still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
        if (-not $still) { break }
        Write-Host "  Killing straggler Omega.exe (PID $($still.Id -join ','))" -ForegroundColor Yellow
        taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
        Start-Sleep -Seconds 2
    }

    $finalCheck = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if ($finalCheck) {
        Write-Host "  [FATAL] Omega.exe still running after all kill attempts." -ForegroundColor Red
        return $false
    }
    $svcFinal = Get-Service -Name $ServiceName
    if ($svcFinal.Status -ne 'Stopped') {
        Write-Host "  [FATAL] Service status is $($svcFinal.Status), expected Stopped." -ForegroundColor Red
        return $false
    }
    Write-Host "  [OK] Confirmed no Omega.exe process AND service=Stopped" -ForegroundColor Green
    return $true
}

function Start-OmegaService {
    # Calls Start-Service and polls for Running state up to 20s. Returns $true
    # on success, $false on timeout / Start-Service exception. Caller is
    # responsible for any post-start verification (stale-binary checks etc).
    try {
        Start-Service -Name $ServiceName -ErrorAction Stop
    } catch {
        Write-Host "  [FATAL] Start-Service failed: $_" -ForegroundColor Red
        return $false
    }
    $startPollStart = Get-Date
    while (((Get-Date) - $startPollStart).TotalSeconds -lt 20) {
        $s = Get-Service -Name $ServiceName
        if ($s.Status -eq 'Running') {
            Write-Host "  [OK] Service=Running" -ForegroundColor Green
            return $true
        }
        Start-Sleep -Seconds 1
    }
    Write-Host "  [FATAL] Service did not reach Running state in 20s. Check NSSM event log." -ForegroundColor Red
    return $false
}

function Rotate-ServiceLogs {
    # Moves omega_service_stdout.log + omega_service_stderr.log into
    # logs\archive\ with a timestamp suffix. Falls back to Clear-Content on
    # locked files. Lets the post-start verify scan find a fresh banner.
    if (-not (Test-Path $LogStdout)) { return }
    $archiveDir = "$OmegaDir\logs\archive"
    if (-not (Test-Path $archiveDir)) { New-Item -ItemType Directory -Path $archiveDir -Force | Out-Null }
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    try {
        Move-Item -Path $LogStdout -Destination "$archiveDir\omega_service_stdout_$stamp.log" -Force -ErrorAction Stop
        Write-Host "  [OK] Rotated stdout log -> archive\omega_service_stdout_$stamp.log" -ForegroundColor DarkGray
    } catch {
        Write-Host "  [WARN] Could not rotate stdout log (locked?): $_ -- attempting truncate" -ForegroundColor Yellow
        try { Clear-Content $LogStdout -ErrorAction Stop }
        catch { Write-Host "  [WARN] Truncate also failed: $_" -ForegroundColor Yellow }
    }
    if (Test-Path $LogStderr) {
        try { Move-Item -Path $LogStderr -Destination "$archiveDir\omega_service_stderr_$stamp.log" -Force -ErrorAction Stop }
        catch { }
    }
}

function Test-StaleBinaryAfterStart {
    # Post-start stale-binary checks (v3.5):
    #   1. Resolve the running Omega.exe's path via Win32_Process.ExecutablePath
    #      and compare its LastWriteTimeUtc to the freshly-built Omega.exe.
    #      Diff > 10s = wrong binary running.
    #   2. Tail logs\omega_service_stderr.log for `[Omega] Git hash: <sha7>`
    #      and confirm it matches the expected short hash. Poll up to 30s.
    # Returns $true if both checks pass, $false otherwise.
    param([string]$ExpectedShortHash)

    Start-Sleep -Seconds $StartupWaitSec

    $runningProc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if (-not $runningProc) {
        Write-Host "  [FATAL] Omega.exe not running ${StartupWaitSec}s after service start" -ForegroundColor Red
        Write-Host "  Check: $LogStdout" -ForegroundColor Red
        return $false
    }
    $runningPid = ($runningProc | Select-Object -First 1).Id
    $cimProc    = Get-CimInstance Win32_Process -Filter "ProcessId = $runningPid" -ErrorAction SilentlyContinue
    if (-not $cimProc -or [string]::IsNullOrEmpty($cimProc.ExecutablePath)) {
        Write-Host "  [FATAL] Cannot determine path of running Omega.exe (PID $runningPid)" -ForegroundColor Red
        return $false
    }
    $runningExePath = $cimProc.ExecutablePath
    try {
        $runningExeTime = (Get-Item $runningExePath -ErrorAction Stop).LastWriteTimeUtc
        $builtExeTime   = (Get-Item $OmegaExe -ErrorAction Stop).LastWriteTimeUtc
    } catch {
        Write-Host "  [FATAL] Cannot stat exe timestamps: $_" -ForegroundColor Red
        return $false
    }
    $diffSec = [math]::Abs(($runningExeTime - $builtExeTime).TotalSeconds)
    if ($diffSec -gt 10) {
        Write-Host ""
        Write-Host "  *********************************************************" -ForegroundColor Red
        Write-Host "  *  WRONG BINARY RUNNING -- ABORTING                     *" -ForegroundColor Red
        Write-Host "  *  Running EXE : $runningExePath" -ForegroundColor Red
        Write-Host "  *  Running time: $($runningExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
        Write-Host "  *  Expected    : $($builtExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
        Write-Host "  *  Diff: ${diffSec}s -- old binary running!" -ForegroundColor Red
        Write-Host "  *********************************************************" -ForegroundColor Red
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        return $false
    }
    Write-Host "  [OK] EXE timestamp matches built binary (+${diffSec}s)" -ForegroundColor Green

    if (-not $ExpectedShortHash) {
        # Caller has no hash to compare (e.g. start without a build). Skip
        # the log-hash assertion -- the SHA256 stamp check already covered it.
        return $true
    }

    $hashFound = $false
    $hashInLog = ""
    for ($hi = 0; $hi -lt 15; $hi++) {
        if (Test-Path $LogStderr) {
            $tail = Get-Content $LogStderr -Tail 200 -ErrorAction SilentlyContinue
            $hashLine = $tail | Where-Object { $_ -match "\[Omega\] Git hash:" } | Select-Object -Last 1
            if ($hashLine -and $hashLine -match "Git hash:\s*([0-9a-f]{7,40})") {
                $hashInLog = $Matches[1]
                $hashFound = $true
                break
            }
        }
        Start-Sleep -Seconds 2
    }

    if (-not $hashFound) {
        Write-Host "  [FATAL] Git hash line not found in log after 30s" -ForegroundColor Red
        Write-Host "  Check: $LogStderr" -ForegroundColor Red
        return $false
    }
    # Prefix-compare so a length mismatch between the logged hash and
    # $ExpectedShortHash is NOT a false "stale" (e.g. 7-char log vs 8-char
    # expected). Stale only when neither is a prefix of the other.
    $hashOk = $hashInLog.StartsWith($ExpectedShortHash) -or `
              $ExpectedShortHash.StartsWith($hashInLog)
    if (-not $hashOk) {
        Write-Host ""
        Write-Host "  *********************************************************" -ForegroundColor Red
        Write-Host "  *  STALE BINARY DETECTED -- WRONG HASH IN LOG          *" -ForegroundColor Red
        Write-Host "  *  Expected hash : $ExpectedShortHash" -ForegroundColor Red
        Write-Host "  *  Log reports   : $hashInLog" -ForegroundColor Red
        Write-Host "  *********************************************************" -ForegroundColor Red
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        return $false
    }
    Write-Host "  [OK] Git hash confirmed in log: $hashInLog == $ExpectedShortHash" -ForegroundColor Green
    return $true
}

function Test-WatermarkConfig {
    # Reads omega_config.ini and returns @{Mode=...; Watermark=...; Testing=$true/$false}.
    # mode=LIVE with watermark=0 (or NOT_FOUND) is fatal -- caller decides exit.
    param([string]$ConfigFile = $ConfigSrc)
    $wmMatch   = Select-String -Path $ConfigFile -Pattern "session_watermark_pct\s*=\s*([0-9.]+)" -ErrorAction SilentlyContinue
    $modeMatch = Select-String -Path $ConfigFile -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
    $watermark = if ($wmMatch)   { $wmMatch.Matches[0].Groups[1].Value   } else { "NOT_FOUND" }
    $mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "NOT_FOUND" }
    $testing   = ($watermark -eq "NOT_FOUND" -or [double]$watermark -eq 0.0)
    return @{ Mode = $mode; Watermark = $watermark; Testing = $testing }
}

function Test-SymbolsIni {
    # Per-symbol numeric verification of symbols.ini. Returns the list of
    # FAIL strings (empty list = OK). Pulled verbatim from DEPLOY_OMEGA.ps1.
    $expected = @{
        "XAUUSD"  = @{ "MIN_RANGE" = "6.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "25000"; "MAX_SPREAD" = "2.50";  "MAX_RANGE" = "12.00"  }
        "XAGUSD"  = @{ "MIN_RANGE" = "0.40";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "12000"; "MAX_RANGE" = "0.30"   }
        "US500.F" = @{ "MIN_RANGE" = "12.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "25.00"  }
        "USTEC.F" = @{ "MIN_RANGE" = "42.00";   "MIN_STRUCTURE_MS" = "45000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "90.00"  }
        "DJ30.F"  = @{ "MIN_RANGE" = "86.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "180.00" }
        "NAS100"  = @{ "MIN_RANGE" = "42.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "90.00"  }
        "GER40"   = @{ "MIN_RANGE" = "44.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "90.00"  }
        "UK100"   = @{ "MIN_RANGE" = "20.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "40.00"  }
        "ESTX50"  = @{ "MIN_RANGE" = "11.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "22.00"  }
        "USOIL.F" = @{ "MIN_RANGE" = "0.50";    "MIN_STRUCTURE_MS" = "25000"; "BREAKOUT_FAIL_MS" = "12000"; "MAX_RANGE" = "1.20"   }
        "UKBRENT" = @{ "MIN_RANGE" = "0.50";    "MIN_STRUCTURE_MS" = "25000"; "BREAKOUT_FAIL_MS" = "12000"; "MAX_RANGE" = "1.20"   }
        "EURUSD"  = @{ "MIN_RANGE" = "0.00035"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
        "GBPUSD"  = @{ "MIN_RANGE" = "0.00040"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
        "AUDUSD"  = @{ "MIN_RANGE" = "0.00025"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
        "NZDUSD"  = @{ "MIN_RANGE" = "0.00025"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
        "USDJPY"  = @{ "MIN_RANGE" = "0.12";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    }
    $iniPath = "$OmegaDir\symbols.ini"
    if (-not (Test-Path $iniPath)) { return @("symbols.ini not found at $iniPath") }
    $iniLines = Get-Content $iniPath
    $failures = @()
    $currentSection = ""
    foreach ($line in $iniLines) {
        $line = $line.Trim()
        if ($line -match '^\[(.+)\]') { $currentSection = $Matches[1] }
        if ($currentSection -and $expected.ContainsKey($currentSection)) {
            foreach ($key in $expected[$currentSection].Keys) {
                if ($line -match "^$key\s*=\s*(.+)") {
                    $actual = $Matches[1].Trim() -replace '\s*;.*$', ''
                    $expect = $expected[$currentSection][$key]
                    if ([double]$actual -ne [double]$expect) {
                        $failures += "  FAIL [$currentSection] $key = $actual  (expected $expect)"
                    }
                }
            }
        }
    }
    return $failures
}

# ==============================================================================
# Subcommand: stop
# ==============================================================================
function Invoke-Stop {
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host "   OMEGA  |  STOP" -ForegroundColor Cyan
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host ""

    if (-not (Confirm-CfePosition)) { return 0 }

    Write-Host "Stopping Omega service..." -ForegroundColor Yellow
    if (-not (Stop-OmegaService -Force:$ForceKill)) { return 1 }
    Write-Host ""
    Write-Host "  [OK] Omega service stopped." -ForegroundColor Green
    return 0
}

# ==============================================================================
# Subcommand: start
# ==============================================================================
function Invoke-Start {
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host "   OMEGA  |  START  (no rebuild)" -ForegroundColor Cyan
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host ""

    if (-not (Test-Path $OmegaExe)) {
        Write-Host "[ERROR] $OmegaExe not found -- run OMEGA.ps1 deploy first" -ForegroundColor Red
        return 1
    }

    Write-Host "Verifying stamp..." -ForegroundColor Yellow
    if (-not (Test-StampMatchesExe)) {
        Write-Host ""
        Write-Host "  *** STALE BINARY DETECTED -- LAUNCH BLOCKED ***" -ForegroundColor Red
        Write-Host "  Run OMEGA.ps1 deploy to rebuild." -ForegroundColor Yellow
        return 1
    }
    $stamp = Read-Stamp
    $displayHash = if ($stamp.GIT_HASH_SHORT) {
        $stamp.GIT_HASH_SHORT
    } elseif ($stamp.GIT_HASH) {
        $stamp.GIT_HASH.Substring(0, [Math]::Min(7, $stamp.GIT_HASH.Length))
    } else { "unknown" }

    Write-Host "=======================================================" -ForegroundColor Yellow
    Write-Host "  BINARY CHECK -- OK" -ForegroundColor Green
    Write-Host "=======================================================" -ForegroundColor Yellow
    Write-Host "  source commit  = $($stamp.GIT_HASH)" -ForegroundColor Green
    Write-Host "  head at build  = $($stamp.HEAD_HASH)" -ForegroundColor DarkGray
    if ($stamp.GIT_HASH -ne $stamp.HEAD_HASH) {
        Write-Host "  (HEAD was a log-push -- source commit is what matters)" -ForegroundColor Cyan
    }
    Write-Host "  exe SHA256     = $($stamp.EXE_SHA256.Substring(0,16))...  [VERIFIED]" -ForegroundColor Green
    Write-Host "  built          = $($stamp.BUILD_TIME)" -ForegroundColor Cyan

    # Fetch + warn if behind origin source
    Push-Location $OmegaDir
    try {
        $savedPref = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        git fetch origin 2>&1 | Out-Null
        $ErrorActionPreference = $savedPref
        $remoteSrcLine = (git log --oneline -1 "origin/$Branch" -- `
            src include CMakeLists.txt omega_config.ini symbols.ini `
            OMEGA.ps1 INSTALL_OMEGA.ps1 cmake-discover.ps1 2>$null)
        if ($remoteSrcLine -and $remoteSrcLine.Trim() -match '^([a-f0-9]+)\s+') {
            $remoteSrcHash = (git rev-parse $Matches[1] 2>$null).Trim()
            if ($remoteSrcHash -and $remoteSrcHash -ne $stamp.GIT_HASH) {
                Write-Host ""
                Write-Host "  [WARN] Newer source commit available on origin/$Branch" -ForegroundColor Yellow
                Write-Host "         Remote : $($remoteSrcLine.Trim())" -ForegroundColor Yellow
                Write-Host "         Running: $displayHash" -ForegroundColor Yellow
                Write-Host "         Run OMEGA.ps1 deploy to rebuild." -ForegroundColor Yellow
            }
        }
    } finally {
        Pop-Location
    }

    $cfg = Test-WatermarkConfig
    Write-Host "  mode           = $($cfg.Mode)" -ForegroundColor Cyan
    Write-Host "  watermark_pct  = $($cfg.Watermark)" -ForegroundColor Cyan
    Write-Host "=======================================================" -ForegroundColor Yellow
    Write-Host ""

    if ($cfg.Mode -eq "LIVE" -and $cfg.Testing) {
        Write-Host "  *** FATAL: mode=LIVE with watermark=0 -- BLOCKED ***" -ForegroundColor Red
        return 1
    }

    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $svc) {
        Write-Host "[FATAL] Omega service does not exist. Run INSTALL_OMEGA.ps1 first." -ForegroundColor Red
        return 1
    }
    if ($svc.Status -eq 'Running') {
        Write-Host "  [OK] Service already running" -ForegroundColor Green
        return 0
    }

    Write-Host "Starting Omega.exe  [source=$displayHash  mode=$($cfg.Mode)]..." -ForegroundColor Cyan
    if (-not (Start-OmegaService)) { return 1 }
    if (-not (Test-StaleBinaryAfterStart -ExpectedShortHash $displayHash)) { return 1 }
    Write-Host "  [OK] Omega running." -ForegroundColor Green
    return 0
}

# ==============================================================================
# Subcommand: restart  (stop + start, no rebuild)
# ==============================================================================
function Invoke-Restart {
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host "   OMEGA  |  RESTART  (no rebuild)" -ForegroundColor Cyan
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host ""

    if (-not (Confirm-CfePosition)) { return 0 }

    Write-Host "[1/3] Stopping Omega service..." -ForegroundColor Yellow
    if (-not (Stop-OmegaService -Force:$ForceKill)) { return 1 }
    Write-Host ""

    Write-Host "[2/3] Verifying stamp + config..." -ForegroundColor Yellow
    if (-not (Test-StampMatchesExe)) {
        Write-Host "  *** STALE BINARY DETECTED -- RESTART BLOCKED ***" -ForegroundColor Red
        Write-Host "  Run OMEGA.ps1 deploy to rebuild." -ForegroundColor Yellow
        return 1
    }
    $stamp = Read-Stamp
    $displayHash = if ($stamp.GIT_HASH_SHORT) { $stamp.GIT_HASH_SHORT } else { "unknown" }
    $cfg = Test-WatermarkConfig
    if ($cfg.Mode -eq "LIVE" -and $cfg.Testing) {
        Write-Host "  *** FATAL: mode=LIVE with watermark=0 -- BLOCKED ***" -ForegroundColor Red
        return 1
    }
    Write-Host "  [OK] stamp + config validated  [source=$displayHash mode=$($cfg.Mode) watermark=$($cfg.Watermark)]" -ForegroundColor Green
    Rotate-ServiceLogs
    Write-Host ""

    Write-Host "[3/3] Starting Omega service..." -ForegroundColor Yellow
    if (-not (Start-OmegaService)) { return 1 }
    if (-not (Test-StaleBinaryAfterStart -ExpectedShortHash $displayHash)) { return 1 }

    if (-not $SkipVerify -and (Test-Path "$OmegaDir\VERIFY_STARTUP.ps1")) {
        Write-Host ""
        # S-2026-06-20: poll for the boot git-hash marker (NEW hash) instead of flat 60s.
        Write-Host "  Waiting for boot marker (git-hash, max 60s) then VERIFY_STARTUP..." -ForegroundColor Cyan
        $latestLog    = "$OmegaDir\logs\latest.log"
        $bootStart    = Get-Date
        $bootDeadline = $bootStart.AddSeconds(60)
        # match the 7-char short hash the binary actually logs (version_generated.hpp
        # uses git --short=7), tolerant of a longer stamp hash.
        $bootPat      = "Git hash.*" + [regex]::Escape($displayHash.Substring(0, [Math]::Min(7, $displayHash.Length)))
        $booted       = $false
        while ((Get-Date) -lt $bootDeadline) {
            Start-Sleep -Seconds 2
            if (Test-Path $latestLog) {
                try {
                    if ((Get-Content $latestLog -Tail 60 -ErrorAction SilentlyContinue) -match $bootPat) { $booted = $true; break }
                } catch { }
            }
        }
        if ($booted) {
            Write-Host ("  [OK] boot marker ({0}) seen in {1:N0}s" -f $displayHash, ((Get-Date) - $bootStart).TotalSeconds) -ForegroundColor Green
        } else {
            Write-Host "  [WARN] no boot marker in 60s -- running VERIFY_STARTUP anyway" -ForegroundColor Yellow
        }
        & "$OmegaDir\VERIFY_STARTUP.ps1" -OmegaDir $OmegaDir
    }
    Write-Host ""
    Write-Host "  [OK] Omega restarted [source=$displayHash mode=$($cfg.Mode)]" -ForegroundColor Green
    return 0
}

# S-2026-06-29: run a python step with a HARD timeout so a dead IBKR connection (data farms
# OFF on weekends / gateway down) cannot hang the whole deploy at [2b]. ROOT CAUSE of the
# 11min+ "down" reports: rebuild_warmups.py / refresh_warmup_seeds.py connect to IB Gateway and
# block forever when the farms are off, stalling the deploy BEFORE the build. On timeout we kill
# the py process TREE (py.exe launcher spawns python.exe child) and return $false so the caller
# falls back to the committed git-snapshot seeds -- the intended NON-FATAL behavior.
function Invoke-PyStepWithTimeout {
    param([string]$ScriptAndArgs, [int]$TimeoutSec, [string]$Label, [string]$WorkDir)
    $out = [System.IO.Path]::GetTempFileName()
    $err = "$out.err"
    $proc = $null
    try {
        $proc = Start-Process -FilePath "py" -ArgumentList $ScriptAndArgs -WorkingDirectory $WorkDir `
                  -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err -ErrorAction Stop
        # CRITICAL (2026-06-29): cache the process Handle IMMEDIATELY. A Start-Process -PassThru
        # object does NOT populate .ExitCode after WaitForExit() unless its Handle was read while
        # the process was alive -- it comes back $null, so ($code -eq 0) was ALWAYS $false. That
        # silently made this function ALWAYS return $false: the [2b] audit-skip never fired (every
        # deploy paid the ~300s reseed) and the [2c] fail-closed gate ALWAYS aborted. Proven on VPS.
        $null = $proc.Handle
    } catch {
        Write-Host "  [WARN] $Label could not start: $_ -- keeping prior seeds" -ForegroundColor Yellow
        Remove-Item $out, $err -Force -ErrorAction SilentlyContinue
        return $false
    }
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Host "  [WARN] $Label exceeded ${TimeoutSec}s (IBKR 4001 / data farms down?) -- killing, keeping git-snapshot seeds" -ForegroundColor Yellow
        & taskkill /T /F /PID $proc.Id 2>&1 | Out-Null
        Start-Sleep -Milliseconds 300
        Get-Content $out, $err -ErrorAction SilentlyContinue | Select-Object -Last 4 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        Remove-Item $out, $err -Force -ErrorAction SilentlyContinue
        return $false
    }
    Get-Content $out, $err -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    $code = $proc.ExitCode
    Remove-Item $out, $err -Force -ErrorAction SilentlyContinue
    return ($code -eq 0)
}

# ==============================================================================
# Subcommand: deploy  (full pipeline -- 12 steps)
# ==============================================================================
function Invoke-Deploy {
    $startTime = Get-Date

    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host "   OMEGA  |  DEPLOY  (S12 PS1 Consolidation, 2026-05-07)" -ForegroundColor Cyan
    Write-Host "=======================================================" -ForegroundColor Cyan

    # Singleton mutex -- prevent two deploys racing each other.
    $deployMutex = New-Object System.Threading.Mutex($false, "Global\OmegaDeployMutex")
    try { $gotMutex = $deployMutex.WaitOne(0) } catch { $gotMutex = $true }
    if (-not $gotMutex) {
        Write-Host "[DEPLOY] Another deploy is already running. Exiting." -ForegroundColor Red
        $deployMutex.Dispose()
        return 1
    }

    try {
    $cfg = Test-WatermarkConfig
    $modeColor = if ($cfg.Mode -eq "LIVE") { "Red" } elseif ($cfg.Mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
    Write-Host "  Mode: $($cfg.Mode)" -ForegroundColor $modeColor
    Write-Host ""

    if (-not (Confirm-CfePosition)) { return 0 }

    # --------------------------------------------------------------------------
    # [0/12] Pre-flight zombie cleanup
    # --------------------------------------------------------------------------
    # S-2026-06-24p: per-step deploy timing. $swDeploy = total elapsed; Lap prints the
    # cumulative t+Ns at a phase boundary, so the DELTA between two laps = that phase's
    # cost (answers "why does a deploy take ~8min" with hard numbers: refresh vs build
    # vs restart). Laps at the 3 heavy boundaries + a final total.
    $script:swDeploy = [System.Diagnostics.Stopwatch]::StartNew()
    function Lap($what) { Write-Host ("  [timing] {0}: t+{1}s" -f $what, [int]$script:swDeploy.Elapsed.TotalSeconds) -ForegroundColor DarkGray }
    Write-Host "[0/12] Pre-flight zombie cleanup..." -ForegroundColor DarkCyan
    $zombies = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -in $zombieNames }
    if ($zombies) {
        Write-Host "  Found $($zombies.Count) build-process zombies -- killing:" -ForegroundColor Yellow
        foreach ($z in $zombies) {
            Write-Host "    PID $($z.Id) $($z.Name) (mem $([int]($z.WorkingSet64/1MB))MB)" -ForegroundColor DarkYellow
        }
        $zombies | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    } else {
        Write-Host "  [OK] No build-process zombies" -ForegroundColor Green
    }
    $gitZombies = Get-Process -Name git -ErrorAction SilentlyContinue
    if ($gitZombies) {
        Write-Host "  Killing $($gitZombies.Count) orphan git processes" -ForegroundColor Yellow
        $gitZombies | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
    $gitLocks = Get-ChildItem "$OmegaDir\.git" -Filter "*.lock" -Recurse -ErrorAction SilentlyContinue
    if ($gitLocks) {
        Write-Host "  Removing $($gitLocks.Count) stale git lock files:" -ForegroundColor Yellow
        foreach ($lk in $gitLocks) {
            Write-Host "    $($lk.FullName)" -ForegroundColor DarkYellow
            Remove-Item $lk.FullName -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "  [OK] No stale git lock files" -ForegroundColor Green
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [1/12] Stop service (write deploy sentinel first so watchdog doesn't
    #        treat this as a crash)
    # --------------------------------------------------------------------------
    Set-Content -Path $DeployFlag -Value (Get-Date).ToString("o") -Encoding UTF8
    if ($ColdStop) {
        Write-Host "[1/12] Stopping Omega service (-ColdStop legacy ordering)..." -ForegroundColor Yellow
        if (-not (Stop-OmegaService -Force:$ForceKill)) { return 1 }
    } else {
        Write-Host "[1/12] HOT-SWAP: service stays UP through pull+build; stop deferred to [7/12]" -ForegroundColor Cyan
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [2/12] Pull source via git
    # --------------------------------------------------------------------------
    Write-Host "[2/12] Pulling source from GitHub (branch=$Branch)..." -ForegroundColor Yellow
    $ghSha = ""
    $ghSha7 = ""
    Push-Location $OmegaDir
    try {
        git fetch origin $Branch 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [FATAL] git fetch failed (exit=$LASTEXITCODE)" -ForegroundColor Red
            Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            Pop-Location
            return 1
        }
        $revParseOut = (git rev-parse "origin/$Branch" 2>&1)
        $revParseExit = $LASTEXITCODE
        if ($revParseExit -ne 0 -or [string]::IsNullOrWhiteSpace($revParseOut)) {
            Write-Host "  [FATAL] git rev-parse origin/$Branch failed (exit=$revParseExit output='$revParseOut')" -ForegroundColor Red
            Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            Pop-Location
            return 1
        }
        $ghSha = $revParseOut.Trim()
        if ($ghSha.Length -lt 7) {
            Write-Host "  [FATAL] git rev-parse returned malformed SHA: '$ghSha'" -ForegroundColor Red
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            Pop-Location
            return 1
        }
        $ghSha7 = $ghSha.Substring(0,7)
        Write-Host "  HEAD: $ghSha7" -ForegroundColor Cyan

        # S-2026-06-20: capture pre-reset HEAD so [5/12] can skip the UI build when
        # omega-terminal didn't change in this pull.
        $prevHead = (git rev-parse HEAD 2>$null)

        # DEPLOY-SPEED (S-2026-06-29): the daily OmegaSeedRefresh task keeps the WORKING-TREE
        # warmup CSVs fresh, but `git reset --hard` below reverts them to the static (often-stale)
        # COMMITTED corpus -> [2b] would then ALWAYS reseed (~300s) on EVERY deploy (the skip never
        # fires). Back the working-tree seeds up now; after the reset, tools/preserve_fresh_seeds.py
        # copies back any whose LAST BAR is newer than the committed file. A real committed seed
        # update (the newer one) is left untouched. [2c] re-audits afterward, so this can only ever
        # make seeds FRESHER, never ship stale. Backup lives under logs\ (no-space path for py args).
        $seedBackup = Join-Path $OmegaDir ("logs\_seedbak_" + [Guid]::NewGuid().ToString("N"))
        try {
            New-Item -ItemType Directory -Force -Path (Join-Path $seedBackup "phase1\signal_discovery"), (Join-Path $seedBackup "data") | Out-Null
            Copy-Item "phase1\signal_discovery\*.csv" (Join-Path $seedBackup "phase1\signal_discovery") -ErrorAction SilentlyContinue
            Copy-Item "data\mgc_h1_hist.csv","data\mgc_30m_hist.csv" (Join-Path $seedBackup "data") -ErrorAction SilentlyContinue
        } catch { Write-Host "  [WARN] seed backup before reset failed: $_" -ForegroundColor Yellow }

        git reset --hard "origin/$Branch" 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [FATAL] git reset --hard failed (exit=$LASTEXITCODE)" -ForegroundColor Red
            Remove-Item -Recurse -Force $seedBackup -ErrorAction SilentlyContinue
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            Pop-Location
            return 1
        }

        # restore working-tree seeds fresher than the just-reset committed corpus -> [2b] can SKIP
        try {
            Invoke-PyStepWithTimeout "tools\preserve_fresh_seeds.py --repo $OmegaDir --backup $seedBackup" 60 "preserve_fresh_seeds" $OmegaDir | Out-Null
        } catch { Write-Host "  [WARN] preserve_fresh_seeds failed: $_ -- [2b] will reseed if stale" -ForegroundColor Yellow }
        Remove-Item -Recurse -Force $seedBackup -ErrorAction SilentlyContinue
    } finally {
        Pop-Location
    }
    Write-Host "  [OK] Source at $ghSha7" -ForegroundColor Green
    Write-Host ""

    # --------------------------------------------------------------------------
    # [2a/12] Config-drift guard -- runs AFTER the git reset so it checks the
    #         FRESHLY-PULLED tree. Blocks the build (service already stopped at
    #         [1/12]) if a deploy-line PF claim contradicts the faithful manifest;
    #         restarts the service with the prior binary on block (no downtime).
    # --------------------------------------------------------------------------
    Write-Host "[2a/12] Config-drift guard (post-pull)..." -ForegroundColor Yellow
    if (-not (Invoke-DriftGuard)) {
        Write-Host "  [ABORT] config-drift guard failed -- not building. Restarting prior binary." -ForegroundColor Red
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        return 1
    }

    # --------------------------------------------------------------------------
    # [2b/12] Refresh engine warm-seed CSVs from captured l2_ticks (S-2026-06-01).
    #         Engines seed their breakout box from phase1/signal_discovery/warmup_*.csv.
    #         The git reset just restored the COMMITTED (stale) snapshot; regenerate it
    #         from the live l2_ticks_<SYM>_*.csv captures so the box seeds at CURRENT
    #         price, not a frozen one. A stale box sits off-market and produced phantom
    #         0-second fills (see the gap/arm guards in XauStraddleM30Engine).
    #         NON-FATAL: any failure/skip just leaves the git snapshot in place.
    # --------------------------------------------------------------------------
    Lap "before [2b] seed-refresh"
    Write-Host "[2b/12] Warm-seed refresh (auto-gated)..." -ForegroundColor Yellow
    Push-Location $OmegaDir
    try {
        # S-2026-06-29 DEPLOY-SPEED (corrected after the stamp-gate caused a 17-stale boot):
        # the deploy's [2/12] `git reset --hard` REVERTS the committed warmup CSVs (which are a
        # static, often-stale corpus) -- and [2b] is the ONLY step that regenerates them fresh.
        # So the skip decision MUST measure the ACTUAL post-reset on-disk seed freshness, NOT a
        # time stamp: a stamp is a proxy whose freshly-written CSVs git just discarded on reset,
        # so a "fresh stamp" skip booted 17 enabled engines on the stale committed snapshot.
        # Gate instead on the CHEAP tools/seed_freshness_audit.py (reads each enabled seed's
        # last-bar age, no IBKR, ~seconds; exits 1 if ANY enabled seed is stale): audit clean
        # -> SKIP the up-to-300s refresh; audit stale -> RESEED. -ForceSeed/-SkipSeed override.
        $seedAlert   = Join-Path $OmegaDir "logs\SEED_ALERT.txt"
        $doReseed    = $false
        $reseedWhy   = ""
        if ($SkipSeed) {
            $reseedWhy = "-SkipSeed set -- never reseed (operator override; may boot stale)"
        } elseif ($ForceSeed) {
            $doReseed = $true; $reseedWhy = "-ForceSeed set"
        } else {
            # cheap freshness audit on the post-reset working tree (no IBKR; exit 1 = stale enabled seed).
            # --allow-missing-generated (S-2026-07-01): the RESEED decision must not be polluted by a
            # missing risk_monitor_thresholds.csv -- a reseed cannot fix that. The STRICT [2c] gate
            # (no flag -> audit exit 2 on missing-required) is what fail-closes the ship on it.
            if (Invoke-PyStepWithTimeout "tools\seed_freshness_audit.py --allow-missing-generated" 60 "seed_freshness_audit" $OmegaDir) {
                $reseedWhy = "freshness audit clean -- committed seeds already fresh on disk"
            } else {
                $doReseed = $true; $reseedWhy = "freshness audit reports stale enabled-engine seed(s)"
            }
        }

        if (-not $doReseed) {
            Write-Host "  [skip] seed-refresh skipped ($reseedWhy) -- saves up to ~300s. -ForceSeed to override." -ForegroundColor Cyan
        } else {
            Write-Host "  [reseed] $reseedWhy" -ForegroundColor Cyan
            # tools/seed_refresh.py runs: [1] rebuild from l2_ticks, [2] IBKR refresh (connect-times-out +
            # SKIPS gracefully when the gateway is down -> no hang), [3] freshness audit (sets exit code).
            # Invoke-PyStepWithTimeout returns $true only on exit 0. Outer 300s is a belt+braces backstop.
            if (Invoke-PyStepWithTimeout "tools\seed_refresh.py --port 4002 --repo $OmegaDir" 300 "seed_refresh" $OmegaDir) {
                Write-Host "  [OK] seed_refresh: rebuild + IBKR-refresh + audit clean (enabled engines fresh)" -ForegroundColor Green
                Remove-Item -Path $seedAlert -ErrorAction SilentlyContinue
            } else {
                $alert = "[P0-SEED] seed_refresh: enabled-engine warm-seed(s) STILL STALE (or refresh timed out) -- engine/gate booting on a price view detached from reality. Needs IBKR gateway (4002) live; re-run: py tools\seed_refresh.py --port 4002"
                Write-Host ""
                Write-Host "  ============================================================" -ForegroundColor Red
                Write-Host "  $alert" -ForegroundColor Red
                Write-Host "  ============================================================" -ForegroundColor Red
                $alert | Out-File -FilePath $seedAlert -Encoding utf8
            }
        }
    } catch {
        Write-Host "  [WARN] seed-refresh gate failed: $_ -- keeping git snapshot" -ForegroundColor Yellow
    } finally {
        Pop-Location
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [2c/12] FAIL-CLOSED seed gate (S-2026-06-29). A binary must NEVER go live
    #         with stale enabled-engine seeds. [2b] RESEEDS when stale, but a
    #         reseed can FAIL silently (IBKR 4001 down / timeout) and the old
    #         behaviour just CONTINUED -> shipped a blind binary booting on a
    #         frozen, off-market price view. That is the root cause of the
    #         recurring-staleness incidents. So re-run the cheap audit AFTER [2b]
    #         and, if ANY enabled seed is STILL stale, ABORT the deploy. HOT-SWAP
    #         means the prior binary is still LIVE here -> abort = zero downtime
    #         AND no stale ship. -AllowStaleSeed is the only override (use it only
    #         to deliberately ship while 4001 is down and you accept stale seeds).
    #         S-2026-07-01: the strict audit now ALSO exits 2 when a REQUIRED
    #         generated file is missing (data\risk_monitor_thresholds.csv --
    #         without it the RiskMonitor auto-demote surveillance layer is
    #         silently OFF). Same abort path; [2c-pre] below tries to regenerate
    #         it first when the calibrator binary is present.
    # --------------------------------------------------------------------------
    # [2c-pre] Risk-monitor thresholds regen (best-effort). File is generated
    #          on-box + gitignored, so a fresh clone / wiped data\ loses it.
    $riskThr = Join-Path $OmegaDir "data\risk_monitor_thresholds.csv"
    if (-not (Test-Path $riskThr)) {
        $calBin = Get-ChildItem -Path (Join-Path $OmegaDir "backtest") -File -ErrorAction SilentlyContinue |
                  Where-Object { $_.BaseName -eq "calibrate_risk_thresholds" -and @("", ".exe") -contains $_.Extension } |
                  Select-Object -First 1
        if ($calBin) {
            Write-Host "  [2c-pre] risk_monitor_thresholds.csv missing -- regenerating via backtest\$($calBin.Name) (240s cap)..." -ForegroundColor Yellow
            $calJob = Start-Job -ScriptBlock { param($exe, $wd) Set-Location $wd; & $exe 2>&1 } `
                                -ArgumentList $calBin.FullName, $OmegaDir
            if (Wait-Job $calJob -Timeout 240) {
                Receive-Job $calJob | Select-Object -Last 3 | ForEach-Object { Write-Host "           $_" }
            } else {
                Stop-Job $calJob
                Write-Host "  [WARN] calibrator exceeded 240s -- killed; strict gate below decides." -ForegroundColor Yellow
            }
            Remove-Job $calJob -Force -ErrorAction SilentlyContinue
        } else {
            Write-Host "  [2c-pre] risk_monitor_thresholds.csv MISSING and no calibrator binary -- strict gate below will fail-closed." -ForegroundColor Red
            Write-Host "           Build: cl /O2 /std:c++17 /Iinclude backtest\calibrate_risk_thresholds.cpp  (clang++ on Mac)" -ForegroundColor Red
            Write-Host "           Run from repo root -> writes data\risk_monitor_thresholds.csv. Or -AllowStaleSeed to ship without surveillance." -ForegroundColor Red
        }
    }
    Write-Host "[2c/12] Fail-closed seed gate (post-reseed re-audit)..." -ForegroundColor Yellow
    if ($AllowStaleSeed) {
        Write-Host "  [override] -AllowStaleSeed set -- shipping even if seeds are stale (operator accepts blind boot)." -ForegroundColor Yellow
    } else {
        $seedsFresh = $false
        Push-Location $OmegaDir
        try {
            $seedsFresh = Invoke-PyStepWithTimeout "tools\seed_freshness_audit.py" 60 "seed_freshness_recheck" $OmegaDir
        } finally {
            Pop-Location
        }
        if (-not $seedsFresh) {
            Write-Host "  ============================================================" -ForegroundColor Red
            Write-Host "  [ABORT] seed gate failed after [2b] -- refusing to ship a blind/unguarded binary." -ForegroundColor Red
            Write-Host "          Either enabled-engine seed(s) are STILL STALE (bring IBKR 4001 up, re-deploy)" -ForegroundColor Red
            Write-Host "          or data\risk_monitor_thresholds.csv is MISSING (RiskMonitor surveillance OFF --" -ForegroundColor Red
            Write-Host "          build+run backtest\calibrate_risk_thresholds, see [2c-pre] above)." -ForegroundColor Red
            Write-Host "          Prior binary stays LIVE (hot-swap -> no downtime). -AllowStaleSeed overrides both." -ForegroundColor Red
            Write-Host "  ============================================================" -ForegroundColor Red
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            return 1
        }
        Write-Host "  [OK] all enabled-engine seeds fresh -- safe to build + ship." -ForegroundColor Green
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [3/12] Compute SOURCE_HASH (skip log-only commits) -- this is what gets
    #        stamped, shown in the GUI, and verified by start/restart/watchdog.
    #        Walks recent history for the first commit whose changeset is not
    #        entirely under logs/.
    # --------------------------------------------------------------------------
    Write-Host "[3/12] Computing SOURCE_HASH..." -ForegroundColor Yellow
    Push-Location $OmegaDir
    try {
        $sourceHash      = ""
        $sourceHashShort = ""
        $recentCommits = (git log --oneline -20 2>$null) -split "`n" | Where-Object { $_.Trim() -ne "" }
        foreach ($commitLine in $recentCommits) {
            if ($commitLine -notmatch "^([a-f0-9]+)\s+") { continue }
            $shortHash = $Matches[1]
            $fullHash  = (git rev-parse $shortHash 2>$null).Trim()
            # S-2026-07-07 MERGE-COMMIT FIX: plain `git show --name-only` lists NO files for a
            # merge commit, so a PR merge at HEAD was skipped as "log-only" and SOURCE_HASH fell
            # back to the branch-head commit -- while the build-time omega_version target bakes
            # `git rev-parse HEAD` (the merge hash) into the binary. Stamp validation then failed
            # with GUI hash MISMATCH and left the service STOPPED. diff-tree vs first parent
            # lists the merged files, so a merge commit at HEAD now correctly becomes SOURCE_HASH.
            $filesChanged = (git diff-tree --no-commit-id --name-only -r -m --first-parent $fullHash 2>$null) -split "`n" | Where-Object { $_.Trim() -ne "" }
            # Treat seed-data-only commits like log-only commits: they don't change the binary's
            # CODE, so they must NOT bump SOURCE_HASH (which gates the rebuild + is compared against
            # HEAD in the drift check). A future auto-committed seed refresh would otherwise look
            # like a source change. Excludes logs/, the warmup corpus, and the mgc_*_hist seeds.
            $nonLogFiles  = $filesChanged | Where-Object {
                (-not $_.StartsWith("logs/")) -and
                (-not ($_ -match '^phase1/signal_discovery/.*\.csv$')) -and
                (-not ($_ -match '^data/mgc_(h1|30m)_hist\.csv$'))
            }
            if ($nonLogFiles) {
                $sourceHash      = $fullHash
                $sourceHashShort = $shortHash
                break
            }
        }
        if (-not $sourceHash) {
            $sourceHash      = $ghSha
            $sourceHashShort = $ghSha7
        }
    } finally {
        Pop-Location
    }
    Write-Host "  [OK] SOURCE_HASH = $sourceHashShort  (HEAD = $ghSha7)" -ForegroundColor Green
    Write-Host ""

    # --------------------------------------------------------------------------
    # [4/12] Build prep: defensive zombie re-kill + wipe with retry-on-lock
    # --------------------------------------------------------------------------
    Write-Host "[4/12] Build prep..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) {
        $preWipeZombies = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -in $zombieNames }
        if ($preWipeZombies) {
            Write-Host "  [pre-wipe] $($preWipeZombies.Count) build process(es) re-appeared -- killing:" -ForegroundColor Yellow
            foreach ($z in $preWipeZombies) {
                Write-Host "    PID $($z.Id) $($z.Name) (mem $([int]($z.WorkingSet64/1MB))MB)" -ForegroundColor DarkYellow
            }
            $preWipeZombies | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 500
        }

        $unsuccessfulStamps = @()
        try {
            $unsuccessfulStamps = Get-ChildItem -Path $BuildDir -Recurse -Force -Filter "unsuccessfulbuild" -ErrorAction SilentlyContinue
        } catch { }

        $wipeBackoffMs = @(200, 400, 800, 1600, 3200)

        if ($unsuccessfulStamps -and $unsuccessfulStamps.Count -gt 0) {
            Write-Host "  Detected previous-build crash stamp -- force-wiping build/ directory:" -ForegroundColor Yellow
            foreach ($s in $unsuccessfulStamps) {
                Write-Host "    found: $($s.FullName)" -ForegroundColor DarkYellow
            }
            $wipeOk = $false
            for ($attempt = 0; $attempt -lt 5; $attempt++) {
                if (-not (Test-Path -LiteralPath $BuildDir)) { $wipeOk = $true; break }
                try {
                    Remove-Item -LiteralPath $BuildDir -Recurse -Force -ErrorAction Stop
                    $wipeOk = $true
                    break
                } catch {
                    if ($attempt -lt 4) {
                        $delay = $wipeBackoffMs[$attempt]
                        Write-Host "    [WARN] Full-wipe attempt $($attempt + 1)/5 failed: $_ -- killing zombies and retrying in ${delay}ms" -ForegroundColor Yellow
                        Get-Process -ErrorAction SilentlyContinue |
                            Where-Object { $_.Name -in $zombieNames } |
                            Stop-Process -Force -ErrorAction SilentlyContinue
                        Start-Sleep -Milliseconds $delay
                    } else {
                        Write-Host "    [FAIL] Full-wipe failed after 5 attempts: $_" -ForegroundColor Red
                        Write-Host "    Continuing -- cmake configure will regenerate as much as possible." -ForegroundColor Red
                    }
                }
            }
            New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
            if ($wipeOk) {
                Write-Host "  [OK] build/ fully wiped -- next configure rebuilds from scratch" -ForegroundColor Green
            } else {
                Write-Host "  [PARTIAL] build/ not fully wiped; configure will surface any breakage." -ForegroundColor Yellow
            }
        } elseif (-not $Clean) {
            Write-Host "  [incremental] preserving .obj/.pch -- only changed TUs recompile (-Clean forces full)" -ForegroundColor Cyan
        } else {
            $artifacts = @(Get-ChildItem -Path $BuildDir -Include "*.obj","*.pch","*.pdb","*.iobj","*.ipdb" -Recurse -ErrorAction SilentlyContinue)
            $totalArtifacts = $artifacts.Count
            $remaining = $artifacts
            for ($attempt = 0; $attempt -lt 5; $attempt++) {
                if ($remaining.Count -eq 0) { break }
                $stillLocked = New-Object System.Collections.Generic.List[System.IO.FileSystemInfo]
                foreach ($it in $remaining) {
                    try {
                        Remove-Item -LiteralPath $it.FullName -Force -ErrorAction Stop
                    } catch {
                        [void]$stillLocked.Add($it)
                    }
                }
                if ($stillLocked.Count -eq 0) { break }
                if ($attempt -lt 4) {
                    $delay = $wipeBackoffMs[$attempt]
                    Write-Host "    [WARN] $($stillLocked.Count) artifact(s) locked after attempt $($attempt + 1)/5 -- killing zombies and retrying in ${delay}ms" -ForegroundColor Yellow
                    Get-Process -ErrorAction SilentlyContinue |
                        Where-Object { $_.Name -in $zombieNames } |
                        Stop-Process -Force -ErrorAction SilentlyContinue
                    Start-Sleep -Milliseconds $delay
                } else {
                    Write-Host "    [FAIL] $($stillLocked.Count) of $totalArtifacts artifact(s) still locked after 5 attempts:" -ForegroundColor Red
                    foreach ($lk in $stillLocked) {
                        Write-Host "      $($lk.FullName)" -ForegroundColor Red
                    }
                    Write-Host "    Continuing -- MSBuild may fail on these; deploy will recover via the build-failure path." -ForegroundColor Red
                }
                $remaining = @($stillLocked)
            }
            $deleted = $totalArtifacts - $remaining.Count
            Write-Host "  [OK] Build artifacts wiped ($deleted/$totalArtifacts removed; CMakeCache preserved)" -ForegroundColor Green
        }
    } else {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
        Write-Host "  [OK] Build directory created (first run)" -ForegroundColor Green
    }

    # Force-touch source files so MSVC sees them as newer than any surviving .obj.
    # Scope to src/ + include/ only (the actual build inputs). Recursing the
    # whole OmegaDir (backtest/, phase1/, vendored headers) took ~12s > the 10s
    # guard below and tripped a false "[FATAL] Source touch failed" abort
    # (2026-06-03). src+include touches in <1s.
    if ($Clean) {
    $touchTime = Get-Date
    @("$OmegaDir\src", "$OmegaDir\include") | ForEach-Object {
        if (Test-Path $_) {
            Get-ChildItem -Path $_ -Include "*.cpp","*.hpp","*.h" -Recurse -ErrorAction SilentlyContinue |
                ForEach-Object { $_.LastWriteTime = $touchTime }
        }
    }
    $mainCpp = "$OmegaDir\src\main.cpp"
    if (Test-Path $mainCpp) {
        # Re-touch main.cpp immediately before the age check so the guard is
        # deterministic regardless of how long the loop above took.
        (Get-Item $mainCpp).LastWriteTime = Get-Date
        $mainAge = ((Get-Date) - (Get-Item $mainCpp).LastWriteTime).TotalSeconds
        if ($mainAge -gt 10) {
            Write-Host "  [FATAL] Source touch failed -- main.cpp age=${mainAge}s" -ForegroundColor Red
            Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            return 1
        }
        Write-Host "  [OK] Source timestamps updated (main.cpp age=${mainAge}s)" -ForegroundColor Green
    } else {
        Write-Host "  [WARN] Cannot verify touch -- main.cpp not found" -ForegroundColor Yellow
    }
    } else {
        Write-Host "  [incremental] skipping force-touch -- MSBuild dependency tracking decides rebuilds (-Clean to force full)" -ForegroundColor Cyan
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [5/12] UI build (omega-terminal)
    # --------------------------------------------------------------------------
    Write-Host "[5/12] UI build (omega-terminal)..." -ForegroundColor Yellow
    $uiDir = "$OmegaDir\omega-terminal"
    # S-2026-06-20 DEPLOY-SPEED: skip the ~32s UI build (npm ci + vite) when
    # omega-terminal is unchanged vs the previous HEAD AND a built dist exists.
    $uiSkip = $false
    if ((Test-Path $uiDir) -and $prevHead -and (Test-Path "$uiDir\dist\index.html")) {
        git -C $OmegaDir diff --quiet $prevHead HEAD -- omega-terminal 2>$null
        if ($LASTEXITCODE -eq 0) { $uiSkip = $true }
    }
    if ($uiSkip) {
        Write-Host "  [skip] omega-terminal unchanged since $($prevHead.Substring(0,7)) + dist present -- skipping UI build (~32s saved)" -ForegroundColor Cyan
    } elseif (Test-Path $uiDir) {
        $uiStart = Get-Date
        $nodeOk = $null -ne (Get-Command node -ErrorAction SilentlyContinue)
        $npmOk  = $null -ne (Get-Command npm  -ErrorAction SilentlyContinue)
        if (-not $nodeOk -or -not $npmOk) {
            Write-Host "  [FATAL] node/npm not on PATH (node=$nodeOk, npm=$npmOk)" -ForegroundColor Red
            Write-Host "  Install: choco install nodejs-lts  (then reopen shell)" -ForegroundColor Red
            Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            return 1
        }
        Push-Location $uiDir
        try {
            & npm ci 2>&1 | ForEach-Object { Write-Host "    [npm ci] $_" -ForegroundColor DarkGray }
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  [FATAL] npm ci failed (exit=$LASTEXITCODE)" -ForegroundColor Red
                Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
                Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
                Pop-Location
                return 1
            }
            & npm run build 2>&1 | ForEach-Object { Write-Host "    [vite] $_" -ForegroundColor DarkGray }
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  [FATAL] npm run build failed (exit=$LASTEXITCODE)" -ForegroundColor Red
                Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
                Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
                Pop-Location
                return 1
            }
        } finally {
            Pop-Location
        }
        $uiSec = [math]::Round(((Get-Date) - $uiStart).TotalSeconds, 1)
        $distIndex = "$uiDir\dist\index.html"
        if (-not (Test-Path $distIndex)) {
            Write-Host "  [FATAL] vite reported success but $distIndex missing" -ForegroundColor Red
            Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
            Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
            return 1
        }
        Write-Host "  [OK] UI built in ${uiSec}s -> omega-terminal\dist\" -ForegroundColor Green
    } else {
        Write-Host "  [SKIP] omega-terminal/ not found -- UI build skipped" -ForegroundColor DarkYellow
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [6/12] CMake configure + build (Release, target Omega)
    # --------------------------------------------------------------------------
    Lap "before [6] cmake build (delta from [2b] = seed-refresh cost)"
    Write-Host "[6/12] CMake configure + build..." -ForegroundColor Yellow
    Write-Host "  (streaming cmake output -- errors in red, compiling files in gray)" -ForegroundColor DarkCyan

    # S-2026-06-19 STALE-BINARY FIX. Root cause of chronic "deploy hasn't changed":
    # incremental builds (CMakeCache preserved) silently did NOT recompile main.cpp
    # when only HEADERS changed (e.g. tick_fx.hpp), so a deploy could ship the OLD
    # code + OLD baked git-hash while reporting OK. Force-regenerate the version
    # header from current HEAD AND force main.cpp to recompile so the deployed
    # binary's code + baked OMEGA_GIT_HASH ALWAYS equal the checked-out source.
    Remove-Item -LiteralPath "$OmegaDir\include\version_generated.hpp" -Force -ErrorAction SilentlyContinue
    $mainCppForce = "$OmegaDir\src\main.cpp"
    if (Test-Path $mainCppForce) {
        (Get-Item $mainCppForce).LastWriteTime = Get-Date
        Write-Host "  [stale-fix] forced main.cpp recompile + version_generated.hpp regen (git-hash truth)" -ForegroundColor DarkGreen
    }

    # Dot-source cmake-discover.ps1 -- defines $cmakeExe.
    $cmakeDiscover = Join-Path $PSScriptRoot "cmake-discover.ps1"
    if (-not (Test-Path $cmakeDiscover)) {
        Write-Host "  [FATAL] cmake-discover.ps1 not found at $cmakeDiscover" -ForegroundColor Red
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        return 1
    }
    . $cmakeDiscover

    $cfgStart = Get-Date
    & $cmakeExe -S $OmegaDir -B $BuildDir -DCMAKE_BUILD_TYPE=Release "-DOMEGA_FORCE_GIT_HASH=$sourceHashShort" 2>&1 | ForEach-Object {
        $line = Format-NativeOutputLine $_
        if ($line -match "error|FAILED|CMake Error") {
            Write-Host "    $line" -ForegroundColor Red
        } elseif ($line -match "\[Omega\]|Build hash|Build time") {
            Write-Host "    $line" -ForegroundColor Cyan
        } elseif ($line -match "^-- ") {
            Write-Host "    $line" -ForegroundColor DarkGray
        } else {
            Write-Host "    $line" -ForegroundColor Gray
        }
    }
    $configureExitCode = $LASTEXITCODE
    $cfgSec = [math]::Round(((Get-Date) - $cfgStart).TotalSeconds, 1)

    if ($configureExitCode -ne 0) {
        Write-Host ""
        Write-Host "  *********************************************************" -ForegroundColor Red
        Write-Host "  *  CMAKE CONFIGURE FAILED (exit=$configureExitCode)" -ForegroundColor Red
        Write-Host "  *  Duration: ${cfgSec}s" -ForegroundColor Red
        Write-Host "  *  Service will restart with the PREVIOUS binary" -ForegroundColor Red
        Write-Host "  *********************************************************" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        try { Start-Service -Name $ServiceName -ErrorAction Stop }
        catch { Write-Host "  [FATAL] Could not restart service: $_" -ForegroundColor Red }
        return 1
    }
    Write-Host "  [configure] done in ${cfgSec}s" -ForegroundColor DarkCyan

    $bldStart = Get-Date
    $compileCount = 0
    & $cmakeExe --build $BuildDir --config Release --target Omega 2>&1 | ForEach-Object {
        $line = Format-NativeOutputLine $_
        if ($line -match "error C\d+|fatal error|LINK : fatal|LNK\d{4}") {
            Write-Host "    $line" -ForegroundColor Red
        } elseif ($line -match "warning C\d+") {
            Write-Host "    $line" -ForegroundColor Yellow
        } elseif ($line -match "^\s*([A-Za-z0-9_\-]+\.cpp)\s*$") {
            $compileCount++
            Write-Host "    [$compileCount] $line" -ForegroundColor DarkGray
        } elseif ($line -match "Omega\.vcxproj.*->.*Omega\.exe") {
            Write-Host "    $line" -ForegroundColor Green
        } elseif ($line -match "Generating Code|Creating library|Linking") {
            Write-Host "    $line" -ForegroundColor Cyan
        } elseif ($line -match "Building Custom Rule|Auto build dll exports") {
            # noisy MSBuild chatter -- suppress
        } else {
            Write-Host "    $line" -ForegroundColor DarkGray
        }
    }
    $buildExitCode = $LASTEXITCODE
    $bldSec = [math]::Round(((Get-Date) - $bldStart).TotalSeconds, 1)

    if ($buildExitCode -ne 0) {
        Write-Host ""
        Write-Host "  *********************************************************" -ForegroundColor Red
        Write-Host "  *  BUILD FAILED (cmake --build exit=$buildExitCode)" -ForegroundColor Red
        Write-Host "  *  Duration: ${bldSec}s  |  $compileCount .cpp files compiled" -ForegroundColor Red
        Write-Host "  *  Service will restart with the PREVIOUS binary" -ForegroundColor Red
        Write-Host "  *********************************************************" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        try { Start-Service -Name $ServiceName -ErrorAction Stop }
        catch { Write-Host "  [FATAL] Could not restart service: $_" -ForegroundColor Red }
        return 1
    }
    Write-Host "  [compile+link] done in ${bldSec}s ($compileCount .cpp files compiled)" -ForegroundColor DarkCyan

    if (-not (Test-Path $BuildExe)) {
        Write-Host "  [FATAL] Build reported success (exit=0) but Omega.exe not produced at $BuildExe" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        return 1
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [7/12] Copy binary + assets
    # --------------------------------------------------------------------------
    Write-Host "[7/12] Copying assets..." -ForegroundColor Yellow
    if (-not $ColdStop) {
        Write-Host "  [hot-swap] build done -- stopping service now (downtime window opens here)" -ForegroundColor Cyan
        if (-not (Confirm-CfePosition)) {
            Write-Host "  [hot-swap] aborted pre-stop; service still running on previous binary" -ForegroundColor Yellow
            return 0
        }
        if (-not (Stop-OmegaService -Force:$ForceKill)) { return 1 }
    }
    $copyOk = $false
    for ($i = 0; $i -lt 5; $i++) {
        try { Copy-Item $BuildExe $OmegaExe -Force -ErrorAction Stop; $copyOk = $true; break }
        catch {
            Write-Host "  [WARN] Copy attempt $($i+1) failed (locked?): $_" -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    }
    if (-not $copyOk) {
        Write-Host "  [FATAL] Could not copy new Omega.exe after 5 attempts. Aborting." -ForegroundColor Red
        return 1
    }

    # ----- omega_config.ini handling (2026-04-29 AUDIT NOTE) ---------------
    # The legacy DEPLOY_OMEGA.ps1 copied config\omega_config.ini -> root
    # omega_config.ini at this step. After the 2026-04-29 audit the ROOT file
    # is the canonical config and config\omega_config.ini is a header-only
    # [CONFIG-TOMBSTONE] stub. Doing the legacy copy now silently replaces
    # the real config with the tombstone, leaving Omega.exe to run with no
    # mode / no watermark / no broker creds (mode=NOT_FOUND in the [11/12]
    # banner was the symptom that surfaced this).
    #
    # The git pull in [2/12] already restored the canonical root config from
    # origin/<Branch>, so no copy is needed. We DO still defensively reject a
    # tombstoned root file -- if origin/<Branch> ever ships a tombstoned root
    # by accident, halt the deploy before [12/12] starts the service.
    $rootCfg = "$OmegaDir\omega_config.ini"
    if (-not (Test-Path $rootCfg)) {
        Write-Host "  [FATAL] $rootCfg not found after git reset. Aborting." -ForegroundColor Red
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        return 1
    }
    $cfgFirstLines = Get-Content $rootCfg -TotalCount 5 -ErrorAction SilentlyContinue
    if ($cfgFirstLines -match 'CONFIG-TOMBSTONE|THIS FILE IS DEPRECATED') {
        Write-Host "  [FATAL] $rootCfg is a [CONFIG-TOMBSTONE] stub, not a real config." -ForegroundColor Red
        Write-Host "          Recover with:" -ForegroundColor Red
        Write-Host "            git show origin/$Branch`:omega_config.ini | Out-File $rootCfg -Encoding utf8 -Force" -ForegroundColor Red
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        return 1
    }

    # symbols.ini is regenerated from HEAD on every deploy (its values are
    # version-locked to the engine code). Production headers in include/
    # are never modified to match a different symbols.ini -- it's the
    # other way round.
    Push-Location $OmegaDir
    try {
        git show HEAD:symbols.ini 2>$null | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding ascii -Force  # ascii = no BOM (PS5.1 utf8 adds BOM -> perpetual dirty symbols.ini)
    } catch { } finally { Pop-Location }
    Copy-Item "$OmegaDir\src\gui\www\omega_index.html" "$OmegaDir\omega_index.html" -Force -ErrorAction SilentlyContinue
    Copy-Item "$OmegaDir\src\gui\www\chimera_logo.png" "$OmegaDir\chimera_logo.png" -Force -ErrorAction SilentlyContinue

    $builtAt = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
    Write-Host "  [OK] Built $sourceHashShort at $builtAt" -ForegroundColor Green
    Write-Host ""

    # --------------------------------------------------------------------------
    # [8/12] Write build stamp
    # --------------------------------------------------------------------------
    Write-Host "[8/12] Writing build stamp (source=$sourceHashShort)..." -ForegroundColor Yellow
    $exeHash   = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
    $buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")
    "GIT_HASH=$sourceHash"            | Out-File -FilePath $StampFile -Encoding utf8
    "GIT_HASH_SHORT=$sourceHashShort" | Out-File -FilePath $StampFile -Encoding utf8 -Append
    "HEAD_HASH=$ghSha"                | Out-File -FilePath $StampFile -Encoding utf8 -Append
    "EXE_SHA256=$exeHash"             | Out-File -FilePath $StampFile -Encoding utf8 -Append
    "BUILD_TIME=$buildTime"           | Out-File -FilePath $StampFile -Encoding utf8 -Append
    "EXE_PATH=$OmegaExe"              | Out-File -FilePath $StampFile -Encoding utf8 -Append
    Write-Host "  [OK] Stamp written: source=$sourceHashShort  exe_sha=$($exeHash.Substring(0,16))..." -ForegroundColor Green
    Write-Host ""

    # --------------------------------------------------------------------------
    # [9/12] Validate stamp (re-read everything; cross-check version_generated.hpp)
    # --------------------------------------------------------------------------
    Write-Host "[9/12] Validating stamp..." -ForegroundColor Yellow
    $errors = @()
    $stamp = Read-Stamp
    if (-not $stamp) { $errors += "Stamp file not readable" }
    else {
        $reHashExe = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
        if ($stamp.EXE_SHA256 -ne $reHashExe) {
            $errors += "EXE_SHA256 MISMATCH: stamp=$($stamp.EXE_SHA256.Substring(0,16)) actual=$($reHashExe.Substring(0,16))"
        }
        if ($stamp.GIT_HASH -ne $sourceHash) {
            $errors += "GIT_HASH MISMATCH: stamp=$($stamp.GIT_HASH.Substring(0,7)) expected=$sourceHashShort"
        }
        if (-not $stamp.GIT_HASH)   { $errors += "GIT_HASH field missing from stamp" }
        if (-not $stamp.EXE_SHA256) { $errors += "EXE_SHA256 field missing from stamp" }
        if (-not $stamp.BUILD_TIME) { $errors += "BUILD_TIME field missing from stamp" }
        if (-not $stamp.EXE_PATH)   { $errors += "EXE_PATH field missing from stamp" }

        $versionFile = "$OmegaDir\include\version_generated.hpp"
        if (Test-Path $versionFile) {
            $versionContent = Get-Content $versionFile -Raw
            if ($versionContent -match 'OMEGA_GIT_HASH\s+"([a-f0-9]+)"') {
                $guiHash = $Matches[1]
                # S-2026-06-20: PREFIX-tolerant compare (same convention as
                # Test-StaleBinaryAfterStart). The build-time header uses git
                # --short=7 (e.g. 4766cd9) while the stamp carries SOURCE_HASH
                # (often 9 chars, e.g. 4766cd9aa) -- same commit when one is a
                # prefix of the other. The prior exact-compare aborted the deploy
                # (leaving the service stopped) once the reconfigure-removal made
                # the header the canonical 7-char short hash.
                $hashMatch = $guiHash.StartsWith($stamp.GIT_HASH_SHORT) -or $stamp.GIT_HASH_SHORT.StartsWith($guiHash)
                # S-2026-07-12c: the header bakes `git rev-parse HEAD` while SOURCE_HASH deliberately
                # skips log-only/seed-only commits. A deploy whose HEAD is a seed-only commit (e.g. a
                # warmup CSV refresh) therefore ALWAYS mismatched here and bricked the service (today:
                # header=14311b1 seed-only HEAD, stamp=f7ce84f0 code commit). The build is still
                # exactly HEAD's tree, so accept a header that matches the stamp's HEAD_HASH too.
                if (-not $hashMatch -and $stamp.PSObject.Properties['HEAD_HASH'] -and $stamp.HEAD_HASH) {
                    $headShort = $stamp.HEAD_HASH.Substring(0, [Math]::Min(9, $stamp.HEAD_HASH.Length))
                    $hashMatch = $guiHash.StartsWith($headShort) -or $headShort.StartsWith($guiHash)
                    if ($hashMatch) { Write-Host "  [OK] GUI hash matches HEAD (seed-only commit above SOURCE_HASH): $guiHash ~ $headShort" -ForegroundColor Green }
                }
                if (-not $hashMatch) {
                    $errors += "GUI hash MISMATCH: version_generated.hpp=$guiHash  stamp_source=$($stamp.GIT_HASH_SHORT) -- run cmake configure again."
                } else {
                    Write-Host "  [OK] GUI hash matches source hash: $guiHash ~ $($stamp.GIT_HASH_SHORT)" -ForegroundColor Green
                }
            } else {
                $errors += "version_generated.hpp exists but OMEGA_GIT_HASH not found -- file corrupt"
            }
        } else {
            $errors += "version_generated.hpp not found -- cmake configure did not run or failed"
        }
        if ($errors.Count -eq 0) {
            Write-Host "  [OK] Stamp validated:" -ForegroundColor Green
            Write-Host "       source_hash = $($stamp.GIT_HASH)" -ForegroundColor Green
            Write-Host "       head_hash   = $($stamp.HEAD_HASH)" -ForegroundColor Green
            Write-Host "       exe_sha256  = $($stamp.EXE_SHA256.Substring(0,16))..." -ForegroundColor Green
            Write-Host "       built       = $($stamp.BUILD_TIME)" -ForegroundColor Green
        }
    }
    if ($errors.Count -gt 0) {
        Write-Host ""
        Write-Host "  *** VALIDATION FAILED -- Omega will NOT start ***" -ForegroundColor Red
        $errors | ForEach-Object { Write-Host "  ERROR: $_" -ForegroundColor Red }
        Write-Host "  Stamp file removed. Run OMEGA.ps1 deploy again after fixing the issue." -ForegroundColor Yellow
        Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
        return 1
    }
    Write-Host ""

    # --------------------------------------------------------------------------
    # [10/12] Verify symbols.ini
    # --------------------------------------------------------------------------
    Write-Host "[10/12] Verifying symbols.ini..." -ForegroundColor Yellow
    $symFails = Test-SymbolsIni
    if ($symFails -and $symFails.Count -gt 0) {
        Write-Host "  [ERROR] symbols.ini verification FAILED:" -ForegroundColor Red
        $symFails | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
        return 1
    }
    Write-Host "  [OK] symbols.ini verified" -ForegroundColor Green
    Write-Host ""

    # --------------------------------------------------------------------------
    # [11/12] Watermark + mode check
    # --------------------------------------------------------------------------
    Write-Host "[11/12] Config check..." -ForegroundColor Yellow
    $cfg = Test-WatermarkConfig
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor Yellow
    Write-Host "  DEPLOY READY -- FINAL STATUS" -ForegroundColor Yellow
    Write-Host "=======================================================" -ForegroundColor Yellow
    Write-Host "  source commit  = $sourceHash" -ForegroundColor Green
    Write-Host "  head commit    = $ghSha" -ForegroundColor DarkGray
    Write-Host "  exe SHA256     = $($exeHash.Substring(0,16))...  [VALIDATED]" -ForegroundColor Green
    Write-Host "  mode           = $($cfg.Mode)" -ForegroundColor Cyan
    Write-Host "  watermark_pct  = $($cfg.Watermark)" -ForegroundColor Cyan
    Write-Host "  built          = $buildTime" -ForegroundColor Cyan
    Write-Host "=======================================================" -ForegroundColor Yellow
    Write-Host ""

    if ($cfg.Mode -eq "NOT_FOUND" -or $cfg.Watermark -eq "NOT_FOUND") {
        # NOT_FOUND means the regex `^mode\s*=` (or `session_watermark_pct\s*=`)
        # didn't match anywhere in omega_config.ini. That happens when the file
        # is the [CONFIG-TOMBSTONE] stub or otherwise corrupted. We already
        # rejected a tombstoned root in [7/12], so seeing NOT_FOUND here means
        # something else clobbered the file between [7/12] and now -- not safe
        # to start the service.
        Write-Host "  *** FATAL: mode=$($cfg.Mode), watermark=$($cfg.Watermark) -- omega_config.ini is missing required keys ***" -ForegroundColor Red
        Write-Host "  *** Recover with:                                              ***" -ForegroundColor Red
        Write-Host "  ***   git show origin/$Branch`:omega_config.ini | Out-File $rootCfg -Encoding utf8 -Force ***" -ForegroundColor Red
        Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
        return 1
    }
    if ($cfg.Testing) {
        Write-Host "  *** WARNING: session_watermark_pct=$($cfg.Watermark) (TESTING VALUE) ***" -ForegroundColor Red
        Write-Host "  *** No drawdown protection active                                   ***" -ForegroundColor Red
    }
    if ($cfg.Mode -eq "LIVE" -and $cfg.Testing) {
        Write-Host "  *** FATAL: mode=LIVE with watermark=0 -- BLOCKED ***" -ForegroundColor Red
        Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
        return 1
    }

    # Ensure log dirs exist
    New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
    New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
    New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

    # Truncate latest.log
    if (Test-Path $LatestLog) { Clear-Content $LatestLog -ErrorAction SilentlyContinue }

    # cTrader bar_failed cleanup removed S13 2026-05-08 -- cTrader Open API
    # surface culled. Any leftover ctrader_bar_failed.txt from a pre-S13 build
    # is harmless (no code reads it any more).

    Rotate-ServiceLogs

    Write-Host ""
    Write-Host "########################################################" -ForegroundColor Yellow
    Write-Host "  COMMIT : $sourceHashShort" -ForegroundColor Yellow
    Write-Host "  BUILT  : $builtAt" -ForegroundColor Yellow
    Write-Host "  MODE   : $($cfg.Mode)" -ForegroundColor $modeColor
    Write-Host "  GUI    : http://45.85.3.79:7779" -ForegroundColor Yellow
    Write-Host "  UI     : http://45.85.3.79:7781  (omega-terminal)" -ForegroundColor Yellow
    Write-Host "########################################################" -ForegroundColor Yellow
    Write-Host ""

    # --------------------------------------------------------------------------
    # [12/12] Start service + post-start stale-binary checks
    # --------------------------------------------------------------------------
    Lap "before [12] service start (delta from [6] = build+copy+stamp cost)"
    Write-Host "[12/12] Starting Omega service..." -ForegroundColor Yellow
    if (Test-Path $DeployFlag) { Remove-Item $DeployFlag -Force -ErrorAction SilentlyContinue }
    if (-not (Start-OmegaService)) { return 1 }
    Lap "deploy COMPLETE (delta from [12] = restart+verify cost)"
    if (-not (Test-StaleBinaryAfterStart -ExpectedShortHash $sourceHashShort)) { return 1 }

    if (-not $SkipVerify -and (Test-Path "$OmegaDir\VERIFY_STARTUP.ps1")) {
        Write-Host ""
        # S-2026-06-20: poll for the boot git-hash marker (the NEW hash, so a stale
        # pre-rotation line can't match) instead of a flat 60s wait. ~8-10s typical.
        Write-Host "  Waiting for boot marker (git-hash, max 60s) then VERIFY_STARTUP..." -ForegroundColor Cyan
        $latestLog    = "$OmegaDir\logs\latest.log"
        $bootStart    = Get-Date
        $bootDeadline = $bootStart.AddSeconds(60)
        # match the 7-char short hash the binary actually logs (version_generated.hpp
        # uses git --short=7), tolerant of a longer stamp hash.
        $bootPat      = "Git hash.*" + [regex]::Escape($sourceHashShort.Substring(0, [Math]::Min(7, $sourceHashShort.Length)))
        $booted       = $false
        while ((Get-Date) -lt $bootDeadline) {
            Start-Sleep -Seconds 2
            if (Test-Path $latestLog) {
                try {
                    if ((Get-Content $latestLog -Tail 60 -ErrorAction SilentlyContinue) -match $bootPat) { $booted = $true; break }
                } catch { }
            }
        }
        if ($booted) {
            Write-Host ("  [OK] boot marker ({0}) seen in {1:N0}s" -f $sourceHashShort, ((Get-Date) - $bootStart).TotalSeconds) -ForegroundColor Green
        } else {
            Write-Host "  [WARN] no boot marker in 60s -- running VERIFY_STARTUP anyway" -ForegroundColor Yellow
        }
        & "$OmegaDir\VERIFY_STARTUP.ps1" -OmegaDir $OmegaDir
    }

    $elapsed = (Get-Date) - $startTime
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor Cyan
    Write-Host ("  DONE: {0:mm}m {0:ss}s" -f $elapsed) -ForegroundColor Cyan
    Write-Host "=======================================================" -ForegroundColor Cyan
    return 0
    }
    finally {
        # Always release + dispose the singleton deploy mutex. A completed or
        # early-returning deploy must never leave Global\OmegaDeployMutex held
        # for the life of the (long-running watchdog) process -- the prior code
        # acquired it but never released it, which jammed every later deploy
        # with "Another deploy is already running."
        if ($gotMutex) { try { $deployMutex.ReleaseMutex() } catch { } }
        $deployMutex.Dispose()
    }
}

# ==============================================================================
# Subcommand: watchdog  (long-running monitor; OMEGA_WATCHDOG.ps1 v2.0 logic)
# ==============================================================================
function Get-OpenPositionCount {
    param([string]$Url, [int]$TimeoutSec)
    try {
        # S-2026-06-25: use Invoke-WebRequest so a 200 with an EMPTY array (flat book) is
        # "healthy, 0 positions" -- NOT confused with "down". The /api/v1/omega/positions
        # route returns a JSON ARRAY of open positions; an unreachable binary throws (down).
        $web = Invoke-WebRequest -Uri $Url -TimeoutSec $TimeoutSec -UseBasicParsing -ErrorAction Stop
        if ($web.StatusCode -ne 200) {
            $script:TelemetryLastError = "telemetry HTTP $($web.StatusCode)"
            return $null
        }
        $resp = $null
        try { $resp = $web.Content | ConvertFrom-Json -ErrorAction Stop } catch {
            $script:TelemetryLastError = "telemetry non-JSON body"
            return $null
        }
        $script:TelemetryLastError = $null
        if ($resp -is [array])  { return $resp.Count }                       # positions array (0 = flat)
        if ($null -eq $resp)    { return 0 }                                 # empty body / [] -> flat
        if ($resp.PSObject.Properties.Name -contains 'symbol' -and `
            $resp.PSObject.Properties.Name -notcontains 'open_positions' -and `
            $resp.PSObject.Properties.Name -notcontains 'live_trades') {
            return 1                                                          # a single position object
        }
        $liveTrades = $resp.live_trades
        $openPos    = $resp.open_positions
        $liveCount = 0
        $openCount = 0
        if ($null -ne $liveTrades) {
            if ($liveTrades -is [array]) { $liveCount = $liveTrades.Count }
            elseif ($liveTrades.Count) { $liveCount = [int]$liveTrades.Count }
        }
        if ($null -ne $openPos) {
            if ($openPos -is [array]) { $openCount = $openPos.Count }
            elseif ($openPos.Count) { $openCount = [int]$openPos.Count }
        }
        $script:TelemetryLastError = $null
        return [Math]::Max($liveCount, $openCount)
    } catch {
        $script:TelemetryLastError = $_.Exception.Message
        return $null
    }
}

function Test-SafeToRestart {
    param([switch]$AllowWhenServiceDown, [string]$Url, [int]$TimeoutSec)
    if ($AllowWhenServiceDown) {
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -eq $svc -or $svc.Status -eq 'Stopped') {
            Write-WD "SAFE-TO-RESTART: service is stopped, no live positions possible from dead binary"
            return $true
        }
    }
    $n = Get-OpenPositionCount -Url $Url -TimeoutSec $TimeoutSec
    if ($null -eq $n) {
        Write-WD "SAFE-TO-RESTART: DEFERRED -- position check failed ($script:TelemetryLastError). Assuming open."
        return $false
    }
    if ($n -gt 0) {
        Write-WD "SAFE-TO-RESTART: DEFERRED -- $n open position(s)"
        return $false
    }
    Write-WD "SAFE-TO-RESTART: confirmed 0 open positions"
    return $true
}

function Get-GitHubHead {
    # S-2026-06-25: Trendiisales/Omega is PUBLIC, so the poll does NOT need a token. The
    # old code returned $null when the token file was missing/expired -> GITHUB-POLL
    # "failed to reach GitHub API" forever (the .github_token PAT had expired). Now: use
    # the token if present+valid, else fall back to UNAUTHENTICATED (User-Agent header is
    # mandatory for the GitHub API; 60 req/hr unauth is plenty at the 300s poll interval).
    $uri = "https://api.github.com/repos/Trendiisales/Omega/commits/$Branch"
    $token = try { if (Test-Path $TokenFile) { (Get-Content $TokenFile -Raw).Trim() } else { $null } } catch { $null }
    if ($token) {
        try {
            $resp = Invoke-RestMethod -Uri $uri -Headers @{ Authorization="token $token"; "User-Agent"="OmegaWatchdog" } -TimeoutSec 10 -ErrorAction Stop
            return $resp.sha.Substring(0,7)
        } catch { }   # token likely expired -> fall through to unauthenticated
    }
    try {
        $resp = Invoke-RestMethod -Uri $uri -Headers @{ "User-Agent"="OmegaWatchdog" } -TimeoutSec 10 -ErrorAction Stop
        return $resp.sha.Substring(0,7)
    } catch { return $null }
}

function Get-RunningHash {
    # Resolve the git hash of the CURRENTLY-RUNNING Omega.exe.
    #
    # S22 fix (2026-05-11): the engine prints `[Omega] Git hash: XXXXXXX` to
    # stderr ONCE at startup (omega_main.hpp line 23). After Omega has been up
    # for more than a few minutes the line falls off the tail-300 window of
    # latest.log, and this function returned $null -- which prevented the
    # GITHUB-POLL branch from ever firing AUTO-UPDATE. The fix is to consult
    # omega_build.stamp FIRST (canonical source of truth, written by the deploy
    # pipeline) and fall back to the log scan only if the stamp is missing.
    try {
        $stamp = Read-Stamp
        if ($stamp -and $stamp.GIT_HASH_SHORT) {
            $h = $stamp.GIT_HASH_SHORT.Trim()
            if ($h -match '^[a-f0-9]{7}$') { return $h }
        }
        if ($stamp -and $stamp.GIT_HASH) {
            $h = $stamp.GIT_HASH.Trim()
            if ($h.Length -ge 7 -and $h -match '^[a-f0-9]+$') {
                return $h.Substring(0, 7)
            }
        }
    } catch { }

    try {
        if (-not (Test-Path $LatestLog)) { return $null }
        $tail = Get-Content $LatestLog -Tail 300 -ErrorAction SilentlyContinue
        $line = $tail | Select-String 'Git hash:' | Select-Object -Last 1
        if ($line -match 'Git hash:\s+([a-f0-9]{7})') { return $Matches[1] }
    } catch { }
    return $null
}

function Get-L2CsvPath {
    # Returns the path to the CANONICAL L2 tick CSV that proves the engine is
    # capturing market depth. XAUUSD is the canonical symbol because (a) its
    # writer fires unconditionally on every XAUUSD tick (see tick_gold.hpp
    # line 989) and (b) XAUUSD is live during every market hour the watchdog
    # checks.
    #
    # S22 fix (2026-05-11): pre-S13 the engine wrote a single
    # `l2_ticks_YYYY-MM-DD.csv` and this function correctly pointed at it.
    # S13 cTrader cull (commit 4827ad4, 2026-05-08) split the writer per
    # symbol (l2_ticks_XAUUSD_YYYY-MM-DD.csv, l2_ticks_US500_..., etc.) and
    # this function was not updated, so the watchdog has been false-alerting
    # L2-CSV-MISSING every 15s ever since.
    $today = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd")
    return "$OmegaDir\logs\l2_ticks_XAUUSD_$today.csv"
}

function Test-MarketHours {
    $h = (Get-Date).ToUniversalTime().Hour
    return ($h -ge 7 -and $h -lt 22)
}

function Invoke-Watchdog {
    $WATCHDOG_VERSION = "OMEGA.ps1 watchdog (v3.0 / S12 consolidated 2026-05-07)"
    $script:TelemetryLastError = $null
    $LastGitHubCheck = [DateTime]::MinValue
    $TelemetryHealthy = $false

    $null = New-Item -ItemType Directory -Force -Path (Split-Path $WatchdogLog) -ErrorAction SilentlyContinue

    Write-WD "=== WATCHDOG STARTED === version=$WATCHDOG_VERSION"
    Write-WD "Config: StaleThreshold=${StaleThresholdSec}s L2Threshold=${L2StaleThresholdSec}s Interval=${CheckIntervalSec}s"
    Write-WD "Telemetry URL: $TelemetryUrl"
    Write-WD "Watchdog log: $WatchdogLog (independent of latest.log)"

    $probeN = Get-OpenPositionCount -Url $TelemetryUrl -TimeoutSec $TelemetryTimeoutSec
    if ($null -eq $probeN) {
        Write-WD "STARTUP-WARN: telemetry probe failed ($script:TelemetryLastError). Auto-update deferred until endpoint healthy."
        $TelemetryHealthy = $false
    } else {
        Write-WD "STARTUP-OK: telemetry probe succeeded, open_positions=$probeN"
        $TelemetryHealthy = $true
    }

    $restartCount = 0
    $l2AlertCount = 0
    # S88-followup (2026-05-27): escalation counters for silent-failure modes.
    # Without these, GITHUB-POLL failed silently for 2h+ when PAT expired in
    # May 2026 -- watchdog logged "failed to reach" every 5 min with no
    # threshold escalation. Same pattern for sustained HASH-MISMATCH (running
    # binary stale vs origin/main) which previously had no alert path.
    # Counters reset on the next OK cycle; sentinel file allows external
    # surfacing (dashboard / push notification / cron alert).
    $ghPollFailCount  = 0     # consecutive GITHUB-POLL failures
    $hashMismatchCount = 0    # consecutive cycles with running != github HEAD
    $GhFailAlertThreshold   = 6    # 6 polls * 5 min = 30 min before alert
    $HashMismatchAlertThreshold = 12  # 12 polls * 5 min = 60 min before alert
    $AlertSentinelPath = Join-Path $OmegaDir "logs\WATCHDOG_ALERT.txt"
    function Write-Sentinel([string]$reason) {
        $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss UTC"
        # Append so multiple alerts accumulate; deploy/restart clears the file.
        Add-Content -Path $AlertSentinelPath -Value "$ts | $reason"
    }
    function Clear-Sentinel([string]$reason) {
        if (Test-Path $AlertSentinelPath) {
            Remove-Item -Path $AlertSentinelPath -Force -ErrorAction SilentlyContinue
            Write-WD "WATCHDOG-RECOVERED: $reason -- sentinel cleared"
        }
    }

    while ($true) {
        Start-Sleep -Seconds $CheckIntervalSec

        # --- 1. Service running? ----------------------------------------------
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -eq $svc -or $svc.Status -ne 'Running') {
            Write-WD "SERVICE-DOWN: status=$($svc.Status). Restart check..."
            if (Test-SafeToRestart -AllowWhenServiceDown -Url $TelemetryUrl -TimeoutSec $TelemetryTimeoutSec) {
                Write-WD "SERVICE-DOWN: invoking OMEGA.ps1 deploy -SkipVerify (attempt #$($restartCount + 1))"
                & $PSCommandPath deploy -OmegaDir $OmegaDir -Branch $Branch -SkipVerify
                $restartCount++
                Write-WD "SERVICE-DOWN: restart #$restartCount complete. Waiting ${PostRestartWaitSec}s..."
                Start-Sleep -Seconds $PostRestartWaitSec
            } else {
                Write-WD "SERVICE-DOWN: restart deferred -- will retry next cycle"
            }
            continue
        }

        # --- 2. latest.log stale? ---------------------------------------------
        if (-not (Test-Path $LatestLog)) {
            Write-WD "LOG-MISSING: $LatestLog not found. Restart check..."
            if (Test-SafeToRestart -Url $TelemetryUrl -TimeoutSec $TelemetryTimeoutSec) {
                Write-WD "LOG-MISSING: invoking OMEGA.ps1 deploy -SkipVerify (attempt #$($restartCount + 1))"
                & $PSCommandPath deploy -OmegaDir $OmegaDir -Branch $Branch -SkipVerify
                $restartCount++
                Start-Sleep -Seconds $PostRestartWaitSec
            } else {
                Write-WD "LOG-MISSING: restart deferred"
            }
            continue
        }
        $logAge = $null
        try {
            $logItem = Get-Item $LatestLog -ErrorAction Stop
            $logAge = ((Get-Date) - $logItem.LastWriteTime).TotalSeconds
        } catch {
            Write-WD "LOG-AGE-ERR: could not read $LatestLog timestamp: $_"
        }
        if ($null -ne $logAge -and $logAge -gt $StaleThresholdSec) {
            Write-WD "LOG-STALE: age=${logAge}s (max=${StaleThresholdSec}s). Restart check..."
            if (Test-SafeToRestart -Url $TelemetryUrl -TimeoutSec $TelemetryTimeoutSec) {
                Write-WD "LOG-STALE: invoking OMEGA.ps1 deploy -SkipVerify (attempt #$($restartCount + 1))"
                & $PSCommandPath deploy -OmegaDir $OmegaDir -Branch $Branch -SkipVerify
                $restartCount++
                Write-WD "LOG-STALE: restart #$restartCount complete. Waiting ${PostRestartWaitSec}s..."
                Start-Sleep -Seconds $PostRestartWaitSec
            } else {
                Write-WD "LOG-STALE: restart deferred -- will retry next cycle"
            }
            continue
        }

        # --- 3. L2 CSV writing during market hours? ---------------------------
        if (Test-MarketHours) {
            $l2path = Get-L2CsvPath
            if (-not (Test-Path $l2path)) {
                Write-WD "L2-CSV-MISSING: $l2path not found during market hours!"
                $l2AlertCount++
            } else {
                $l2Age = $null
                $l2Size = 0
                try {
                    $l2Item = Get-Item $l2path -ErrorAction Stop
                    $l2Age = ((Get-Date) - $l2Item.LastWriteTime).TotalSeconds
                    $l2Size = [math]::Round($l2Item.Length / 1MB, 2)
                } catch {
                    Write-WD "L2-AGE-ERR: $_"
                }
                if ($null -ne $l2Age -and $l2Age -gt $L2StaleThresholdSec) {
                    Write-WD "L2-CSV-STALE: age=${l2Age}s size=${l2Size}MB -- L2 DATA LOSS OCCURRING"
                    $l2AlertCount++
                } else {
                    if ($l2AlertCount -gt 0) {
                        Write-WD "L2-CSV-RECOVERED: age=${l2Age}s size=${l2Size}MB after $l2AlertCount alerts"
                        $l2AlertCount = 0
                    }
                }
            }
        }

        # --- 4. Heartbeat + GitHub HEAD auto-update every 5 minutes -----------
        $nowMin = [int](Get-Date).Minute
        if ($nowMin % 5 -eq 0 -and [int](Get-Date).Second -lt $CheckIntervalSec) {
            $l2path = Get-L2CsvPath
            $l2size = "MISSING"
            if (Test-Path $l2path) {
                try { $l2size = [math]::Round((Get-Item $l2path).Length / 1MB, 2) } catch { }
            }
            $running = Get-RunningHash
            $logAgeStr = if ($null -ne $logAge) { "${logAge}s" } else { "unknown" }
            Write-WD "HEARTBEAT log_age=$logAgeStr L2=$l2size MB hash=$running restarts=$restartCount l2alerts=$l2AlertCount telemetry_healthy=$TelemetryHealthy"

            $secsSinceGhCheck = ((Get-Date) - $LastGitHubCheck).TotalSeconds
            if ($secsSinceGhCheck -ge $GitHubPollIntervalSec) {
                $LastGitHubCheck = Get-Date
                $ghHead = Get-GitHubHead
                if (-not $ghHead) {
                    $ghPollFailCount++
                    Write-WD "GITHUB-POLL: failed to reach GitHub API -- will retry (consecutive_fails=$ghPollFailCount)"
                    if ($ghPollFailCount -eq $GhFailAlertThreshold) {
                        $minutesDown = [int](($GhFailAlertThreshold * $GitHubPollIntervalSec) / 60)
                        $msg = "GITHUB-POLL stuck failing for ${minutesDown}+ minutes -- likely PAT expired at $TokenFile or network blocked. Watchdog cannot detect new commits."
                        Write-WD "[WATCHDOG-ALERT] $msg"
                        Write-Sentinel "GITHUB-POLL-DOWN: $msg"
                    }
                } elseif (-not $running) {
                    Write-WD "GITHUB-POLL: ghHead=$ghHead but running hash unknown -- cannot compare. Will retry."
                } elseif ($ghHead -eq $running) {
                    if ($ghPollFailCount -gt 0) { Write-WD "GITHUB-POLL-RECOVERED: after $ghPollFailCount failures"; Clear-Sentinel "GITHUB-POLL recovered" }
                    if ($hashMismatchCount -gt 0) { Write-WD "HASH-RECOVERED: after $hashMismatchCount mismatch cycles"; Clear-Sentinel "HASH recovered" }
                    $ghPollFailCount = 0
                    $hashMismatchCount = 0
                    Write-WD "HASH-OK: running=$running == github=$ghHead"
                } else {
                    if ($ghPollFailCount -gt 0) { $ghPollFailCount = 0; Write-WD "GITHUB-POLL-RECOVERED (now showing mismatch)" }
                    $hashMismatchCount++
                    # ================================================================
                    # S36 2026-05-12 -- AUTO-DEPLOY DISABLED by operator directive.
                    # ================================================================
                    # The watchdog still detects when origin/main has moved ahead
                    # of the running binary and logs the mismatch -- so you keep
                    # visibility into "VPS is N commits behind GitHub" -- but it
                    # will NOT pull / build / restart on its own anymore.
                    #
                    # To apply a new commit on the VPS, run manually from C:\Omega:
                    #     .\OMEGA.ps1 deploy
                    #
                    # To re-enable auto-deploy, revert this block (see git history
                    # of OMEGA.ps1 around 2026-05-12 / S36-Pn-deploy-control commit).
                    Write-WD "HASH-MISMATCH: running=$running github=$ghHead -- AUTO-UPDATE DISABLED (operator directive). Run '.\OMEGA.ps1 deploy' manually to apply. (consecutive_mismatch=$hashMismatchCount)"
                    if ($hashMismatchCount -eq $HashMismatchAlertThreshold) {
                        $minutesStale = [int](($HashMismatchAlertThreshold * $GitHubPollIntervalSec) / 60)
                        $msg = "Running binary stale for ${minutesStale}+ minutes (running=$running github=$ghHead). Run '.\OMEGA.ps1 deploy' on VPS to ship pending commits."
                        Write-WD "[WATCHDOG-ALERT] $msg"
                        Write-Sentinel "HASH-MISMATCH: $msg"
                    }
                }
            }
        }
    }
}

# ==============================================================================
# Help
# ==============================================================================
function Show-Help {
    Write-Host @"

OMEGA.ps1 -- Unified Omega control script

Usage:
    .\OMEGA.ps1 <command> [options]

Commands:
    deploy      Full pipeline: stop, git pull, build (UI + C++), stamp,
                validate, start, verify. Replaces QUICK_RESTART + DEPLOY_OMEGA.
                Options: -Branch <name>  (default 'main')
                         -SkipVerify     (skip post-start VERIFY_STARTUP)
                         -ForceKill      (taskkill /F instead of graceful stop)

    restart     Stop + start service. No rebuild. Stamp-verifies before
                starting. Replaces RESTART_OMEGA / restart half of QUICK_RESTART.
                Options: -SkipVerify, -ForceKill

    start       Start service (or attach if running). Stamp-verifies first.
                Warns if origin/<Branch> has new source commits beyond stamp.
                Replaces START_OMEGA.
                Options: -Branch <name>

    stop        Graceful Stop-Service with -ForceKill fallback. Prompts before
                stopping while a CFE position is open.
                Options: -ForceKill

    watchdog    Long-running monitor loop. Used by INSTALL_OMEGA.ps1
                -InstallWatchdog as the NSSM-wrapped service body.
                Replaces OMEGA_WATCHDOG.ps1.
                Options: -StaleThresholdSec, -L2StaleThresholdSec,
                         -CheckIntervalSec, -PostRestartWaitSec,
                         -GitHubPollIntervalSec, -TelemetryUrl,
                         -TelemetryTimeoutSec

    help        Show this help.

Common options:
    -OmegaDir <path>          Default: C:\Omega
    -GitHubToken <token>      Falls back to <OmegaDir>\.github_token

Examples:
    .\OMEGA.ps1 deploy
    .\OMEGA.ps1 deploy -Branch omega-terminal
    .\OMEGA.ps1 restart
    .\OMEGA.ps1 start
    .\OMEGA.ps1 stop -ForceKill

"@ -ForegroundColor Cyan
}

# ==============================================================================
# Dispatch
# ==============================================================================
switch ($Command) {
    'deploy'   { exit (Invoke-Deploy) }
    'restart'  { exit (Invoke-Restart) }
    'start'    { exit (Invoke-Start) }
    'stop'     { exit (Invoke-Stop) }
    'watchdog' { Invoke-Watchdog; exit 0 }
    'help'     { Show-Help; exit 0 }
    ''         { Show-Help; exit 1 }
    default    {
        Write-Host "Unknown command: $Command" -ForegroundColor Red
        Show-Help
        exit 1
    }
}
