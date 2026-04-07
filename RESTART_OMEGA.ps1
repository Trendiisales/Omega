#Requires -Version 5.1
# ==============================================================================
#  RESTART_OMEGA.ps1  --  THE ONE SCRIPT TO RUN OMEGA
#  Run this. Only this. Every time.
#
#  Steps (in order, every run):
#    1.  Stop Omega (service + process kill, both)
#    2.  Pull latest from GitHub (hard reset origin/main)
#    3.  Wipe build directory (no stale objects)
#    4.  cmake configure (regenerates version_generated.hpp)
#    5.  cmake build (compile, fail hard on error)
#    6.  Copy Omega.exe to C:\Omega\Omega.exe
#    7.  Copy config\omega_config.ini to C:\Omega\omega_config.ini (binary cwd)
#    8.  Copy symbols.ini to C:\Omega\symbols.ini (binary cwd)
#    9.  Delete logs\ctrader_bar_failed.txt
#   10.  Ensure log directories exist
#   11.  Update service exe + AppDirectory if service installed
#   12.  Show commit, mode, GUI URL
#   13.  Start service or direct launch
# ==============================================================================

Set-StrictMode -Version Latest

$OmegaDir  = "C:\Omega"
$BuildExe  = "$OmegaDir\build\Release\Omega.exe"
$OmegaExe  = "$OmegaDir\Omega.exe"
$ConfigSrc = "$OmegaDir\config\omega_config.ini"  # canonical source
$ConfigDst = "$OmegaDir\omega_config.ini"          # binary working directory copy
$SymbolSrc = "$OmegaDir\symbols.ini"               # already in root (git tracked)
$CmakeExe  = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
$NssmExe   = "C:\nssm\nssm-2.24\win64\nssm.exe"
$ServiceName = "OmegaHFT"

function Banner($text, $color="Cyan") {
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor $color
    Write-Host "  $text" -ForegroundColor $color
    Write-Host "=======================================================" -ForegroundColor $color
    Write-Host ""
}
function Step($n,$total,$text) { Write-Host "[$n/$total] $text" -ForegroundColor Yellow }
function OK($text)   { Write-Host "      [OK] $text"    -ForegroundColor Green }
function WARN($text) { Write-Host "      [!!] $text"    -ForegroundColor Yellow }
function FAIL($text) {
    Write-Host ""
    Write-Host "  *** FAILED: $text ***" -ForegroundColor Red
    Write-Host ""
    exit 1
}

Banner "OMEGA  |  RESTART + REBUILD"

# ── [1/13] Stop ──────────────────────────────────────────────────────────────
Step 1 13 "Stopping Omega..."
$ErrorActionPreference = "Continue"

# Step 1a: Stop service gracefully first
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Write-Host "      Stopping $ServiceName service..." -ForegroundColor DarkGray
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
    # Wait up to 15s for service to reach Stopped state
    $svcWait = 0
    while ($svcWait -lt 15) {
        Start-Sleep -Seconds 1; $svcWait++
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($svc.Status -eq "Stopped") { break }
    }
    Write-Host "      Service state: $($svc.Status) (after ${svcWait}s)" -ForegroundColor DarkGray
}

# Step 1b: Force-kill the process regardless
taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
Start-Sleep -Seconds 1

# Step 1c: Kill any survivors
Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# Step 1d: CONFIRM dead -- hard loop, fail if process survives 20s
# This is the critical check that was missing. The singleton mutex in Omega
# can persist several seconds after NSSM sends the stop signal. If we
# start the service while the old process still holds the mutex, the new
# exe hits ERROR_ALREADY_EXISTS and exits immediately -- service fails to start.
Write-Host "      Confirming Omega.exe is gone..." -ForegroundColor DarkGray
$killWait = 0
while ($killWait -lt 20) {
    $proc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if (-not $proc) { break }
    Write-Host "      Still running (PID $($proc.Id)) -- waiting..." -ForegroundColor Yellow
    $proc | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $killWait++
}
$finalCheck = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($finalCheck) {
    FAIL "Omega.exe still running after 20s kill attempts (PID $($finalCheck.Id)) -- cannot proceed"
}

$ErrorActionPreference = "Stop"
OK "Stopped and confirmed dead"

# ── [2/13] Pull ──────────────────────────────────────────────────────────────
Step 2 13 "Pulling origin/main..."
Set-Location $OmegaDir
$ErrorActionPreference = "Continue"
git fetch origin 2>&1 | Out-Null
git reset --hard origin/main 2>&1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
$ErrorActionPreference = "Stop"
$gitHash = (git log --format="%h" -1).Trim()
$gitMsg  = (git log --format="%s" -1).Trim()
OK "HEAD: $gitHash  -- $gitMsg"

