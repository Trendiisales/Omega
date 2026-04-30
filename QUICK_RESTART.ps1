#Requires -Version 5.1
# QUICK_RESTART.ps1  -- v3.3 2026-04-30 PM
# Service-based restart. Stops the Omega NSSM service, pulls source from GitHub,
# builds, starts the service, verifies via service status + log hash.
#
# Architecture:
#   Omega runs as NSSM-wrapped Windows service (auto-start, LocalSystem).
#   NSSM redirects stdout+stderr to C:\Omega\logs\omega_service_stdout.log.
#   This script NEVER uses Start-Process for Omega -- always Start-Service.
#   OMEGA_WATCHDOG.ps1 monitors via Get-Service and calls this script.
#
# Changes from prior version:
#   * Uses Stop-Service / Start-Service instead of taskkill + Start-Process
#   * UTF-8 BOM (PowerShell 5.1 parses box-draw characters correctly)
#   * Removed duplicate $confirm variable (reused for position check AND process kill)
#   * Unified on omega_service_stdout.log as the single source of truth
#   * CFE pre-check uses same log
#
# v3.3 (2026-04-30 PM) -- audit-fixes-33 stderr-mangling fix:
#   * Re-applies the v3.2 wipe-race fix (rolled back at 19d0f93 while an
#     unrelated cl.exe crash was being diagnosed). v3.2 logic is restored
#     verbatim; v3.3 adds ON TOP of it. See v3.2 block below for details.
#   * Fixes mangling of cmake / MSBuild / cl.exe stderr lines in the
#     [3/4] Building section. The v3.1/v3.2 stream did `2>&1 | ForEach-Object
#     { $line = $_.ToString(); ... }`. When PowerShell 5.1 marshals native
#     stderr through `2>&1`, each line becomes an ErrorRecord. For most
#     stderr lines `$_.ToString()` returns the raw text, BUT when the
#     ErrorRecord wraps a RemoteException (which happens when MSBuild emits
#     structured-output diagnostics, see https://aka.ms/cpp/structured-output),
#     ToString() flattens to the literal string
#     '[System.Management.Automation.RemoteException]' and the actual
#     compiler error text is dropped. Net effect: the script saw no error
#     content, classified the line as 'unmatched / gray', and the user saw
#     a build failure with no diagnostic message -- the failure that
#     hid the C2065 g_macroDetector errors in HBG/MCE for several hours
#     on 2026-04-30.
#   * v3.3 introduces Format-NativeOutputLine which checks for ErrorRecord
#     and pulls the real text from .Exception.Message / .TargetObject
#     before falling back to ToString(). Both stream pipelines (cmake
#     configure + cmake --build) call it instead of ToString() directly.
#   * No change to wipe-race retry logic, Step 0/1/2/4, stale-binary
#     checks, or recovery flow.
#
# v3.2 (2026-04-30) -- audit-fixes-32 wipe-race fix:
#   * Hoisted $zombieNames to script scope. Single source of truth shared
#     by Step 0 pre-flight kill, the new defensive pre-wipe re-kill, and
#     the per-attempt re-kill inside the wipe retry loops.
#   * Added defensive pre-wipe zombie re-kill at the top of the
#     "INCREMENTAL BUILD PREP" `if (Test-Path $buildDir)` block. Step 0's
#     kill runs before git pull; by the time we reach the wipe, 5-30s of
#     Stop-Service / git fetch / git reset have elapsed and a build process
#     could (in rare cases) have respawned. This second pass closes the
#     race window observed during the 2026-04-30 deploy where the wipe
#     hit a "Permission denied on main.obj" lock from a stuck cl.exe.
#   * Wrapped BOTH wipe paths in retry-with-backoff:
#       - Full-wipe (`unsuccessfulbuild` stamp present): single
#         Remove-Item with up to 5 attempts, 200ms->3.2s exponential.
#       - Per-file wipe (.obj/.pch/.pdb/.iobj/.ipdb): per-item retry,
#         tracking which files remain locked across attempts so partial
#         progress isn't lost.
#     Between every retry the loop re-kills any zombies in $zombieNames
#     so a stuck cl.exe / link.exe / mspdbsrv can't permanently hold
#     handles on main.obj / link-cache / PDB files. If all 5 attempts
#     still fail the script logs the failure but keeps going -- cmake
#     configure becomes the next gate, and the v3.1 build-failure
#     recovery path (restart service with previous Omega.exe) still
#     applies.
#   * No change to Step 0, Step 1 (Stop-Service), Step 2 (git pull),
#     Step 3 (cmake configure / build), or Step 4 (Start-Service +
#     stale-binary checks). Wipe race fix is fully scoped to the
#     "INCREMENTAL BUILD PREP" section between Steps 2 and 3.
#
# v3.1 (2026-04-23) -- Session 11 footgun fix:
#   * Added $LASTEXITCODE guards after git rev-parse, cmake configure, cmake --build
#   * Build failures now auto-restart service with PREVIOUS binary so live trading
#     is never left down silently. The service-path Omega.exe (C:\Omega\Omega.exe)
#     is only overwritten AFTER a successful build (L302 Copy-Item), so restart
#     on failure uses the last known-good binary automatically.
#   * Root cause: Session 11 commits 10-19 had three silent compile failures
#     that the old script did not detect. Service was (re)started against a
#     stale pre-commit-10 binary for ~30min while configs had moved forward.

