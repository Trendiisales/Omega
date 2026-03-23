# ==============================================================================
#                        OMEGA - DEPLOY AND START
#   Run from anywhere on the VPS - handles everything.
# ==============================================================================

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  Commodities and Indices  |  Breakout System" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# [1/4] Stop any running Omega process
Write-Host "[1/4] Stopping existing Omega process..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Host "      [OK] Stopped (or was not running)" -ForegroundColor Green
Write-Host ""

# [2/4] Pull latest from GitHub
Write-Host "[2/4] Pulling latest from GitHub..." -ForegroundColor Yellow
Set-Location C:\Omega
git fetch origin
git reset --hard origin/main
# Force-overwrite any files that may have wrong encoding or been modified outside git
git checkout HEAD -- symbols.ini
git checkout HEAD -- DEPLOY_OMEGA.ps1
Write-Host "      [OK] Up to date: $(git log --oneline -1)" -ForegroundColor Green
Write-Host ""

# [3/4] Build
Write-Host "[3/4] Building..." -ForegroundColor Yellow
if (Test-Path "C:\Omega\build") {
    Remove-Item -Path "C:\Omega\build" -Recurse -Force
}
New-Item -ItemType Directory -Path "C:\Omega\build" -Force | Out-Null
Set-Location C:\Omega\build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
cmake --build . --config Release
if (-not (Test-Path "C:\Omega\build\Release\Omega.exe")) {
    Write-Host "      [ERROR] Build failed - Omega.exe not found!" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    return
}
Write-Host "      [OK] Omega.exe built" -ForegroundColor Green
Write-Host ""

# [4/4] Copy assets and run
Write-Host "[4/4] Copying assets and starting..." -ForegroundColor Yellow
$rel = "C:\Omega\build\Release"
$configSource = "C:\Omega\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "C:\Omega\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found in repo" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    return
}
Copy-Item $configSource                          "$rel\omega_config.ini"  -Force
Copy-Item "C:\Omega\symbols.ini"                 "$rel\symbols.ini"       -Force
Copy-Item "C:\Omega\src\gui\www\omega_index.html" "$rel\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\chimera_logo.png" "$rel\chimera_logo.png" -Force -ErrorAction SilentlyContinue
Write-Host "      [OK] Assets copied (omega_config.ini + symbols.ini + GUI)" -ForegroundColor Green
Write-Host ""

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Starting Omega.exe..." -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location $rel
.\Omega.exe omega_config.ini
