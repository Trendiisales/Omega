#Requires -RunAsAdministrator
# ==============================================================================
#  OMEGA -- SERVICE MANAGEMENT UTILITY
# ==============================================================================
#  Omega.exe is a CONSOLE APPLICATION and runs as a DIRECT PROCESS.
#  It does NOT run as a Windows Service under normal operation.
#
#  Normal start/stop:
#    Start:  Start-Process -FilePath C:\Omega\Omega.exe -ArgumentList "omega_config.ini" -WorkingDirectory C:\Omega -NoNewWindow
#    Stop:   taskkill /F /IM Omega.exe /T
#    Restart: .\QUICK_RESTART.ps1
#
#  This script handles two tasks:
#    1. CLEAN  -- removes any broken/stale "Omega" Windows service (DEFAULT)
#    2. NSSM   -- optionally installs Omega as a proper service via NSSM
#                 (only needed if you want auto-start on VPS reboot without
#                  a scheduled task; QUICK_RESTART.ps1 does NOT use the service)
#
#  USAGE:
#    Remove broken service:         .\INSTALL_SERVICE.ps1
#    Install NSSM service wrapper:  .\INSTALL_SERVICE.ps1 -InstallNssm
# ==============================================================================

param(
    [string]  $OmegaDir    = "C:\Omega",
    [switch]  $InstallNssm
)

$ServiceName = "Omega"
$OmegaExe    = "$OmegaDir\Omega.exe"
$NssmExe     = "C:\nssm\nssm-2.24\win64\nssm.exe"

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  SERVICE MANAGEMENT" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# ==============================================================================
# ALWAYS: Remove any existing broken service
# ==============================================================================
$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "  Found existing '$ServiceName' service -- removing..." -ForegroundColor Yellow
    Write-Host "  Current status : $($existing.Status)" -ForegroundColor Yellow

    # Check what binary the service points to
    $svcPath = (Get-WmiObject Win32_Service -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue).PathName
    Write-Host "  Service binary : $svcPath" -ForegroundColor Yellow

    # Stop if running
    if ($existing.Status -eq "Running") {
        Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
    }

    # Remove via sc.exe (works without NSSM)
    sc.exe delete $ServiceName
    Start-Sleep -Seconds 2

    # Confirm gone
    $check = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($check) {
        Write-Host "  [WARN] Service still present after delete -- may need reboot to fully clear" -ForegroundColor Yellow
    } else {
        Write-Host "  [OK] Broken service removed" -ForegroundColor Green
    }
} else {
    Write-Host "  No existing '$ServiceName' service found -- nothing to remove" -ForegroundColor Green
}

Write-Host ""

if (-not $InstallNssm) {
    Write-Host "  Omega runs as a direct process. Use QUICK_RESTART.ps1 to manage it." -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  To install as an NSSM service (optional, for auto-start on reboot):" -ForegroundColor Gray
    Write-Host "    .\INSTALL_SERVICE.ps1 -InstallNssm" -ForegroundColor Gray
    Write-Host ""
    exit 0
}

# ==============================================================================
# OPTIONAL: Install NSSM service wrapper
# ==============================================================================
Write-Host "  Installing NSSM service wrapper..." -ForegroundColor Yellow
Write-Host ""

# Check Omega.exe exists
if (-not (Test-Path $OmegaExe)) {
    Write-Host "  [FATAL] $OmegaExe not found -- build Omega first via QUICK_RESTART.ps1" -ForegroundColor Red
    exit 1
}

# Download NSSM if not present
if (-not (Test-Path $NssmExe)) {
    Write-Host "  NSSM not found. Downloading..." -ForegroundColor Yellow
    Invoke-WebRequest "https://nssm.cc/release/nssm-2.24.zip" -OutFile "C:\nssm.zip"
    Expand-Archive "C:\nssm.zip" -DestinationPath "C:\nssm" -Force
    if (-not (Test-Path $NssmExe)) {
        Write-Host "  [FATAL] NSSM download/extract failed" -ForegroundColor Red
        exit 1
    }
    Write-Host "  [OK] NSSM installed at $NssmExe" -ForegroundColor Green
}

# Install service
Write-Host "  Installing $ServiceName service pointing to $OmegaExe..." -ForegroundColor Yellow

& $NssmExe install $ServiceName $OmegaExe "omega_config.ini"
& $NssmExe set $ServiceName AppDirectory $OmegaDir
& $NssmExe set $ServiceName AppStdout "$OmegaDir\logs\omega_service_stdout.log"
& $NssmExe set $ServiceName AppStderr "$OmegaDir\logs\omega_service_stderr.log"
& $NssmExe set $ServiceName AppRotateFiles 1
& $NssmExe set $ServiceName AppRotateSeconds 86400
& $NssmExe set $ServiceName AppRotateBytes 10485760
& $NssmExe set $ServiceName Start SERVICE_AUTO_START
& $NssmExe set $ServiceName ObjectName LocalSystem
& $NssmExe set $ServiceName AppRestartDelay 5000
& $NssmExe set $ServiceName AppThrottle 30000

# Verify it points to the right binary
$svcPath = (Get-WmiObject Win32_Service -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue).PathName
Write-Host "  Service binary : $svcPath" -ForegroundColor Cyan

Write-Host "  [OK] NSSM service installed" -ForegroundColor Green
Write-Host ""
Write-Host "  NOTE: QUICK_RESTART.ps1 uses direct Start-Process, NOT this service." -ForegroundColor Yellow
Write-Host "  The NSSM service provides auto-start on VPS reboot ONLY." -ForegroundColor Yellow
Write-Host "  To start manually: Start-Service Omega" -ForegroundColor Gray
Write-Host "  To stop manually:  Stop-Service Omega  (or taskkill /F /IM Omega.exe /T)" -ForegroundColor Gray
Write-Host ""
