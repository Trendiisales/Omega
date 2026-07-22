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
# 2026-06-04: matches the bridge's live --symbols (register_omega_ibkr_bridge.ps1).
# NAS100/US500 dropped (empty depth, no CME index sub).
# 2026-07-17h: MGC REMOVED. It was dropped from the bridge --symbols on 2026-07-03
# (commit 2f5733ec, swap MGC->DJ30 to free the 3rd/last depth slot) but THIS
# watchdog was never updated in sync -- so for ~2 weeks it false-alarmed
# "MISSING MGC" and (MISSING => restart-fixable, no backoff) killed the HEALTHY
# bridge python -- which serves XAUUSD depth + all stock L1 -- and restarted the
# task every 2 min during market hours. That is the recurring "VPS down / won't
# restart" incident (bridge flaps every 2 min, feeds churn, desk sees it as down).
# MGC's only consumer (MgcFastDon) was retired 2026-07-17f (8b83b77a). Keep this
# list in sync with the bridge --symbols or the watchdog kills the healthy bridge.
#
# 2026-07-22: SWITCHED FROM L2 DEPTH -> L1 TOP-OF-BOOK for the freshness check.
# Root cause of a multi-hour gold-price outage: this watchdog keyed on
# ibkr_l2_XAUUSD_<date>.csv (L2 DEPTH), but gold L2 depth was RETIRED 2026-07-10
# (OmegaIndexHtml: "gold L2 depth feed retired ... gold_bids/gold_asks/l2_gold no
# longer consumed") and XAU depth no longer establishes reliably (NQ/YM take the
# 3 depth slots; XAU depth request returns nothing). So the L2 file went MISSING
# every day => the watchdog (MISSING = restart-fixable, NO backoff) killed the
# HEALTHY bridge every 2 min -- and that kill also drops the L1 XAUUSD *price*
# feed the desk/telemetry actually needs (gold tile). The price feed is L1
# (ibkr_l1_XAUUSD_<date>.csv), which writes fine. Guard THAT instead: it is the
# real gold-price health signal + a genuine bridge death still stalls it.
$L1Check  = $true      # true = watch ibkr_l1_ (top-of-book price); false = ibkr_l2_ (depth)
$Symbols  = @('XAUUSD')
$Dir      = 'C:\Omega\logs\ibkr_l2'
$StaleSec = 180        # CSV must be touched within this many seconds
$MinBytes = 200        # header alone is ~80-120 bytes; any data writes > 200
$AlertLog = 'C:\Omega\logs\ibkr_l2_alerts.log'
$TaskName = 'OmegaIbkrBridge'

# EMPTY-only (header-only CSV) backoff. A header-only file is an UPSTREAM
# subscription / entitlement / IBKR-session problem that a bridge restart cannot
# fix (proven 2026-07-02: MGC sat header-only through a full day of 2-min restarts
# while an IBKR "different IP" session held the market-data line). Restarting on it
# just churns the bridge -- which also serves the healthy symbols (XAUUSD) -- every
# 2 min. So throttle restarts triggered ONLY by EMPTY to once per this many seconds.
# STALE / MISSING (bridge or connection actually died) still restart every run.
$EmptyBackoffSec = 1800
$EmptyStateFile  = 'C:\Omega\logs\ibkr_l2_empty_restart.state'

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
$hasRestartFixable = $false   # STALE or MISSING -> the bridge/connection died, restart IS the fix
$hasEmpty          = $false   # header-only -> upstream subscription/session issue, restart won't help

$prefix = if ($L1Check) { "ibkr_l1_" } else { "ibkr_l2_" }
foreach ($s in $Symbols) {
    $path = Join-Path $Dir ($prefix + $s + "_" + $today + ".csv")
    if (-not (Test-Path $path)) {
        $problems += "MISSING $s : $path does not exist"
        $hasRestartFixable = $true
        continue
    }
    $f      = Get-Item $path
    $ageSec = [int]($nowUtc - $f.LastWriteTimeUtc).TotalSeconds
    if ($f.Length -lt $MinBytes) {
        $problems += ("EMPTY   $s : size=$($f.Length)B age=${ageSec}s "  +
                      "(header only or no events)")
        $hasEmpty = $true
    } elseif ($ageSec -gt $StaleSec) {
        $problems += ("STALE   $s : age=${ageSec}s > ${StaleSec}s "      +
                      "(size=$($f.Length)B)")
        $hasRestartFixable = $true
    }
}

# ---- happy path: nothing to alarm on ----------------------------------------
if ($problems.Count -eq 0) {
    # Clear the EMPTY backoff timer so a fresh header-only failure later gets an
    # immediate first restart attempt rather than inheriting a stale timestamp.
    if (Test-Path $EmptyStateFile) { Remove-Item $EmptyStateFile -Force -EA SilentlyContinue }
    exit 0
}

# ---- alarm path: log + auto-restart bridge -----------------------------------
$msg = "[$nowUtc UTC] L2 ALARM`n  " + ($problems -join "`n  ")
Add-Content -Path $AlertLog -Value $msg
Write-Host $msg -ForegroundColor Red

# ---- restart decision: back off on EMPTY-only ------------------------------
# STALE/MISSING => bridge or its connection died => restart is the correct fix
# (aggressive, every run). EMPTY-only (header-only) => upstream subscription /
# entitlement / IBKR-session problem a restart CANNOT fix; throttle to once per
# $EmptyBackoffSec so we don't churn the bridge (and its healthy symbols) every
# 2 min. The alarm above is logged EVERY run regardless, so visibility is kept.
$shouldRestart = $true
if ($hasEmpty -and -not $hasRestartFixable) {
    $last = 0
    if (Test-Path $EmptyStateFile) {
        try { $last = [int64]((Get-Content $EmptyStateFile -Raw).Trim()) } catch { $last = 0 }
    }
    $nowEpoch  = [int64][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $sinceLast = $nowEpoch - $last
    if ($sinceLast -lt $EmptyBackoffSec) {
        Add-Content -Path $AlertLog -Value ("  EMPTY-only: bridge restart SUPPRESSED " +
            "(backoff ${sinceLast}s < ${EmptyBackoffSec}s) -- header-only is an upstream " +
            "subscription/session issue a restart can't fix; not churning the bridge")
        exit 1
    }
    $nowEpoch | Out-File -Encoding ascii $EmptyStateFile
    Add-Content -Path $AlertLog -Value ("  EMPTY-only: backoff window elapsed (${sinceLast}s) " +
        "-- one restart attempt, then back off ${EmptyBackoffSec}s")
}

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
