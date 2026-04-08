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

# ── HASH vs HEAD CHECK -- catches stale binary immediately ───────────────────
$ErrorActionPreference = "Continue"
$gitHeadFull = & git -C $OmegaDir rev-parse HEAD 2>$null
$gitHead7 = if ($gitHeadFull -and $gitHeadFull.Length -ge 7) { $gitHeadFull.Substring(0,7) } else { "unknown" }
$ErrorActionPreference = "Continue"
if ($RunHash -eq "unknown" -or $gitHead7 -eq "unknown") {
    Write-Host "   HASH CHECK: could not verify (git or version_generated.hpp missing)" -ForegroundColor Yellow
} elseif ($RunHash -eq $gitHead7) {
    Write-Host "   HASH CHECK: OK -- binary matches HEAD ($RunHash)" -ForegroundColor Green
} else {
    Write-Host "   !! HASH MISMATCH !! running=$RunHash HEAD=$gitHead7" -ForegroundColor Red
    Write-Host "   !! BINARY IS STALE -- run QUICK_RESTART.ps1 NOW !!" -ForegroundColor Red
}
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

# Check L2 tick file and read live imbalance from it
if (Test-Path $L2TickFile) {
    $l2Lines = (Get-Content $L2TickFile | Measure-Object -Line).Lines
    $l2Age   = [int]((Get-Date) - (Get-Item $L2TickFile).LastWriteTime).TotalSeconds
    # Read last 5 data rows to get current imbalance value
    $l2LastRows  = Get-Content $L2TickFile | Where-Object { $_ -match '^[0-9]' } | Select-Object -Last 5
    $l2ImbValues = @()
    foreach ($row in $l2LastRows) {
        $cols = $row -split ','
        if ($cols.Count -ge 4) {
            try { $l2ImbValues += [double]$cols[3] } catch {}
        }
    }
    $l2ImbDisplay = if ($l2ImbValues.Count -gt 0) {
        $latest = $l2ImbValues[-1]
        $signal = if ($latest -gt 0.75) { "BID-HEAVY (LONG)" } `
                  elseif ($latest -lt 0.25) { "ASK-HEAVY (SHORT)" } `
                  elseif ([Math]::Abs($latest - 0.5) -lt 0.02) { "NEUTRAL" } `
                  else { "MILD" }
        "imb=$([math]::Round($latest,3)) [$signal]"
    } else { "no data" }

    if ($l2Lines -gt 1 -and $l2Age -lt 120) {
        $imbColor = if ($l2ImbValues.Count -gt 0 -and [Math]::Abs($l2ImbValues[-1] - 0.5) -lt 0.001) { "Yellow" } else { "Green" }
        Write-Host "  [L2 LOG] $l2Lines rows age=${l2Age}s  $l2ImbDisplay" -ForegroundColor $imbColor
        if ($l2ImbValues.Count -gt 0 -and [Math]::Abs($l2ImbValues[-1] - 0.5) -lt 0.001) {
            Write-Host "  [L2 WARN] imbalance=0.500 -- check [CTRADER-L2-CHECK] in log for bid_lvls/ask_lvls" -ForegroundColor Yellow
        }
    } elseif ($l2Lines -le 1) {
        Write-Host "  [L2 LOG] WARNING: $L2TickFile exists but has no data rows" -ForegroundColor Yellow
    } else {
        Write-Host "  [L2 LOG] STALE ${l2Age}s  $l2ImbDisplay" -ForegroundColor Red
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
Write-Host ""

# ── GOLDFLOW BLOCKING GATES CHECK ────────────────────────────────────────────
if (Test-Path $TodayLog) {
    # ENGINE_CULLED
    $culledLine = @(Select-String -Path $TodayLog -Pattern "ENGINE_CULLED" -ErrorAction SilentlyContinue)
    if ($culledLine.Count -gt 0) {
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        Write-Host "  *** GoldFlow ENGINE CULLED *** 4 SL_HITs -- BLOCKED all day ***" -ForegroundColor Red
        Write-Host "  Resets at midnight UTC" -ForegroundColor Yellow
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
    }
    # BARS_PERMANENTLY_UNAVAILABLE
    $barsLine = @(Select-String -Path $TodayLog -Pattern "BARS_PERMANENTLY_UNAVAILABLE" -ErrorAction SilentlyContinue)
    if ($barsLine.Count -gt 0) {
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        Write-Host "  *** BARS PERMANENTLY UNAVAILABLE *** GoldFlow blind ***" -ForegroundColor Red
        Write-Host "  Restart Omega to retry bar seeding" -ForegroundColor Yellow
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
    }
}
Write-Host ""

# Color rules -- checked in order, first match wins
$rules = @(
    # CRITICAL -- red
    @{ pattern = "ERROR|FAIL|CRASH|DEAD|ALERT|BLOCKED|REJECTED|SL_HIT|FORCE_CLOSE|HARD_STOP|L2_DEAD|L2-WATCHDOG.*DEAD|GF-GATE-BLOCK|GF-OUTER-BLOCK|GFE-SPREAD-BLOCK|GFE-ASIA-ATR-BLOCK|GFE-ASIA-RATIO-BLOCK|GFE-EXHAUST-BLOCK|GFE-DIST-BLOCK|GFE-RECENCY-BLOCK|GFE-RSI-BLOCK|TREND-BLOCK|GFE-FADE-BLOCK|GFE-DOMINANCE-BLOCK"; color = "Red" },
    # TRADE EVENTS -- bright
    @{ pattern = "\[GFE\] ENTRY|\[GFE\] EXIT|PARTIAL_1R|PARTIAL_2R|TRAIL_HIT|TP_HIT|TRADE_OPEN|TRADE_CLOSE|LIVE.*ENTRY|LIVE.*EXIT|GOLD-FLOW.*ENTRY|GF-RELOAD.*ENTRY|GF-ADDON.*ENTRY"; color = "Yellow" },
    # L2 / CTRADER -- cyan
    @{ pattern = "Account 43014358 authorized|L2-WATCHDOG.*ALIVE|CTRADER-L2-CHECK|CTRADER.*authorized|Depth feed ACTIVE|L2.*live|GOLD-L2-LIVE"; color = "Cyan" },
    # L2 imbalance stuck neutral -- yellow warning
    @{ pattern = "imb=0\.500|imb_level=0\.500|ALL NEUTRAL|NEUTRAL.*imb"; color = "Yellow" },
    # WARNINGS -- orange-ish
    @{ pattern = "WARN|warn|shadow|SHADOW|TIMEOUT|IMM_REVERSAL|STALE|RESUB|reconnect"; color = "DarkYellow" },
    # SESSION / REGIME -- green
    @{ pattern = "LOGON ACCEPTED|session=ACTIVE|EXPANSION_BREAKOUT|TREND_CONTINUATION|BREAKOUT|regime=|DRIFT-RESET|ASIA-RSI-SNAP|GFE-PERSIST-RESET|GFE-REVERSAL-BYPASS|GFE-COMP-BYPASS|GF-COMP-BYPASS|GF-VEL-REENTRY"; color = "Green" },
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
