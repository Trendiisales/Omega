# register_capitulation_task.ps1 — daily CapitulationDaily run on the Omega VPS.
# Swing engine: each weekday morning reprotect (re-trail stops on open longs) + scan
# new entries, then monitor kill switch; GTC orders persist overnight server-side.
# Launch 14:00 London (idles to 09:45/10:00 ET passes). Same gotchas as gap-short task.
# Account gate: IB **paper** gateway 4002. Add --orders for the live paper rehearsal.

$ErrorActionPreference = "Stop"
$Exe   = "C:\Omega\build\CapitulationDaily.exe"
$Args  = ""                       # "--orders" for paper-submit rehearsal
$Log   = "C:\Omega\logs\capitulation_daily.log"
$Task  = "OmegaCapitulationDaily"
$User  = "$env:COMPUTERNAME\trader"

if (-not (Test-Path $Exe)) { Write-Error "missing $Exe — build target CapitulationDaily first"; exit 1 }

$runner = @"
`$ts = Get-Date -Format yyyy-MM-ddTHH:mm:ss
Add-Content '$Log' "[`$ts] CapitulationDaily start $Args"
& '$Exe' $Args *>> '$Log'
`$ts2 = Get-Date -Format yyyy-MM-ddTHH:mm:ss
Add-Content '$Log' "[`$ts2] CapitulationDaily exit `$LASTEXITCODE"
"@
$runnerPath = "C:\Omega\ibkr\run_capitulation_daily.ps1"
Set-Content -Path $runnerPath -Value $runner -Encoding UTF8

$action  = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$runnerPath`""
$trigger = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday,Tuesday,Wednesday,Thursday,Friday -At 14:00
$set     = New-ScheduledTaskSettingsSet -StartWhenAvailable -DontStopOnIdleEnd -ExecutionTimeLimit (New-TimeSpan -Hours 6)

$task = New-ScheduledTask -Action $action -Trigger $trigger -Settings $set
try {
    Register-ScheduledTask -TaskName $Task -InputObject $task -User $User -RunLevel Limited -Force | Out-Null
    Write-Host "registered $Task (user=$User) launch 14:00 London weekdays -> $Exe $Args"
} catch {
    Write-Error "register failed, existing task untouched: $_"; exit 1
}
Get-ScheduledTask -TaskName $Task | Select-Object TaskName, State
