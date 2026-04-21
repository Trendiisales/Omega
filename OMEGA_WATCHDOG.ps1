# OMEGA_WATCHDOG.ps1  -- v2.0 2026-04-21
# Hardened watchdog. Replaces previous version which had:
#   * `Get-Date - X` syntax error (parsed as param, threw at runtime) -> stale detection silently broken
#   * Position check via non-existent /api/state endpoint -> always restarted regardless of open positions
#   * Fail-unsafe catch-all -> any HTTP error -> $hasOpen = false -> restart
# This version:
#   * Fixes date arithmetic: ((Get-Date) - $other).TotalSeconds
#   * Uses /api/telemetry which exists and returns live_trades[] array
#   * Fail-SAFE: any uncertainty -> assume positions open -> defer restart
#   * Position check applied to ALL restart triggers, not just GitHub HEAD mismatch
#   * Startup self-check probes telemetry endpoint and logs shape
#
# Install as service via INSTALL_WATCHDOG.ps1. NSSM wraps:
#   powershell.exe -NonInteractive -ExecutionPolicy Bypass -File C:\Omega\OMEGA_WATCHDOG.ps1
# Logs to: C:\Omega\logs\watchdog.log (independent of Omega's latest.log).

param(
    [int]$StaleThresholdSec    = 60,
    [int]$L2StaleThresholdSec  = 120,
    [int]$CheckIntervalSec     = 15,
    [int]$PostRestartWaitSec   = 30,
    [int]$GitHubPollIntervalSec = 300,
    [string]$TelemetryUrl      = "http://localhost:7779/api/telemetry",
    [int]$TelemetryTimeoutSec  = 5
)

$WATCHDOG_VERSION = "v2.0-2026-04-21"

$ServiceName   = "Omega"
$OmegaDir      = "C:\Omega"
$RestartScript = "$OmegaDir\QUICK_RESTART.ps1"
$LogFile       = "$OmegaDir\logs\latest.log"
$WatchdogLog   = "$OmegaDir\logs\watchdog.log"
$TokenFile     = "$OmegaDir\.github_token"

$LastGitHubCheck     = [DateTime]::MinValue
$TelemetryHealthy    = $false   # set to $true after successful startup probe + stays true on success
$TelemetryLastError  = $null

function Write-WD {
    param([string]$msg)
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $line = "$ts [WATCHDOG] $msg"
    Write-Host $line
    Add-Content -Path $WatchdogLog -Value $line -ErrorAction SilentlyContinue
}

# ----------------------------------------------------------------------------
# Get-OpenPositionCount
# ----------------------------------------------------------------------------
# Returns the number of open positions, or $null if detection failed.
# $null means "unknown" and the caller MUST treat that as "assume open".
# Never returns 0 on exception -- that's the fail-unsafe bug we're fixing.
# ----------------------------------------------------------------------------
function Get-OpenPositionCount {
    try {
        $resp = Invoke-RestMethod -Uri $TelemetryUrl -TimeoutSec $TelemetryTimeoutSec -ErrorAction Stop
        if ($null -eq $resp) {
            $script:TelemetryLastError = "telemetry returned null"
            return $null
        }

        # Prefer live_trades (authoritative -- what the cockpit uses).
        # Fall back to open_positions array if live_trades absent.
        $liveTrades = $resp.live_trades
        $openPos    = $resp.open_positions

        $liveCount = 0
        $openCount = 0

        if ($null -ne $liveTrades) {
            if ($liveTrades -is [array]) { $liveCount = $liveTrades.Count }
            elseif ($liveTrades.Count) { $liveCount = [int]$liveTrades.Count }
        }
        if ($null -ne $openPos) {
            if ($openPos -is [array]) { $openCount = $openPos.Count }
            elseif ($openPos.Count) { $openCount = [int]$openPos.Count }
        }

        # Take the max of the two counts -- if either says "open", we treat as open.
        $n = [Math]::Max($liveCount, $openCount)
        $script:TelemetryLastError = $null
        return $n
    } catch {
        $script:TelemetryLastError = $_.Exception.Message
        return $null
    }
}

