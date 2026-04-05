#Requires -RunAsAdministrator
# ==============================================================================
#  OMEGA -- INSTALL AS WINDOWS SERVICE
# ==============================================================================
#  Installs Omega.exe as a Windows Service using NSSM so it:
#    - Survives RDP disconnects
#    - Auto-restarts on crash
#    - Starts automatically on VPS reboot
#    - Runs as SYSTEM (no user session needed)
#
#  NSSM download (run once if not present):
#    Invoke-WebRequest https://nssm.cc/release/nssm-2.24.zip -OutFile C:\nssm.zip
#    Expand-Archive C:\nssm.zip -DestinationPath C:\nssm
#
#  USAGE:
#    Run once to install:   .\INSTALL_SERVICE.ps1
#    Then use:              .\QUICK_RESTART.ps1  (manages the service)
#
#  SERVICE COMMANDS (manual):
#    Start:   Start-Service OmegaHFT
#    Stop:    Stop-Service OmegaHFT
#    Status:  Get-Service OmegaHFT
#    Logs:    C:\Omega\logs\omega_service_stdout.log
# ==============================================================================

param([string] $OmegaDir = "C:\Omega")

$ServiceName = "OmegaHFT"
$NssmExe     = "C:\nssm\nssm-2.24\win64\nssm.exe"
$OmegaExe    = "$OmegaDir\Omega.exe"

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  INSTALL WINDOWS SERVICE" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# --- Check NSSM installed ----------------------------------------------------
if (-not (Test-Path $NssmExe)) {
    Write-Host "  NSSM not found. Downloading..." -ForegroundColor Yellow
    Invoke-WebRequest "https://nssm.cc/release/nssm-2.24.zip" -OutFile "C:\nssm.zip"
    Expand-Archive "C:\nssm.zip" -DestinationPath "C:\nssm" -Force
    Write-Host "  [OK] NSSM installed at $NssmExe" -ForegroundColor Green
}

# --- Remove existing service if present --------------------------------------
$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "  Removing existing $ServiceName service..." -ForegroundColor Yellow
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
    & $NssmExe remove $ServiceName confirm
    Start-Sleep -Seconds 2
}

# --- Install service ---------------------------------------------------------
Write-Host "  Installing $ServiceName service..." -ForegroundColor Yellow

& $NssmExe install $ServiceName $OmegaExe "omega_config.ini"
& $NssmExe set $ServiceName AppDirectory $OmegaDir
& $NssmExe set $ServiceName AppStdout "$OmegaDir\logs\omega_service_stdout.log"
& $NssmExe set $ServiceName AppStderr "$OmegaDir\logs\omega_service_stderr.log"
& $NssmExe set $ServiceName AppRotateFiles 1
& $NssmExe set $ServiceName AppRotateSeconds 86400
& $NssmExe set $ServiceName AppRotateBytes 10485760
& $NssmExe set $ServiceName Start SERVICE_AUTO_START
& $NssmExe set $ServiceName ObjectName LocalSystem
& $NssmExe set $ServiceName AppRestartDelay 5000   # 5s restart on crash
& $NssmExe set $ServiceName AppThrottle 30000       # 30s throttle after 3 fast crashes

Write-Host "  [OK] Service installed" -ForegroundColor Green
Write-Host ""
Write-Host "  Starting service..." -ForegroundColor Yellow
Start-Service $ServiceName
Start-Sleep -Seconds 3

$svc = Get-Service -Name $ServiceName
Write-Host "  Status: $($svc.Status)" -ForegroundColor $(if ($svc.Status -eq "Running") { "Green" } else { "Red" })
Write-Host ""
Write-Host "  Service survives RDP disconnects." -ForegroundColor Green
Write-Host "  Auto-restarts on crash (5s delay)." -ForegroundColor Green
Write-Host "  Starts on VPS reboot." -ForegroundColor Green
Write-Host ""
Write-Host "  Use QUICK_RESTART.ps1 as normal -- it now controls the service." -ForegroundColor Cyan
