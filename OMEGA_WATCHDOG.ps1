# OMEGA_WATCHDOG.ps1
# Monitors latest.log for staleness and auto-restarts Omega if log goes dead.
# Run this in a separate PowerShell window or as a scheduled task.
# It will keep Omega alive indefinitely.
#
# Usage: .\OMEGA_WATCHDOG.ps1
# To run minimised at startup: add to Task Scheduler with trigger = At startup

param(
    [int]$StaleThresholdSec = 60,    # restart if no new log line in 60s
    [int]$CheckIntervalSec  = 15,    # check every 15s
    [int]$PostRestartWaitSec = 30    # wait 30s after restart before monitoring again
)

$LogFile     = "C:\Omega\logs\latest.log"
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

Write-WD "Started. StaleThreshold=${StaleThresholdSec}s CheckInterval=${CheckIntervalSec}s"
Write-WD "Monitoring: $LogFile"

$restartCount = 0

while ($true) {
    Start-Sleep -Seconds $CheckIntervalSec

    # Check if service is running
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

    # Check log file freshness
    if (-not (Test-Path $LogFile)) {
        Write-WD "latest.log missing. Restarting..."
        & $RestartScript
        $restartCount++
        Start-Sleep -Seconds $PostRestartWaitSec
        continue
    }

    $lastWrite = (Get-Item $LogFile).LastWriteTime
    $ageSec    = (Get-Date - $lastWrite).TotalSeconds

    if ($ageSec -gt $StaleThresholdSec) {
        Write-WD "latest.log stale for ${ageSec}s (threshold=${StaleThresholdSec}s). Restarting..."
        & $RestartScript
        $restartCount++
        Write-WD "Restart #$restartCount complete. Waiting ${PostRestartWaitSec}s..."
        Start-Sleep -Seconds $PostRestartWaitSec
    } else {
        # Healthy -- log heartbeat every 5 minutes
        $nowMin = [int](Get-Date).Minute
        if ($nowMin % 5 -eq 0 -and [int](Get-Date).Second -lt $CheckIntervalSec) {
            Write-WD "OK -- log age=${ageSec}s restarts=$restartCount"
        }
    }
}
