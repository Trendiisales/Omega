#Requires -Version 5.1
# ==============================================================================
#  RESTART_OMEGA.ps1  --  THE ONE SCRIPT TO RUN OMEGA
#
#  Does everything in the correct order, every time:
#    1. Stop Omega (service + process, both methods)
#    2. Pull latest from GitHub (hard reset to origin/main)
#    3. Clean wipe build directory
#    4. cmake configure  (regenerates version_generated.hpp with correct hash)
#    5. cmake build      (compile)
#    6. Fail hard if build failed -- never launch broken binary
#    7. Copy exe to C:\Omega\Omega.exe
#    8. Delete ctrader_bar_failed.txt (prevents poisoned bar requests)
#    9. Show commit hash, mode, GUI URL
#   10. Start OmegaHFT service if installed, else direct launch
#
#  REPLACES: QUICK_RESTART.ps1, DEPLOY_OMEGA.ps1, REBUILD_AND_START.ps1
#  Run this. Only this. Nothing else.
# ==============================================================================

Set-StrictMode -Version Latest

$OmegaDir  = "C:\Omega"
$BuildExe  = "$OmegaDir\build\Release\Omega.exe"
$OmegaExe  = "$OmegaDir\Omega.exe"
$ConfigIni = "$OmegaDir\config\omega_config.ini"
$CmakeExe  = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"

# ── Helpers ───────────────────────────────────────────────────────────────────
function Banner($text, $color = "Cyan") {
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor $color
    Write-Host "  $text" -ForegroundColor $color
    Write-Host "=======================================================" -ForegroundColor $color
    Write-Host ""
}
function Step($n, $total, $text) {
    Write-Host "[$n/$total] $text" -ForegroundColor Yellow
}
function OK($text)   { Write-Host "      [OK] $text"    -ForegroundColor Green }
function FAIL($text) {
    Write-Host ""
    Write-Host "  *** FAILED: $text ***" -ForegroundColor Red
    Write-Host ""
    exit 1
}

Banner "OMEGA  |  RESTART + REBUILD"

# ── [1/7] Stop ────────────────────────────────────────────────────────────────
Step 1 7 "Stopping Omega..."

$ErrorActionPreference = "Continue"

# Stop service if installed
$svc = Get-Service -Name "OmegaHFT" -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Stop-Service "OmegaHFT" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}

# Kill process tree
taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
Start-Sleep -Seconds 2
Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Confirm dead
$still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($still) {
    $still | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}

$ErrorActionPreference = "Stop"
OK "Stopped"

# ── [2/7] Pull ────────────────────────────────────────────────────────────────
Step 2 7 "Pulling origin/main..."

Set-Location $OmegaDir
$ErrorActionPreference = "Continue"
git fetch origin 2>&1 | Out-Null
git reset --hard origin/main 2>&1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
$ErrorActionPreference = "Stop"

$gitHash  = (git log --format="%h" -1).Trim()
$gitMsg   = (git log --format="%s" -1).Trim()
OK "HEAD: $gitHash  -- $gitMsg"

# ── [3/7] Clean build dir ────────────────────────────────────────────────────
Step 3 7 "Wiping build directory..."

if (Test-Path "$OmegaDir\build") {
    Remove-Item -Recurse -Force "$OmegaDir\build" -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
OK "Build directory clean"

# ── [4/7] cmake configure ────────────────────────────────────────────────────
Step 4 7 "cmake configure  (regenerates version_generated.hpp)..."

if (-not (Test-Path $CmakeExe)) {
    FAIL "cmake not found at $CmakeExe"
}

$ErrorActionPreference = "Continue"
& $CmakeExe -S $OmegaDir -B "$OmegaDir\build" -DCMAKE_BUILD_TYPE=Release 2>&1 |
    Where-Object { $_ -match "\[Omega\]|error|Error" } |
    ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
$ErrorActionPreference = "Stop"

# Verify version_generated.hpp was written with the correct hash
$verFile = "$OmegaDir\include\version_generated.hpp"
if (-not (Test-Path $verFile)) {
    FAIL "version_generated.hpp not created by cmake configure"
}
$verContent = Get-Content $verFile -Raw
$guiHash = "unknown"
if ($verContent -match 'OMEGA_GIT_HASH\s+"([a-f0-9]+)"') { $guiHash = $Matches[1] }
if ($guiHash -ne $gitHash) {
    FAIL "version_generated.hpp hash=$guiHash does not match HEAD=$gitHash"
}
OK "configure done  (version_generated.hpp hash=$guiHash confirmed)"

# ── [5/7] cmake build ────────────────────────────────────────────────────────
Step 5 7 "cmake build..."

$ErrorActionPreference = "Continue"
& $CmakeExe --build "$OmegaDir\build" --config Release 2>&1 |
    ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
$buildExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"

if ($buildExit -ne 0) { FAIL "Build failed (exit $buildExit) -- fix compile errors above" }
if (-not (Test-Path $BuildExe)) { FAIL "$BuildExe not found after build" }
OK "Build succeeded"

# ── [6/7] Copy + clean state ─────────────────────────────────────────────────
Step 6 7 "Copying exe + cleaning state files..."

Copy-Item $BuildExe $OmegaExe -Force
OK "Omega.exe copied from build\Release\"

# Delete poisoned bar_failed file -- causes bars to never seed on restart
$barFailed = "$OmegaDir\logs\ctrader_bar_failed.txt"
if (Test-Path $barFailed) {
    Remove-Item $barFailed -Force
    OK "Deleted ctrader_bar_failed.txt"
}

# Ensure log directories exist
New-Item -ItemType Directory -Path "$OmegaDir\logs"          -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow"   -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades"   -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\kelly"    -Force | Out-Null
OK "Log directories ready"

# ── [7/7] Launch ─────────────────────────────────────────────────────────────
Step 7 7 "Launching..."

# Read mode from config
$ErrorActionPreference = "Continue"
$modeMatch = Select-String -Path $ConfigIni -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = switch ($mode) { "LIVE" { "Red" } "SHADOW" { "Yellow" } default { "Cyan" } }
$ErrorActionPreference = "Stop"

Banner "READY TO LAUNCH" "Green"
Write-Host "  Commit  : $gitHash  -- $gitMsg"   -ForegroundColor Cyan
Write-Host "  Mode    : $mode"                   -ForegroundColor $modeColor
Write-Host "  GUI     : http://185.167.119.59:7779" -ForegroundColor Green
Write-Host ""

Set-Location $OmegaDir

$svc = Get-Service -Name "OmegaHFT" -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "  Starting OmegaHFT service..." -ForegroundColor Cyan
    Start-Service "OmegaHFT"
    Start-Sleep -Seconds 3
    $svc = Get-Service -Name "OmegaHFT"
    $col = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  Service status: $($svc.Status)" -ForegroundColor $col
    if ($svc.Status -ne "Running") { FAIL "OmegaHFT service failed to start" }
} else {
    Write-Host "  WARNING: OmegaHFT service not installed." -ForegroundColor Yellow
    Write-Host "  Run INSTALL_SERVICE.ps1 once to fix this permanently." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Launching directly (Ctrl+C to stop)..." -ForegroundColor Cyan
    & ".\Omega.exe" "omega_config.ini"
}
