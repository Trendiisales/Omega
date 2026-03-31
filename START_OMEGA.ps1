# ==============================================================================
#                   OMEGA -- START (no rebuild)
# ==============================================================================
$ErrorActionPreference = "Stop"

$repo = "C:\Omega"
$exe  = "C:\Omega\Omega.exe"

Set-Location $repo
git fetch origin | Out-Null
$localHead  = (git rev-parse HEAD).Trim()
$remoteHead = (git rev-parse origin/main).Trim()
if ($localHead -ne $remoteHead) {
    Write-Host "[ERROR] Local HEAD is behind origin/main." -ForegroundColor Red
    Write-Host "        Run REBUILD_AND_START.ps1 to pull/build exact latest commit." -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $exe)) {
    Write-Host "[ERROR] Omega.exe not found at $exe -- run REBUILD_AND_START.ps1 first" -ForegroundColor Red
    exit 1
}

# ==============================================================================
# PRE-LIVE CONFIG CHECK -- warns if testing values are active
# ==============================================================================
$configFile = "C:\Omega\config\omega_config.ini"
$watermark = (Select-String -Path $configFile -Pattern "session_watermark_pct=(\S+)").Matches[0].Groups[1].Value
$mode      = (Select-String -Path $configFile -Pattern "^mode=(\S+)").Matches[0].Groups[1].Value

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  CONFIG CHECK" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  mode                  = $mode" -ForegroundColor Cyan
Write-Host "  session_watermark_pct = $watermark" -ForegroundColor Cyan

$testingActive = $false

if ([double]$watermark -eq 0.0) {
    Write-Host ""
    Write-Host "  *** WARNING: session_watermark_pct=0.0 (TESTING VALUE) ***" -ForegroundColor Red
    Write-Host "  *** No drawdown protection active                        ***" -ForegroundColor Red
    Write-Host "  *** Set to 0.27 in config/omega_config.ini before LIVE   ***" -ForegroundColor Red
    Write-Host "  *** See PRE_LIVE_CHECKLIST.md for full checklist         ***" -ForegroundColor Red
    $testingActive = $true
}

if ($mode -eq "LIVE" -and $testingActive) {
    Write-Host ""
    Write-Host "  *** FATAL: mode=LIVE with testing values active ***" -ForegroundColor Red
    Write-Host "  *** Fix config before running in LIVE mode       ***" -ForegroundColor Red
    Write-Host "  *** See PRE_LIVE_CHECKLIST.md                    ***" -ForegroundColor Red
    exit 1
}

Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

Write-Host "Starting Omega at commit $localHead..." -ForegroundColor Cyan
Set-Location C:\Omega
.\Omega.exe omega_config.ini
