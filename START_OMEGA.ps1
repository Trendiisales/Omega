# ==============================================================================
#                   OMEGA — START (no rebuild)
# ==============================================================================
$exe = "C:\Omega\build\Release\Omega.exe"
if (-not (Test-Path $exe)) {
    Write-Host "[ERROR] Omega.exe not found — run REBUILD_AND_START.ps1 first" -ForegroundColor Red
    exit 1
}
Write-Host "Starting Omega..." -ForegroundColor Cyan
cd C:\Omega\build\Release
.\Omega.exe omega_config.ini