param(
    [switch]$SkipVerify,
    [string]$OmegaDir = "C:\Omega",
    [string]$GitHubToken = "",
    [int]$StopTimeoutSec = 30,
    [int]$StartupWaitSec = 15,
    [switch]$ForceKill
)

Set-StrictMode -Off
$ErrorActionPreference = "Continue"

# ──────────────────────────────────────────────────────────────────────────────
# Build-process zombie names (hoisted to script scope in v3.2).
# Single source of truth used by:
#   * Step 0 pre-flight zombie cleanup
#   * Defensive pre-wipe zombie re-kill (top of "INCREMENTAL BUILD PREP")
#   * Per-attempt zombie re-kill inside the wipe retry loops
# Adding a name here automatically picks it up in all three places.
# ──────────────────────────────────────────────────────────────────────────────
$zombieNames = @('cl', 'link', 'MSBuild', 'mspdbsrv', 'tracker',
                 'VBCSCompiler', 'cvtres', 'rc', 'cmake', 'cmake-gui',
                 'lib', 'mt', 'ml', 'cvtcil', 'CL')

# ──────────────────────────────────────────────────────────────────────────────
# Format-NativeOutputLine (added v3.3 audit-fixes-33).
#
# When PowerShell 5.1 captures native command stderr via `nativeCmd 2>&1`,
# each stderr line arrives in the pipeline as a System.Management.Automation
# .ErrorRecord. Calling .ToString() on those ErrorRecords USUALLY returns the
# raw stderr text -- but when the underlying Exception is a RemoteException
# (PowerShell's wrapper for structured-output diagnostics emitted by MSBuild
# on Visual Studio 17.x with /nologo + structured output, see
# https://aka.ms/cpp/structured-output), ToString() flattens to the literal
# string "[System.Management.Automation.RemoteException]" and the real text
# is unreachable from .ToString().
#
# Concretely, the v3.1/v3.2 build streamer caught these mangled lines in its
# else-branch and printed them DarkGray, which made build failures with real
# C2065 / C2660 / LNK errors appear silent: the user saw the framing banner
# 'BUILD FAILED (cmake --build exit=N)' with no diagnostic content above it.
#
# This helper unwraps in priority order:
#   1. ErrorRecord.Exception.Message  (the original cl.exe / MSBuild text
#                                      for normal stderr ErrorRecords)
#   2. ErrorRecord.TargetObject       (where some PSv5 builds stash the raw
#                                      text when the wrapper exception is a
#                                      RemoteException with empty .Message)
#   3. ToString()                     (everything else: regular strings,
#                                      typed objects, etc.)
#
# Returning empty string for $null avoids a NullReferenceException in the
# downstream regex matches.
# ──────────────────────────────────────────────────────────────────────────────
function Format-NativeOutputLine {
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

if ($GitHubToken -eq "") {
    $tf = "$OmegaDir\.github_token"
    if (Test-Path $tf) { $GitHubToken = (Get-Content $tf -Raw).Trim() }
}

$ServiceName  = "Omega"
$OmegaExe     = "$OmegaDir\Omega.exe"
$BuildExe     = "$OmegaDir\build\Release\Omega.exe"
$buildDir     = "$OmegaDir\build"
$cmakeExe     = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
$ConfigSrc    = "$OmegaDir\omega_config.ini"
$LogStdout    = "$OmegaDir\logs\omega_service_stdout.log"
$LogStderr    = "$OmegaDir\logs\omega_service_stderr.log"
$startTime    = Get-Date

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  QUICK RESTART  v3.3" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

$modeMatch = Select-String -Path $ConfigSrc -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue

# ==============================================================================
# PRE-CHECK: Warn if CandleFlow position is open
# A restart force-closes any open CFE position at current market price.
# ==============================================================================
if (Test-Path $LogStdout) {
    $tail = Get-Content $LogStdout -Tail 1000
    $lastEntry = ($tail | Select-String "\[CFE\] ENTRY") | Select-Object -Last 1
    $lastExit  = ($tail | Select-String "\[CFE\] EXIT")  | Select-Object -Last 1
    $cfeOpen = $false
    if ($lastEntry) {
        if (-not $lastExit) {
            $cfeOpen = $true
        } elseif ($lastEntry.LineNumber -gt $lastExit.LineNumber) {
            $cfeOpen = $true
        }
    }
    if ($cfeOpen) {
        Write-Host ""
        Write-Host "╔══════════════════════════════════════════════════════════╗" -ForegroundColor Red
        Write-Host "║  WARNING: CandleFlow position is OPEN                    ║" -ForegroundColor Red
        Write-Host "║  Restarting now will FORCE CLOSE at current market price ║" -ForegroundColor Red
        Write-Host "╚══════════════════════════════════════════════════════════╝" -ForegroundColor Red
        Write-Host ""
        Write-Host "  Last entry:" -ForegroundColor Yellow
        Write-Host "  $($lastEntry.Line)" -ForegroundColor Yellow
        Write-Host ""
        Write-Host "  Type YES to force-restart anyway (will take the loss): " -ForegroundColor Red -NoNewline
        $userConfirm = Read-Host
        if ($userConfirm -ne "YES") {
            Write-Host ""
            Write-Host "  Restart cancelled. Wait for position to close then retry." -ForegroundColor Green
            Write-Host ""
            exit 0
        }
        Write-Host "  Proceeding with restart..." -ForegroundColor Red
        Write-Host ""
    } else {
        Write-Host "  [OK] No open CandleFlow position detected" -ForegroundColor Green
    }
}

$mode = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = if ($mode -eq "LIVE") { "Red" } elseif ($mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
Write-Host "  Mode: $mode" -ForegroundColor $modeColor
Write-Host ""

# ==============================================================================
# STEP 0: PRE-FLIGHT ZOMBIE CLEANUP (added 2026-04-30, audit-fixes-29)
# ------------------------------------------------------------------------------
# Why: previous QUICK_RESTART runs that crashed mid-build (OOM, Ctrl-C, transient
# error) leave orphan compile processes running -- cl.exe, link.exe, MSBuild,
# mspdbsrv, tracker, VBCSCompiler. Each one holds file handles on the .cpp/.obj
# files it was compiling. The next QUICK_RESTART can't write main.obj or even
# delete .git/index.lock because of these zombies. Compounds across runs and
# eventually exhausts memory -> VPS crash.
#
# Fix: at the very top of QUICK_RESTART, kill ALL build-related processes
# unconditionally. They should all be dead before a new build anyway, and if
# any are legitimate they'll be respawned by cmake/MSBuild within seconds.
# Also clear all .git/*.lock files defensively.
#
# 2026-04-30 incident: VPS crash after running 8 rebuild cycles in one day
# without zombie cleanup. Multiple cl.exe holdovers found holding main.cpp
# AND main.obj; build couldn't proceed; eventually OOM'd the system.
# ==============================================================================
Write-Host "[0/4] Pre-flight zombie cleanup..." -ForegroundColor DarkCyan
# $zombieNames hoisted to script-top in v3.2 -- shared with the defensive
# pre-wipe re-kill and the per-attempt re-kill inside the wipe retry loops.
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

# Also kill any orphan git processes (rare, but they hold .git/index.lock)
$gitZombies = Get-Process -Name git -ErrorAction SilentlyContinue
if ($gitZombies) {
    Write-Host "  Killing $($gitZombies.Count) orphan git processes" -ForegroundColor Yellow
    $gitZombies | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

# Clear all stale git lock files (a crashed git process leaves these behind)
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

# ==============================================================================
# STEP 1: STOP OMEGA SERVICE (graceful via NSSM)
# ==============================================================================
# NSSM sends CTRL_BREAK_EVENT to the wrapped process on Stop-Service, which
# triggers Omega's console_ctrl_handler -> graceful shutdown (positions flat,
# state saved, FIX Logout). Timeout after $StopTimeoutSec seconds, then /F.
# ==============================================================================
Write-Host "[1/4] Stopping Omega service..." -ForegroundColor Yellow

$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($null -eq $svc) {
    Write-Host "  [FATAL] Omega service does not exist. Run INSTALL_OMEGA_SERVICE.ps1 first." -ForegroundColor Red
    exit 1
}

Write-Host "  Current status: $($svc.Status)" -ForegroundColor Cyan

if ($svc.Status -eq 'Running' -or $svc.Status -eq 'StartPending') {
    if ($ForceKill) {
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

        # Poll for Stopped state
        $stopStart = Get-Date
        $stopped   = $false
        while (((Get-Date) - $stopStart).TotalSeconds -lt $StopTimeoutSec) {
            $s = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            if ($s.Status -eq 'Stopped') { $stopped = $true; break }
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
} else {
    Write-Host "  [OK] Service already stopped" -ForegroundColor Green
}

# Mop up any stragglers
for ($i = 0; $i -lt 5; $i++) {
    $still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if (-not $still) { break }
    Write-Host "  Killing straggler Omega.exe (PID $($still.Id -join ','))" -ForegroundColor Yellow
    taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
    Start-Sleep -Seconds 2
}

# Final confirmation
$finalCheck = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($finalCheck) {
    Write-Host "  [FATAL] Omega.exe still running after all kill attempts. Aborting." -ForegroundColor Red
    exit 1
}

$svcFinal = Get-Service -Name $ServiceName
if ($svcFinal.Status -ne 'Stopped') {
    Write-Host "  [FATAL] Service status is $($svcFinal.Status), expected Stopped. Aborting." -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] Confirmed no Omega.exe process AND service=Stopped" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# STEP 2: PULL SOURCE VIA GIT
# ==============================================================================
Write-Host "[2/4] Pulling source from GitHub..." -ForegroundColor Yellow

Push-Location $OmegaDir
try {
    git fetch origin main 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [FATAL] git fetch failed (exit=$LASTEXITCODE)" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        Pop-Location
        exit 1
    }

    $revParseOut = (git rev-parse origin/main 2>&1)
    $revParseExit = $LASTEXITCODE
    if ($revParseExit -ne 0 -or [string]::IsNullOrWhiteSpace($revParseOut)) {
        Write-Host "  [FATAL] git rev-parse origin/main failed (exit=$revParseExit output='$revParseOut')" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        Pop-Location
        exit 1
    }
    $ghSha  = $revParseOut.Trim()
    if ($ghSha.Length -lt 7) {
        Write-Host "  [FATAL] git rev-parse returned malformed SHA: '$ghSha'" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        Pop-Location
        exit 1
    }
    $ghSha7 = $ghSha.Substring(0,7)
    Write-Host "  HEAD: $ghSha7" -ForegroundColor Cyan

    git reset --hard origin/main 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [FATAL] git reset --hard failed (exit=$LASTEXITCODE)" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        Pop-Location
        exit 1
    }
} finally {
    Pop-Location
}

Write-Host "  [OK] Source at $ghSha7" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# INCREMENTAL BUILD PREP
# ==============================================================================
# v3.2 (2026-04-30) wipe-race fix:
#   * Defensive zombie re-kill immediately before touching build/. Step 0's
#     kill happens BEFORE git pull; by the time we reach the wipe, 5-30s of
#     Stop-Service / git fetch / git reset have run, and a build process
#     could (rarely) have respawned. This second pass closes the race.
#   * Both wipe paths retry with exponential backoff (5 attempts, 200ms ->
#     400 -> 800 -> 1600 -> 3200), re-killing zombies between attempts.
#     Original behaviour silently swallowed Remove-Item failures, leaving
#     partial build state for the next configure to stumble over. The
#     retry-with-kill loop dislodges file-handle locks reliably.
#   * If all 5 attempts still fail the script logs the failure but keeps
#     going -- cmake configure becomes the next gate, and the v3.1
#     build-failure recovery (restart service with previous Omega.exe)
#     still applies.
if (Test-Path $buildDir) {
    # ── Defensive zombie re-kill (v3.2) ──
    $preWipeZombies = Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in $zombieNames }
    if ($preWipeZombies) {
        Write-Host "  [pre-wipe] $($preWipeZombies.Count) build process(es) re-appeared since Step 0 -- killing:" -ForegroundColor Yellow
        foreach ($z in $preWipeZombies) {
            Write-Host "    PID $($z.Id) $($z.Name) (mem $([int]($z.WorkingSet64/1MB))MB)" -ForegroundColor DarkYellow
        }
        $preWipeZombies | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }

    # ── Detect previous-build crash and force a full wipe in that case ──
    # When a previous compile crashed mid-flight (Ctrl-C, OOM, killed orphan
    # MSBuild), MSBuild leaves an `unsuccessfulbuild` stamp file in
    # `Omega.dir\Release\Omega.tlog\`. The stamp causes the next compile
    # to die immediately with `cmake --build exit=-1` and 0 files compiled,
    # because MSBuild's internal tracking logs are inconsistent with the
    # CMakeCache state.
    #
    # Fix: scan for the stamp; if present, do a full-wipe of $buildDir.
    # The next configure regenerates everything from scratch (~5s overhead)
    # instead of inheriting broken state. (audit-fixes-30 2026-04-30)
    $unsuccessfulStamps = @()
    try {
        $unsuccessfulStamps = Get-ChildItem -Path $buildDir -Recurse -Force -Filter "unsuccessfulbuild" -ErrorAction SilentlyContinue
    } catch {}

    # Backoff schedule for wipe retries (ms): 200, 400, 800, 1600, 3200
    $wipeBackoffMs = @(200, 400, 800, 1600, 3200)

    if ($unsuccessfulStamps -and $unsuccessfulStamps.Count -gt 0) {
        Write-Host "  Detected previous-build crash stamp -- force-wiping build/ directory:" -ForegroundColor Yellow
        foreach ($s in $unsuccessfulStamps) {
            Write-Host "    found: $($s.FullName)" -ForegroundColor DarkYellow
        }

        # Full-wipe with retry-on-lock (v3.2)
        $wipeOk = $false
        for ($attempt = 0; $attempt -lt 5; $attempt++) {
            if (-not (Test-Path -LiteralPath $buildDir)) { $wipeOk = $true; break }
            try {
                Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction Stop
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

        # Always (re)create the build dir even if wipe partially failed.
        # New-Item -Force is a no-op if it already exists.
        New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
        if ($wipeOk) {
            Write-Host "  [OK] build/ fully wiped -- next configure rebuilds from scratch" -ForegroundColor Green
        } else {
            Write-Host "  [PARTIAL] build/ not fully wiped; configure will surface any breakage." -ForegroundColor Yellow
        }
    } else {
        # Per-file wipe with retry-on-lock (v3.2)
        $artifacts = @(Get-ChildItem -Path $buildDir -Include "*.obj","*.pch","*.pdb","*.iobj","*.ipdb" -Recurse -ErrorAction SilentlyContinue)
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
                Write-Host "    Continuing -- MSBuild may fail on these; QUICK_RESTART will recover via the existing build-failure path." -ForegroundColor Red
            }
            $remaining = @($stillLocked)
        }
        $deleted = $totalArtifacts - $remaining.Count
        Write-Host "  [OK] Build artifacts wiped ($deleted/$totalArtifacts removed; CMakeCache preserved)" -ForegroundColor Green
    }
} else {
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    Write-Host "  [OK] Build directory created (first run)" -ForegroundColor Green
}

# Force-touch source files so MSVC sees them as newer than any surviving .obj
$touchTime = Get-Date
Get-ChildItem -Path $OmegaDir -Include "*.cpp","*.hpp","*.h" -Recurse -ErrorAction SilentlyContinue |
    ForEach-Object { $_.LastWriteTime = $touchTime }

$mainCpp = "$OmegaDir\src\main.cpp"
if (Test-Path $mainCpp) {
    $mainAge = ((Get-Date) - (Get-Item $mainCpp).LastWriteTime).TotalSeconds
    if ($mainAge -gt 10) {
        Write-Host "  [FATAL] Source touch failed -- main.cpp age=${mainAge}s" -ForegroundColor Red
        Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
        Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
        exit 1
    }
    Write-Host "  [OK] Source timestamps updated (main.cpp age=${mainAge}s)" -ForegroundColor Green
} else {
    Write-Host "  [WARN] Cannot verify touch -- main.cpp not found" -ForegroundColor Yellow
}
Write-Host ""

# ==============================================================================
# STEP 3: BUILD
# ==============================================================================
Write-Host "[3/4] Building..." -ForegroundColor Yellow
Write-Host "  (streaming cmake output -- errors in red, compiling files in gray)" -ForegroundColor DarkCyan

# --- CONFIGURE ---
$cfgStart = Get-Date
& $cmakeExe -S $OmegaDir -B $buildDir -DCMAKE_BUILD_TYPE=Release "-DOMEGA_FORCE_GIT_HASH=$ghSha7" 2>&1 | ForEach-Object {
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
    Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
    Write-Host "  ║  CMAKE CONFIGURE FAILED (exit=$configureExitCode)" -ForegroundColor Red
    Write-Host "  ║  Duration: ${cfgSec}s" -ForegroundColor Red
    Write-Host "  ║  Service will restart with the PREVIOUS binary" -ForegroundColor Red
    Write-Host "  ║  so live trading is not left down." -ForegroundColor Red
    Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
    Write-Host ""
    Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
    try {
        Start-Service -Name $ServiceName -ErrorAction Stop
        Write-Host "  [OK] Previous binary running again. Fix cmake configure before retry." -ForegroundColor Yellow
    } catch {
        Write-Host "  [FATAL] Could not restart service: $_" -ForegroundColor Red
    }
    exit 1
}
Write-Host "  [configure] done in ${cfgSec}s" -ForegroundColor DarkCyan

# --- BUILD (compile + link) ---
$bldStart = Get-Date
$compileCount = 0
& $cmakeExe --build $buildDir --config Release --target Omega 2>&1 | ForEach-Object {
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
        # Noisy MSBuild chatter - suppress
    } else {
        Write-Host "    $line" -ForegroundColor DarkGray
    }
}
$buildExitCode = $LASTEXITCODE
$bldSec = [math]::Round(((Get-Date) - $bldStart).TotalSeconds, 1)

if ($buildExitCode -ne 0) {
    Write-Host ""
    Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
    Write-Host "  ║  BUILD FAILED (cmake --build exit=$buildExitCode)" -ForegroundColor Red
    Write-Host "  ║  Duration: ${bldSec}s  |  $compileCount .cpp files compiled" -ForegroundColor Red
    Write-Host "  ║  Service will restart with the PREVIOUS binary" -ForegroundColor Red
    Write-Host "  ║  so live trading is not left down." -ForegroundColor Red
    Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
    Write-Host ""
    Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
    try {
        Start-Service -Name $ServiceName -ErrorAction Stop
        Write-Host "  [OK] Previous binary running again. Fix compile errors before retry." -ForegroundColor Yellow
    } catch {
        Write-Host "  [FATAL] Could not restart service: $_" -ForegroundColor Red
    }
    exit 1
}
Write-Host "  [compile+link] done in ${bldSec}s ($compileCount .cpp files compiled)" -ForegroundColor DarkCyan

if (-not (Test-Path $BuildExe)) {
    Write-Host "  [FATAL] Build reported success (exit=0) but Omega.exe not produced at $BuildExe" -ForegroundColor Red
    Write-Host "  [RECOVERY] Restarting service with previous Omega.exe..." -ForegroundColor Yellow
    Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
    exit 1
}

# Copy binary to service path with retry
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
    exit 1
}

