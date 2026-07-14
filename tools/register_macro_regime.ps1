# =============================================================================
# register_macro_regime.ps1 - (re-)create the OmegaMacroRegime task
#
# Durable DAILY producer for tools/fetch_macro_regime.py: pulls yfinance
# (^VIX, ^VIX3M, HYG, LQD, DX-Y.NYB) -> writes
#   C:\Omega\data\index_regime.txt     (IndexRiskGate.hpp risk-off gate)
#   C:\Omega\data\vix_term_ratio.txt   (IndexSeasonalEngine per-instance VIX gate)
#
# WHY (S-2026-07-14 latent-class sweep P1-1): both producers existed but were
# NEVER SCHEDULED anywhere — the 6 gate_by_vix IndexSeasonal engines and the
# IndexRiskGate ran permanently on a stale/absent file. Both gates degrade
# gracefully (>4d stale -> ungated / risk-on, the validated baseline), so
# nothing halted — the gates were just silently inert (comment-as-mechanism +
# absence-blindness classes). This task makes the fiction real.
#
# VIX cash closes 16:15 ET; ETF closes 16:00 ET. Run daily 21:30 UTC (after the
# EDT session fully closes; matches the producer docstring) + once AtStartup.
#
# Same durability pattern as register_macro_gold_gate.ps1 (S4U principal;
# COMPUTERNAME\USERNAME for the SID; -Force atomic replace). NOTE: needs
# yfinance in the venv:  C:\Omega\bracket-bot\.venv\Scripts\python.exe -m pip install yfinance
#
# Run:
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_macro_regime.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'OmegaMacroRegime'
$Py       = 'C:\Omega\bracket-bot\.venv\Scripts\python.exe'   # yfinance pull
$Script   = 'C:\Omega\tools\fetch_macro_regime.py'

if (-not (Test-Path $Py))     { Write-Error "Python venv not at $Py"; exit 1 }
if (-not (Test-Path $Script)) { Write-Error "producer not at $Script"; exit 1 }

$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument "`"$Script`"" `
                                  -WorkingDirectory 'C:\Omega'

# daily after the full US close (VIX cash 16:15 ET) + a one-shot at boot so the
# gate files seed immediately
$daily   = New-ScheduledTaskTrigger -Daily -At '21:30'
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

# seed once now so the gate files exist immediately (don't wait for 21:30 / reboot)
Start-ScheduledTask -TaskName $TaskName
Write-Host "'$TaskName' registered + seeded (daily 21:30 UTC + AtStartup)."
