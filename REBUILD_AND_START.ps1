# ==============================================================================
#                   OMEGA — CLEAN REBUILD AND START
# ==============================================================================

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA — CLEAN REBUILD                               " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "[1/3] Stopping Omega..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Host "      [OK]" -ForegroundColor Green
Write-Host ""

Write-Host "[2/3] Clean build..." -ForegroundColor Yellow
cd C:\Omega
git pull origin main
Remove-Item -Path "C:\Omega\build" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "C:\Omega\build" -Force | Out-Null
cd C:\Omega\build
cmake ..
cmake --build . --config Release

if (-not (Test-Path "Release\Omega.exe")) {
    Write-Host "      [ERROR] Build failed!" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] Omega.exe built" -ForegroundColor Green
Write-Host ""

Write-Host "[3/3] Copying assets and starting..." -ForegroundColor Yellow
Copy-Item "C:\Omega\omega_config.ini"             "Release\omega_config.ini"  -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\omega_index.html"  "Release\omega_index.html"  -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\chimera_logo.png"  "Release\chimera_logo.png"  -Force -ErrorAction SilentlyContinue

cd Release
.\Omega.exe omega_config.ini
