# ==============================================================================
#  OMEGA - Push log + state files to git
#
#  Pushes (all to logs/ branch, overwritten each time):
#    logs/latest.log               -- full current session log
#    logs/startup_report.txt       -- last VERIFY_STARTUP output (PASS/FAIL/WARN)
#    logs/gold_flow_atr.dat        -- current ATR seed state
#    logs/gold_flow_atr_backup.dat -- belt-and-suspenders ATR copy
#    logs/gold_stack_state.dat     -- vol regime baseline
#    logs/session_snapshot.txt     -- human-readable engine state summary (Claude reads this)
#
#  Called: after DEPLOY_OMEGA.ps1, after VERIFY_STARTUP.ps1, or manually
#  Manual: powershell -File C:\Omega\push_log.ps1 -RepoRoot C:\Omega
# ==============================================================================

param(
    [string]$RepoRoot = "C:\Omega"
)

$ErrorActionPreference = "Continue"
Set-Location $RepoRoot

# ------------------------------------------------------------------------------
# 1. Copy today's log -> latest.log
# ------------------------------------------------------------------------------
$today    = Get-Date -Format "yyyy-MM-dd"
$logPath  = "$RepoRoot\logs\omega_$today.log"
$destPath = "$RepoRoot\logs\latest.log"

if (-not (Test-Path $logPath)) { $logPath = "$RepoRoot\logs\omega.log" }
if (-not (Test-Path $logPath)) { $logPath = "$RepoRoot\logs\latest.log" }

if (Test-Path $logPath) {
    if ($logPath -ne $destPath) { Copy-Item $logPath $destPath -Force }
    Write-Host "[push_log] latest.log ready" -ForegroundColor Cyan
} else {
    Write-Host "[push_log] WARNING: No log file found" -ForegroundColor Yellow
}

# ------------------------------------------------------------------------------
# 2. Write session_snapshot.txt  (the file Claude reads to parse engine state)
# ------------------------------------------------------------------------------
$snapshotPath = "$RepoRoot\logs\session_snapshot.txt"