# ----------------------------------------------------------------------------
# Test-SafeToRestart
# ----------------------------------------------------------------------------
# Returns $true only if we can confirm zero open positions.
# Returns $false on any uncertainty (network error, malformed response, null, etc.)
# This is the CORE SAFETY GATE that every restart path must pass through.
# Exception: if $AllowWhenServiceDown is $true AND service is truly stopped,
# we return $true because no positions CAN be open from a dead binary.
# ----------------------------------------------------------------------------
function Test-SafeToRestart {
    param([switch]$AllowWhenServiceDown)

    if ($AllowWhenServiceDown) {
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -eq $svc -or $svc.Status -eq 'Stopped') {
            Write-WD "SAFE-TO-RESTART: service is stopped, no live positions possible from dead binary"
            return $true
        }
    }

    $n = Get-OpenPositionCount
    if ($null -eq $n) {
        Write-WD "SAFE-TO-RESTART: DEFERRED -- position check failed ($script:TelemetryLastError). Assuming open."
        return $false
    }
    if ($n -gt 0) {
        Write-WD "SAFE-TO-RESTART: DEFERRED -- $n open position(s)"
        return $false
    }
    Write-WD "SAFE-TO-RESTART: confirmed 0 open positions"
    return $true
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
        if (-not (Test-Path $LogFile)) { return $null }
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

# ----------------------------------------------------------------------------
# Startup
# ----------------------------------------------------------------------------
Write-WD "=== WATCHDOG STARTED === version=$WATCHDOG_VERSION"
Write-WD "Config: StaleThreshold=${StaleThresholdSec}s L2Threshold=${L2StaleThresholdSec}s Interval=${CheckIntervalSec}s"
Write-WD "Telemetry URL: $TelemetryUrl"
Write-WD "Watchdog log: $WatchdogLog (independent of latest.log)"

# Startup self-check: probe telemetry endpoint once to see if it's reachable
# and if the expected fields exist. If it fails here, we keep running but
# log a warning -- service-down detection still works, but GitHub auto-update
# will be deferred until telemetry is healthy.
$probeN = Get-OpenPositionCount
if ($null -eq $probeN) {
    Write-WD "STARTUP-WARN: telemetry probe failed ($TelemetryLastError). Auto-update deferred until endpoint healthy."
    $TelemetryHealthy = $false
} else {
    Write-WD "STARTUP-OK: telemetry probe succeeded, open_positions=$probeN"
    $TelemetryHealthy = $true
}

$restartCount = 0
$l2AlertCount = 0

