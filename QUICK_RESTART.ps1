#Requires -Version 5.1
# QUICK_RESTART.ps1  -- v3.1 2026-04-23
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
Write-Host "   OMEGA  |  QUICK RESTART  v3.1" -ForegroundColor Cyan
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
if (Test-Path $buildDir) {
    Get-ChildItem -Path $buildDir -Include "*.obj","*.pch","*.pdb","*.iobj","*.ipdb" -Recurse -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Host "  [OK] Build artifacts wiped (.obj/.pch deleted, CMakeCache preserved)" -ForegroundColor Green
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
    $line = $_.ToString()
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
    $line = $_.ToString()
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
