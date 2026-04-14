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
    [int]$PostRestartWaitSec  = 30,
    [int]$GitHubPollIntervalSec = 300   # Check GitHub HEAD every 5 minutes
)

$ServiceName   = "Omega"
$OmegaDir      = "C:\Omega"
$RestartScript = "$OmegaDir\QUICK_RESTART.ps1"   # Use QUICK_RESTART not RESTART_OMEGA
$LogFile       = "$OmegaDir\logs\latest.log"
$WatchdogLog   = "$OmegaDir\logs\watchdog.log"  # INDEPENDENT -- never stops even if latest.log dies
$TokenFile     = "$OmegaDir\.github_token"
$LastGitHubCheck = [DateTime]::MinValue
$LastKnownGitHubHead = $null

function Write-WD {
    param([string]$msg)
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $line = "$ts [WATCHDOG] $msg"
    Write-Host $line
    Add-Content -Path $WatchdogLog -Value $line -ErrorAction SilentlyContinue
}

function Get-GitHubHead {
    try {
        $token = if (Test-Path $TokenFile) { (Get-Content $TokenFile -Raw).Trim() } else { $null }
        if (-not $token) { return $null }
        $hdr = @{ Authorization="token $token"; "User-Agent"="OmegaWatchdog" }
        $resp = Invoke-RestMethod -Uri "https://api.github.com/repos/Trendiisales/Omega/commits/main" `
                    -Headers $hdr -TimeoutSec 10 -ErrorAction Stop
        return $resp.sha.Substring(0,7)
    } catch { return $null }
}

function Get-RunningHash {
    try {
        $tail = Get-Content $LogFile -Tail 300 -ErrorAction SilentlyContinue
        $line = $tail | Select-String 'Git hash:' | Select-Object -Last 1
        if ($line -match 'Git hash:\s+([a-f0-9]{7})') { return $Matches[1] }
    } catch {}
    return $null
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

    # 4. Heartbeat + GitHub HEAD auto-update check every 5 minutes
    $nowMin = [int](Get-Date).Minute
    if ($nowMin % 5 -eq 0 -and [int](Get-Date).Second -lt $CheckIntervalSec) {
        $l2path  = Get-L2CsvPath
        $l2size  = if (Test-Path $l2path) { [math]::Round((Get-Item $l2path).Length/1MB,2) } else { "MISSING" }
        $running = Get-RunningHash
        Write-WD "HEARTBEAT log_age=${logAge}s L2=$l2size MB hash=$running restarts=$restartCount l2alerts=$l2AlertCount"

        # GitHub HEAD poll -- auto-restart if running hash differs from HEAD
        $secsSinceGhCheck = ([DateTime]::Now - $LastGitHubCheck).TotalSeconds
        if ($secsSinceGhCheck -ge $GitHubPollIntervalSec) {
            $LastGitHubCheck = [DateTime]::Now
            $ghHead = Get-GitHubHead
            if ($ghHead -and $running -and $ghHead -ne $running) {
                Write-WD "AUTO-UPDATE: running=$running github=$ghHead -- triggering QUICK_RESTART"
                # Only auto-update if no open positions (check via api/state)
                $hasOpen = $false
                try {
                    $state = Invoke-RestMethod -Uri "http://localhost:7779/api/state" -TimeoutSec 5
                    $hasOpen = ($state.open_positions -gt 0)
                } catch {}
                if ($hasOpen) {
                    Write-WD "AUTO-UPDATE DEFERRED: $($state.open_positions) open position(s) -- will retry next cycle"
                } else {
                    & $RestartScript -OmegaDir $OmegaDir -SkipVerify
                    $restartCount++
                    Write-WD "AUTO-UPDATE complete. New hash: $ghHead restarts=$restartCount"
                    Start-Sleep -Seconds $PostRestartWaitSec
                }
            } elseif ($ghHead) {
                Write-WD "HASH-OK: running=$running == github=$ghHead"
            }
        }
    }
}

