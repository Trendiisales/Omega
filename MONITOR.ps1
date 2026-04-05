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

$LogFile = "$OmegaDir\logs\latest.log"

if (-not (Test-Path $LogFile)) {
    Write-Host "Log not found: $LogFile" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA MONITOR  |  Ctrl+C to stop" -ForegroundColor Cyan
Write-Host "   Log: $LogFile" -ForegroundColor DarkGray
if ($Filter) { Write-Host "   Filter: $Filter" -ForegroundColor Yellow }
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
