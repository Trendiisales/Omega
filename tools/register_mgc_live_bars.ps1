# =============================================================================
# register_mgc_live_bars.ps1 - (re-)create the OmegaMgcLiveBars task
#
# Durable scheduled task for tools/mgc_live_bars.py: the live MGC 30m
# TRADES-bar producer (OHLCV + volume) for MgcFastDonchian30m. Loops every
# PERIOD seconds, appends new CLOSED bars to data\mgc_30m_live.csv and
# refreshes data\mgc_hvn.json (prior-day POC + HVN). clientId 88 (distinct
# from the DOM bridge's 99). Uses the bracket-bot venv Python (has ib_async).
#
# 2026-06-04: registered as a DURABLE task (was a bare Start-Process that
# died on the next reboot / on any cleanup, leaving MGC volume recording
# silently dead). RECORDING IS VITAL (operator, non-optional) -- this and
# the OmegaIbkrBridge task are the two MGC recorders and both must survive
# reboot without an interactive logon.
#
# Triggers AtStartup + every-5-min repeat (IgnoreNew so a live instance is
# never double-started; a dead one is respawned within 5 min). S4U principal
# (stored creds, no interactive logon). COMPUTERNAME\USERNAME, NOT
# USERDOMAIN (which is WORKGROUP over ssh and fails the SID mapping).
#
# Run:
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_mgc_live_bars.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaMgcLiveBars'
$Py       = 'C:\Omega\bracket-bot\.venv\Scripts\pythonw.exe'  # has ib_async
$Script   = 'C:\Omega\tools\mgc_live_bars.py'
$Port     = 4001        # IB Gateway LIVE (2026-07-18 fresh live login on default 4001).
$Period   = 300         # seconds between polls

if (-not (Test-Path $Py))     { Write-Error "Python venv not at $Py";   exit 1 }
if (-not (Test-Path $Script)) { Write-Error "producer not at $Script";  exit 1 }

$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"

$argList = "`"$Script`" $Port $Period"

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument $argList `
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

if (Get-ScheduledTask -TaskName $TaskName -EA SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed existing '$TaskName'"
}

Register-ScheduledTask -TaskName $TaskName `
    -Action $action `
    -Trigger @($startup, $repeat) `
    -Settings $settings `
    -Principal $principal `
    -Description ("Live MGC 30m TRADES bars + HVN producer for " +
                  "MgcFastDonchian30m (clientId 88). AtStartup + 5min repeat. " +
                  "Bracket-bot venv Python. Recording is operator-mandated " +
                  "vital/non-optional.") | Out-Null

Write-Host "Registered '$TaskName' OK."
Write-Host "Start now: Start-ScheduledTask -TaskName '$TaskName'"
