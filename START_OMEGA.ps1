#Requires -Version 5.1
# ==============================================================================
#                   OMEGA -- START (no rebuild)
#
#  Use when: binary is already up-to-date and you just want to restart.
#  ALWAYS verifies that C:\Omega\Omega.exe matches the build stamp BEFORE
#  starting. If stamp is missing or hash mismatches, start is BLOCKED.
#
#  To get a new binary: run DEPLOY_OMEGA.ps1 or REBUILD_AND_START.ps1.
# ==============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaDir  = "C:\Omega"
$OmegaExe  = "C:\Omega\Omega.exe"
$StampFile = "C:\Omega\omega_build.stamp"

Set-Location $OmegaDir

# [1] Verify exe exists
if (-not (Test-Path $OmegaExe)) {
    Write-Host "[ERROR] $OmegaExe not found -- run DEPLOY_OMEGA.ps1 first" -ForegroundColor Red
    exit 1
}

# [2] Verify stamp exists and exe hash matches
if (-not (Test-Path $StampFile)) {
    Write-Host "[ERROR] Build stamp not found at $StampFile" -ForegroundColor Red
    Write-Host "        Run DEPLOY_OMEGA.ps1 or REBUILD_AND_START.ps1 to create a valid stamp." -ForegroundColor Yellow
    exit 1
}

$stampLines  = Get-Content $StampFile
$stampHash   = (($stampLines | Where-Object { $_ -match '^EXE_SHA256=' }) -replace '^EXE_SHA256=', '').Trim()
$stampGit    = (($stampLines | Where-Object { $_ -match '^GIT_HASH=' })   -replace '^GIT_HASH=',   '').Trim()
$stampTime   = (($stampLines | Where-Object { $_ -match '^BUILD_TIME=' })  -replace '^BUILD_TIME=',  '').Trim()
$currentHash = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash.Trim()

if ($stampHash -ne $currentHash) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  *** STALE BINARY DETECTED -- LAUNCH BLOCKED ***" -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Write-Host "  The exe at $OmegaExe does not match the last deploy stamp." -ForegroundColor Red
    Write-Host "  Stamp hash : $($stampHash.Substring(0,16))..." -ForegroundColor Red
    Write-Host "  Exe hash   : $($currentHash.Substring(0,16))..." -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Write-Host "  This means Omega.exe was modified or replaced after the last deploy." -ForegroundColor Yellow
    Write-Host "  Run DEPLOY_OMEGA.ps1 to rebuild and write a fresh stamp." -ForegroundColor Yellow
    Write-Host "" -ForegroundColor Red
    exit 1
}

# [3] Verify git HEAD matches stamp
$savedPref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
git fetch origin 2>&1 | Out-Null
$ErrorActionPreference = $savedPref
$localHead  = (git rev-parse HEAD).Trim()
$remoteHead = (git rev-parse origin/main).Trim()

if ($localHead -ne $remoteHead) {
    Write-Host "[WARN] Local HEAD ($($localHead.Substring(0,7))) is behind origin/main ($($remoteHead.Substring(0,7)))" -ForegroundColor Yellow
    Write-Host "       The running binary may not reflect the latest code." -ForegroundColor Yellow
    Write-Host "       Run DEPLOY_OMEGA.ps1 to pull and rebuild latest." -ForegroundColor Yellow
    Write-Host ""
}

if ($stampGit -ne $localHead) {
    Write-Host "[WARN] Stamp git hash ($($stampGit.Substring(0,7))) != local HEAD ($($localHead.Substring(0,7)))" -ForegroundColor Yellow
    Write-Host "       Binary was built from a different commit than HEAD." -ForegroundColor Yellow
    Write-Host "       Run DEPLOY_OMEGA.ps1 if this is unexpected." -ForegroundColor Yellow
    Write-Host ""
}

# [4] Pre-live config check
$configFile = "$OmegaDir\omega_config.ini"
$wmMatch    = Select-String -Path $configFile -Pattern "session_watermark_pct\s*=\s*([0-9.]+)" -ErrorAction SilentlyContinue
$modeMatch  = Select-String -Path $configFile -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$watermark  = if ($wmMatch)   { $wmMatch.Matches[0].Groups[1].Value   } else { "NOT_FOUND" }
$mode       = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "NOT_FOUND" }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  CONFIG + BINARY CHECK" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  git HEAD              = $localHead" -ForegroundColor Cyan
Write-Host "  stamp git             = $stampGit" -ForegroundColor Cyan
Write-Host "  stamp built           = $stampTime" -ForegroundColor Cyan
Write-Host "  exe SHA256            = $($currentHash.Substring(0,16))...  [VERIFIED]" -ForegroundColor Green
Write-Host "  mode                  = $mode" -ForegroundColor Cyan
Write-Host "  session_watermark_pct = $watermark" -ForegroundColor Cyan

$testingActive = $false

if ($watermark -eq "NOT_FOUND" -or [double]$watermark -eq 0.0) {
    Write-Host ""
    Write-Host "  *** WARNING: session_watermark_pct=0.0 (TESTING VALUE) ***" -ForegroundColor Red
    Write-Host "  *** No drawdown protection. Set to 0.27 before LIVE.   ***" -ForegroundColor Red
    $testingActive = $true
}

if ($mode -eq "LIVE" -and $testingActive) {
    Write-Host ""
    Write-Host "  *** FATAL: mode=LIVE with testing values -- BLOCKED ***" -ForegroundColor Red
    Write-Host "  *** See PRE_LIVE_CHECKLIST.md                       ***" -ForegroundColor Red
    Write-Host "=======================================================" -ForegroundColor Yellow
    exit 1
}

Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

Write-Host "Starting Omega.exe from $OmegaDir  [git=$($localHead.Substring(0,7))]..." -ForegroundColor Cyan
Set-Location $OmegaDir
.\Omega.exe omega_config.ini
