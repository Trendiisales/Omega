# =============================================================================
# register_macro_gold_gate.ps1 - (re-)create the OmegaMacroGoldGate task
#
# Durable DAILY producer for tools/macro_gold_gate.py: pulls FRED (real yields +
# dollar) -> writes logs/macro/macro_gold_gate.tsv, the flat file MacroGoldGate.hpp
# reads each gold tick. Fail-safe: a stale/missing file reverts the gate to the
# price core (de-risk only when real yields spike). So a missed run is benign --
# but without this task the gate never refreshes and stays permanently inert.
#
# FRED updates ~16:30 ET. VPS clock is UTC, so run daily 20:40 UTC (after EDT
# update) + once AtStartup to seed immediately on boot.
#
# Same durability pattern as register_bigcap_feed_bridge.ps1 / register_mgc_live_bars.ps1
# (S4U principal; COMPUTERNAME\USERNAME for the SID; -Force atomic replace).
#
# Run:
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_macro_gold_gate.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaMacroGoldGate'
$Py       = 'C:\Omega\bracket-bot\.venv\Scripts\python.exe'   # FRED pull (needs requests/urllib)
$Script   = 'C:\Omega\tools\macro_gold_gate.py'

if (-not (Test-Path $Py))     { Write-Error "Python venv not at $Py"; exit 1 }
if (-not (Test-Path $Script)) { Write-Error "producer not at $Script"; exit 1 }

$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument "`"$Script`"" `
                                  -WorkingDirectory 'C:\Omega'

# daily after the FRED update + a one-shot at boot so the gate file seeds immediately
$daily   = New-ScheduledTaskTrigger -Daily -At '20:40'
$startup = New-ScheduledTaskTrigger -AtStartup

$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 15) `
    -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 5) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

$principal = New-ScheduledTaskPrincipal -UserId $User `
                                        -LogonType S4U `
                                        -RunLevel Highest

Register-ScheduledTask -TaskName $TaskName `
                       -Action $action `
                       -Trigger @($daily, $startup) `
                       -Settings $settings `
                       -Principal $principal `
                       -Force | Out-Null

# seed once now so the gate file exists immediately (don't wait for 20:40 / reboot)
Start-ScheduledTask -TaskName $TaskName
Write-Host "'$TaskName' registered + seeded (daily 20:40 UTC + AtStartup)."
