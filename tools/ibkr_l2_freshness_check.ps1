# =============================================================================
# ibkr_l2_freshness_check.ps1 -- L2 capture freshness watchdog
#
# Built 2026-05-29 (task #17) after a 24h silent IBKR bridge failure where
# the OmegaIbkrBridge scheduled task aborted on every retry (ModuleNotFoundError:
# ib_async) and the L2 CSVs sat at 0 bytes with no alert. Per-symbol L2 data
# is critical -- engines that gate on l2_imbalance fall back to 0.5 neutral
# when stale, blinding direction signals across the gold path.
#
# What this check does (runs every 2 min via OmegaIbkrL2Freshness task):
#   1. Each watched symbol must have today's CSV at C:\Omega\logs\ibkr_l2\
#      ibkr_l2_<SYM>_<YYYY-MM-DD>.csv
#   2. CSV must be > MIN_BYTES (= header only is failure)
#   3. CSV must be updated within STALE_SEC seconds
#   4. If any check fails: append to alert log, kill stale python processes
#      that look like the bridge, restart OmegaIbkrBridge scheduled task.
#
# What this DOES NOT do:
#   - Restart IB Gateway. Gateway lives in its own process tree; if it has
#     died this script logs the symptom but cannot recover the upstream.
#     IBC (Interactive Brokers Controller) is the supported headless
#     auto-relogin solution -- documented in
#     tools/register_omega_ibkr_bridge.ps1 header.
#
# To register as a scheduled task running every 2 min:
#   powershell -ExecutionPolicy Bypass -File `
#     C:\Omega\tools\register_ibkr_l2_watchdog.ps1
# =============================================================================

$ErrorActionPreference = 'Continue'

# ---- config ------------------------------------------------------------------
$Symbols  = @('XAUUSD', 'NAS100', 'US500')
$Dir      = 'C:\Omega\logs\ibkr_l2'
$StaleSec = 180        # CSV must be touched within this many seconds
$MinBytes = 200        # header alone is ~80-120 bytes; any data writes > 200
$AlertLog = 'C:\Omega\logs\ibkr_l2_alerts.log'
$TaskName = 'OmegaIbkrBridge'

# ---- guard: skip outside market hours ---------------------------------------
# IBKR L2 dries up overnight + on weekends. Don't alarm during known-quiet
# windows (would page-fatigue the operator).
#   XAUUSD trades Sun 22:00 UTC -> Fri 22:00 UTC (5d * 24h)
#   ES/NQ futures Sun 22:00 UTC -> Fri 21:00 UTC with 60-min daily break
#     16:15-17:15 ET (= 21:15-22:15 UTC summer, 20:15-21:15 UTC winter)
# Coarse rule: skip Saturday + Sunday before 22:00 UTC. Skip nothing else;
# brief mid-week gaps will alarm but are useful signal not noise.
$nowUtc  = (Get-Date).ToUniversalTime()
$dow     = $nowUtc.DayOfWeek
$hourUtc = $nowUtc.Hour
$isWeekendQuiet =
    ($dow -eq 'Saturday') -or
    ($dow -eq 'Sunday' -and $hourUtc -lt 22) -or
    ($dow -eq 'Friday' -and $hourUtc -ge 22)
if ($isWeekendQuiet) {
    # Silent skip -- weekend / post-close. Don't alarm, don't restart anything.
    exit 0
}

# ---- per-symbol check loop ---------------------------------------------------
$today    = $nowUtc.ToString("yyyy-MM-dd")
$problems = @()

foreach ($s in $Symbols) {
    $path = Join-Path $Dir ("ibkr_l2_" + $s + "_" + $today + ".csv")
    if (-not (Test-Path $path)) {
        $problems += "MISSING $s : $path does not exist"
        continue
    }
    $f      = Get-Item $path
    $ageSec = [int]($nowUtc - $f.LastWriteTimeUtc).TotalSeconds
    if ($f.Length -lt $MinBytes) {
        $problems += ("EMPTY   $s : size=$($f.Length)B age=${ageSec}s "  +
                      "(header only or no events)")
    } elseif ($ageSec -gt $StaleSec) {
        $problems += ("STALE   $s : age=${ageSec}s > ${StaleSec}s "      +
                      "(size=$($f.Length)B)")
    }
}

# ---- happy path: nothing to alarm on ----------------------------------------
if ($problems.Count -eq 0) {
    exit 0
}

# ---- alarm path: log + auto-restart bridge -----------------------------------
$msg = "[$nowUtc UTC] L2 ALARM`n  " + ($problems -join "`n  ")
Add-Content -Path $AlertLog -Value $msg
Write-Host $msg -ForegroundColor Red

# Kill any python process that looks like the bridge so the relaunch is clean.
# Uses WMI to inspect command-lines (Get-Process doesn't expose them by default).
# NOTE: the bridge runs under pythonw.exe (windowless, via the scheduled task), so
# matching only 'python.exe' missed it entirely -- the zombie survived and the
# Start-ScheduledTask below no-op'd (task already "running"). Match BOTH.
$stale = Get-CimInstance Win32_Process `
        -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" -EA SilentlyContinue |
    Where-Object { $_.CommandLine -like '*ibkr_dom_bridge*' }
foreach ($p in $stale) {
    try {
        Stop-Process -Id $p.ProcessId -Force -EA SilentlyContinue
        Add-Content -Path $AlertLog -Value "  killed stale bridge PID $($p.ProcessId)"
    } catch { }
}

Start-Sleep -Seconds 10   # let Gateway release the client_id

# Restart the bridge scheduled task. If task does not exist, log and exit non-0.
$task = Get-ScheduledTask -TaskName $TaskName -EA SilentlyContinue
if (-not $task) {
    Add-Content -Path $AlertLog -Value "  ABORT: scheduled task '$TaskName' not found"
    exit 2
}
Start-ScheduledTask -TaskName $TaskName
Add-Content -Path $AlertLog -Value "  restarted task '$TaskName'"

exit 1
