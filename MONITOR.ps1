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

# ── RUNNING HASH -- always shown, never ask for it again ────────────────────
$VerFile  = "$OmegaDir\include\version_generated.hpp"
$RunHash  = "unknown"
$RunMode  = "unknown"
$RunTime  = "unknown"
if (Test-Path $VerFile) {
    $verContent = Get-Content $VerFile -Raw
    if ($verContent -match '"([a-f0-9]{7,})"') { $RunHash = $matches[1] }
}
$CfgFile = "$OmegaDir\omega_config.ini"
if (Test-Path $CfgFile) {
    $modeMatch = Select-String -Path $CfgFile -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
    if ($modeMatch) { $RunMode = $modeMatch.Matches[0].Groups[1].Value }
}
if (Test-Path "$OmegaDir\Omega.exe") {
    $RunTime = (Get-Item "$OmegaDir\Omega.exe").LastWriteTime.ToUniversalTime().ToString("yyyy-MM-dd HH:mm UTC")
}
$modeColor = if ($RunMode -eq "LIVE") { "Red" } elseif ($RunMode -eq "SHADOW") { "Yellow" } else { "Cyan" }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA MONITOR  |  Ctrl+C to stop" -ForegroundColor Cyan
Write-Host "   Log: $LogFile" -ForegroundColor DarkGray
Write-Host "   HASH: $RunHash  |  BUILT: $RunTime" -ForegroundColor White
Write-Host "   MODE: $RunMode  |  GUI: http://185.167.119.59:7779" -ForegroundColor $modeColor
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

# ── FIX CONNECTION HEALTH CHECK ──────────────────────────────────────────────
# SSL error 6 drops happen every ~3 minutes from BlackBull broker keepalive.
# Each drop + warmup = ~17 seconds of blocked entries.
# Over a 12hr day = 68+ minutes of invisible blackout if not monitored.
# This check reads today's log and reports drop count, last drop time,
# and estimated total blackout time so it is NEVER invisible again.
Write-Host ""
$TodayLog = "$OmegaDir\logs\omega_$(Get-Date -Format 'yyyy-MM-dd').log"
if (Test-Path $TodayLog) {
    $sslDrops    = @(Select-String -Path $TodayLog -Pattern "SSL error|Disconnected.*reconnecting" -ErrorAction SilentlyContinue)
    $dropCount   = $sslDrops.Count
    $lastDrop    = ""
    if ($dropCount -gt 0) {
        $lastLine = $sslDrops[-1].Line
        if ($lastLine -match "^(\d{2}:\d{2}:\d{2})") { $lastDrop = $matches[1] }
    }
    # Estimate blackout: each drop = reconnect(7s) + warmup(10s)
    $blackoutMin = [Math]::Round($dropCount * 17 / 60, 1)

    if ($dropCount -gt 5) {
        Write-Host "  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        Write-Host "  *** FIX DROPS: $dropCount today | ~${blackoutMin}min BLACKOUT ***" -ForegroundColor Red
        Write-Host "  Last drop: $lastDrop | Entries BLOCKED during each drop" -ForegroundColor Red
        Write-Host "  CAUSE: BlackBull SSL keepalive timeout every ~3 minutes" -ForegroundColor Yellow
        Write-Host "  FIX NOW: Set connection_warmup_sec=0 in omega_config.ini" -ForegroundColor Yellow
        Write-Host "  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
    } elseif ($dropCount -gt 0) {
        Write-Host "  [FIX] $dropCount reconnect(s) today | ~${blackoutMin}min blackout | Last: $lastDrop" -ForegroundColor Yellow
    } else {
        Write-Host "  [FIX] Connection stable -- 0 SSL drops today" -ForegroundColor Green
    }
} else {
    Write-Host "  [FIX] No log for today -- Omega may not be running" -ForegroundColor Yellow
}
Write-Host "=======================================================" -ForegroundColor Cyan

# ── FEED-STALE CHECK ─────────────────────────────────────────────────────────
if (Test-Path $TodayLog) {
    $lastStale    = @(Select-String -Path $TodayLog -Pattern "FEED-STALE.*entries BLOCKED" -ErrorAction SilentlyContinue)
    $lastRestored = @(Select-String -Path $TodayLog -Pattern "RESTORED|UNBLOCKED" -ErrorAction SilentlyContinue)
    if ($lastStale.Count -gt $lastRestored.Count) {
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        Write-Host "  *** FEED-STALE *** GOLDFLOW ENTRIES BLOCKED ***" -ForegroundColor Red
        Write-Host "  XAUUSD depth starved -- will auto-clear in <60s" -ForegroundColor Yellow
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
    } elseif ($lastStale.Count -gt 0) {
        Write-Host "  [FEED-STALE] $($lastStale.Count) starvation event(s) today -- all recovered" -ForegroundColor Green
    } else {
        Write-Host "  [FEED-STALE] Clean -- no starvation today" -ForegroundColor Green
    }
}
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# Color rules -- checked in order, first match wins
$rules = @(
    # CRITICAL -- red
    @{ pattern = "ERROR|FAIL|CRASH|DEAD|ALERT|BLOCKED|REJECTED|SL_HIT|FORCE_CLOSE|HARD_STOP|L2_DEAD|L2-WATCHDOG.*DEAD|GF-GATE-BLOCK|GF-OUTER-BLOCK|GFE-SPREAD-BLOCK|GFE-ASIA-ATR-BLOCK|GFE-ASIA-RATIO-BLOCK|GFE-EXHAUST-BLOCK|GFE-DIST-BLOCK|GFE-RECENCY-BLOCK|GFE-RSI-BLOCK|TREND-BLOCK|GFE-FADE-BLOCK|GFE-DOMINANCE-BLOCK|FEED-STALE.*BLOCKED"; color = "Red" },
    # TRADE EVENTS -- bright
    @{ pattern = "\[GFE\] ENTRY|\[GFE\] EXIT|PARTIAL_1R|PARTIAL_2R|TRAIL_HIT|TP_HIT|TRADE_OPEN|TRADE_CLOSE|LIVE.*ENTRY|LIVE.*EXIT|GOLD-FLOW.*ENTRY|GF-RELOAD.*ENTRY|GF-ADDON.*ENTRY"; color = "Yellow" },
    # L2 / CTRADER -- cyan
    @{ pattern = "Account 43014358 authorized|L2-WATCHDOG.*ALIVE|l2_imb=[^0]|CTRADER.*authorized|Depth feed ACTIVE|L2.*live"; color = "Cyan" },
    # WARNINGS -- orange-ish
    @{ pattern = "WARN|warn|shadow|SHADOW|TIMEOUT|IMM_REVERSAL|STALE|RESUB|reconnect"; color = "DarkYellow" },
    # SESSION / REGIME -- green
    @{ pattern = "LOGON ACCEPTED|session=ACTIVE|EXPANSION_BREAKOUT|TREND_CONTINUATION|BREAKOUT|regime=|DRIFT-RESET|ASIA-RSI-SNAP|GFE-PERSIST-RESET|GFE-REVERSAL-BYPASS|GFE-COMP-BYPASS|GF-COMP-BYPASS|GF-VEL-REENTRY|FEED-STALE.*RESTORED|FEED-STALE.*UNBLOCKED"; color = "Green" },
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
