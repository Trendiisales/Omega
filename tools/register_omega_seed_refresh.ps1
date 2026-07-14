# =============================================================================
# register_omega_seed_refresh.ps1 - (re-)create the OmegaSeedRefresh task
#
# S-2026-07-14bj: the task was found pointing at tools\refresh_warmup_seeds.py
# -- a zombie wrapper DELETED in S-2026-07-14an -- with a BLANK WorkingDirectory
# (seed_refresh.py writes relative data\ paths, so even a live script would
# have written into System32). The nightly 23:30 run would have failed silently
# from the first deploy carrying the deletion. This script is the durable,
# idempotent fix: repoint to tools\seed_refresh.py (all 3 phases: rebuild ->
# ibkr -> registry audit; rebuild self-skips without pandas; audit exit code
# surfaces as the task result so a broken registry shows up as a failed task).
#
# Run:
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_omega_seed_refresh.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaSeedRefresh'
$Py       = 'C:\Program Files\Python312\python.exe'
$Script   = 'C:\Omega\tools\seed_refresh.py'
$Port     = 4002        # IB Gateway paper. Live = 4001.

if (-not (Test-Path $Py))     { Write-Error "python not at $Py";    exit 1 }
if (-not (Test-Path $Script)) { Write-Error "refresher not at $Script"; exit 1 }

$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument "`"$Script`" --port $Port --repo C:\Omega" `
                                  -WorkingDirectory 'C:\Omega'

# nightly 23:30 local (the historical slot; post-CME-close, pre-midnight)
$trigger = New-ScheduledTaskTrigger -Daily -At '23:30'

$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 15) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

$principal = New-ScheduledTaskPrincipal -UserId $User `
                                        -LogonType S4U `
                                        -RunLevel Highest

if (Get-ScheduledTask -TaskName $TaskName -EA SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed existing '$TaskName'"
}

Register-ScheduledTask -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Principal $principal `
    -Description ("Nightly warmup-seed refresh (tools/seed_refresh.py: log rebuild + " +
                  "IBKR pulls incl. data\mgc_*_hist.csv + registry audit). Repointed " +
                  "S-2026-07-14bj off the deleted refresh_warmup_seeds.py zombie; " +
                  "WorkingDirectory C:\Omega is load-bearing (relative data\ writes).") | Out-Null

Write-Host "Registered '$TaskName' OK."
Write-Host "Run now: Start-ScheduledTask -TaskName '$TaskName'"
