# ============================================================================
# register_tasks.ps1 - schedule the bracket strategies via Windows Task Scheduler
#
# MUST be run in an ADMINISTRATOR PowerShell:
#   powershell -ExecutionPolicy Bypass -File deploy\register_tasks.ps1
#
# Strategy trigger times are in UTC. Set the VPS timezone to UTC (recommended)
# so Task Scheduler local time == UTC:  Set-TimeZone -Id "UTC"
# ============================================================================
$ErrorActionPreference = 'Stop'

$BotDir = Split-Path -Parent $PSScriptRoot
$Py = Join-Path $BotDir ".venv\Scripts\python.exe"
$Wrapper = Join-Path $BotDir "scripts\run_with_heartbeat.ps1"
$PSExe = (Get-Command powershell.exe).Source

if (-not (Test-Path $Py)) {
    Write-Error "venv python not found at $Py - run deploy\windows_setup.ps1 first."
    exit 1
}
if (-not (Test-Path $Wrapper)) {
    Write-Error "heartbeat wrapper not found at $Wrapper"
    exit 1
}

# --- Timezone sanity check (strategy times are UTC) -------------------------
$tz = Get-TimeZone
Write-Host ("VPS timezone: " + $tz.Id)
if ($tz.BaseUtcOffset.TotalMinutes -ne 0) {
    Write-Warning "VPS is NOT on UTC. The -At times below assume UTC."
    Write-Warning "Either run  Set-TimeZone -Id 'UTC'  (recommended) or adjust the -At times in this script."
}

# --- Helper -----------------------------------------------------------------
# Every task now runs through scripts\run_with_heartbeat.ps1, which captures
# stdout/stderr, writes heartbeat records, and posts a webhook on non-zero
# exit. The wrapper receives: -TaskName <name> -Script <path> -ScriptArgs <...>
function New-OmegaTask {
    param([string]$Name, [string]$Script, [string]$ScriptArgs, $Trigger)
    $wrapperArgs = '-ExecutionPolicy Bypass -NoProfile -File "' + $Wrapper + '" ' +
                   '-TaskName "' + $Name + '" ' +
                   '-Script "' + $Script + '" ' +
                   '-ScriptArgs "' + $ScriptArgs + '"'
    $action   = New-ScheduledTaskAction -Execute $PSExe -Argument $wrapperArgs -WorkingDirectory $BotDir
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Hours 3)
    if (Get-ScheduledTask -TaskName $Name -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $Name -Confirm:$false
    }
    Register-ScheduledTask -TaskName $Name -Action $action -Trigger $Trigger `
        -Settings $settings -RunLevel Highest -Description "Omega gold-bracket strategy" | Out-Null
    Write-Host ("Registered: " + $Name)
}

$Weekdays = 'Monday','Tuesday','Wednesday','Thursday','Friday'

# --- Daily bracket: 13:00 UTC, Mon-Fri -------------------------------------
New-OmegaTask "Omega Daily Bracket 1300" `
    "live\daily_bracket.py" `
    "--paper --qty 1 --instrument MGC --strategy DAILY1300" `
    (New-ScheduledTaskTrigger -Weekly -DaysOfWeek $Weekdays -At "13:00")

# --- Daily bracket: 14:00 UTC, Mon-Fri -------------------------------------
New-OmegaTask "Omega Daily Bracket 1400" `
    "live\daily_bracket.py" `
    "--paper --qty 1 --instrument MGC --strategy DAILY1400" `
    (New-ScheduledTaskTrigger -Weekly -DaysOfWeek $Weekdays -At "14:00")

# --- Sunday bracket: 22:55 UTC, Sunday -------------------------------------
New-OmegaTask "Omega Sunday Bracket" `
    "live\sunday_bracket.py" `
    "--paper --qty 1 --instrument MGC" `
    (New-ScheduledTaskTrigger -Weekly -DaysOfWeek Sunday -At "22:55")

Write-Host ""
Write-Host "All tasks registered. Review with:  Get-ScheduledTask -TaskName 'Omega*'"
Write-Host "Tasks run in --paper mode. To go live, change --paper to --live above and re-run this script."
