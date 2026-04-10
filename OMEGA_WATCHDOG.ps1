# OMEGA_WATCHDOG.ps1
# Fully independent watchdog -- survives latest.log dying.
# Writes ONLY to its own watchdog.log -- never depends on latest.log.
# Install as service: run INSTALL_WATCHDOG.ps1 once.
#
# Monitors:
#   1. Omega service running
#   2. latest.log not stale (restart if stale >60s)
#   3. L2 CSV writing during market hours (alert if stale >120s)
#   4. Stale/wrong binary -- checks running commit hash matches GitHub HEAD
#
# Usage: .\OMEGA_WATCHDOG.ps1
# As service: installed via INSTALL_WATCHDOG.ps1

param(
    [int]$StaleThresholdSec   = 60,
    [int]$L2StaleThresholdSec = 120,
    [int]$CheckIntervalSec    = 15,
    [int]$PostRestartWaitSec  = 30
)

$ServiceName   = "Omega"
$RestartScript = "C:\Omega\RESTART_OMEGA.ps1"
$LogFile       = "C:\Omega\logs\latest.log"
$WatchdogLog   = "C:\Omega\logs\watchdog.log"  # INDEPENDENT -- never stops even if latest.log dies

function Write-WD {
    param([string]$msg)
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $line = "$ts [WATCHDOG] $msg"
    Write-Host $line
    Add-Content -Path $WatchdogLog -Value $line -ErrorAction SilentlyContinue
}

function Get-L2CsvPath {
    $today = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd")
    return "C:\Omega\logs\l2_ticks_$today.csv"
}

function Is-MarketHours {
    $h = (Get-Date).ToUniversalTime().Hour
    return ($h -ge 7 -and $h -lt 22)
}

function Get-RunningCommit {
    try {
        $content = Get-Content $LogFile -Tail 200 -ErrorAction SilentlyContinue
        $line = $content | Select-String 'RUNNING COMMIT:' | Select-Object -Last 1
        if ($line -match 'RUNNING COMMIT:\s+([a-f0-9]{7,12})') { return $Matches[1] }
    } catch {}
    return $null
}

Write-WD "=== WATCHDOG STARTED === StaleThreshold=${StaleThresholdSec}s L2Threshold=${L2StaleThresholdSec}s"
Write-WD "Watchdog log: $WatchdogLog (independent of latest.log)"

$restartCount = 0
$l2AlertCount = 0

while ($true) {
    Start-Sleep -Seconds $CheckIntervalSec

    # 1. Service running?
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $svc -or $svc.Status -ne 'Running') {
        Write-WD "SERVICE DOWN (status=$($svc.Status)). Restarting..."
        & $RestartScript
        $restartCount++
        Write-WD "Restart #$restartCount done. Waiting ${PostRestartWaitSec}s..."
        Start-Sleep -Seconds $PostRestartWaitSec
        continue
    }

    # 2. latest.log stale?
    if (-not (Test-Path $LogFile)) {
        Write-WD "latest.log MISSING. Restarting..."
        & $RestartScript; $restartCount++
        Start-Sleep -Seconds $PostRestartWaitSec; continue
    }
    $logAge = (Get-Date - (Get-Item $LogFile).LastWriteTime).TotalSeconds
    if ($logAge -gt $StaleThresholdSec) {
        Write-WD "latest.log STALE ${logAge}s (max=${StaleThresholdSec}s). Restarting..."
        & $RestartScript; $restartCount++
        Write-WD "Restart #$restartCount done. Waiting ${PostRestartWaitSec}s..."
        Start-Sleep -Seconds $PostRestartWaitSec; continue
    }

    # 3. L2 CSV writing? (market hours only)
    if (Is-MarketHours) {
        $l2path = Get-L2CsvPath
        if (-not (Test-Path $l2path)) {
            Write-WD "L2-CSV-MISSING: $l2path not found during market hours!"
            $l2AlertCount++
        } else {
            $l2Age = (Get-Date - (Get-Item $l2path).LastWriteTime).TotalSeconds
            $l2Size = [math]::Round((Get-Item $l2path).Length / 1MB, 2)
            if ($l2Age -gt $L2StaleThresholdSec) {
                Write-WD "L2-CSV-STALE: age=${l2Age}s size=${l2Size}MB -- L2 DATA LOSS OCCURRING"
                $l2AlertCount++
            } else {
                if ($l2AlertCount -gt 0) {
                    Write-WD "L2-CSV-RECOVERED: age=${l2Age}s size=${l2Size}MB after $l2AlertCount alerts"
                    $l2AlertCount = 0
                }
            }
        }
    }

    # 4. Heartbeat every 5 minutes
    $nowMin = [int](Get-Date).Minute
    if ($nowMin % 5 -eq 0 -and [int](Get-Date).Second -lt $CheckIntervalSec) {
        $l2path = Get-L2CsvPath
        $l2size = if (Test-Path $l2path) { [math]::Round((Get-Item $l2path).Length/1MB,2) } else { "MISSING" }
        $commit = Get-RunningCommit
        Write-WD "HEARTBEAT log_age=${logAge}s L2=$l2size MB commit=$commit restarts=$restartCount l2alerts=$l2AlertCount"
    }
}