# ── [3/13] Wipe build ────────────────────────────────────────────────────────
Step 3 13 "Wiping build directory..."
if (Test-Path "$OmegaDir\build") {
    Remove-Item -Recurse -Force "$OmegaDir\build" -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
OK "Build directory clean"

# ── [4/13] cmake configure ───────────────────────────────────────────────────
Step 4 13 "cmake configure..."
if (-not (Test-Path $CmakeExe)) { FAIL "cmake not found at $CmakeExe" }
$ErrorActionPreference = "Continue"
& $CmakeExe -S $OmegaDir -B "$OmegaDir\build" -DCMAKE_BUILD_TYPE=Release 2>&1 |
    Where-Object { $_ -match "\[Omega\]|error|Error" } |
    ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
$ErrorActionPreference = "Stop"

$verFile = "$OmegaDir\include\version_generated.hpp"
if (-not (Test-Path $verFile)) { FAIL "version_generated.hpp not created" }
$verContent = Get-Content $verFile -Raw
$guiHash = "unknown"
if ($verContent -match 'OMEGA_GIT_HASH\s+"([a-f0-9]+)"') { $guiHash = $Matches[1] }
if ($guiHash -ne $gitHash) { FAIL "version hash mismatch: hpp=$guiHash HEAD=$gitHash" }
OK "Configure done (hash $guiHash confirmed)"

# ── [5/13] cmake build ───────────────────────────────────────────────────────
Step 5 13 "cmake build..."
$ErrorActionPreference = "Continue"
& $CmakeExe --build "$OmegaDir\build" --config Release 2>&1 |
    ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
$buildExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($buildExit -ne 0)              { FAIL "Build failed (exit $buildExit)" }
if (-not (Test-Path $BuildExe))    { FAIL "$BuildExe not found after build" }
OK "Build succeeded"

# ── [6/13] Copy exe ──────────────────────────────────────────────────────────
Step 6 13 "Copying exe..."
Copy-Item $BuildExe $OmegaExe -Force
OK "Omega.exe  ->  $OmegaExe"

# ── [7/13] Copy config to binary working directory ───────────────────────────
Step 7 13 "Copying config to binary working directory..."
# MANDATORY: binary runs from C:\Omega and looks for omega_config.ini in cwd.
# Canonical config is in config\omega_config.ini. Must copy to root every deploy.
if (-not (Test-Path $ConfigSrc)) { FAIL "Config not found: $ConfigSrc" }
Copy-Item $ConfigSrc $ConfigDst -Force
OK "omega_config.ini  ->  $ConfigDst"

# ── [8/13] Copy symbols.ini to binary working directory ──────────────────────
Step 8 13 "Verifying symbols.ini in root..."
# symbols.ini is git-tracked in root already, but ensure it's current after pull
if (-not (Test-Path $SymbolSrc)) { FAIL "symbols.ini not found: $SymbolSrc" }
OK "symbols.ini present at $SymbolSrc"

# ── [9/13] Clean state files ─────────────────────────────────────────────────
Step 9 13 "Cleaning state files..."
$barFailed = "$OmegaDir\logs\ctrader_bar_failed.txt"
if (Test-Path $barFailed) {
    Remove-Item $barFailed -Force
    OK "Deleted ctrader_bar_failed.txt"
}

# ── [10/13] Ensure log directories ───────────────────────────────────────────
Step 10 13 "Ensuring log directories..."
@("$OmegaDir\logs", "$OmegaDir\logs\shadow", "$OmegaDir\logs\trades", "$OmegaDir\logs\kelly") |
    ForEach-Object { New-Item -ItemType Directory -Path $_ -Force | Out-Null }
OK "Log directories ready"

# ── [11/13] Update service exe path if installed ─────────────────────────────
Step 11 13 "Checking service configuration..."
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc) {
    if (Test-Path $NssmExe) {
        # Update service exe, AppDirectory, and args so it always uses current binary
        $ErrorActionPreference = "Continue"
        & $NssmExe set $ServiceName Application $OmegaExe 2>&1 | Out-Null
        & $NssmExe set $ServiceName AppDirectory $OmegaDir 2>&1 | Out-Null
        & $NssmExe set $ServiceName AppParameters "omega_config.ini" 2>&1 | Out-Null
        $ErrorActionPreference = "Stop"
        OK "Service exe + AppDirectory updated via NSSM"
    } else {
        WARN "NSSM not found -- service exe path not updated. Service may run old binary."
    }
} else {
    WARN "OmegaHFT service not installed -- will launch directly. Run INSTALL_SERVICE.ps1 once."
}

