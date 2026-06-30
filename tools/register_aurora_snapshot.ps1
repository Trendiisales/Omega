# register_aurora_snapshot.ps1 -- scheduled task that keeps the Aurora
# liquidity map fresh for BOTH the omega-terminal AUR panel AND two LIVE
# trading consumers:
#   1. include/AuroraGate.hpp  -- order-flow entry gate (gold ORB + index),
#      reads logs\aurora\aurora_gate.tsv (NQ tape proxies NAS100/US500).
#   2. ~/Crypto shadow_refresh.cpp read_live_nq() -- SSH-pulls aurora_NQ.json
#      for the LIVE NDX mark (NDX TSMom50/RSIrev legs + BE-ratchet). When this
#      file goes stale the crypto book silently falls back to the daily close.
# It is NOT just a GUI heatmap. Do not disable it without replacing the feed.
#
# RUNS `--once` ON A 60s PERIODIC TRIGGER (not a resident --interval loop).
# WHY: the old resident `pythonw --interval 60` loop detached from Task
# Scheduler's instance tracking, so -MultipleInstances IgnoreNew could not see
# the live process and the 5-min restart trigger spawned a NEW loop each time
# -- 4 duplicates piled to 257MB and a 2026-06-28 session disabled the whole
# task (+ removed it from omega_health CRIT) to stop the leak, which killed the
# two live feeds above for ~4 days unnoticed. A short-lived `--once` run that
# exits every minute cannot orphan/pileup, mirroring the non-leaking Gex /
# Healthcheck periodic tasks. ExecutionTimeLimit is the backstop reaper.
#
# Freshness is now monitored: omega_health.py chk_aurora goes RED (loud channel
# -- HEALTH_RED.flag + :7790 + mac notification) if aurora_NQ.json stops
# updating during US RTH. So a future death surfaces instead of hiding.
#
# Register (interactive or over ssh):
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_aurora_snapshot.ps1

$TaskName = 'OmegaAuroraSnapshot'
$Py       = 'C:\Omega\bracket-bot\.venv\Scripts\pythonw.exe'  # console-less
$Script   = 'C:\Omega\ibkr\aurora_snapshot.py'
$InDir    = 'C:\Omega\logs\ibkr_l2'      # bridge output (tape + L2)
$OutDir   = 'C:\Omega\logs\aurora'       # aurora_*.json + aurora_gate.tsv

if (-not (Test-Path $Py))     { Write-Error "Python venv not at $Py";   exit 1 }
if (-not (Test-Path $Script)) { Write-Error "snapshot not at $Script";  exit 1 }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# COMPUTERNAME (not USERDOMAIN): local WORKGROUP account, ssh-safe SID mapping.
$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"
Write-Host "Python  : $Py"
Write-Host "Script  : $Script  (--once, periodic 60s)"

# --once: compute one snapshot from today's recorded tape, write JSONs + gate
# file, exit. The task fires it every 60s.
$argList = "`"$Script`" --once --in-dir `"$InDir`" --out-dir `"$OutDir`""

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument $argList `
                                  -WorkingDirectory 'C:\Omega'

# Periodic: one trigger that repeats every 60s, effectively forever. No resident
# loop, no AtStartup loop to orphan.
$repeat  = New-ScheduledTaskTrigger -Once -At ([DateTime]::Now.AddMinutes(-1)) `
    -RepetitionInterval (New-TimeSpan -Minutes 1) `
    -RepetitionDuration (New-TimeSpan -Days 3650)

# ExecutionTimeLimit 4 min = reaper: a single --once pass takes a few seconds;
# 4 min kills any wedged run long before the next minute-tick, so duplicates
# cannot accumulate even if IgnoreNew ever loses tracking.
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 4) `
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
    -Trigger $repeat `
    -Settings $settings `
    -Principal $principal `
    -Description ("Recompute the Aurora order-flow liquidity map every 60s " +
                  "(--once, periodic) from the recorded MGC/NQ footprint tape; " +
                  "writes logs\aurora\aurora_*.json + aurora_gate.tsv. LIVE " +
                  "consumers: AuroraGate.hpp entry gate + Crypto NDX live mark. " +
                  "Monitored by omega_health chk_aurora. Do NOT disable without " +
                  "replacing the feed.") | Out-Null

Write-Host "Registered '$TaskName' OK."
Write-Host ""
Write-Host "Start now:           Start-ScheduledTask -TaskName '$TaskName'"
Write-Host "Watch status:        Get-ScheduledTaskInfo -TaskName '$TaskName'"
Write-Host "Watch output:        Get-Content C:\Omega\logs\aurora\aurora_NQ.json -Tail 5"
