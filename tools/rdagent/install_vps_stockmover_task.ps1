# install_vps_stockmover_task.ps1 — register the VPS-native StockDayMover daily feed as a
# Windows Scheduled Task (NOT cron; operator: "dont want crons caused issues"). Idempotent:
# re-running replaces the task cleanly. Runs vps_stockmover_feed.py once per weekday after the
# US cash close (22:30 UTC ~ 18:30 ET), writing the slim C:\Omega\data\rdagent\sp500_long_close.csv
# that the in-binary companion poller consumes.
#
# One-time setup on the VPS:
#   python -m pip install --user yfinance pandas
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\rdagent\install_vps_stockmover_task.ps1
$ErrorActionPreference = "Stop"
$TaskName = "OmegaStockMoverFeed"
$Py       = "C:\Program Files\Python312\python.exe"
$Script   = "C:\Omega\tools\rdagent\vps_stockmover_feed.py"
$Log      = "C:\Omega\logs\vps_stockmover_feed.log"

if (-not (Test-Path $Py))     { $Py = (Get-Command python).Source }
if (-not (Test-Path $Script)) { throw "feed script not found: $Script" }

# wrapper: run the feed, append stdout/stderr to a log
$cmd = "cmd.exe"
$arg = "/c `"`"$Py`" `"$Script`" >> `"$Log`" 2>&1`""

$action  = New-ScheduledTaskAction -Execute $cmd -Argument $arg
# 22:35 UTC every weekday (Mon-Fri); the VPS clock is UTC.
$trigger = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday,Tuesday,Wednesday,Thursday,Friday -At 10:35PM
$set     = New-ScheduledTaskSettingsSet -StartWhenAvailable -DontStopOnIdleEnd -ExecutionTimeLimit (New-TimeSpan -Minutes 20)

Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $set `
    -Description "Omega StockDayMover BE-floor companion daily close feed (slim 39-name yfinance pull)" -RunLevel Highest | Out-Null

Write-Host "[install] registered task '$TaskName' (weekdays 22:35 UTC) -> $Script"
Write-Host "[install] test now: Start-ScheduledTask -TaskName $TaskName ; Get-Content $Log -Tail 5"
