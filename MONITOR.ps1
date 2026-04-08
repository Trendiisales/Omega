#Requires -Version 5.1
# ==============================================================================
#  OMEGA -- LIVE LOG MONITOR
# ==============================================================================
#  Tails C:\Omega\logs\latest.log with color highlighting.
#  Press Ctrl+C to stop monitoring (Omega keeps running).
#
#  USAGE:
#    .\MONITOR.ps1              # monitor latest.log
#    .\MONITOR.ps1 -Tail 100    # show last 100 lines then follow
#    .\MONITOR.ps1 -Filter gold # only show lines containing "gold" (case-insensitive)
# ==============================================================================

param(
    [string] $OmegaDir = "C:\Omega",
    [int]    $Tail     = 30,
    [string] $Filter   = ""
)

# Resolve live log: prefer latest.log if fresh (<60s), else fall back to today's dated log
$LogFile   = "$OmegaDir\logs\latest.log"
$DatedLog  = "$OmegaDir\logs\omega_$(Get-Date -Format 'yyyy-MM-dd').log"

if (Test-Path $LogFile) {
    $age = (Get-Date) - (Get-Item $LogFile).LastWriteTime
    if ($age.TotalSeconds -gt 60 -and (Test-Path $DatedLog)) {
        Write-Host "  [WARN] latest.log stale ($([int]$age.TotalSeconds)s) -- using dated log instead" -ForegroundColor Yellow
        $LogFile = $DatedLog
    }
} elseif (Test-Path $DatedLog) {
    Write-Host "  [WARN] latest.log not found -- using dated log" -ForegroundColor Yellow
    $LogFile = $DatedLog
} else {
    Write-Host "Log not found: $LogFile" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA MONITOR  |  Ctrl+C to stop" -ForegroundColor Cyan
Write-Host "   Log: $LogFile" -ForegroundColor DarkGray
if ($Filter) { Write-Host "   Filter: $Filter" -ForegroundColor Yellow }
Write-Host "=======================================================" -ForegroundColor Cyan

# ── L2 STATUS CHECK -- NON-NEGOTIABLE ────────────────────────────────────────
# L2/DOM data from cTrader Open API (ctid=43014358) is the basis of GoldFlow.
# If L2_ALERT.txt contains L2_DEAD, ALL GoldFlow entries are blocked.
# This banner must be visible every time MONITOR starts.
$L2AlertFile = "$OmegaDir\logs\L2_ALERT.txt"
$L2TickFile  = "$OmegaDir\logs\l2_ticks_$(Get-Date -Format 'yyyy-MM-dd').csv"

Write-Host ""
if (Test-Path $L2AlertFile) {
    $alertContent = Get-Content $L2AlertFile -Raw
    if ($alertContent -match "L2_DEAD") {
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        Write-Host "  *** L2 DEAD *** cTrader DOM DATA NOT FLOWING ***" -ForegroundColor Red
        Write-Host "  GoldFlow entries are BLOCKED -- NO TRADES FIRING" -ForegroundColor Red
        Write-Host "  ctid=43014358 must deliver depth events" -ForegroundColor Red
        Write-Host "  Fix: restart Omega or check cTrader Open API" -ForegroundColor Red
        # Show diagnosis if available
        if ($alertContent -match "diagnosis=(.+)") { Write-Host "  Diagnosis: $($matches[1])" -ForegroundColor Yellow }
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
    } else {
        Write-Host "  [L2] OK -- cTrader depth flowing" -ForegroundColor Green
    }
} else {
    Write-Host "  [L2] No alert file -- status unknown (Omega may not have started yet)" -ForegroundColor Yellow
}

# Check if today's L2 tick file exists and has data
if (Test-Path $L2TickFile) {
    $l2Lines = (Get-Content $L2TickFile | Measure-Object -Line).Lines
    $l2Age   = [int]((Get-Date) - (Get-Item $L2TickFile).LastWriteTime).TotalSeconds
    if ($l2Lines -gt 1 -and $l2Age -lt 120) {
        Write-Host "  [L2 LOG] $L2TickFile -- $l2Lines rows, last write ${l2Age}s ago" -ForegroundColor Green
    } elseif ($l2Lines -le 1) {
        Write-Host "  [L2 LOG] WARNING: $L2TickFile exists but has no data rows" -ForegroundColor Yellow
    } else {
        Write-Host "  [L2 LOG] WARNING: $L2TickFile stale -- last write ${l2Age}s ago" -ForegroundColor Yellow
    }
} else {
    Write-Host "  [L2 LOG] NO L2 TICK FILE TODAY: $L2TickFile" -ForegroundColor Red
    Write-Host "  [L2 LOG] L2 data is not being saved -- check Omega is running" -ForegroundColor Red
}
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# Color rules -- checked in order, first match wins
$rules = @(
    # CRITICAL -- red
    @{ pattern = "ERROR|FAIL|CRASH|DEAD|ALERT|BLOCKED|REJECTED|SL_HIT|FORCE_CLOSE|HARD_STOP|L2_DEAD|L2-WATCHDOG.*DEAD"; color = "Red" },
    # TRADE EVENTS -- bright
    @{ pattern = "\[GFE\] ENTRY|\[GFE\] EXIT|PARTIAL_1R|PARTIAL_2R|TRAIL_HIT|TP_HIT|TRADE_OPEN|TRADE_CLOSE|LIVE.*ENTRY|LIVE.*EXIT"; color = "Yellow" },
    # L2 / CTRADER -- cyan
    @{ pattern = "Account 43014358 authorized|L2-WATCHDOG.*ALIVE|l2_imb=[^0]|CTRADER.*authorized|Depth feed ACTIVE|L2.*live"; color = "Cyan" },
    # WARNINGS -- orange-ish
    @{ pattern = "WARN|warn|shadow|SHADOW|TIMEOUT|IMM_REVERSAL|STALE|RESUB|reconnect"; color = "DarkYellow" },
    # SESSION / REGIME -- green
    @{ pattern = "LOGON ACCEPTED|session=ACTIVE|EXPANSION_BREAKOUT|TREND_CONTINUATION|BREAKOUT|regime="; color = "Green" },
    # STARTUP -- magenta
    @{ pattern = "\[OMEGA\] version=|\[CTRADER\] Depth client started|\[CTRADER\] Account|\[STARTUP\]|\[CONFIG\]"; color = "Magenta" },
    # DEFAULT
    @{ pattern = ".*"; color = "Gray" }
)

function Get-LineColor($line) {
    foreach ($rule in $rules) {
        if ($line -match $rule.pattern) { return $rule.color }
    }
    return "Gray"
}

# Stream log with color
Get-Content $LogFile -Tail $Tail -Wait | ForEach-Object {
    $line = $_
    if ($Filter -and $line -notmatch $Filter) { return }
    $color = Get-LineColor $line
    Write-Host $line -ForegroundColor $color
}