$builtAt = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
Write-Host "  [OK] Built $ghSha7 at $builtAt" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# STEP 4: START SERVICE
# ==============================================================================
Write-Host "[4/4] Starting Omega service..." -ForegroundColor Yellow

New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# Truncate latest.log
$latestLog = "$OmegaDir\logs\latest.log"
if (Test-Path $latestLog) { Clear-Content $latestLog -ErrorAction SilentlyContinue }

# Rotate omega_service_stdout.log -- NSSM will create a fresh one on service start.
# This lets the verify step below actually find the new startup banner without
# scanning through hundreds of MB of historical tick output from previous runs.
$stdoutLog = "$OmegaDir\logs\omega_service_stdout.log"
if (Test-Path $stdoutLog) {
    $stderrLog  = "$OmegaDir\logs\omega_service_stderr.log"
    $archiveDir = "$OmegaDir\logs\archive"
    if (-not (Test-Path $archiveDir)) { New-Item -ItemType Directory -Path $archiveDir -Force | Out-Null }
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    try {
        Move-Item -Path $stdoutLog -Destination "$archiveDir\omega_service_stdout_$stamp.log" -Force -ErrorAction Stop
        Write-Host "  [OK] Rotated stdout log -> archive\omega_service_stdout_$stamp.log" -ForegroundColor DarkGray
    } catch {
        Write-Host "  [WARN] Could not rotate stdout log (locked?): $_ -- attempting truncate" -ForegroundColor Yellow
        try { Clear-Content $stdoutLog -ErrorAction Stop }
        catch { Write-Host "  [WARN] Truncate also failed: $_ -- verify may scan stale content" -ForegroundColor Yellow }
    }
    if (Test-Path $stderrLog) {
        try { Move-Item -Path $stderrLog -Destination "$archiveDir\omega_service_stderr_$stamp.log" -Force -ErrorAction Stop }
        catch { }
    }
}