# ----------------------------------------------------------------------------
# Main loop
# ----------------------------------------------------------------------------
while ($true) {
    Start-Sleep -Seconds $CheckIntervalSec

    # --- 1. Service running? --------------------------------------------------
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $svc -or $svc.Status -ne 'Running') {
        Write-WD "SERVICE-DOWN: status=$($svc.Status). Restart check..."
        if (Test-SafeToRestart -AllowWhenServiceDown) {
            Write-WD "SERVICE-DOWN: restarting (attempt #$($restartCount + 1))"
            & $RestartScript
            $restartCount++
            Write-WD "SERVICE-DOWN: restart #$restartCount complete. Waiting ${PostRestartWaitSec}s..."
            Start-Sleep -Seconds $PostRestartWaitSec
        } else {
            Write-WD "SERVICE-DOWN: restart deferred -- will retry next cycle"
        }
        continue
    }

    # --- 2. latest.log stale? -------------------------------------------------
    if (-not (Test-Path $LogFile)) {
        Write-WD "LOG-MISSING: $LogFile not found. Restart check..."
        if (Test-SafeToRestart) {
            Write-WD "LOG-MISSING: restarting (attempt #$($restartCount + 1))"
            & $RestartScript
            $restartCount++
            Start-Sleep -Seconds $PostRestartWaitSec
        } else {
            Write-WD "LOG-MISSING: restart deferred"
        }
        continue
    }

    $logAge = $null
    try {
        $logItem = Get-Item $LogFile -ErrorAction Stop
        $logAge = ((Get-Date) - $logItem.LastWriteTime).TotalSeconds
    } catch {
        Write-WD "LOG-AGE-ERR: could not read $LogFile timestamp: $_"
    }

    if ($null -ne $logAge -and $logAge -gt $StaleThresholdSec) {
        Write-WD "LOG-STALE: age=${logAge}s (max=${StaleThresholdSec}s). Restart check..."
        if (Test-SafeToRestart) {
            Write-WD "LOG-STALE: restarting (attempt #$($restartCount + 1))"
            & $RestartScript
            $restartCount++
            Write-WD "LOG-STALE: restart #$restartCount complete. Waiting ${PostRestartWaitSec}s..."
            Start-Sleep -Seconds $PostRestartWaitSec
        } else {
            Write-WD "LOG-STALE: restart deferred -- will retry next cycle"
        }
        continue
    }

    # --- 3. L2 CSV writing during market hours? -------------------------------
    if (Is-MarketHours) {
        $l2path = Get-L2CsvPath
        if (-not (Test-Path $l2path)) {
            Write-WD "L2-CSV-MISSING: $l2path not found during market hours!"
            $l2AlertCount++
        } else {
            $l2Age = $null
            $l2Size = 0
            try {
                $l2Item = Get-Item $l2path -ErrorAction Stop
                $l2Age = ((Get-Date) - $l2Item.LastWriteTime).TotalSeconds
                $l2Size = [math]::Round($l2Item.Length / 1MB, 2)
            } catch {
                Write-WD "L2-AGE-ERR: $_"
            }

            if ($null -ne $l2Age -and $l2Age -gt $L2StaleThresholdSec) {
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

    # --- 4. Heartbeat + GitHub HEAD auto-update every 5 minutes ---------------
    $nowMin = [int](Get-Date).Minute
    if ($nowMin % 5 -eq 0 -and [int](Get-Date).Second -lt $CheckIntervalSec) {
        $l2path = Get-L2CsvPath
        $l2size = "MISSING"
        if (Test-Path $l2path) {
            try { $l2size = [math]::Round((Get-Item $l2path).Length / 1MB, 2) } catch {}
        }
        $running = Get-RunningHash
        $logAgeStr = if ($null -ne $logAge) { "${logAge}s" } else { "unknown" }
        Write-WD "HEARTBEAT log_age=$logAgeStr L2=$l2size MB hash=$running restarts=$restartCount l2alerts=$l2AlertCount telemetry_healthy=$TelemetryHealthy"

        $secsSinceGhCheck = ((Get-Date) - $LastGitHubCheck).TotalSeconds
        if ($secsSinceGhCheck -ge $GitHubPollIntervalSec) {
            $LastGitHubCheck = Get-Date
            $ghHead = Get-GitHubHead

            if (-not $ghHead) {
                Write-WD "GITHUB-POLL: failed to reach GitHub API -- will retry"
            } elseif (-not $running) {
                Write-WD "GITHUB-POLL: ghHead=$ghHead but running hash unknown -- cannot compare. Will retry."
            } elseif ($ghHead -eq $running) {
                Write-WD "HASH-OK: running=$running == github=$ghHead"
            } else {
                Write-WD "AUTO-UPDATE: running=$running github=$ghHead -- restart check..."
                if (Test-SafeToRestart) {
                    Write-WD "AUTO-UPDATE: triggering QUICK_RESTART (attempt #$($restartCount + 1))"
                    & $RestartScript -OmegaDir $OmegaDir -SkipVerify
                    $restartCount++
                    Write-WD "AUTO-UPDATE: restart #$restartCount complete. Expected new hash: $ghHead"
                    Start-Sleep -Seconds $PostRestartWaitSec
                } else {
                    Write-WD "AUTO-UPDATE: deferred -- will retry next cycle"
                }
            }
        }
    }
}
