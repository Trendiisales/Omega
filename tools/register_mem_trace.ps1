# register_mem_trace.ps1 — register the RAM leak-finder (mem_trace.ps1) as a SAFE task.
# Run ONCE on the VPS (after a reboot when responsive). Idempotent (-Force).
# Every 10min appends top-8 RSS + free MB to C:\Omega\logs\mem_trace.csv.
# Read after 1-2h: the monotonically-climbing process IS the leak. SAFE (read-only).
$a = New-ScheduledTaskAction -Execute 'powershell.exe' `
     -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Omega\tools\mem_trace.ps1'
$t = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(-1) -RepetitionInterval (New-TimeSpan -Minutes 10)
$p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'OmegaMemTrace' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
Write-Output ("OmegaMemTrace registered: " + (Get-ScheduledTask -TaskName 'OmegaMemTrace').State + " (every 10min -> C:\Omega\logs\mem_trace.csv)")
