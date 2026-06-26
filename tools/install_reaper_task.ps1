# install_reaper_task.ps1 -- register the self-maintaining OmegaReaper + re-enable L2 freshness.
$ErrorActionPreference = 'Stop'

$action  = New-ScheduledTaskAction -Execute 'powershell.exe' `
            -Argument '-NoProfile -ExecutionPolicy Bypass -File C:\Omega\tools\reap_runaway_tasks.ps1'
$trigger = New-ScheduledTaskTrigger -Once -At ((Get-Date).AddMinutes(2)) `
            -RepetitionInterval (New-TimeSpan -Minutes 30) `
            -RepetitionDuration (New-TimeSpan -Days 3650)
$set     = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5) `
            -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
Register-ScheduledTask -TaskName 'OmegaReaper' -Action $action -Trigger $trigger `
    -Settings $set -RunLevel Highest -User 'SYSTEM' -Force | Out-Null
Write-Output ("OmegaReaper registered: " + (Get-ScheduledTask OmegaReaper).State + " (30-min interval, 5-min cap)")

Enable-ScheduledTask -TaskName 'OmegaIbkrL2Freshness' | Out-Null
Write-Output ("OmegaIbkrL2Freshness: " + (Get-ScheduledTask OmegaIbkrL2Freshness).State)
