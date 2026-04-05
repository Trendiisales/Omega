#Requires -Version 5.1
# ==============================================================================
#  OMEGA - QUICK RESTART
#  BINARY LOCATION RULE (IMMUTABLE):
#    cmake builds to: C:\Omega\build\Release\Omega.exe
#    This script launches: C:\Omega\Omega.exe
#    Sync happens AFTER stop, BEFORE launch -- always runs latest build.
# ==============================================================================

param(
    [switch] $SkipVerify,
    [int]    $WaitSec  = 45,
    [string] $OmegaDir = "C:\Omega"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Ensure git always rebases on pull -- prevents merge commits that create phantom hashes
$ErrorActionPreference = "Continue"
git -C $OmegaDir config pull.rebase true 2>$null | Out-Null
$ErrorActionPreference = "Stop"

$BuildExe  = "$OmegaDir\build\Release\Omega.exe"
$OmegaExe  = "$OmegaDir\Omega.exe"
$ConfigSrc = "$OmegaDir\omega_config.ini"

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  QUICK RESTART" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# --- Verify at least one binary exists ---------------------------------------
if (-not (Test-Path $OmegaExe) -and -not (Test-Path $BuildExe)) {
    Write-Host "  [ERROR] No binary found. Run cmake --build build --config Release" -ForegroundColor Red
    exit 1
}

# --- Show config mode --------------------------------------------------------
$ErrorActionPreference = "Continue"
$modeMatch = Select-String -Path $ConfigSrc -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = if ($mode -eq "LIVE") { "Red" } elseif ($mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
Write-Host "  Mode    : $mode" -ForegroundColor $modeColor
Write-Host ""
$ErrorActionPreference = "Stop"

# --- [1] Stop Omega ----------------------------------------------------------
Write-Host "[1/4] Stopping Omega..." -ForegroundColor Yellow
$ErrorActionPreference = "Continue"
$svcCheck = Get-Service -Name "OmegaHFT" -ErrorAction SilentlyContinue
if ($svcCheck -and $svcCheck.Status -eq "Running") {
    Stop-Service "OmegaHFT" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
Start-Sleep -Seconds 1
Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($still) {
    $still | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
$ErrorActionPreference = "Stop"
Write-Host "      [OK] Stopped" -ForegroundColor Green
Write-Host ""

# --- Sync build output (file unlocked now) -----------------------------------
if (Test-Path $BuildExe) {
    $buildTime  = (Get-Item $BuildExe).LastWriteTime
    $launchTime = if (Test-Path $OmegaExe) { (Get-Item $OmegaExe).LastWriteTime } else { [DateTime]::MinValue }
    if ($buildTime -gt $launchTime) {
        Copy-Item $BuildExe $OmegaExe -Force
        Write-Host "  [SYNC] Updated Omega.exe from build\Release\Omega.exe" -ForegroundColor Green
    } else {
        Write-Host "  [SYNC] Omega.exe already current" -ForegroundColor DarkGray
    }
}

# --- THE ONE TRUE HASH -- last CODE commit (skip state/log commits) ----------
$ErrorActionPreference = "Continue"
# State commits are auto-pushed with message "state: log+snapshot+atr HASH DATE"
# Walk back through history to find the last real code commit
$gitHash = $null
$logLines = (git -C $OmegaDir log --oneline -20 2>$null)
foreach ($line in $logLines) {
    if ($line -notmatch "^[0-9a-f]+ state:") {
        $gitHash = ($line -split " ")[0]
        break
    }
}
if (-not $gitHash) { $gitHash = (git -C $OmegaDir rev-parse --short HEAD 2>$null) }
if (-not $gitHash) { $gitHash = "unknown" }

# Update stamp file to match reality
$buildTimeStr = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss") + " UTC"
"GIT_HASH_SHORT=$gitHash`nBUILD_TIME=$buildTimeStr" | Set-Content "$OmegaDir\omega_build.stamp"
$ErrorActionPreference = "Stop"

# --- [2] Clean state files ---------------------------------------------------
Write-Host "[2/4] Cleaning state files..." -ForegroundColor Yellow
$ErrorActionPreference = "Continue"
$barFailed = "$OmegaDir\logs\ctrader_bar_failed.txt"
if (Test-Path $barFailed) {
    Remove-Item $barFailed -Force
    Write-Host "      [OK] Deleted ctrader_bar_failed.txt" -ForegroundColor Green
} else {
    Write-Host "      [OK] ctrader_bar_failed.txt not present" -ForegroundColor DarkGray
}
$ErrorActionPreference = "Stop"
Write-Host ""

# --- [3] Ensure log dirs -----------------------------------------------------
Write-Host "[3/4] Verifying log directories..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null
Write-Host "      [OK] Log directories ready" -ForegroundColor Green
Write-Host ""

# --- [4] Launch --------------------------------------------------------------
Write-Host "[4/4] Launching Omega.exe..." -ForegroundColor Yellow
Set-Location $OmegaDir

# =====================================================================
#  THE ONE TRUE HASH -- always matches what is running
# =====================================================================
Write-Host ""
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host "  RUNNING COMMIT : $gitHash" -ForegroundColor Yellow
Write-Host "  BINARY TIME    : $buildTimeStr" -ForegroundColor Yellow
Write-Host "  MODE           : $mode" -ForegroundColor $modeColor
Write-Host "  GUI            : http://185.167.119.59:7779" -ForegroundColor Yellow
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host ""

# --- Launch service or direct ------------------------------------------------
$svc = Get-Service -Name "OmegaHFT" -ErrorAction SilentlyContinue

if ($svc) {
    Write-Host "  [SERVICE] Starting OmegaHFT..." -ForegroundColor Cyan
    Start-Service "OmegaHFT"
    Start-Sleep -Seconds 3
    $svc = Get-Service -Name "OmegaHFT"
    $svcColor = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  [SERVICE] Status: $($svc.Status)" -ForegroundColor $svcColor
    Write-Host ""
} else {
    Write-Host "  [DIRECT] WARNING: Service not installed -- process dies on disconnect." -ForegroundColor Yellow
    Write-Host "           Run INSTALL_SERVICE.ps1 once to fix permanently." -ForegroundColor Yellow
    Write-Host ""
    $proc = Start-Process -FilePath $OmegaExe -ArgumentList "omega_config.ini" `
                          -WorkingDirectory $OmegaDir -PassThru -NoNewWindow
    Write-Host "  Omega PID: $($proc.Id)" -ForegroundColor DarkGray
    Write-Host ""
}

if (-not $SkipVerify) {
    Write-Host "  Running VERIFY_STARTUP..." -ForegroundColor Cyan
    Write-Host ""
    Start-Sleep -Seconds 3
    & "$OmegaDir\VERIFY_STARTUP.ps1" -WaitSec $WaitSec -OmegaDir $OmegaDir
}
