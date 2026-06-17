# =============================================================================
# register_bigcap_feed_bridge.ps1 - (re-)create the OmegaBigCapBridge task
#
# Durable scheduled task for pump/bigcap_feed_bridge.py: the IBKR real-time
# big-cap day-mover scanner feeding g_bigcap_momo (BigCapMomo shadow) over the
# B/S/P/C protocol on port 7784 (OMEGA_BIGCAP_BRIDGE consumer thread).
# IB Gateway paper 4002, clientId 34 (pump scanner uses 33, DOM bridge 99,
# MGC bars 88). Uses the bracket-bot venv Python (has ib_async).
#
# Same durability pattern as register_mgc_live_bars.ps1 (2026-06-04 lesson:
# bare Start-Process recorders die silently; AtStartup + 5-min IgnoreNew
# respawn + S4U principal; COMPUTERNAME not USERDOMAIN for the SID).
#
# Run:
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_bigcap_feed_bridge.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaBigCapBridge'
$Py       = 'C:\Omega\bracket-bot\.venv\Scripts\pythonw.exe'  # has ib_async
$Script   = 'C:\Omega\pump\bigcap_feed_bridge.py'

if (-not (Test-Path $Py))     { Write-Error "Python venv not at $Py";  exit 1 }
if (-not (Test-Path $Script)) { Write-Error "bridge not at $Script";   exit 1 }

$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"

# ── S-2026-06-17: select the bridge path PERMANENTLY (Machine scope persists across
#    deploys + reboots). Registering the bridge implies Omega should CONSUME it, so
#    set OMEGA_BIGCAP_BRIDGE=1 and drop the mutually-exclusive in-process IBKR path
#    (OMEGA_BIGCAP_IBKR -> stub/no-op unless the binary is built with OMEGA_WITH_IBKR).
#    Root cause this fixes: IBKR path was selected on a non-IBKR build = zero trades,
#    silent, for weeks. NOTE: the Omega SERVICE must restart to read new Machine env
#    -- OMEGA.ps1 deploy does that. ──
[Environment]::SetEnvironmentVariable('OMEGA_BIGCAP_BRIDGE','1',  'Machine')
[Environment]::SetEnvironmentVariable('OMEGA_BIGCAP_IBKR',  $null,'Machine')
Write-Host "env: OMEGA_BIGCAP_BRIDGE=1 set, OMEGA_BIGCAP_IBKR removed (Machine). Restart Omega service to apply."

# kill any STALE bigcap bridge procs (the 2026-06-17 double-launch: two pythonw
# bound-fighting :7784). Start-ScheduledTask below relaunches exactly one.
Get-CimInstance Win32_Process -Filter "Name like 'pythonw%'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like '*bigcap_feed_bridge.py*' } |
    ForEach-Object { Write-Host "killing stale bridge pid $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument "`"$Script`"" `
                                  -WorkingDirectory 'C:\Omega'

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

# Register-or-update WITHOUT an unregister-first window (the 2026-06-04
# bridge-register lesson: a failed Register after Unregister leaves recording
# down). Register with -Force overwrites atomically.
Register-ScheduledTask -TaskName $TaskName `
                       -Action $action `
                       -Trigger @($startup, $repeat) `
                       -Settings $settings `
                       -Principal $principal `
                       -Force | Out-Null

Start-ScheduledTask -TaskName $TaskName
Write-Host "'$TaskName' registered + started."
