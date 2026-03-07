# ==============================================================================
#                        OMEGA — DEPLOY & START
#   Mirrors ChimeraMetals deploy pattern exactly.
#   Run from anywhere on the VPS — handles everything.
# ==============================================================================

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  Commodities & Indices  |  Breakout System  " -ForegroundColor Cyan
Write-Host "   Deploy Script v1.0                                   " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# ── [1/4] Stop any running Omega process ─────────────────────────────────────
Write-Host "[1/4] Stopping existing Omega process..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Host "      [OK] Stopped (or was not running)" -ForegroundColor Green
Write-Host ""

# ── [2/4] Pull latest from GitHub ────────────────────────────────────────────
Write-Host "[2/4] Pulling latest from GitHub..." -ForegroundColor Yellow
cd C:\Omega
git pull origin main
if ($LASTEXITCODE -ne 0) {
    Write-Host "      [ERROR] git pull failed!" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] Up to date" -ForegroundColor Green
Write-Host ""

# ── [3/4] Build ───────────────────────────────────────────────────────────────
Write-Host "[3/4] Building..." -ForegroundColor Yellow
if (-not (Test-Path "C:\Omega\build")) {
    New-Item -ItemType Directory -Path "C:\Omega\build" -Force | Out-Null
}
cd C:\Omega\build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
cmake --build . --config Release
if (-not (Test-Path "C:\Omega\build\Release\Omega.exe")) {
    Write-Host "      [ERROR] Build failed — Omega.exe not found!" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] Omega.exe built" -ForegroundColor Green
Write-Host ""

# ── [4/4] Copy assets + run ───────────────────────────────────────────────────
Write-Host "[4/4] Copying assets and starting..." -ForegroundColor Yellow
$rel = "C:\Omega\build\Release"
Copy-Item "C:\Omega\omega_config.ini"            "$rel\omega_config.ini"  -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\omega_index.html" "$rel\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\chimera_logo.png" "$rel\chimera_logo.png" -Force -ErrorAction SilentlyContinue
Write-Host "      [OK] Assets copied" -ForegroundColor Green
Write-Host ""

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Starting Omega.exe — watch for:" -ForegroundColor Cyan
Write-Host "  [OMEGA] Connecting live-uk-eqx-02.p.c-trader.com:5211" -ForegroundColor White
Write-Host "  [OMEGA] LOGON ACCEPTED" -ForegroundColor White
Write-Host "  [OMEGA] Subscribed: MES MNQ MCL ES NQ CL VIX DX ZN YM RTY" -ForegroundColor White
Write-Host "  [OmegaHTTP] port 7779" -ForegroundColor White
Write-Host "  [OmegaWS]   WebSocket port 7780" -ForegroundColor White
Write-Host "  GUI -> http://localhost:7779" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

cd $rel
.\Omega.exe omega_config.ini
