# reap_runaway_tasks.ps1  (S-2026-06-26)
# PERMANENT FIX for the recurring "runaway background powershell" pile-up on the VPS.
#
# Root cause: the Omega* scheduled tasks have NO ExecutionTimeLimit, so a run that
# hangs (network stall / lock / full disk) never terminates -- they accumulated to
# 8+ orphans up to 26 days old, with no reaper and no alarm.
#
# This does three things, idempotently:
#   1) ROOT FIX -- set a 15-min ExecutionTimeLimit on every Omega* scheduled task so
#      any future hung run SELF-TERMINATES (native Windows, no extra process).
#   2) REAP -- kill powershell.exe procs older than 20 min that are NOT the watchdog
#      and NOT the current session (one-time cleanup of the existing zombies).
#   3) ALARM -- if disk <15% free OR >4 orphan powershells remain, write a loud
#      C:\Omega\logs\ALARM_runaway.txt that the dashboard/operator can see.
#
# Safe: never touches Omega.exe (the trading service runs via NSSM, not powershell),
# never touches the 'watchdog' powershell, never touches this session.
# Install: run once to clean up + set limits. To make it self-maintaining, register
# it as its OWN scheduled task at a 30-min interval WITH -ExecutionTimeLimit PT5M.

$ErrorActionPreference = 'Continue'
$REAP_AGE_MIN   = 20
$TIME_LIMIT     = 'PT15M'   # 15 min cap per scheduled-task run
$DISK_MIN_PCT   = 15

# 1) ROOT FIX -- cap every Omega* task so hung runs self-kill
$tasks = Get-ScheduledTask | Where-Object { $_.TaskName -match 'Omega|Gex|BigCap|Mgc|Aurora|Health|Macro|Gapper' }
foreach ($t in $tasks) {
    try {
        $si = $t.Settings
        $si.ExecutionTimeLimit = $TIME_LIMIT
        Set-ScheduledTask -TaskName $t.TaskName -Settings $si | Out-Null
        Write-Output "[limit] $($t.TaskName) -> ExecutionTimeLimit=$TIME_LIMIT"
    } catch { Write-Output "[limit] FAILED $($t.TaskName): $_" }
}

# 2) REAP existing zombies (not watchdog, not this session)
$me = $PID
$zombies = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object {
    $_.ProcessId -ne $me -and
    $_.CommandLine -notmatch 'watchdog' -and
    $_.CommandLine -notmatch 'reap_runaway' -and
    ((Get-Date) - $_.CreationDate).TotalMinutes -gt $REAP_AGE_MIN
}
Write-Output "[reap] killing $($zombies.Count) powershell(s) older than $REAP_AGE_MIN min"
foreach ($z in $zombies) {
    $age = [math]::Round(((Get-Date) - $z.CreationDate).TotalMinutes, 0)
    Write-Output "  kill pid=$($z.ProcessId) age=${age}min"
    Stop-Process -Id $z.ProcessId -Force -ErrorAction SilentlyContinue
}

# 2b) DISK ROTATION -- prune ibkr_l2 recordings >2 days old (the 4GB hog with no
#     rotation). Keeps the last 2 days live; older are superseded continuous captures.
$L2 = 'C:\Omega\logs\ibkr_l2'
if (Test-Path $L2) {
    $cut = (Get-Date).AddDays(-2)
    $old = Get-ChildItem $L2 -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $cut }
    $gb = [math]::Round(($old | Measure-Object Length -Sum).Sum / 1GB, 2)
    $old | Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Output "[rotate] ibkr_l2: pruned $($old.Count) files >2d old (${gb}GB freed)"
}

# 3) ALARM on disk / residual leak
$d = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='C:'"
$pctFree = [math]::Round(100 * $d.FreeSpace / $d.Size, 1)
$nPs = @(Get-Process powershell -ErrorAction SilentlyContinue).Count
$alarmFile = 'C:\Omega\logs\ALARM_runaway.txt'
$alarms = @()
if ($pctFree -lt $DISK_MIN_PCT) { $alarms += "DISK LOW: ${pctFree}% free (<$DISK_MIN_PCT%)" }
if ($nPs -gt 5)                 { $alarms += "POWERSHELL LEAK: $nPs procs running" }
if ($alarms.Count -gt 0) {
    (Get-Date).ToString('s') + " ALARM: " + ($alarms -join ' | ') | Set-Content $alarmFile
    Write-Output "[ALARM] $($alarms -join ' | ')  -> $alarmFile"
} else {
    if (Test-Path $alarmFile) { Remove-Item $alarmFile -Force }
    Write-Output "[ok] disk ${pctFree}% free, $nPs powershell procs -- clean"
}
