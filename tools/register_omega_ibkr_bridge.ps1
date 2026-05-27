# =============================================================================
# register_omega_ibkr_bridge.ps1 - (re-)create the OmegaIbkrBridge task
#
# Replaces any existing OmegaIbkrBridge task with one that:
#   - subscribes IBKR L2 (reqMktDepth) for the full Omega engine symbol set,
#     writes raw CSV to C:\Omega\logs\ibkr_l2\, and broadcasts each book
#     update as newline-JSON on 127.0.0.1:9701 for Omega.exe to consume
#   - triggers at user logon + every 5 min repeat (idempotent: the 5-min
#     trigger no-ops while bridge is already running via MultipleInstances
#     IgnoreNew)
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
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaIbkrBridge'
$Py       = 'C:\Program Files\Python312\python.exe'
$Script   = 'C:\Omega\tools\ibkr_dom_bridge.py'
$OutDir   = 'C:\Omega\logs\ibkr_l2'
$Port     = 4002        # IB Gateway paper. Live = 4001.
$ClientId = 42
$TcpPort  = 9701        # Omega.exe consumer connects here.
$MaxLvl   = 5

$Symbols = @(
    'XAUUSD','XAGUSD',
    'US500','NAS100','DJ30','GER40','UK100','ESTX50',
    'USOIL','UKBRENT','NGAS','VIX','DX',
    'EURUSD','GBPUSD','AUDUSD','USDJPY','NZDUSD'
) -join ','

if (-not (Test-Path $Py))     { Write-Error "Python not at $Py";      exit 1 }
if (-not (Test-Path $Script)) { Write-Error "bridge not at $Script";  exit 1 }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$User = "$env:USERDOMAIN\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"
Write-Host "Symbols: $Symbols"

$argList = "`"$Script`" --host 127.0.0.1 --port $Port --client-id $ClientId " +
           "--symbols $Symbols --out-dir `"$OutDir`" --tcp-port $TcpPort " +
           "--max-levels $MaxLvl"

$action = New-ScheduledTaskAction -Execute $Py -Argument $argList -WorkingDirectory 'C:\Omega'

$logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $User

# 5-min repeat: idempotent because MultipleInstances=IgnoreNew. If the bridge
# is running, the repeat fire is dropped. If it has died, the fire restarts it.
$repeat = New-ScheduledTaskTrigger -Once -At ([DateTime]::Now.AddMinutes(-1)) `
    -RepetitionInterval (New-TimeSpan -Minutes 5) `
    -RepetitionDuration (New-TimeSpan -Days 3650)

# Belt-and-suspenders auto-restart from Task Scheduler itself.
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Days 30) `
    -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed existing '$TaskName'"
}

Register-ScheduledTask -TaskName $TaskName `
    -Action $action `
    -Trigger @($logonTrigger, $repeat) `
    -Settings $settings `
    -Principal $principal `
    -Description "Capture IBKR L2 for the full Omega symbol set; broadcasts on 127.0.0.1:9701" | Out-Null

Write-Host "Registered '$TaskName' OK."
Write-Host ""
Write-Host "Start now:           Start-ScheduledTask -TaskName '$TaskName'"
Write-Host "Watch task status:   Get-ScheduledTaskInfo -TaskName '$TaskName'"
Write-Host "Watch raw output:    Get-ChildItem $OutDir | Sort LastWriteTime -Desc | Select -First 20"
