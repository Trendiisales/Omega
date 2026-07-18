# =============================================================================
# register_data_pullers.ps1 — fix the RECURRING data-staleness root cause.
#
# WHY: pull_stock_bars.py (BigCapMomo/Luke 15m+d1 stock universe) and
# pull_nas_cash.py (ConnorsNas / index NDX intraday) are MANUAL-ONLY scripts —
# never scheduled. They only ran when a human typed them (last 06-20 / 06-10),
# so the engines that consume data\stocks\*.csv + data\NDX_15m.csv silently
# starved while the freshness monitor watched the wrong (Mac daily) files.
# This registers BOTH producers as durable VPS scheduled tasks against the live
# IB Gateway (already up — MGC/crypto feeds prove it). Daily post-US-close pull
# + AtStartup. No code change — just the scheduling that was always missing.
#
# Run ONCE on the VPS (responsive window):
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_data_pullers.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$Py   = 'C:\Omega\bracket-bot\.venv\Scripts\pythonw.exe'   # has ib_async
$User = "$env:COMPUTERNAME\$env:USERNAME"

if (-not (Test-Path $Py)) { Write-Error "venv python not at $Py"; exit 1 }

function Register-Puller($name, $script, $argline, $hourUtc) {
    if (-not (Test-Path "C:\Omega\$script")) { Write-Warning "missing C:\Omega\$script — skipping $name"; return }
    $action = New-ScheduledTaskAction -Execute $Py -Argument "`"C:\Omega\$script`" $argline" -WorkingDirectory 'C:\Omega'
    # daily at $hourUtc + AtStartup (so a reboot re-pulls); StartWhenAvailable catches missed runs
    $tDaily   = New-ScheduledTaskTrigger -Daily -At ([DateTime]::Today.AddHours($hourUtc))
    $tStartup = New-ScheduledTaskTrigger -AtStartup
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -MultipleInstances IgnoreNew `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 20) `
        -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 5) `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $principal = New-ScheduledTaskPrincipal -UserId $User -LogonType S4U -RunLevel Highest
    if (Get-ScheduledTask -TaskName $name -EA SilentlyContinue) { Unregister-ScheduledTask -TaskName $name -Confirm:$false }
    Register-ScheduledTask -TaskName $name -Action $action -Trigger @($tDaily,$tStartup) -Settings $settings -Principal $principal `
        -Description "Data producer for engine universe (fixes manual-only staleness). Daily ${hourUtc}:00 UTC + AtStartup." | Out-Null
    Write-Host "Registered $name ($script) — daily ${hourUtc}:00 UTC + AtStartup"
}

# stocks (BigCapMomo/Luke) — 30-name mega-cap universe, port 4002, 2Y of 15m+d1
Register-Puller 'OmegaPullStockBars' 'tools\pull_stock_bars.py' 'NVDA,TSLA,AMD,META,AAPL,MSFT,AMZN,GOOGL,AVGO,PLTR,ORCL,CRM,ADBE,NFLX,MU,COIN,MSTR,SMCI,UBER,ABNB,SHOP,ARM,DELL,NOW,SNOW,PANW,CRWD,DLTR,LRCX,AMAT 4001' 21
# NDX cash intraday (ConnorsNas / index) — port 4002, 2Y
Register-Puller 'OmegaPullNasCash'   'tools\pull_nas_cash.py'   '4001' 21

Write-Host ""
Write-Host "DONE. Verify after first run: Get-ChildItem C:\Omega\data\stocks\*_15m.csv | Sort LastWriteTime -Desc | Select -First 3"
