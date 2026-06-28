# install_prune_hung_ssh.ps1 — register the SAFE hung-ssh-query guard as a scheduled task.
# Run ONCE on the VPS (after a reboot when the box is responsive). Idempotent (-Force).
# The guarded script (prune_hung_ssh_queries.ps1) kills ONLY -EncodedCommand powershells (ssh queries),
# NEVER -File (feed bridges) or python (feeds). See that script's header for the hard-safety contract.
$a = New-ScheduledTaskAction -Execute 'powershell.exe' `
     -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Omega\tools\prune_hung_ssh_queries.ps1'
$t = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(-1) -RepetitionInterval (New-TimeSpan -Minutes 5)
$p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 3) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'OmegaPruneHungSsh' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
Write-Output ("OmegaPruneHungSsh registered: " + (Get-ScheduledTask -TaskName 'OmegaPruneHungSsh').State + " (every 5min, kills only hung -EncodedCommand ssh queries)")
