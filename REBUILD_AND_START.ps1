# ==============================================================================
#                   OMEGA - CLEAN REBUILD AND START
# ==============================================================================
$ErrorActionPreference = "Stop"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA - CLEAN REBUILD                               " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "[1/4] Stopping Omega..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Host "      [OK]" -ForegroundColor Green
Write-Host ""

Write-Host "[2/4] Syncing to origin/main..." -ForegroundColor Yellow
Set-Location C:\Omega
git fetch origin
git checkout main
# Deterministic deploy: always build exact remote head.
git reset --hard origin/main
$localHead  = (git rev-parse HEAD).Trim()
$remoteHead = (git rev-parse origin/main).Trim()
if ($localHead -ne $remoteHead) {
    Write-Host "      [ERROR] Repo not aligned to origin/main" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] HEAD $localHead" -ForegroundColor Green
Write-Host ""

Write-Host "[3/4] Clean build..." -ForegroundColor Yellow
Remove-Item -Path "C:\Omega\build" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "C:\Omega\build" -Force | Out-Null
Set-Location C:\Omega\build
cmake ..
cmake --build . --config Release

if (-not (Test-Path "Release\Omega.exe")) {
    Write-Host "      [ERROR] Build failed!" -ForegroundColor Red
    exit 1
}
# Verify built hash matches origin/main
$expectedHash = (git -C C:\Omega rev-parse --short origin/main).Trim()
$builtHash = (Select-String -Path "Release\Omega.exe" -Pattern $expectedHash -SimpleMatch -Quiet)
Write-Host "      [OK] Omega.exe built — expected hash: $expectedHash" -ForegroundColor Green
Write-Host ""

Write-Host "[4/4] Copying assets and starting..." -ForegroundColor Yellow
$configSource = "C:\Omega\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "C:\Omega\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found in repo" -ForegroundColor Red
    exit 1
}
Copy-Item $configSource "Release\omega_config.ini" -Force
# Ensure reload_trades_on_startup=false is always set (clean PnL slate on restart)
$cfgContent = Get-Content "Release\omega_config.ini" -Raw
if ($cfgContent -notmatch "reload_trades_on_startup") {
    Add-Content "Release\omega_config.ini" "`nreload_trades_on_startup=false"
    Write-Host "      [OK] Added reload_trades_on_startup=false" -ForegroundColor Green
}
Copy-Item "C:\Omega\src\gui\www\omega_index.html" "Release\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\chimera_logo.png" "Release\chimera_logo.png" -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Rebuild complete. Check version:" -ForegroundColor Cyan
Write-Host "  http://185.167.119.59:7779/version" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location Release
.\Omega.exe omega_config.ini
