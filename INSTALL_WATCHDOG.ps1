# INSTALL_WATCHDOG.ps1
# Installs OMEGA_WATCHDOG.ps1 as a permanent Windows service via NSSM.
# Run once as Administrator. The watchdog will then start automatically
# on boot and restart itself if it crashes -- fully independent of Omega.
#
# Usage: .\INSTALL_WATCHDOG.ps1
# To uninstall: .\INSTALL_WATCHDOG.ps1 -Uninstall

param([switch]$Uninstall)

$WatchdogServiceName = "OmegaWatchdog"
$NssmExe  = "C:\nssm\nssm.exe"
$PsExe    = "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
$Script   = "C:\Omega\OMEGA_WATCHDOG.ps1"
$WdLog    = "C:\Omega\logs\watchdog.log"

if (-not (Test-Path $NssmExe)) {
    Write-Host "ERROR: NSSM not found at $NssmExe" -ForegroundColor Red
    Write-Host "Install NSSM first: https://nssm.cc/download" -ForegroundColor Yellow
    exit 1
}

if ($Uninstall) {
    Write-Host "Uninstalling $WatchdogServiceName..." -ForegroundColor Yellow
    & $NssmExe stop $WatchdogServiceName 2>$null
    & $NssmExe remove $WatchdogServiceName confirm 2>$null
    Write-Host "Done." -ForegroundColor Green
    exit 0
}

# Stop existing if running
$existing = Get-Service -Name $WatchdogServiceName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "Stopping existing $WatchdogServiceName..." -ForegroundColor Yellow
    & $NssmExe stop $WatchdogServiceName 2>$null
    & $NssmExe remove $WatchdogServiceName confirm 2>$null
    Start-Sleep -Seconds 2
}

Write-Host "Installing $WatchdogServiceName as Windows service..." -ForegroundColor Cyan

& $NssmExe install $WatchdogServiceName $PsExe "-NonInteractive -ExecutionPolicy Bypass -File `"$Script`""
& $NssmExe set $WatchdogServiceName AppDirectory "C:\Omega"
& $NssmExe set $WatchdogServiceName DisplayName "Omega Watchdog"
& $NssmExe set $WatchdogServiceName Description "Monitors Omega service and L2 CSV logging. Auto-restarts on failure."
& $NssmExe set $WatchdogServiceName Start SERVICE_AUTO_START
& $NssmExe set $WatchdogServiceName AppStdout $WdLog
& $NssmExe set $WatchdogServiceName AppStderr $WdLog
& $NssmExe set $WatchdogServiceName AppRotateFiles 1
& $NssmExe set $WatchdogServiceName AppRotateSeconds 86400

Start-Service $WatchdogServiceName
Start-Sleep -Seconds 3

$svc = Get-Service -Name $WatchdogServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') {
    Write-Host "[OK] $WatchdogServiceName is RUNNING" -ForegroundColor Green
    Write-Host "     Watchdog log: $WdLog" -ForegroundColor Gray
    Write-Host "     Monitors: Omega service + latest.log staleness + L2 CSV data loss" -ForegroundColor Gray
} else {
    Write-Host "ERROR: $WatchdogServiceName failed to start" -ForegroundColor Red
    exit 1
}
