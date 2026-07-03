# install_prune_hung_ssh.ps1 — register the SAFE hung-ssh-query guard as a scheduled task.
# Run ONCE on the VPS (after a reboot when the box is responsive). Idempotent (-Force).
# The guarded script (prune_hung_ssh_queries.ps1) kills ONLY -EncodedCommand / -Command ad-hoc ssh
# query powershells, NEVER -File (feed bridges) or python (feeds). See that script's header for
# the hard-safety contract.
#
# 2026-07-04 REBOOT-PROOF FIX (operator livid, RED recurred): the prior trigger was
#   `New-ScheduledTaskTrigger -Once -At (now-1min) -RepetitionInterval 5min`.
# A `-Once -At <pasttime>` trigger DOES NOT re-fire after a reboot, and this 3GB box gets
# rebooted constantly for RAM -> the guard died on the FIRST reboot (lastRun 2026-07-03 21:18,
# lastResult 267014 = SCHED_S_TASK_TERMINATED) and never ran again for ~12h while query shells
# piled up. Fix: a DAILY trigger (re-arms every midnight, survives reboots) carrying a 24h/5min
# repetition, plus -StartWhenAvailable so a run missed during a reboot/asleep window fires on wake.
$a = New-ScheduledTaskAction -Execute 'powershell.exe' `
     -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Omega\tools\prune_hung_ssh_queries.ps1'
# Daily anchor re-arms the repetition every day (does not expire like a -Once window);
# the inner repetition drives the every-5-min cadence for the 24h until the next re-arm.
$t = New-ScheduledTaskTrigger -Daily -At 12:00am
$t.Repetition = (New-ScheduledTaskTrigger -Once -At 12:00am `
                 -RepetitionInterval (New-TimeSpan -Minutes 5) `
                 -RepetitionDuration (New-TimeSpan -Hours 24)).Repetition
$p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 3) `
     -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
     -StartWhenAvailable
Register-ScheduledTask -TaskName 'OmegaPruneHungSsh' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName 'OmegaPruneHungSsh'   # reap the current backlog immediately
$i = Get-ScheduledTaskInfo -TaskName 'OmegaPruneHungSsh'
Write-Output ("OmegaPruneHungSsh re-registered (reboot-proof Daily+5min): state=" +
              (Get-ScheduledTask -TaskName 'OmegaPruneHungSsh').State +
              " nextRun=" + $i.NextRunTime)
