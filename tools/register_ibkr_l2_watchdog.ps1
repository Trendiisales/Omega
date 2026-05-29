# =============================================================================
# register_ibkr_l2_watchdog.ps1 -- (re-)create the OmegaIbkrL2Freshness task.
#
# Registers a scheduled task that runs ibkr_l2_freshness_check.ps1 every 2
# minutes. On stale / empty / missing per-symbol CSVs the watchdog kills the
# bridge process and restarts the OmegaIbkrBridge task. Survives reboots
# (uses AtStartup trigger + S4U principal so no interactive logon needed).
#
# Run as ADMINISTRATOR:
#   powershell -ExecutionPolicy Bypass -File `
#     C:\Omega\tools\register_ibkr_l2_watchdog.ps1
#
# Companion to:
#   tools\ibkr_dom_bridge.py                    -- the bridge itself
#   tools\register_omega_ibkr_bridge.ps1        -- registers the bridge task
#   tools\ibkr_l2_freshness_check.ps1           -- this watchdog's script
#   tools\ibkr_dom_bridge_requirements.txt      -- pinned bridge venv deps
# =============================================================================

$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaIbkrL2Freshness'
$Script   = 'C:\Omega\tools\ibkr_l2_freshness_check.ps1'

if (-not (Test-Path $Script)) {
    Write-Error "watchdog script not at $Script"
    exit 1
}

# Run as the currently-logged-in user account with stored credentials (S4U).
# RunLevel Highest is needed so the watchdog can Stop-Process / Start-
# ScheduledTask other tasks.
$User      = "$env:USERDOMAIN\$env:USERNAME"
$principal = New-ScheduledTaskPrincipal -UserId $User `
                                        -LogonType S4U `
                                        -RunLevel Highest

# powershell.exe runs the script with -ExecutionPolicy Bypass so signed-script
# requirements don't kill the watchdog on first install.
$arg    = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$Script`""  # hidden window -- else a PS console pops every 2min task tick
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
                                  -Argument $arg `
                                  -WorkingDirectory 'C:\Omega'

# Triggers:
#   1. AtStartup: fires once on boot so the watchdog comes back if VPS
#      reboots without anyone RDP-ing in.
#   2. Every-2-min repeat (anchored to now). Repetition interval >= 1 min
#      per Task Scheduler limits; 2 min is the tightest sensible cadence
#      since the bridge needs ~10s to settle after a restart.
$startup = New-ScheduledTaskTrigger -AtStartup
$repeat  = New-ScheduledTaskTrigger -Once -At (Get-Date) `
             -RepetitionInterval (New-TimeSpan -Minutes 2) `
             -RepetitionDuration (New-TimeSpan -Days 3650)

# MultipleInstances IgnoreNew: if a previous watchdog run hasn't returned
# yet (rare; the script exits in < 15s normally) the next 2-min tick drops
# silently rather than stacking up.
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 5) `
    -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

if (Get-ScheduledTask -TaskName $TaskName -EA SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed existing '$TaskName'"
}

Register-ScheduledTask -TaskName $TaskName `
                       -Action $action `
                       -Trigger @($startup, $repeat) `
                       -Settings $settings `
                       -Principal $principal `
                       -Description ("Watchdog: every 2 min, checks IBKR L2 " +
                                     "CSV freshness; restarts OmegaIbkrBridge " +
                                     "on stale / empty / missing files.") `
    | Out-Null

Write-Host "Registered '$TaskName' OK."
Write-Host ""
Write-Host "Fire now to verify:   Start-ScheduledTask -TaskName '$TaskName'"
Write-Host "View result:          Get-ScheduledTaskInfo -TaskName '$TaskName' |"
Write-Host "                          Select LastRunTime, LastTaskResult, NextRunTime"
Write-Host "Tail alert log:       Get-Content C:\Omega\logs\ibkr_l2_alerts.log -Tail 20"
