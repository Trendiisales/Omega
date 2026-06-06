# register_gapshort_task.ps1 — daily GapShortDaily run on the Omega VPS.
# GapShortDaily self-gates ET internally (entry 09:35, cover 15:55), so ONE launch
# per weekday morning covers the whole session. Launch 13:30 London (well before
# 09:35 ET under BST) — the loop idles until the ET entry window opens.
#
# GOTCHAS baked in (from OmegaMgcLiveBars lessons):
#  - use $env:COMPUTERNAME not $env:USERDOMAIN (WORKGROUP over ssh -> SID fail 0x80070534)
#  - Unregister THEN Register guarded: a failed Register must NOT delete a working task
#  - runner is a foreground wrapper (bare Start-Process dies silent)
#
# Account gate: connects IB **paper** gateway 4002. Add  --orders  to ARGS for the
# live paper rehearsal (submits to DU paper acct). Omit = log-only dry run.

$ErrorActionPreference = "Stop"
$Exe   = "C:\Omega\build\GapShortDaily.exe"
$Args  = ""                       # set to "--orders" for the paper-submit rehearsal
$Log   = "C:\Omega\logs\gapshort_daily.log"
$Task  = "OmegaGapShortDaily"
$User  = "$env:COMPUTERNAME\trader"

if (-not (Test-Path $Exe)) { Write-Error "missing $Exe — build target GapShortDaily first"; exit 1 }

# foreground wrapper (detached Start-Process dies silently on this box)
$runner = @"
`$ts = Get-Date -Format yyyy-MM-ddTHH:mm:ss
Add-Content '$Log' "[`$ts] GapShortDaily start $Args"
& '$Exe' $Args *>> '$Log'
`$ts2 = Get-Date -Format yyyy-MM-ddTHH:mm:ss
Add-Content '$Log' "[`$ts2] GapShortDaily exit `$LASTEXITCODE"
"@
$runnerPath = "C:\Omega\ibkr\run_gapshort_daily.ps1"
Set-Content -Path $runnerPath -Value $runner -Encoding UTF8

$action  = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$runnerPath`""
# weekdays 13:30 London (idles to 09:35 ET). DST-safe: ET gate is internal.
$trigger = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday,Tuesday,Wednesday,Thursday,Friday -At 13:30
$set     = New-ScheduledTaskSettingsSet -StartWhenAvailable -DontStopOnIdleEnd -ExecutionTimeLimit (New-TimeSpan -Hours 10)

# guarded register: only unregister AFTER the new definition validates
$task = New-ScheduledTask -Action $action -Trigger $trigger -Settings $set
try {
    Register-ScheduledTask -TaskName $Task -InputObject $task -User $User -RunLevel Limited -Force | Out-Null
    Write-Host "registered $Task (user=$User) launch 13:30 London weekdays -> $Exe $Args"
} catch {
    Write-Error "register failed, existing task untouched: $_"; exit 1
}
Get-ScheduledTask -TaskName $Task | Select-Object TaskName, State
