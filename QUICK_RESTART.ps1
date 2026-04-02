#Requires -Version 5.1
# ==============================================================================
#  OMEGA - QUICK RESTART  (no build, no git pull)
#
#  PURPOSE:
#    Restart Omega in ~10 seconds without recompiling.
#    Use when:
#      - You changed omega_config.ini (hot-reload handles most, but some
#        settings only take effect on restart e.g. session times, log paths)
#      - You need a clean restart after a crash or hang
#      - You want to pick up a new ATR seed after a long pause
#      - You're between sessions and want clean state
#
#  DOES NOT:
#    - Pull from GitHub
#    - Rebuild the binary
#    - Change any code
#
#  USAGE:
#    .\QUICK_RESTART.ps1              # restart with current config
#    .\QUICK_RESTART.ps1 -SkipVerify  # don't run VERIFY_STARTUP after launch
#    .\QUICK_RESTART.ps1 -WaitSec 60  # verify for 60s instead of 45s
#
#  AFTER RESTART:
#    VERIFY_STARTUP.ps1 runs automatically (unless -SkipVerify).
#    startup_report.txt is written to C:\Omega\logs\
# ==============================================================================

param(
    [switch] $SkipVerify,
    [int]    $WaitSec  = 45,
    [string] $OmegaDir = "C:\Omega"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaExe   = "$OmegaDir\Omega.exe"
$ConfigSrc  = "$OmegaDir\omega_config.ini"
$StampFile  = "$OmegaDir\omega_build.stamp"

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  QUICK RESTART  (no rebuild)" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# --- Verify exe exists -------------------------------------------------------
if (-not (Test-Path $OmegaExe)) {
    Write-Host "  [ERROR] Omega.exe not found at $OmegaExe" -ForegroundColor Red
    Write-Host "          Run DEPLOY_OMEGA.ps1 first to build the binary." -ForegroundColor Yellow
    exit 1
}

# --- Show what binary we're restarting with ----------------------------------
if (Test-Path $StampFile) {
    $stamp     = Get-Content $StampFile
    $gitShort  = (($stamp | Where-Object { $_ -match '^GIT_HASH_SHORT=' }) -replace '^GIT_HASH_SHORT=', '').Trim()
    $buildTime = (($stamp | Where-Object { $_ -match '^BUILD_TIME=' })     -replace '^BUILD_TIME=',     '').Trim()
    Write-Host "  Binary  : $OmegaExe" -ForegroundColor DarkGray
    Write-Host "  Commit  : $gitShort" -ForegroundColor DarkGray
    Write-Host "  Built   : $buildTime" -ForegroundColor DarkGray
} else {
    Write-Host "  Binary  : $OmegaExe  (no stamp file -- unknown build)" -ForegroundColor Yellow
}

# --- Show config mode --------------------------------------------------------
$ErrorActionPreference = "Continue"
$modeMatch = Select-String -Path $ConfigSrc -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = if ($mode -eq "LIVE") { "Red" } elseif ($mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
Write-Host "  Mode    : $mode" -ForegroundColor $modeColor
Write-Host ""
$ErrorActionPreference = "Stop"

# --- [1] Kill existing Omega -------------------------------------------------
Write-Host "[1/4] Stopping Omega..." -ForegroundColor Yellow
$ErrorActionPreference = "Continue"
taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
Start-Sleep -Seconds 1
Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($still) {
    Write-Host "       WARNING: Omega still running -- forcing..." -ForegroundColor Red
    $still | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
$ErrorActionPreference = "Stop"
Write-Host "      [OK] Stopped" -ForegroundColor Green
Write-Host ""

# --- [2] Delete poisoned bar_failed file (safe to always do) -----------------
Write-Host "[2/4] Cleaning state files..." -ForegroundColor Yellow
$barFailedFile = "$OmegaDir\logs\ctrader_bar_failed.txt"
if (Test-Path $barFailedFile) {
    Remove-Item $barFailedFile -Force
    Write-Host "      [OK] Deleted ctrader_bar_failed.txt (prevents bar subscription poison)" -ForegroundColor Green
} else {
    Write-Host "      [OK] ctrader_bar_failed.txt not present (clean)" -ForegroundColor DarkGray
}

# Optionally delete stale ATR dat -- only if explicitly requested (risky to do by default)
# Uncomment below if you want QUICK_RESTART to force a fresh ATR seed:
# $atrDat = "$OmegaDir\logs\gold_flow_atr.dat"
# if (Test-Path $atrDat) { Remove-Item $atrDat -Force; Write-Host "      [OK] Deleted gold_flow_atr.dat (fresh ATR seed)" -ForegroundColor Yellow }
Write-Host ""

# --- [3] Ensure logs dirs exist ----------------------------------------------
Write-Host "[3/4] Verifying log directories..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null
Write-Host "      [OK] Log directories ready" -ForegroundColor Green
Write-Host ""

# --- [4] Launch --------------------------------------------------------------
Write-Host "[4/4] Launching Omega.exe..." -ForegroundColor Yellow
Set-Location $OmegaDir

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Green
Write-Host "  QUICK RESTART COMPLETE" -ForegroundColor Green
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "  Mode: $mode  |  Commit: $gitShort" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Green
Write-Host ""

if (-not $SkipVerify) {
    Write-Host "  Starting Omega in background and running VERIFY_STARTUP..." -ForegroundColor Cyan
    Write-Host "  startup_report.txt will be written to C:\Omega\logs\" -ForegroundColor DarkGray
    Write-Host ""

    # Start Omega in background so VERIFY_STARTUP can tail the log
    $proc = Start-Process -FilePath $OmegaExe -ArgumentList "omega_config.ini" `
                          -WorkingDirectory $OmegaDir -PassThru -NoNewWindow
    Write-Host "  Omega PID: $($proc.Id)" -ForegroundColor DarkGray
    Write-Host ""

    # Small pause to let Omega write first lines
    Start-Sleep -Seconds 3

    # Run verifier
    & "$OmegaDir\VERIFY_STARTUP.ps1" -WaitSec $WaitSec -OmegaDir $OmegaDir

} else {
    Write-Host "  Launching Omega in foreground (Ctrl+C to stop)..." -ForegroundColor Cyan
    Write-Host "  Run .\VERIFY_STARTUP.ps1 in a separate terminal to check startup state." -ForegroundColor DarkGray
    Write-Host ""
    & ".\Omega.exe" "omega_config.ini"
    Write-Host ""
    Write-Host "  Omega exited." -ForegroundColor Yellow
}
