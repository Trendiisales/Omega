#Requires -Version 5.1
<#
.SYNOPSIS
    Omega process watchdog -- monitors Omega.exe and auto-restarts on crash.

.DESCRIPTION
    Runs as a background service on the VPS. Checks Omega.exe every 15 seconds.
    On crash: waits 10s (allow broker to clean up open orders), then restarts.
    Logs all events to logs/watchdog.log with timestamps.

    STALE BINARY PREVENTION:
    Every restart verifies C:\Omega\Omega.exe against omega_build.stamp before
    launching. If stamp is missing, or the exe SHA256 does not match the stamp,
    the watchdog ABORTS the restart and sends an alert instead of silently
    running stale code. This was the root cause of the 7-week stale binary --
    the watchdog would restart build\Release\Omega.exe (old path) regardless
    of what was deployed to C:\Omega\Omega.exe.

    ROOT CAUSE FIX (commits e0a4253 + this script):
    - Watchdog now ONLY ever starts C:\Omega\Omega.exe (canonical path).
    - Every restart reads omega_build.stamp and verifies exe SHA256 matches.
    - If stamp is absent or mismatched: abort + alert, do NOT restart.
    - Heartbeat log line includes git hash from stamp for visibility.

    OTHER SAFETY:
    - On restart, waits for connection_warmup_sec before Omega accepts entries.
    - If Omega crashes 3 times within 10 minutes, pause 5 minutes (crash loop).
    - Desktop notification on crash + restart.
    - Auto-disable detection: if Omega exits cleanly (code 0 = auto_disable_after_trades
      limit hit), watchdog does NOT restart -- it logs and waits for manual restart.

.USAGE
    # Background (persists after terminal close):
    Start-Process powershell -ArgumentList "-ExecutionPolicy Bypass -File C:\Omega\OmegaWatchdog.ps1" -WindowStyle Minimized

    # Foreground (testing):
    .\OmegaWatchdog.ps1

    # Stop watchdog:
    Get-Process powershell | Where-Object { $_.MainWindowTitle -like "*OmegaWatchdog*" } | Stop-Process
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

# ==============================================================================
# Configuration
# ==============================================================================
$OmegaDir           = "C:\Omega"
$OmegaExe           = "C:\Omega\Omega.exe"       # CANONICAL -- only ever start this
$StampFile          = "C:\Omega\omega_build.stamp"
$LogFile            = "C:\Omega\logs\watchdog.log"
$CheckIntervalSec   = 15
$RestartDelaySec    = 10
$MaxCrashesIn10Min  = 3
$CrashLoopPauseSec  = 300
$MaxRestarts        = 0     # 0 = unlimited

$null = New-Item -ItemType Directory -Force -Path (Split-Path $LogFile)

