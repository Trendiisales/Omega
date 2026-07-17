#Requires -RunAsAdministrator
#Requires -Version 5.1
# ==============================================================================
#                  INSTALL_OMEGA.ps1  --  NSSM SERVICE INSTALLER
#                  S12 PS1 Consolidation, 2026-05-07
# ==============================================================================
#
# Replaces (and supersedes) the following pre-S12 installers:
#   INSTALL_SERVICE.ps1   --> .\INSTALL_OMEGA.ps1 -InstallService
#                             (also still removes any stale "Omega" service by
#                              default with -CleanService).
#   INSTALL_WATCHDOG.ps1  --> .\INSTALL_OMEGA.ps1 -InstallWatchdog
#                             (now wraps `OMEGA.ps1 watchdog` instead of the
#                              deleted OMEGA_WATCHDOG.ps1 file).
#
# What this script does, per flag:
#   -CleanService     (default ON when no flags given): finds a stale or broken
#                     "Omega" Windows service and removes it via sc.exe delete.
#                     This is the safe pre-install step from the old
#                     INSTALL_SERVICE.ps1 -- a no-op if nothing exists.
#   -InstallService   Installs Omega.exe as the NSSM-wrapped Windows service
#                     "Omega" (auto-start, LocalSystem). Configures stdout/
#                     stderr redirection to logs\omega_service_*.log with daily
#                     rotation. Downloads NSSM 2.24 to C:\nssm\ if missing.
#   -InstallWatchdog  Installs the watchdog as the NSSM-wrapped Windows service
#                     "OmegaWatchdog" (auto-start). Service body is
#                     `powershell.exe -File <OmegaDir>\OMEGA.ps1 watchdog`.
#                     Auto-installs NSSM if missing.
#   -All              Shorthand for -CleanService -InstallService -InstallWatchdog
#                     in that order. The most common one-shot setup on a fresh
#                     VPS or after an Omega/<-> directory wipe.
#   -Uninstall        Removes BOTH services (Omega and OmegaWatchdog) and exits.
#                     Does NOT touch C:\Omega or NSSM itself.
#
# Usage:
#   .\INSTALL_OMEGA.ps1                        # CleanService only (no-op if absent)
#   .\INSTALL_OMEGA.ps1 -All                   # Full install (clean + service + watchdog)
#   .\INSTALL_OMEGA.ps1 -InstallService        # Just (re)install the Omega service
#   .\INSTALL_OMEGA.ps1 -InstallWatchdog       # Just (re)install OmegaWatchdog
#   .\INSTALL_OMEGA.ps1 -Uninstall             # Remove both services
#
# Notes:
#   * The Omega service runs Omega.exe with `omega_config.ini` as argv[1] and
#     `C:\Omega` as the working directory. Stop-Service triggers a graceful
#     CTRL_BREAK_EVENT via NSSM -- handled by Omega's console_ctrl_handler.
#   * The OmegaWatchdog service runs powershell.exe with
#     `-NonInteractive -ExecutionPolicy Bypass -File C:\Omega\OMEGA.ps1 watchdog`.
#     Its log is logs\watchdog.log (independent of latest.log).
#   * NSSM AppRotateFiles=1, AppRotateSeconds=86400 (daily), AppRotateBytes=10MB
#     for the Omega service stdout/stderr logs.
#   * NSSM AppRestartDelay=5000ms, AppThrottle=30000ms for the Omega service so
#     a tight crash loop is rate-limited at the NSSM layer.
# ==============================================================================

[CmdletBinding()]
param(
    [string]$OmegaDir         = "C:\Omega",
    [string]$NssmRoot         = "C:\nssm",
    [string]$NssmVersion      = "2.24",
    [switch]$All,
    [switch]$CleanService,
    [switch]$InstallService,
    [switch]$InstallWatchdog,
    [switch]$Uninstall
)

Set-StrictMode -Off
$ErrorActionPreference = "Continue"

# ------------------------------------------------------------------------------
# Common variables
# ------------------------------------------------------------------------------
$OmegaSvcName      = "Omega"
$WatchdogSvcName   = "OmegaWatchdog"
$OmegaExe          = "$OmegaDir\Omega.exe"
$OmegaScript       = "$OmegaDir\OMEGA.ps1"
$OmegaConfig       = "omega_config.ini"
$ServiceStdoutLog  = "$OmegaDir\logs\omega_service_stdout.log"
$ServiceStderrLog  = "$OmegaDir\logs\omega_service_stderr.log"
$WatchdogLog       = "$OmegaDir\logs\watchdog.log"
$NssmDir           = "$NssmRoot\nssm-$NssmVersion\win64"
$NssmExe           = "$NssmDir\nssm.exe"
$PsExe             = "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"

