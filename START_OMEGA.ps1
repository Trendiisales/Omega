#Requires -Version 5.1
# ==============================================================================
#  OMEGA -- START (no rebuild)
#  Use when binary is already deployed and you just want to restart.
#  Verifies exe SHA256 against stamp BEFORE starting. Hard blocks on mismatch.
# ==============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaDir  = "C:\Omega"
$OmegaExe  = "C:\Omega\Omega.exe"
$StampFile = "C:\Omega\omega_build.stamp"

Set-Location $OmegaDir

if (-not (Test-Path $OmegaExe)) {
    Write-Host "[ERROR] $OmegaExe not found -- run DEPLOY_OMEGA.ps1 first" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $StampFile)) {
    Write-Host "[ERROR] Build stamp not found -- run DEPLOY_OMEGA.ps1 first" -ForegroundColor Red
    exit 1
}

$stampLines  = Get-Content $StampFile
$stampHash   = (($stampLines | Where-Object { $_ -match '^EXE_SHA256=' })      -replace '^EXE_SHA256=',     '').Trim()
$stampGit    = (($stampLines | Where-Object { $_ -match '^GIT_HASH=' })        -replace '^GIT_HASH=',       '').Trim()
$stampShort  = (($stampLines | Where-Object { $_ -match '^GIT_HASH_SHORT=' })  -replace '^GIT_HASH_SHORT=', '').Trim()
$stampHead   = (($stampLines | Where-Object { $_ -match '^HEAD_HASH=' })       -replace '^HEAD_HASH=',      '').Trim()
$stampTime   = (($stampLines | Where-Object { $_ -match '^BUILD_TIME=' })      -replace '^BUILD_TIME=',     '').Trim()
$currentHash = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash.Trim()

# Exe hash must match stamp -- hard block if not
if ($stampHash -ne $currentHash) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  *** STALE BINARY DETECTED -- LAUNCH BLOCKED ***" -ForegroundColor Red
    Write-Host "  Stamp SHA256 : $($stampHash.Substring(0,16))..." -ForegroundColor Red
    Write-Host "  Exe SHA256   : $($currentHash.Substring(0,16))..." -ForegroundColor Red
    Write-Host "  Run DEPLOY_OMEGA.ps1 to rebuild." -ForegroundColor Yellow
    exit 1
}

# Show what we are running -- source hash not HEAD
$displayHash = if ($stampShort) { $stampShort } else { $stampGit.Substring(0, [Math]::Min(7, $stampGit.Length)) }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  BINARY CHECK -- OK" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  source commit  = $stampGit" -ForegroundColor Green
Write-Host "  head at build  = $stampHead" -ForegroundColor DarkGray
if ($stampGit -ne $stampHead) {
    Write-Host "  (HEAD was a log-push -- source commit is what matters)" -ForegroundColor Cyan
}
Write-Host "  exe SHA256     = $($currentHash.Substring(0,16))...  [VERIFIED]" -ForegroundColor Green
Write-Host "  built          = $stampTime" -ForegroundColor Cyan

# Fetch and warn if behind origin/main source
$savedPref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
git fetch origin 2>&1 | Out-Null
$ErrorActionPreference = $savedPref

$remoteSrcLine = (git log --oneline -1 origin/main -- `
    src include CMakeLists.txt omega_config.ini symbols.ini `
    DEPLOY_OMEGA.ps1 OmegaWatchdog.ps1 2>$null).Trim()
if ($remoteSrcLine -match '^([a-f0-9]+)\s+') {
    $remoteSrcHash = (git rev-parse $Matches[1]).Trim()
    if ($remoteSrcHash -ne $stampGit) {
        Write-Host "" -ForegroundColor Yellow
        Write-Host "  [WARN] Newer source commit available on origin/main:" -ForegroundColor Yellow
        Write-Host "         Remote: $remoteSrcLine" -ForegroundColor Yellow
        Write-Host "         Running: $displayHash" -ForegroundColor Yellow
        Write-Host "         Run DEPLOY_OMEGA.ps1 to rebuild with latest code." -ForegroundColor Yellow
    }
}

# Config check
$configFile = "$OmegaDir\omega_config.ini"
$wmMatch    = Select-String -Path $configFile -Pattern "session_watermark_pct\s*=\s*([0-9.]+)" -ErrorAction SilentlyContinue
$modeMatch  = Select-String -Path $configFile -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$watermark  = if ($wmMatch)   { $wmMatch.Matches[0].Groups[1].Value   } else { "NOT_FOUND" }
$mode       = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "NOT_FOUND" }

Write-Host "  mode           = $mode" -ForegroundColor Cyan
Write-Host "  watermark_pct  = $watermark" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

$testingActive = ($watermark -eq "NOT_FOUND" -or [double]$watermark -eq 0.0)
if ($mode -eq "LIVE" -and $testingActive) {
    Write-Host "  *** FATAL: mode=LIVE with watermark=0 -- BLOCKED ***" -ForegroundColor Red
    exit 1
}

Write-Host "Starting Omega.exe  [source=$displayHash  mode=$mode]..." -ForegroundColor Cyan
Write-Host ""
Set-Location $OmegaDir
Start-Process powershell -ArgumentList "-NoExit -Command & 'C:\Omega\Omega.exe' 'omega_config.ini'" -WorkingDirectory $OmegaDir -WindowStyle Normal
Write-Host "  [OK] Omega started in separate window." -ForegroundColor Green