# ==============================================================================
# Logging
# ==============================================================================
function Write-WatchdogLog {
    param([string]$Message, [string]$Level = "INFO")
    $ts   = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")
    $line = "[$ts] [$Level] $Message"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

# ==============================================================================
# Toast notification
# ==============================================================================
function Send-Notification {
    param([string]$Title, [string]$Body)
    try {
        [Windows.UI.Notifications.ToastNotificationManager,
         Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
        $template = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent(
            [Windows.UI.Notifications.ToastTemplateType]::ToastText02)
        $template.GetElementsByTagName("text")[0].AppendChild(
            $template.CreateTextNode($Title)) | Out-Null
        $template.GetElementsByTagName("text")[1].AppendChild(
            $template.CreateTextNode($Body)) | Out-Null
        $toast = [Windows.UI.Notifications.ToastNotification]::new($template)
        [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier(
            "Omega Watchdog").Show($toast)
    } catch { }
}

# ==============================================================================
# Stamp verification
#   Returns $true if C:\Omega\Omega.exe SHA256 matches omega_build.stamp.
#   Returns $false (and logs reason) if check fails.
#   IMPORTANT: a failed check means DO NOT RESTART -- alert instead.
# ==============================================================================
function Test-BinaryStamp {
    if (-not (Test-Path $OmegaExe)) {
        Write-WatchdogLog "STALE-CHECK: $OmegaExe not found -- cannot restart" "ERROR"
        return $false
    }

    if (-not (Test-Path $StampFile)) {
        Write-WatchdogLog "STALE-CHECK: $StampFile not found -- no stamp means no restart" "ERROR"
        Write-WatchdogLog "STALE-CHECK: Run DEPLOY_OMEGA.ps1 to create a valid stamp" "ERROR"
        Send-Notification "Omega Watchdog - STALE EXE" "No stamp file. Manual restart required. Run DEPLOY_OMEGA.ps1"
        return $false
    }

    $stampLines  = Get-Content $StampFile -ErrorAction SilentlyContinue
    $stampHash   = (($stampLines | Where-Object { $_ -match '^EXE_SHA256=' })      -replace '^EXE_SHA256=',     '').Trim()
    $stampGit    = (($stampLines | Where-Object { $_ -match '^GIT_HASH=' })        -replace '^GIT_HASH=',       '').Trim()
    $stampShort  = (($stampLines | Where-Object { $_ -match '^GIT_HASH_SHORT=' })  -replace '^GIT_HASH_SHORT=', '').Trim()
    $stampTime   = (($stampLines | Where-Object { $_ -match '^BUILD_TIME=' })      -replace '^BUILD_TIME=',     '').Trim()
    $currentHash = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash.Trim()
    $displayHash = if ($stampShort) { $stampShort } else { $stampGit.Substring(0, [Math]::Min(7,$stampGit.Length)) }

    if ([string]::IsNullOrWhiteSpace($stampHash)) {
        Write-WatchdogLog "STALE-CHECK: Stamp EXE_SHA256 field empty -- corrupt stamp" "ERROR"
        Send-Notification "Omega Watchdog - CORRUPT STAMP" "Stamp corrupt. Run DEPLOY_OMEGA.ps1"
        return $false
    }

    if ([string]::IsNullOrWhiteSpace($stampGit)) {
        Write-WatchdogLog "STALE-CHECK: Stamp GIT_HASH field empty -- corrupt stamp" "ERROR"
        Send-Notification "Omega Watchdog - CORRUPT STAMP" "Stamp GIT_HASH missing. Run DEPLOY_OMEGA.ps1"
        return $false
    }

    if ($stampHash -ne $currentHash) {
        Write-WatchdogLog "STALE-CHECK: EXE HASH MISMATCH -- RESTART BLOCKED" "ERROR"
        Write-WatchdogLog "STALE-CHECK: stamp_sha256=$($stampHash.Substring(0,16))...  exe_sha256=$($currentHash.Substring(0,16))..." "ERROR"
        Write-WatchdogLog "STALE-CHECK: source_commit=$displayHash  built=$stampTime" "ERROR"
        Write-WatchdogLog "STALE-CHECK: Omega.exe was replaced after last deploy. Run DEPLOY_OMEGA.ps1." "ERROR"
        Send-Notification "Omega Watchdog - STALE EXE BLOCKED" "Exe hash mismatch. source=$displayHash. Run DEPLOY_OMEGA.ps1."
        return $false
    }

    Write-WatchdogLog "STALE-CHECK: OK  source=$displayHash  sha256=$($currentHash.Substring(0,16))...  built=$stampTime" "INFO"
    return $true
}

# ==============================================================================
# Crash loop detector
# ==============================================================================
$crashTimes = [System.Collections.Generic.Queue[DateTime]]::new()

function Test-CrashLoop {
    $cutoff = (Get-Date).AddMinutes(-10)
    while ($crashTimes.Count -gt 0 -and $crashTimes.Peek() -lt $cutoff) {
        $null = $crashTimes.Dequeue()
    }
    return $crashTimes.Count -ge $MaxCrashesIn10Min
}

function Record-Crash { $crashTimes.Enqueue((Get-Date)) }

# ==============================================================================
# Start Omega -- always from canonical path and working directory
# ==============================================================================
function Start-Omega {
    # Verify stamp before every start
    if (-not (Test-BinaryStamp)) {
        Write-WatchdogLog "Start-Omega: stamp check failed -- NOT starting Omega" "ERROR"
        return $null
    }

    if (-not (Test-Path $OmegaExe)) {
        Write-WatchdogLog "Start-Omega: $OmegaExe not found -- cannot start" "ERROR"
        return $null
    }

    Write-WatchdogLog "Starting $OmegaExe (WorkingDir=$OmegaDir)..."
    $proc = Start-Process -FilePath $OmegaExe `
                          -WorkingDirectory $OmegaDir `
                          -ArgumentList "omega_config.ini" `
                          -PassThru `
                          -WindowStyle Normal
    Write-WatchdogLog "Omega.exe started -- PID $($proc.Id)"
    return $proc
}

# ==============================================================================
# Main watchdog loop
# ==============================================================================
Write-WatchdogLog "=== Omega Watchdog started ===" "INFO"
Write-WatchdogLog "Monitoring: $OmegaExe  check_interval=${CheckIntervalSec}s  stamp=$StampFile"
Write-WatchdogLog "Crash loop threshold: $MaxCrashesIn10Min crashes/10min -> ${CrashLoopPauseSec}s pause"

# Initial stamp check on watchdog start
if (-not (Test-BinaryStamp)) {
    Write-WatchdogLog "WARNING: Stamp check failed at watchdog startup. Will not auto-restart until stamp is valid." "WARN"
}

$restartCount = 0
$omegaProc    = $null

# Attach to already-running Omega if present
$existing = Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($existing) {
    Write-WatchdogLog "Omega already running -- attaching to PID $($existing.Id)"
    $omegaProc = $existing
} else {
    $omegaProc = Start-Omega
}

while ($true) {
    Start-Sleep -Seconds $CheckIntervalSec

    # Check if process is still alive
    $isRunning = $false
    $exitCode  = $null
    if ($null -ne $omegaProc) {
        try {
            $check    = Get-Process -Id $omegaProc.Id -ErrorAction Stop
            $isRunning = (-not $check.HasExited)
        } catch {
            $isRunning = $false
        }
        if (-not $isRunning -and $omegaProc.HasExited) {
            $exitCode = $omegaProc.ExitCode
        }
    }

    if ($isRunning) {
        # Heartbeat every 5 minutes -- include git hash so log shows what is running
        if ((Get-Date).Minute % 5 -eq 0 -and (Get-Date).Second -lt $CheckIntervalSec) {
            $stampGit = "unknown"
            if (Test-Path $StampFile) {
                $sl = Get-Content $StampFile -ErrorAction SilentlyContinue
                $stampGit = (($sl | Where-Object { $_ -match '^GIT_HASH=' }) -replace '^GIT_HASH=', '').Trim()
            }
            $upMin = [int]((Get-Date) - $omegaProc.StartTime).TotalMinutes
            Write-WatchdogLog "HEARTBEAT: PID=$($omegaProc.Id)  uptime=${upMin}min  git=$($stampGit.Substring(0, [Math]::Min(7,$stampGit.Length)))" "DEBUG"
        }
        continue
    }

    # -------------------------------------------------------------------------
    # Omega is not running
    # -------------------------------------------------------------------------
    Write-WatchdogLog "Omega.exe exited (exit_code=$exitCode)" "WARN"

    # Exit code 0 = clean shutdown. Check if this was a deploy stop (sentinel file)
    # or a real clean exit (auto_disable_after_trades limit hit).
    $sentinelFile = "C:\Omega\deploy_in_progress.flag"
    $isDeployStop = (Test-Path $sentinelFile)

    if ($exitCode -eq 0 -and -not $isDeployStop) {
        Write-WatchdogLog "Clean exit (code=0) -- NOT auto-restarting. This may be auto_disable_after_trades limit." "WARN"
        Write-WatchdogLog "Check logs and restart manually with START_OMEGA.ps1 when ready." "WARN"
        Send-Notification "Omega Stopped (Clean Exit)" "Exit code 0 -- possible trade limit. Manual restart required."
        # Keep watchdog alive but idle -- re-attach if Omega started externally
        $omegaProc = $null
        Start-Sleep -Seconds 60
        $reattach = Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($reattach) {
            Write-WatchdogLog "Re-attaching to externally started Omega PID $($reattach.Id)"
            $omegaProc = $reattach
        }
        continue
    }

    if ($isDeployStop) {
        Write-WatchdogLog "Deploy sentinel detected -- DEPLOY_OMEGA.ps1 stopped Omega. Waiting for new process..." "INFO"
        # Wait up to 5 minutes for deploy to finish and Omega to restart
        $deadline = (Get-Date).AddMinutes(5)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 5
            $reattach = Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($reattach) {
                Write-WatchdogLog "Deploy complete -- re-attaching to Omega PID $($reattach.Id)" "INFO"
                $omegaProc = $reattach
                break
            }
        }
        if ($null -eq $omegaProc -or -not (Get-Process -Name "Omega" -ErrorAction SilentlyContinue)) {
            Write-WatchdogLog "Deploy did not restart Omega within 5min -- build may have failed. Check DEPLOY_OMEGA.ps1 output." "ERROR"
            Send-Notification "Omega Deploy Failed" "Omega did not restart after deploy. Check build output."
        }
        continue
    }

    # Crash detected
    Send-Notification "Omega Crashed" "Exit code: $exitCode. Checking stamp before restart..."
    Record-Crash

    # Stamp check BEFORE restart -- this is the stale binary guard
    if (-not (Test-BinaryStamp)) {
        Write-WatchdogLog "RESTART BLOCKED: stamp check failed after crash. Fix with DEPLOY_OMEGA.ps1." "ERROR"
        # Stay in idle loop -- keep checking every interval but don't restart
        $omegaProc = $null
        while ($true) {
            Start-Sleep -Seconds $CheckIntervalSec
            # Check if stamp was fixed and Omega started externally
            $ext = Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($ext) {
                Write-WatchdogLog "Omega started externally -- re-attaching to PID $($ext.Id)"
                $omegaProc = $ext
                break
            }
            # Re-check stamp every minute
            if ((Get-Date).Second -lt $CheckIntervalSec -and (Get-Date).Minute % 1 -eq 0) {
                if (Test-BinaryStamp) {
                    Write-WatchdogLog "Stamp now valid -- resuming normal watchdog operation" "INFO"
                    break
                }
            }
        }
        continue
    }

    # Check crash loop
    if (Test-CrashLoop) {
        Write-WatchdogLog "CRASH LOOP DETECTED ($MaxCrashesIn10Min crashes/10min) -- pausing ${CrashLoopPauseSec}s" "ERROR"
        Send-Notification "Omega Crash Loop" "$MaxCrashesIn10Min crashes in 10min. Pausing ${CrashLoopPauseSec}s."
        Start-Sleep -Seconds $CrashLoopPauseSec
    } else {
        Write-WatchdogLog "Waiting ${RestartDelaySec}s for broker cleanup..."
        Start-Sleep -Seconds $RestartDelaySec
    }

    # Restart limit check
    if ($MaxRestarts -gt 0 -and $restartCount -ge $MaxRestarts) {
        Write-WatchdogLog "Max restarts ($MaxRestarts) reached -- watchdog stopping" "ERROR"
        break
    }

    # Restart
    $restartCount++
    Write-WatchdogLog "Restarting Omega.exe (attempt #$restartCount)..."
    $omegaProc = Start-Omega

    if ($null -ne $omegaProc) {
        Send-Notification "Omega Restarted" "PID $($omegaProc.Id). Warmup period active."
        Write-WatchdogLog "Restart #$restartCount OK -- PID $($omegaProc.Id). Omega enforces connection_warmup_sec before new entries."
    } else {
        Write-WatchdogLog "Restart #$restartCount FAILED -- stamp check blocked or exe missing" "ERROR"
        Start-Sleep -Seconds 60
    }
}

Write-WatchdogLog "=== Omega Watchdog stopped ==="