# Default behaviour when the user passes no action flags: do a CleanService
# pass only. This matches the old INSTALL_SERVICE.ps1 behaviour exactly.
if (-not ($All -or $CleanService -or $InstallService -or $InstallWatchdog -or $Uninstall)) {
    $CleanService = $true
}
if ($All) {
    $CleanService    = $true
    $InstallService  = $true
    $InstallWatchdog = $true
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  INSTALL  (S12 PS1 Consolidation)" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  OmegaDir       = $OmegaDir" -ForegroundColor DarkGray
Write-Host "  Service name   = $OmegaSvcName" -ForegroundColor DarkGray
Write-Host "  Watchdog name  = $WatchdogSvcName" -ForegroundColor DarkGray
Write-Host "  NSSM           = $NssmExe" -ForegroundColor DarkGray
Write-Host ""

# ------------------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------------------
function Ensure-Nssm {
    if (Test-Path $NssmExe) { return $true }
    Write-Host "  NSSM not found. Downloading nssm-$NssmVersion..." -ForegroundColor Yellow
    $zip = "$env:TEMP\nssm-$NssmVersion.zip"
    try {
        Invoke-WebRequest "https://nssm.cc/release/nssm-$NssmVersion.zip" -OutFile $zip -UseBasicParsing -ErrorAction Stop
    } catch {
        Write-Host "  [FATAL] Could not download NSSM: $_" -ForegroundColor Red
        return $false
    }
    if (-not (Test-Path $NssmRoot)) { New-Item -ItemType Directory -Path $NssmRoot -Force | Out-Null }
    try {
        Expand-Archive -Path $zip -DestinationPath $NssmRoot -Force -ErrorAction Stop
    } catch {
        Write-Host "  [FATAL] Could not extract NSSM zip: $_" -ForegroundColor Red
        return $false
    }
    if (-not (Test-Path $NssmExe)) {
        Write-Host "  [FATAL] NSSM extracted but $NssmExe still missing" -ForegroundColor Red
        return $false
    }
    Write-Host "  [OK] NSSM installed at $NssmExe" -ForegroundColor Green
    return $true
}

function Remove-NssmService {
    param([string]$ServiceName)
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (-not $svc) {
        Write-Host "  [OK] No existing '$ServiceName' service -- nothing to remove" -ForegroundColor Green
        return $true
    }
    Write-Host "  Removing existing '$ServiceName' (status=$($svc.Status))..." -ForegroundColor Yellow
    if ($svc.Status -eq 'Running') {
        if (Test-Path $NssmExe) { & $NssmExe stop $ServiceName 2>$null | Out-Null }
        Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
    }
    if (Test-Path $NssmExe) { & $NssmExe remove $ServiceName confirm 2>$null | Out-Null }
    & sc.exe delete $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    $check = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($check) {
        Write-Host "  [WARN] '$ServiceName' still present after delete -- may need reboot to fully clear" -ForegroundColor Yellow
        return $false
    }
    Write-Host "  [OK] '$ServiceName' removed" -ForegroundColor Green
    return $true
}

# ------------------------------------------------------------------------------
# -Uninstall (mutually exclusive shortcut)
# ------------------------------------------------------------------------------
if ($Uninstall) {
    Write-Host "[-Uninstall] Removing OmegaWatchdog and Omega services..." -ForegroundColor Yellow
    Remove-NssmService -ServiceName $WatchdogSvcName | Out-Null
    Remove-NssmService -ServiceName $OmegaSvcName    | Out-Null
    Write-Host ""
    Write-Host "  [DONE] Uninstall complete. C:\Omega and NSSM itself are untouched." -ForegroundColor Green
    exit 0
}

# ------------------------------------------------------------------------------
# -CleanService  (always safe to run; no-op if nothing exists)
# ------------------------------------------------------------------------------
if ($CleanService) {
    Write-Host "[-CleanService] Removing any stale '$OmegaSvcName' service..." -ForegroundColor Yellow
    $existing = Get-Service -Name $OmegaSvcName -ErrorAction SilentlyContinue
    if ($existing) {
        $svcPath = (Get-WmiObject Win32_Service -Filter "Name='$OmegaSvcName'" -ErrorAction SilentlyContinue).PathName
        Write-Host "  Existing service binary : $svcPath" -ForegroundColor DarkGray
    }
    Remove-NssmService -ServiceName $OmegaSvcName | Out-Null
    Write-Host ""
}

# ------------------------------------------------------------------------------
# -InstallService  (Omega.exe -> NSSM-wrapped Windows service "Omega")
# ------------------------------------------------------------------------------
if ($InstallService) {
    Write-Host "[-InstallService] Installing Omega NSSM service..." -ForegroundColor Yellow

    if (-not (Test-Path $OmegaExe)) {
        Write-Host "  [FATAL] $OmegaExe not found." -ForegroundColor Red
        Write-Host "  Build Omega first via:  .\OMEGA.ps1 deploy" -ForegroundColor Red
        exit 1
    }
    if (-not (Ensure-Nssm)) { exit 1 }

    # Always remove existing first (idempotent reinstall)
    Remove-NssmService -ServiceName $OmegaSvcName | Out-Null
    Start-Sleep -Seconds 1

    Write-Host "  Installing service '$OmegaSvcName' -> $OmegaExe $OmegaConfig" -ForegroundColor Yellow
    & $NssmExe install $OmegaSvcName $OmegaExe $OmegaConfig
    & $NssmExe set $OmegaSvcName AppDirectory      $OmegaDir
    & $NssmExe set $OmegaSvcName AppStdout         $ServiceStdoutLog
    & $NssmExe set $OmegaSvcName AppStderr         $ServiceStderrLog
    & $NssmExe set $OmegaSvcName AppRotateFiles    1
    & $NssmExe set $OmegaSvcName AppRotateSeconds  86400
    & $NssmExe set $OmegaSvcName AppRotateBytes    10485760
    & $NssmExe set $OmegaSvcName Start             SERVICE_AUTO_START
    & $NssmExe set $OmegaSvcName ObjectName        LocalSystem
    & $NssmExe set $OmegaSvcName AppRestartDelay   5000
    & $NssmExe set $OmegaSvcName AppThrottle       30000
    # S88-followup (2026-05-27): wire IBKR DOM bridge consumer at startup.
    # Without OMEGA_IBKR_BRIDGE=1, the consumer thread in omega_main.hpp:557
    # never starts -> g_ibkr_l2.xau.fresh() always false -> the L2 logger at
    # tick_gold.hpp:1205-1213 writes 0 for depth_bid_levels/depth_ask_levels/
    # l2_bid_vol/l2_ask_vol on every tick. Captures Apr-30 to May-26 lost ~all
    # depth data because of this. Setting the env here keeps the fix across
    # re-installs of the service.
    # S-2026-07-16: BOTH bridge envs must be set here. OMEGA_BIGCAP_BRIDGE=1 wires
    # the bigcap L1 consumer (omega_main.hpp:1050, port :7784) that feeds the
    # in-binary daily-close writer -> data/rdagent/sp500_long_close.csv. This env
    # was NEVER set on the omega-new box, so the bigcap up-jump ladder silently
    # rode the yfinance fallback and its daily feed went stale (operator asked about
    # "staleness every single day"). A re-provision that set only OMEGA_IBKR_BRIDGE=1
    # here is exactly how the var stayed missing. nssm set with multiple values writes
    # a REG_MULTI_SZ (one env per line) -- pass BOTH so neither is ever dropped again.
    # Guarded end-to-end by tools/feedpath_selftest.py ([SERVICE-ENV] + [BIGCAP-CONSUMER]).
    # S-2026-07-18 REAL-MONEY CUTOVER: OMEGA_IBKR_PORT=4001 + OMEGA_IBKR_LIVE_ORDERS=1
    # route execution to the LIVE gateway (4001) with paper_only cleared. A re-provision
    # that omits them silently reverts the book to paper — pass ALL FOUR, always.
    & $NssmExe set $OmegaSvcName AppEnvironmentExtra "OMEGA_IBKR_BRIDGE=1" "OMEGA_BIGCAP_BRIDGE=1" "OMEGA_IBKR_PORT=4001" "OMEGA_IBKR_LIVE_ORDERS=1"
    & $NssmExe set $OmegaSvcName DisplayName       "Omega Trading Engine"
    & $NssmExe set $OmegaSvcName Description       "Omega commodities + indices breakout trading engine. Managed by NSSM. Use OMEGA.ps1 to start/stop/restart."

    # Ensure log directories exist before NSSM tries to open them
    New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
    New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
    New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

    $svcPath = (Get-WmiObject Win32_Service -Filter "Name='$OmegaSvcName'" -ErrorAction SilentlyContinue).PathName
    Write-Host "  Service binary : $svcPath" -ForegroundColor Cyan
    Write-Host "  [OK] '$OmegaSvcName' installed (auto-start, LocalSystem)" -ForegroundColor Green
    Write-Host "       Start manually:  Start-Service $OmegaSvcName     -- or .\OMEGA.ps1 start" -ForegroundColor DarkGray
    Write-Host "       Stop manually :  Stop-Service  $OmegaSvcName     -- or .\OMEGA.ps1 stop" -ForegroundColor DarkGray
    Write-Host ""
}

# ------------------------------------------------------------------------------
# -InstallWatchdog  (OMEGA.ps1 watchdog -> NSSM-wrapped service "OmegaWatchdog")
# ------------------------------------------------------------------------------
if ($InstallWatchdog) {
    Write-Host "[-InstallWatchdog] Installing OmegaWatchdog NSSM service..." -ForegroundColor Yellow

    if (-not (Test-Path $OmegaScript)) {
        Write-Host "  [FATAL] $OmegaScript not found -- watchdog needs OMEGA.ps1 to run." -ForegroundColor Red
        exit 1
    }
    if (-not (Ensure-Nssm)) { exit 1 }

    # Always remove existing first (idempotent reinstall)
    Remove-NssmService -ServiceName $WatchdogSvcName | Out-Null
    Start-Sleep -Seconds 1

    # The watchdog body is `OMEGA.ps1 watchdog`. Quote $OmegaScript so paths
    # with spaces survive NSSM's command parser. -NonInteractive prevents the
    # script from blocking on Read-Host; -ExecutionPolicy Bypass keeps it
    # running on hosts with a restrictive default policy.
    $wdArgs = "-NonInteractive -ExecutionPolicy Bypass -File `"$OmegaScript`" watchdog -OmegaDir `"$OmegaDir`""

    Write-Host "  Installing service '$WatchdogSvcName' -> $PsExe $wdArgs" -ForegroundColor Yellow
    & $NssmExe install $WatchdogSvcName $PsExe $wdArgs
    & $NssmExe set $WatchdogSvcName AppDirectory     $OmegaDir
    & $NssmExe set $WatchdogSvcName DisplayName      "Omega Watchdog"
    & $NssmExe set $WatchdogSvcName Description      "Monitors Omega service + latest.log staleness + L2 CSV data loss + GitHub HEAD. Defers any restart while open positions are detected."
    & $NssmExe set $WatchdogSvcName Start            SERVICE_AUTO_START
    & $NssmExe set $WatchdogSvcName AppStdout        $WatchdogLog
    & $NssmExe set $WatchdogSvcName AppStderr        $WatchdogLog
    & $NssmExe set $WatchdogSvcName AppRotateFiles   1
    & $NssmExe set $WatchdogSvcName AppRotateSeconds 86400
    & $NssmExe set $WatchdogSvcName AppRestartDelay  5000

    New-Item -ItemType Directory -Path "$OmegaDir\logs" -Force | Out-Null

    Write-Host "  Starting '$WatchdogSvcName'..." -ForegroundColor Yellow
    Start-Service $WatchdogSvcName
    Start-Sleep -Seconds 3
    $svc = Get-Service -Name $WatchdogSvcName -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq 'Running') {
        Write-Host "  [OK] '$WatchdogSvcName' is RUNNING" -ForegroundColor Green
        Write-Host "       Watchdog log : $WatchdogLog" -ForegroundColor DarkGray
        Write-Host "       Monitors     : Omega service + latest.log + L2 CSV + GitHub HEAD" -ForegroundColor DarkGray
    } else {
        Write-Host "  [ERROR] '$WatchdogSvcName' failed to start (status=$($svc.Status))" -ForegroundColor Red
        Write-Host "          Check $WatchdogLog and Windows Event Log for details." -ForegroundColor Red
        exit 1
    }
    Write-Host ""
}

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   INSTALL COMPLETE" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Use OMEGA.ps1 from here on:" -ForegroundColor Yellow
Write-Host "    .\OMEGA.ps1 deploy        # full pipeline (build + start)" -ForegroundColor Yellow
Write-Host "    .\OMEGA.ps1 restart       # stop + start (no rebuild)" -ForegroundColor Yellow
Write-Host "    .\OMEGA.ps1 start         # start service (stamp-verified)" -ForegroundColor Yellow
Write-Host "    .\OMEGA.ps1 stop          # graceful stop" -ForegroundColor Yellow
Write-Host "    .\OMEGA.ps1 help          # show all options" -ForegroundColor Yellow
Write-Host ""
exit 0