try {
    $snap = [System.Collections.Generic.List[string]]::new()
    $snap.Add("OMEGA SESSION SNAPSHOT")
    $snap.Add("Generated : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') UTC")
    $snap.Add("=" * 60)

    # ATR dat file
    $atrDat = "$RepoRoot\logs\gold_flow_atr.dat"
    if (Test-Path $atrDat) {
        $atrContent = (Get-Content $atrDat -Raw).Trim()
        $snap.Add("[ATR-DAT] $atrContent")
        if ($atrContent -match "atr=([\d.]+)") {
            $atrVal = [double]$Matches[1]
            if     ($atrVal -le 2.0) { $snap.Add("[ATR-WARN] atr=$atrVal AT FLOOR -- check seed") }
            elseif ($atrVal -le 5.0) { $snap.Add("[ATR-WARN] atr=$atrVal <= 5.0 -- may still be floor-pinned") }
            else                     { $snap.Add("[ATR-OK]   atr=$atrVal healthy") }
        }
    } else {
        $snap.Add("[ATR-DAT] MISSING")
    }

    $snap.Add("")

    # Parse last 500 lines of latest.log
    if (Test-Path $destPath) {
        $log = Get-Content $destPath -Tail 500 -ErrorAction SilentlyContinue

        $keys = @(
            @{label="[DIAG]       "; pattern="OMEGA-DIAG.*PnL"},
            @{label="[REGIME]     "; pattern="GOLD-REGIME\]"},
            @{label="[VOL]        "; pattern="GOLD-VOL\]"},
            @{label="[GOLD-DIAG]  "; pattern="GOLD-DIAG\]"},
            @{label="[BRK-DIAG]   "; pattern="GOLD-BRK-DIAG\]"},
            @{label="[L2]         "; pattern="L2-STATUS\]"},
            @{label="[SUPERVISOR] "; pattern="SUPERVISOR-XAUUSD\]"},
            @{label="[SEED]       "; pattern="GFE-SEED\]"},
            @{label="[BAR-INFO]   "; pattern="GF-BAR-INFO"},
            @{label="[VIX]        "; pattern="TICK.*VIX\.F"},
            @{label="[GOLD-PRICE] "; pattern="TICK.*XAUUSD"}
        )
        foreach ($k in $keys) {
            $line = $log | Where-Object { $_ -match $k.pattern } | Select-Object -Last 1
            if ($line) { $snap.Add("$($k.label) $($line.Trim())") }
        }

        # Last 3 GFE messages
        $gfeMsgs = $log | Where-Object { $_ -match "\[GFE\]" } | Select-Object -Last 3
        foreach ($m in $gfeMsgs) { $snap.Add("[GFE]         $($m.Trim())") }

        $snap.Add("")

        # Gate block counts
        $noRoom  = @($log | Where-Object { $_ -match "NO_ROOM_TO_TARGET" }).Count
        $compVol = @($log | Where-Object { $_ -match "COMPRESSION_NO_VOL" }).Count
        $barBlk  = @($log | Where-Object { $_ -match "GF-BAR-BLOCK" }).Count
        $sprZ    = @($log | Where-Object { $_ -match "SPREAD-Z.*anomalous" }).Count
        $snap.Add("[GATE-COUNTS-LAST500] NO_ROOM=$noRoom COMP_NO_VOL=$compVol BAR_BLOCK=$barBlk SPREAD_Z=$sprZ")

        # Last 5 gate blocks with ATR values
        $gates = $log | Where-Object { $_ -match "GF-GATE-BLOCK" } | Select-Object -Last 5
        foreach ($g in $gates) { $snap.Add("[GATE]        $($g.Trim())") }

        $snap.Add("")
    }

    # Embed the startup report
    $rptPath = "$RepoRoot\logs\startup_report.txt"
    if (Test-Path $rptPath) {
        $snap.Add("[STARTUP-REPORT-BEGIN]")
        Get-Content $rptPath | ForEach-Object { $snap.Add("  $_") }
        $snap.Add("[STARTUP-REPORT-END]")
    }

    $snap.Add("END-OF-SNAPSHOT")
    $snap | Out-File -FilePath $snapshotPath -Encoding utf8 -Force
    Write-Host "[push_log] session_snapshot.txt written ($($snap.Count) lines)" -ForegroundColor Cyan
} catch {
    Write-Host "[push_log] WARNING: snapshot write failed -- $_" -ForegroundColor Yellow
}

# ------------------------------------------------------------------------------
# 3. Backup ATR dat
# ------------------------------------------------------------------------------
$atrSrc = "$RepoRoot\logs\gold_flow_atr.dat"
$atrBak = "$RepoRoot\logs\gold_flow_atr_backup.dat"
if (Test-Path $atrSrc) {
    Copy-Item $atrSrc $atrBak -Force
    Write-Host "[push_log] ATR backup written" -ForegroundColor Cyan
}

# ------------------------------------------------------------------------------
# 4. Git add all tracked state files
# ------------------------------------------------------------------------------
$track = @(
    "logs/latest.log",
    "logs/startup_report.txt",
    "logs/session_snapshot.txt",
    "logs/gold_flow_atr.dat",
    "logs/gold_flow_atr_backup.dat",
    "logs/gold_stack_state.dat",
    "logs/day_results.csv"
)

foreach ($f in $track) {
    $fp = "$RepoRoot\$($f -replace '/', '\')"
    if (Test-Path $fp) { git add -f $f 2>&1 | Out-Null }
}

git add -f "logs/kelly/*.csv" 2>&1 | Out-Null

$staged = git diff --cached --name-only 2>&1
if (-not $staged) {
    Write-Host "[push_log] No changes -- nothing to push" -ForegroundColor Yellow
    exit 0
}

$hash    = (git rev-parse --short HEAD 2>$null).Trim()
$ts      = Get-Date -Format "yyyy-MM-dd HH:mm"
$message = "state: log+snapshot+atr $hash $ts"

git commit -m $message 2>&1 | Out-Null
# Pull before push -- VPS may be behind if Claude pushed a fix between log pushes
git pull --rebase origin main 2>&1 | Out-Null
git push origin main    2>&1

Write-Host "[push_log] Pushed: log + snapshot + ATR state" -ForegroundColor Green