Write-Host ""
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host "  COMMIT : $ghSha7" -ForegroundColor Yellow
Write-Host "  BUILT  : $builtAt" -ForegroundColor Yellow
Write-Host "  MODE   : $mode" -ForegroundColor $modeColor
Write-Host "  GUI    : http://185.167.119.59:7779" -ForegroundColor Yellow
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host ""

try {
    Start-Service -Name $ServiceName -ErrorAction Stop
} catch {
    Write-Host "  [FATAL] Start-Service failed: $_" -ForegroundColor Red
    exit 1
}

# Wait for service to reach Running state
$startPollStart = Get-Date
$running = $false
while (((Get-Date) - $startPollStart).TotalSeconds -lt 20) {
    $s = Get-Service -Name $ServiceName
    if ($s.Status -eq 'Running') { $running = $true; break }
    Start-Sleep -Seconds 1
}
if (-not $running) {
    Write-Host "  [FATAL] Service did not reach Running state in 20s. Check NSSM event log." -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] Service=Running" -ForegroundColor Green

# Wait for process to stabilise
Start-Sleep -Seconds $StartupWaitSec

# ── STALE BINARY CHECK 1: process EXE timestamp ──────────────────────────────
$runningProc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if (-not $runningProc) {
    Write-Host "  [FATAL] Omega.exe not running ${StartupWaitSec}s after service start" -ForegroundColor Red
    Write-Host "  Check: $LogStdout" -ForegroundColor Red
    exit 1
}
$runningPid = ($runningProc | Select-Object -First 1).Id
$cimProc    = Get-CimInstance Win32_Process -Filter "ProcessId = $runningPid" -ErrorAction SilentlyContinue
if (-not $cimProc -or [string]::IsNullOrEmpty($cimProc.ExecutablePath)) {
    Write-Host "  [FATAL] Cannot determine path of running Omega.exe (PID $runningPid)" -ForegroundColor Red
    exit 1
}
$runningExePath = $cimProc.ExecutablePath
$runningExeTime = (Get-Item $runningExePath -ErrorAction Stop).LastWriteTimeUtc
$builtExeTime   = (Get-Item $OmegaExe -ErrorAction Stop).LastWriteTimeUtc
$diffSec = [math]::Abs(($runningExeTime - $builtExeTime).TotalSeconds)
if ($diffSec -gt 10) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
    Write-Host "  ║  WRONG BINARY RUNNING -- ABORTING                ║" -ForegroundColor Red
    Write-Host "  ║  Running EXE : $runningExePath" -ForegroundColor Red
    Write-Host "  ║  Running time: $($runningExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
    Write-Host "  ║  Expected    : $($builtExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
    Write-Host "  ║  Diff: ${diffSec}s -- old binary running!" -ForegroundColor Red
    Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "  [OK] EXE timestamp matches built binary (+${diffSec}s)" -ForegroundColor Green

