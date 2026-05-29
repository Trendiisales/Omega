# =============================================================================
# register_omega_ibkr_bridge.ps1 - (re-)create the OmegaIbkrBridge task
#
# Replaces any existing OmegaIbkrBridge task with one that:
#   - uses the bracket-bot venv Python (the only one with ibapi + ib_async)
#   - connects with clientId=99 (was 42 historically; old id was stuck on the
#     Gateway after a crashed session for hours, blocking re-subscribe)
#   - subscribes IBKR L2 (reqMktDepth) for the Omega engine symbol set,
#     writes raw CSV to C:\Omega\logs\ibkr_l2\, and broadcasts each book
#     update as newline-JSON on 127.0.0.1:9701 for Omega.exe to consume
#   - triggers AtStartup + every 5 min repeat -- survives VPS reboot without
#     anyone RDP-ing in (was -AtLogOn, required interactive session)
#   - principal is S4U (stored credentials, no interactive logon needed)
#   - auto-restarts on failure: 99 attempts, 1-min interval
#
# Run in ADMINISTRATOR PowerShell:
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_omega_ibkr_bridge.ps1
#
# Default symbol list covers every instrument in omega_config.ini:
#   FX  (IDEALPRO): EURUSD GBPUSD AUDUSD USDJPY NZDUSD
#   CMD (SMART)  : XAUUSD XAGUSD
#   IDX (CME)    : ES NQ YM
#   IDX (EUREX)  : DAX ESTX50
#   IDX (ICEEU)  : Z (FTSE)
#   ENG (NYMEX)  : CL NG    BRENT (IPE: COIL)
#   VOL (CFE)    : VX
#   DXY (NYBOT)  : DX
#
# USTEC and NAS100 both map to NQ on IBKR; only NAS100 is in the default
# list to avoid double-subscribing the same conId. Omega.exe dispatches
# USTEC.F off the NAS100 feed downstream.
#
# Some symbols will fail until the IBKR account has the matching market-data
# subscription (e.g. CFE for VIX, ICEEUSOFT for COIL). The bridge logs each
# FAILED line individually and continues -- fix subs in the IBKR portal and
# rerun the task.
#
# COMPANION CHECKS:
#   tools\ibkr_l2_freshness_check.ps1   -- alarms on stale / empty CSVs
#   tools\register_ibkr_l2_watchdog.ps1 -- registers the freshness check task
#   tools\ibkr_dom_bridge_requirements.txt -- pinned bridge venv deps
# Run register_ibkr_l2_watchdog.ps1 after this script so the freshness
# watchdog covers any future silent bridge failure.
#
# HISTORY:
#   2026-05-28 original (system Python; -AtLogOn; clientId 42).
#   2026-05-29 (this rev) post-IBKR-bridge-outage:
#     - 24h silent failure: scheduled task aborted every retry on
#       ModuleNotFoundError: ib_async. System Python had no ibapi either.
#     - Bracket-bot venv at C:\Omega\bracket-bot\.venv had ibapi but not
#       ib_async; pip-installed both there and locked in
#       ibkr_dom_bridge_requirements.txt.
#     - Trigger swap -AtLogOn -> -AtStartup so VPS reboot recovery does
#       not require an RDP login.
#     - clientId 42 -> 99: the Gateway retained 42 from the crashed
#       client after compulsory restart; using a new id removes the
#       collision race.
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaIbkrBridge'
$Py       = 'C:\Omega\bracket-bot\.venv\Scripts\python.exe'
$Script   = 'C:\Omega\tools\ibkr_dom_bridge.py'
$OutDir   = 'C:\Omega\logs\ibkr_l2'
$Port     = 4002        # IB Gateway paper. Live = 4001.
$ClientId = 99
$TcpPort  = 9701        # Omega.exe consumer connects here.
$MaxLvl   = 5

# IBKR caps concurrent reqMktDepth at 3 streams on this account tier.
# Until month-end booster pack(s) activate, restrict the live set to the
# three highest-priority symbols. The bridge logs Error 309 for every
# subscription beyond the third, leaving the corresponding CSV empty,
# so adding extras here just wastes startup time.
# Once additional depth slots are subscribed, append symbols below in
# priority order: DJ30, GER40, UK100, EURUSD, GBPUSD, USOIL, ...
$Symbols = @(
    'XAUUSD','NAS100','US500'
) -join ','

if (-not (Test-Path $Py))     { Write-Error "Python venv not at $Py";   exit 1 }
if (-not (Test-Path $Script)) { Write-Error "bridge not at $Script";    exit 1 }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$User = "$env:USERDOMAIN\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"
Write-Host "Python  : $Py"
Write-Host "ClientId: $ClientId"
Write-Host "Symbols : $Symbols"

$argList = "`"$Script`" --host 127.0.0.1 --port $Port --client-id $ClientId " +
           "--symbols $Symbols --out-dir `"$OutDir`" --tcp-port $TcpPort " +
           "--max-levels $MaxLvl"

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument $argList `
                                  -WorkingDirectory 'C:\Omega'

# Triggers:
#   1. AtStartup: fires once on every boot (no interactive logon needed
#      because principal LogonType is S4U).
#   2. Every-5-min repeat. Idempotent because MultipleInstances=IgnoreNew --
#      if the bridge is still running, the repeat fire is dropped. If it
#      has died, the fire restarts it.
$startup = New-ScheduledTaskTrigger -AtStartup
$repeat  = New-ScheduledTaskTrigger -Once -At ([DateTime]::Now.AddMinutes(-1)) `
    -RepetitionInterval (New-TimeSpan -Minutes 5) `
    -RepetitionDuration (New-TimeSpan -Days 3650)

# Belt-and-suspenders auto-restart from Task Scheduler itself.
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Days 30) `
    -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

# S4U principal: task runs under the named user account with stored
# credentials, no interactive session required. Survives reboot without
# RDP. Highest run-level is needed so the bridge can bind 127.0.0.1:9701
# (the TCP broadcaster Omega.exe consumes) without UAC prompts.
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
    -Description ("Capture IBKR L2 for the Omega symbol set; broadcasts on " +
                  "127.0.0.1:9701. AtStartup + 5min repeat. Uses bracket-bot " +
                  "venv Python (has ibapi+ib_async per " +
                  "ibkr_dom_bridge_requirements.txt). Companion: " +
                  "OmegaIbkrL2Freshness watchdog.") | Out-Null

Write-Host "Registered '$TaskName' OK."
Write-Host ""
Write-Host "Start now:           Start-ScheduledTask -TaskName '$TaskName'"
Write-Host "Watch task status:   Get-ScheduledTaskInfo -TaskName '$TaskName'"
Write-Host "Watch raw output:    Get-ChildItem $OutDir | Sort LastWriteTime -Desc | Select -First 20"
Write-Host ""
Write-Host "ALSO REGISTER THE WATCHDOG so future silent failures self-recover:"
Write-Host "  powershell -ExecutionPolicy Bypass -File `"C:\Omega\tools\register_ibkr_l2_watchdog.ps1`""
