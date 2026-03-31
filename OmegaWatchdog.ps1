#Requires -Version 5.1
<#
.SYNOPSIS
    Omega process watchdog -- monitors Omega.exe and auto-restarts on crash.

.DESCRIPTION
    Runs as a background service on the VPS. Checks Omega.exe every 15 seconds.
    On crash: waits 10s (allow broker to clean up open orders), then restarts.
    Logs all events to logs/watchdog.log with timestamps.

    CRITICAL SAFETY:
    - On restart, waits for connection_warmup_sec (default 30s) before Omega
      will accept new entries -- this is enforced inside Omega itself.
    - If Omega crashes 3 times within 10 minutes, watchdog pauses 5 minutes
      before the next restart attempt (prevents crash-restart loops).
    - Sends a desktop notification on crash + restart (Windows toast).

.USAGE
    # Run in background (starts minimised, persists after terminal close):
    Start-Process powershell -ArgumentList "-ExecutionPolicy Bypass -File C:\Omega\OmegaWatchdog.ps1" -WindowStyle Minimized

    # Run in foreground (for testing):
    .\OmegaWatchdog.ps1

    # Stop watchdog:
    Get-Process powershell | Where-Object { $_.MainWindowTitle -like "*OmegaWatchdog*" } | Stop-Process
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

# ?? Configuration ??????????????????????????????????????????????????????????????
$OmegaDir     = "C:\Omega"
$OmegaExe     = "C:\Omega\Omega.exe"
$LogFile      = "C:\Omega\logs\watchdog.log"
$CheckIntervalSec  = 15     # How often to check if Omega is running
$RestartDelaySec   = 10     # Wait after crash before restart (broker cleanup)
$MaxCrashesIn10Min = 3      # Crash loop threshold
$CrashLoopPauseSec = 300    # Pause if crash loop detected (5 minutes)
$MaxRestarts       = 0      # 0 = unlimited restarts

# ?? Logging ????????????????????????????????????????????????????????????????????
$null = New-Item -ItemType Directory -Force -Path (Split-Path $LogFile)

function Write-WatchdogLog {
    param([string]$Message, [string]$Level = "INFO")
    $ts  = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")
    $line = "[$ts] [$Level] $Message"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

# ?? Toast notification (Windows 10/11) ????????????????????????????????????????
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
    } catch {
        # Toast not available on all Windows versions -- silent fail
    }
}

# ?? Crash loop detector ????????????????????????????????????????????????????????
$crashTimes = [System.Collections.Generic.Queue[DateTime]]::new()

function Test-CrashLoop {
    $cutoff = (Get-Date).AddMinutes(-10)
    # Remove crashes older than 10 min
    while ($crashTimes.Count -gt 0 -and $crashTimes.Peek() -lt $cutoff) {
        $null = $crashTimes.Dequeue()
    }
    return $crashTimes.Count -ge $MaxCrashesIn10Min
}

function Record-Crash {
    $crashTimes.Enqueue((Get-Date))
}

# ?? Start Omega ????????????????????????????????????????????????????????????????
function Start-Omega {
    if (-not (Test-Path $OmegaExe)) {
        Write-WatchdogLog "Omega.exe not found at $OmegaExe -- cannot start" "ERROR"
        return $null
    }
    Write-WatchdogLog "Starting Omega.exe..."
    $proc = Start-Process -FilePath $OmegaExe `
                          -WorkingDirectory $OmegaDir `
                          -PassThru `
                          -WindowStyle Normal
    Write-WatchdogLog "Omega.exe started -- PID $($proc.Id)"
    return $proc
}

# ?? Main watchdog loop ?????????????????????????????????????????????????????????
Write-WatchdogLog "=== Omega Watchdog started ===" "INFO"
Write-WatchdogLog "Monitoring: $OmegaExe  check_interval=${CheckIntervalSec}s"
Write-WatchdogLog "Crash loop threshold: $MaxCrashesIn10Min crashes/10min ? ${CrashLoopPauseSec}s pause"

$restartCount = 0
$omegaProc    = $null

# Check if Omega is already running
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
    if ($null -ne $omegaProc) {
        try {
            $check = Get-Process -Id $omegaProc.Id -ErrorAction Stop
            $isRunning = (-not $check.HasExited)
        } catch {
            $isRunning = $false
        }
    }

    if ($isRunning) {
        # All good -- log heartbeat every 5 minutes
        if ((Get-Date).Minute % 5 -eq 0 -and (Get-Date).Second -lt $CheckIntervalSec) {
            Write-WatchdogLog "Omega.exe alive -- PID $($omegaProc.Id)  uptime=$(
                [int]((Get-Date) - $omegaProc.StartTime).TotalMinutes)min" "DEBUG"
        }
        continue
    }

    # ?? Omega is not running ??????????????????????????????????????????????????
    $exitCode = if ($null -ne $omegaProc -and $omegaProc.HasExited) { $omegaProc.ExitCode } else { "unknown" }
    Write-WatchdogLog "Omega.exe exited (exit_code=$exitCode) -- crash detected" "WARN"
    Send-Notification "Omega Crashed" "Exit code: $exitCode. Restarting in ${RestartDelaySec}s..."
    Record-Crash

    # Check for crash loop
    if (Test-CrashLoop) {
        Write-WatchdogLog "CRASH LOOP DETECTED ($MaxCrashesIn10Min crashes in 10min) -- pausing ${CrashLoopPauseSec}s before restart" "ERROR"
        Send-Notification "Omega Crash Loop" "$MaxCrashesIn10Min crashes in 10min. Pausing ${CrashLoopPauseSec}s."
        Start-Sleep -Seconds $CrashLoopPauseSec
    } else {
        # Normal restart delay -- allows broker to clean up any partial orders
        Write-WatchdogLog "Waiting ${RestartDelaySec}s for broker cleanup..."
        Start-Sleep -Seconds $RestartDelaySec
    }

    # Check restart limit
    if ($MaxRestarts -gt 0 -and $restartCount -ge $MaxRestarts) {
        Write-WatchdogLog "Max restarts ($MaxRestarts) reached -- watchdog stopping" "ERROR"
        break
    }

    # Restart
    $restartCount++
    Write-WatchdogLog "Restarting Omega.exe (attempt #$restartCount)..."
    $omegaProc = Start-Omega

    if ($null -ne $omegaProc) {
        Send-Notification "Omega Restarted" "PID $($omegaProc.Id). Omega will block entries for warmup period."
        Write-WatchdogLog "Omega.exe restart #$restartCount successful -- PID $($omegaProc.Id)"
        Write-WatchdogLog "NOTE: Omega enforces connection_warmup_sec before accepting new entries"
    } else {
        Write-WatchdogLog "Restart #$restartCount FAILED -- $OmegaExe not found or failed to launch" "ERROR"
        Start-Sleep -Seconds 30
    }
}

Write-WatchdogLog "=== Omega Watchdog stopped ==="