# ── [12/13] Show launch summary ──────────────────────────────────────────────
Step 12 13 "Reading config..."
$ErrorActionPreference = "Continue"
$modeMatch = Select-String -Path $ConfigDst -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = switch ($mode) { "LIVE" { "Red" } "SHADOW" { "Yellow" } default { "Cyan" } }
$ErrorActionPreference = "Stop"

Banner "READY TO LAUNCH" "Green"
Write-Host "  Commit  : $gitHash  -- $gitMsg" -ForegroundColor Cyan
Write-Host "  Mode    : $mode"                -ForegroundColor $modeColor
Write-Host "  Config  : $ConfigDst"           -ForegroundColor DarkGray
Write-Host "  GUI     : http://185.167.119.59:7779" -ForegroundColor Green
Write-Host ""

# ── [13/13] Launch ───────────────────────────────────────────────────────────
Step 13 13 "Launching..."
Set-Location $OmegaDir

$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "  Starting $ServiceName service..." -ForegroundColor Cyan
    Start-Service $ServiceName

    # Wait up to 30s for service to reach Running state
    # Do not rely on a fixed sleep -- service startup time varies 3-15s
    $startWait = 0
    while ($startWait -lt 30) {
        Start-Sleep -Seconds 1; $startWait++
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($svc.Status -eq "Running") { break }
        Write-Host "      Waiting for service... ($startWait s) state=$($svc.Status)" -ForegroundColor DarkGray
    }
    $col = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  Service status: $($svc.Status) (after ${startWait}s)" -ForegroundColor $col
    if ($svc.Status -ne "Running") { FAIL "$ServiceName failed to reach Running state after 30s" }
    OK "Omega running as service"
} else {
    Write-Host "  Launching directly (Ctrl+C to stop)..." -ForegroundColor Cyan
    Write-Host ""
    & ".\Omega.exe" "omega_config.ini"
    exit
}

# ── POST-LAUNCH VERIFICATION ─────────────────────────────────────────────────
# Wait for engine to write its startup log lines, then run OMEGA_STATUS.ps1
# to verify the correct binary is running and all systems are connected.
# This is MANDATORY -- do not skip. If hash mismatches, the wrong binary is running.
Write-Host ""
Write-Host "  Waiting 20s for engine startup..." -ForegroundColor DarkGray
$waited = 0
while ($waited -lt 20) {
    Start-Sleep -Seconds 1
    $waited++
    # Check if RUNNING COMMIT line has appeared in log
    $logPath = "$OmegaDir\logs\latest.log"
    if (Test-Path $logPath) {
        $commitLine = Get-Content $logPath | Select-String "RUNNING COMMIT" | Select-Object -Last 1
        if ($commitLine) {
            Write-Host "  Engine log confirmed after ${waited}s" -ForegroundColor Green
            break
        }
    }
    if ($waited % 5 -eq 0) { Write-Host "  Still waiting... (${waited}s)" -ForegroundColor DarkGray }
}

# Extract running hash from log and compare to git HEAD
$runningHash = "NOT_FOUND"
$logPath = "$OmegaDir\logs\latest.log"
if (Test-Path $logPath) {
    $commitLine = Get-Content $logPath | Select-String "RUNNING COMMIT" | Select-Object -Last 1
    if ($commitLine -and ($commitLine -match "RUNNING COMMIT:\s+([a-f0-9]+)")) {
        $runningHash = $Matches[1]
    }
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  POST-LAUNCH HASH VERIFICATION" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
if ($runningHash -eq $gitHash) {
    Write-Host "  [OK] HASH VERIFIED: running=$runningHash == HEAD=$gitHash" -ForegroundColor Green
} else {
    Write-Host "  [!!] HASH MISMATCH: running=$runningHash != HEAD=$gitHash" -ForegroundColor Red
    Write-Host "  STOP. Do not trade. The wrong binary is running." -ForegroundColor Red
    Write-Host "  Run .\RESTART_OMEGA.ps1 again." -ForegroundColor Red
    exit 1
}

# Run full status check
Write-Host ""
Write-Host "  Running full status check..." -ForegroundColor Cyan
Write-Host ""
if (Test-Path "$OmegaDir\OMEGA_STATUS.ps1") {
    & "$OmegaDir\OMEGA_STATUS.ps1"
} else {
    Write-Host "  OMEGA_STATUS.ps1 not found -- run it manually to verify system state" -ForegroundColor Yellow
}
