#Requires -Version 5.1
# ==============================================================================
#                   OMEGA - CLEAN REBUILD AND START
#
#  Use when: you want a guaranteed clean build from scratch, pulling the exact
#  current origin/main. Preferred over DEPLOY_OMEGA.ps1 after a long gap.
#
#  STALE BINARY PREVENTION:
#    - Deletes build\ entirely before rebuilding (no stale object files)
#    - Verifies local HEAD matches origin/main before building
#    - Copies exe to C:\Omega\Omega.exe (canonical watchdog path)
#    - Writes omega_build.stamp with git hash + exe SHA256
#    - Verifies stamp hash matches the exe that is about to be launched
#    - Watermark/mode check blocks LIVE launch with testing config
# ==============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaDir  = "C:\Omega"
$OmegaExe  = "C:\Omega\Omega.exe"
$BuildExe  = "C:\Omega\build\Release\Omega.exe"
$StampFile = "C:\Omega\omega_build.stamp"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA - CLEAN REBUILD                               " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# [1/7] Stop
Write-Host "[1/7] Stopping Omega..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Write-Host "      [OK]" -ForegroundColor Green
Write-Host ""

# [2/7] Sync to origin/main
Write-Host "[2/7] Syncing to origin/main..." -ForegroundColor Yellow
Set-Location $OmegaDir
git fetch origin 2>&1 | Out-Null
git checkout main 2>&1 | Out-Null
git reset --hard origin/main 2>&1 | Out-Null

$localHead  = (git rev-parse HEAD).Trim()
$remoteHead = (git rev-parse origin/main).Trim()
if ($localHead -ne $remoteHead) {
    Write-Host "      [ERROR] Repo not aligned to origin/main after reset" -ForegroundColor Red
    Write-Host "      local=$localHead  remote=$remoteHead" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] HEAD $localHead  ($(git log --oneline -1))" -ForegroundColor Green
Write-Host ""

# [3/7] Clean build
Write-Host "[3/7] Clean build..." -ForegroundColor Yellow
Remove-Item -Path "$OmegaDir\build" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
Set-Location "$OmegaDir\build"
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
cmake --build . --config Release 2>&1

if (-not (Test-Path $BuildExe)) {
    Write-Host "      [ERROR] Build failed -- $BuildExe not found" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] Build succeeded" -ForegroundColor Green
Write-Host ""

# [4/7] Copy assets -- exe to canonical path
Write-Host "[4/7] Copying assets to $OmegaDir..." -ForegroundColor Yellow

$configSource = "$OmegaDir\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "$OmegaDir\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found" -ForegroundColor Red
    exit 1
}

Copy-Item $BuildExe     $OmegaExe                       -Force
Copy-Item $configSource "$OmegaDir\omega_config.ini"    -Force
git show HEAD:symbols.ini | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding utf8 -Force
Copy-Item "$OmegaDir\src\gui\www\omega_index.html" "$OmegaDir\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "$OmegaDir\src\gui\www\chimera_logo.png" "$OmegaDir\chimera_logo.png" -Force -ErrorAction SilentlyContinue

Write-Host "      [OK] Omega.exe -> $OmegaExe" -ForegroundColor Green
Write-Host "      [OK] config + symbols + GUI assets copied" -ForegroundColor Green
Write-Host ""

# [5/7] Write build stamp
Write-Host "[5/7] Writing build stamp..." -ForegroundColor Yellow
$exeHash   = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")

"GIT_HASH=$localHead"   | Out-File -FilePath $StampFile -Encoding utf8
"EXE_SHA256=$exeHash"  | Out-File -FilePath $StampFile -Encoding utf8 -Append
"BUILD_TIME=$buildTime" | Out-File -FilePath $StampFile -Encoding utf8 -Append
"EXE_PATH=$OmegaExe"   | Out-File -FilePath $StampFile -Encoding utf8 -Append

# Verify: re-read stamp and confirm hash matches the exe we just copied
$stampLines  = Get-Content $StampFile
$stampHash   = ($stampLines | Where-Object { $_ -match '^EXE_SHA256=' }) -replace '^EXE_SHA256=', ''
$currentHash = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
if ($stampHash.Trim() -ne $currentHash.Trim()) {
    Write-Host "      [ERROR] Stamp hash does not match exe -- file system error?" -ForegroundColor Red
    Write-Host "      stamp=$stampHash" -ForegroundColor Red
    Write-Host "      exe  =$currentHash" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] Stamp verified: git=$localHead  sha256=$($exeHash.Substring(0,16))..." -ForegroundColor Green
Write-Host ""

# [6/7] Pre-live config check
Write-Host "[6/7] Config check..." -ForegroundColor Yellow
$configFile = "$OmegaDir\omega_config.ini"
$wmMatch    = Select-String -Path $configFile -Pattern "session_watermark_pct\s*=\s*([0-9.]+)" -ErrorAction SilentlyContinue
$modeMatch  = Select-String -Path $configFile -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$watermark  = if ($wmMatch)   { $wmMatch.Matches[0].Groups[1].Value   } else { "NOT_FOUND" }
$mode       = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "NOT_FOUND" }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  CONFIG CHECK" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  git HEAD              = $localHead" -ForegroundColor Cyan
Write-Host "  exe SHA256            = $($exeHash.Substring(0,16))..." -ForegroundColor Cyan
Write-Host "  mode                  = $mode" -ForegroundColor Cyan
Write-Host "  session_watermark_pct = $watermark" -ForegroundColor Cyan

$testingActive = $false

if ($watermark -eq "NOT_FOUND" -or [double]$watermark -eq 0.0) {
    Write-Host ""
    Write-Host "  *** WARNING: session_watermark_pct=0.0 (TESTING VALUE) ***" -ForegroundColor Red
    Write-Host "  *** No drawdown protection. Set to 0.27 before LIVE.   ***" -ForegroundColor Red
    Write-Host "  *** See PRE_LIVE_CHECKLIST.md                          ***" -ForegroundColor Red
    $testingActive = $true
}

if ($mode -eq "LIVE" -and $testingActive) {
    Write-Host ""
    Write-Host "  *** FATAL: mode=LIVE with testing values -- BLOCKED ***" -ForegroundColor Red
    Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# [7/7] Launch
Write-Host "[7/7] Starting Omega.exe from $OmegaDir..." -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  git=$localHead  |  mode=$mode" -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location $OmegaDir
.\Omega.exe omega_config.ini
