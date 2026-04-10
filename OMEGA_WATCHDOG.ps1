# OMEGA_WATCHDOG.ps1
# Monitors latest.log AND L2 CSV for staleness, auto-restarts Omega if either goes dead.
# Run in a separate PowerShell window or as a scheduled task.
#
# Usage: .\OMEGA_WATCHDOG.ps1

param(
    [int]$StaleThresholdSec    = 60,   # restart if latest.log has no new line in 60s
    [int]$L2StaleThresholdSec  = 120,  # alert if L2 CSV has no new row in 120s (markets open only)
    [int]$CheckIntervalSec     = 15,   # check every 15s
    [int]$PostRestartWaitSec   = 30    # wait 30s after restart before monitoring again
)

$LogFile       = "C:\Omega\logs\latest.log"
$RestartScript = "C:\Omega\RESTART_OMEGA.ps1"
$ServiceName   = "Omega"
$WatchdogLog   = "C:\Omega\logs\watchdog.log"

function Write-WD {
    param([string]$msg)
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $line = "$ts [WATCHDOG] $msg"
    Write-Host $line
    Add-Content -Path $WatchdogLog -Value $line
}

function Get-L2CsvPath {
    $today = Get-Date -Format "yyyy-MM-dd"
    return "C:\Omega\logs\l2_ticks_$today.csv"
}

function Is-MarketHours {
    # UTC hour -- markets open 07:00-22:00 UTC (London + NY sessions)
    $utcHour = (Get-Date).ToUniversalTime().Hour
    return ($utcHour -ge 7 -and $utcHour -lt 22)
}

Write-WD "Started. StaleThreshold=${StaleThresholdSec}s L2Threshold=${L2StaleThresholdSec}s CheckInterval=${CheckIntervalSec}s"
Write-WD "Monitoring: $LogFile"

$restartCount  = 0
$l2AlertCount  = 0

while ($true) {
    Start-Sleep -Seconds $CheckIntervalSec

    # ── Service check ────────────────────────────────────────────────────────
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $svc) {
        Write-WD "ERROR: Service '$ServiceName' not found. Check service name."
        continue
    }
    if ($svc.Status -ne 'Running') {
        Write-WD "Service not running (status=$($svc.Status)). Restarting..."
        & $RestartScript
        $restartCount++
        Write-WD "Restart #$restartCount complete. Waiting ${PostRestartWaitSec}s..."
        Start-Sleep -Seconds $PostRestartWaitSec
        continue
    }

    # ── latest.log staleness check ───────────────────────────────────────────
    if (-not (Test-Path $LogFile)) {
        Write-WD "latest.log MISSING. Restarting..."
        & $RestartScript
        $restartCount++
        Start-Sleep -Seconds $PostRestartWaitSec
        continue
    }
    $logAge = (Get-Date - (Get-Item $LogFile).LastWriteTime).TotalSeconds
    if ($logAge -gt $StaleThresholdSec) {
        Write-WD "latest.log STALE for ${logAge}s (threshold=${StaleThresholdSec}s). Restarting..."
        & $RestartScript
        $restartCount++
        Write-WD "Restart #$restartCount complete. Waiting ${PostRestartWaitSec}s..."
        Start-Sleep -Seconds $PostRestartWaitSec
        continue
    }

    # ── L2 CSV staleness check (market hours only) ────────────────────────────
    if (Is-MarketHours) {
        $l2path = Get-L2CsvPath
        if (-not (Test-Path $l2path)) {
            Write-WD "L2-CSV-MISSING: $l2path does not exist during market hours!"
            $l2AlertCount++
        } else {
            $l2Age = (Get-Date - (Get-Item $l2path).LastWriteTime).TotalSeconds
            if ($l2Age -gt $L2StaleThresholdSec) {
                Write-WD "L2-CSV-STALE: $l2path last write ${l2Age}s ago (threshold=${L2StaleThresholdSec}s) -- L2 data may be lost!"
                $l2AlertCount++
                # Don't restart for L2 staleness alone -- engine may still be trading
                # But log it clearly so it's always visible
            } else {
                # L2 healthy -- reset alert count
                if ($l2AlertCount -gt 0) {
                    Write-WD "L2-CSV-RECOVERED: $l2path writing again (age=${l2Age}s)"
                    $l2AlertCount = 0
                }
            }
        }
    }

    # ── Heartbeat every 5 minutes ─────────────────────────────────────────────
    $nowMin = [int](Get-Date).Minute
    if ($nowMin % 5 -eq 0 -and [int](Get-Date).Second -lt $CheckIntervalSec) {
        $l2path = Get-L2CsvPath
        $l2size = if (Test-Path $l2path) { [math]::Round((Get-Item $l2path).Length/1MB, 2) } else { 0 }
        Write-WD "OK -- log_age=${logAge}s L2_size=${l2size}MB restarts=$restartCount l2_alerts=$l2AlertCount"
    }
}
