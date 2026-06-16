# register_aurora_snapshot.ps1 -- scheduled task that keeps the Aurora
# liquidity map fresh for the omega-terminal AUR panel.
#
# Runs ibkr/aurora_snapshot.py as a RESIDENT loop (--interval 60): every 60s it
# re-runs the footprint shelf engine over today's recorded MGC/NQ tape
# (logs\ibkr_l2\ibkr_trades_*.csv + ibkr_l2_*.csv) and rewrites
# logs\aurora\aurora_all.json. The C++ /api/v1/omega/aurora route serves that
# file; the panel polls the route every 5s.
#
# Companion to OmegaIbkrBridge (which produces the tape this consumes). Same
# S4U / AtStartup / restart-if-dead idiom so it survives reboot + crash without
# an interactive session. aurora_snapshot.py is pure-stdlib (no numpy/pandas),
# so any Python works; we reuse the bracket-bot venv pythonw for a console-less
# run, consistent with the bridge.
#
# Register (interactive or over ssh):
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_aurora_snapshot.ps1

$TaskName = 'OmegaAuroraSnapshot'
$Py       = 'C:\Omega\bracket-bot\.venv\Scripts\pythonw.exe'  # console-less
$Script   = 'C:\Omega\ibkr\aurora_snapshot.py'
$InDir    = 'C:\Omega\logs\ibkr_l2'      # bridge output (tape + L2)
$OutDir   = 'C:\Omega\logs\aurora'       # aurora_all.json lives here
$Interval = 60                            # seconds between recomputes

if (-not (Test-Path $Py))     { Write-Error "Python venv not at $Py";   exit 1 }
if (-not (Test-Path $Script)) { Write-Error "snapshot not at $Script";  exit 1 }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# COMPUTERNAME (not USERDOMAIN): local WORKGROUP account, ssh-safe SID mapping.
$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"
Write-Host "Python  : $Py"
Write-Host "Script  : $Script  (--interval $Interval)"

$argList = "`"$Script`" --interval $Interval --in-dir `"$InDir`" --out-dir `"$OutDir`""

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument $argList `
                                  -WorkingDirectory 'C:\Omega'

# AtStartup once + every-5-min repeat that is dropped while the resident loop
# is alive (IgnoreNew) and restarts it if it died.
$startup = New-ScheduledTaskTrigger -AtStartup
$repeat  = New-ScheduledTaskTrigger -Once -At ([DateTime]::Now.AddMinutes(-1)) `
    -RepetitionInterval (New-TimeSpan -Minutes 5) `
    -RepetitionDuration (New-TimeSpan -Days 3650)

$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Days 30) `
    -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) `
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
    -Trigger @($startup, $repeat) `
    -Settings $settings `
    -Principal $principal `
    -Description ("Recompute the Aurora order-flow liquidity map every " +
                  "$Interval s from the recorded MGC/NQ footprint tape; writes " +
                  "logs\aurora\aurora_all.json for the /api/v1/omega/aurora " +
                  "route + AUR terminal panel. Consumes OmegaIbkrBridge output.") | Out-Null

Write-Host "Registered '$TaskName' OK."
Write-Host ""
Write-Host "Start now:           Start-ScheduledTask -TaskName '$TaskName'"
Write-Host "Watch status:        Get-ScheduledTaskInfo -TaskName '$TaskName'"
Write-Host "Watch output:        Get-Content C:\Omega\logs\aurora\aurora_all.json -Tail 5"
