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
$expectedHash = (git -C C:\Omega rev-parse --short origin/main).Trim()
Write-Host "      [OK] Omega.exe built - hash: $expectedHash" -ForegroundColor Green
Write-Host ""

Write-Host "[4/4] Copying assets and starting..." -ForegroundColor Yellow
$configSource = "C:\Omega\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "C:\Omega\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found in repo" -ForegroundColor Red
    exit 1
}
Copy-Item $configSource "Release\omega_config.ini" -Force
$cfgContent = Get-Content "Release\omega_config.ini" -Raw
if ($cfgContent -notmatch "reload_trades_on_startup") {
    Add-Content "Release\omega_config.ini" "`nreload_trades_on_startup=false"
    Write-Host "      [OK] Added reload_trades_on_startup=false" -ForegroundColor Green
}
Copy-Item "C:\Omega\src\gui\www\omega_index.html" "Release\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\chimera_logo.png" "Release\chimera_logo.png" -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Rebuild complete. Hash: $expectedHash" -ForegroundColor Cyan
Write-Host "  GUI: http://185.167.119.59:7779" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location Release
# Copy exe + config to C:\Omega root so it always runs from the right working directory
Copy-Item ".\Omega.exe" "C:\Omega\Omega.exe" -Force
Copy-Item ".\omega_config.ini" "C:\Omega\omega_config.ini" -Force
Copy-Item ".\omega_index.html" "C:\Omega\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item ".\chimera_logo.png" "C:\Omega\chimera_logo.png" -Force -ErrorAction SilentlyContinue

# Ensure logs directory exists before starting
New-Item -ItemType Directory -Path "C:\Omega\logs" -Force | Out-Null
Write-Host "      [OK] C:\Omega\logs ready" -ForegroundColor Green

# Run from C:\Omega so all paths resolve correctly
# Tee output to log file at shell level as belt-and-suspenders
Set-Location C:\Omega
$logFile = "C:\Omega\logs\omega_$(Get-Date -Format 'yyyy-MM-dd').log"
Write-Host "Starting Omega.exe — log: $logFile" -ForegroundColor Cyan
.\Omega.exe omega_config.ini 2>&1 | Tee-Object -FilePath $logFile -Append