# ── STALE BINARY CHECK 2: Git hash in startup log ────────────────────────────
$hashFound = $false
$hashInLog = ""
for ($hi = 0; $hi -lt 15; $hi++) {
    if (Test-Path $LogStderr) {
        $tail = Get-Content $LogStderr -Tail 200 -ErrorAction SilentlyContinue
        $hashLine = $tail | Where-Object { $_ -match "\[Omega\] Git hash:" } | Select-Object -Last 1
        if ($hashLine -and $hashLine -match "Git hash:\s*([0-9a-f]{7})") {
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
    exit 1
}

if ($hashInLog -ne $ghSha7) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
    Write-Host "  ║  STALE BINARY DETECTED -- WRONG HASH IN LOG      ║" -ForegroundColor Red
    Write-Host "  ║  Expected hash : $ghSha7" -ForegroundColor Red
    Write-Host "  ║  Log reports   : $hashInLog" -ForegroundColor Red
    Write-Host "  ║  Stopping service. Investigate before next restart." -ForegroundColor Red
    Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "  [OK] Git hash confirmed in log: $hashInLog == $ghSha7" -ForegroundColor Green

if (-not $SkipVerify) {
    Write-Host ""
    Write-Host "  Waiting 60s then running VERIFY_STARTUP..." -ForegroundColor Cyan
    Start-Sleep -Seconds 60
    & "$OmegaDir\VERIFY_STARTUP.ps1" -OmegaDir $OmegaDir
}

$elapsed = (Get-Date) - $startTime
Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ("  DONE: {0:mm}m {0:ss}s" -f $elapsed) -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
