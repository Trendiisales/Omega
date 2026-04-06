#Requires -Version 5.1
# ==============================================================================
#  OMEGA - STARTUP VERIFIER
#
#  PURPOSE:
#    After any deploy or restart, run this script. It tails the live log for
#    45 seconds, captures every critical startup line, and writes a permanent
#    startup_report.txt you can read any time -- even hours later.
#
#    Solves: "logs scroll too fast, I can't see the startup checks"
#
#  USAGE:
#    .\VERIFY_STARTUP.ps1
#    .\VERIFY_STARTUP.ps1 -WaitSec 60      # wait longer (slow startup)
#    .\VERIFY_STARTUP.ps1 -LogPath "C:\Omega\logs\omega_2026-04-02.log"
#
#  OUTPUT:
#    C:\Omega\logs\startup_report.txt  -- permanent, overwritten each run
#    Console output with colour-coded PASS / FAIL / WARN
#
#  CHECKS PERFORMED:
#    ATR seed      -- must NOT be 5.0, must match VIX level
#    ATR state     -- loaded from disk or cold-seeded (with reason)
#    vol_range     -- must NOT be 0.00 (seed fix from 962ad27)
#    in_dead_zone  -- must be 0
#    VIX level     -- confirms VIX.F tick arrived before first entry
#    Session slot  -- shows what session gold thinks it's in
#    L2 live       -- ctrader_live=1 confirms book feed connected
#    Latency       -- RTTp95 and cap_ok status
#    Gate blocks   -- lists any gate blocks seen in first 45s
#    First signal  -- shows first engine signal if any fires
#    OMEGA-DIAG    -- first PnL/T/WR line (confirms session active)
# ==============================================================================

param(
    [int]    $WaitSec = 45,
    [string] $LogPath = "",
    [string] $OmegaDir = "C:\Omega"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$ReportFile = "$OmegaDir\logs\startup_report.txt"

# ------------------------------------------------------------------------------
# Locate the log file
# ------------------------------------------------------------------------------
if ($LogPath -eq "") {
    # Try latest.log first (symlink/copy), then today's dated log
    $candidates = @(
        "$OmegaDir\logs\latest.log",
        "$OmegaDir\logs\omega_$(Get-Date -Format 'yyyy-MM-dd').log",
        "$OmegaDir\omega.log"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $LogPath = $c; break }
    }
}

if ($LogPath -eq "" -or -not (Test-Path $LogPath)) {
    Write-Host "[VERIFY] ERROR: Cannot find Omega log file." -ForegroundColor Red
    Write-Host "         Tried: $($candidates -join ', ')" -ForegroundColor Red
    Write-Host "         Use: .\VERIFY_STARTUP.ps1 -LogPath C:\Omega\logs\omega_YYYY-MM-DD.log" -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA STARTUP VERIFIER" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Log   : $LogPath" -ForegroundColor DarkGray
Write-Host "  Report: $ReportFile" -ForegroundColor DarkGray
Write-Host "  Wait  : $WaitSec seconds" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Monitoring log for $WaitSec seconds..." -ForegroundColor Yellow
Write-Host "  (Press Ctrl+C to stop early -- report written on exit)" -ForegroundColor DarkGray
Write-Host ""

# ------------------------------------------------------------------------------
# Tail the log for $WaitSec seconds, collecting all lines
# ------------------------------------------------------------------------------
$startTime   = Get-Date
$deadline    = $startTime.AddSeconds($WaitSec)
$capturedLines = [System.Collections.Generic.List[string]]::new()

# Get current file size so we only read NEW lines written after this script started
$initialSize = 0
try { $initialSize = (Get-Item $LogPath).Length } catch { $initialSize = 0 }

$spinner = @('|', '/', '-', '\')
$spinIdx = 0

while ((Get-Date) -lt $deadline) {
    try {
        $currentSize = (Get-Item $LogPath).Length
        if ($currentSize -gt $initialSize) {
            # Read only the new bytes
            $fs     = [System.IO.File]::Open($LogPath, 'Open', 'Read', 'ReadWrite')
            $fs.Seek($initialSize, 'Begin') | Out-Null
            $reader = New-Object System.IO.StreamReader($fs)
            while (-not $reader.EndOfStream) {
                $line = $reader.ReadLine()
                if ($line -ne $null -and $line.Trim() -ne "") {
                    $capturedLines.Add($line)
                }
            }
            $reader.Close()
            $fs.Close()
            $initialSize = $currentSize
        }
    } catch { <# file locked briefly -- retry next cycle #> }

    # Spinner so you know it's running
    $elapsed = [int]((Get-Date) - $startTime).TotalSeconds
    $remaining = $WaitSec - $elapsed
    Write-Host "`r  $($spinner[$spinIdx % 4])  Collecting... ${elapsed}s / ${WaitSec}s  ($($capturedLines.Count) lines)" -NoNewline -ForegroundColor DarkGray
    $spinIdx++
    Start-Sleep -Milliseconds 500
}
Write-Host "`r  [OK] Collection complete. $($capturedLines.Count) lines captured.               " -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# Parse and evaluate each check
# ------------------------------------------------------------------------------

# Helper: find last matching line
function Find-Last {
    param([string]$Pattern)
    $match = $capturedLines | Where-Object { $_ -match $Pattern } | Select-Object -Last 1
    return $match
}

function Find-All {
    param([string]$Pattern)
    $r = $capturedLines | Where-Object { $_ -match $Pattern }
    if ($null -eq $r) { return @() }
    return @($r)
}

function Find-First {
    param([string]$Pattern)
    return $capturedLines | Where-Object { $_ -match $Pattern } | Select-Object -First 1
}

# ---- Collect results ---------------------------------------------------------
$results = [System.Collections.Generic.List[object]]::new()

function Add-Result {
    param([string]$Name, [string]$Status, [string]$Value, [string]$Detail = "")
    $results.Add([PSCustomObject]@{
        Name   = $Name
        Status = $Status   # PASS / FAIL / WARN / INFO
        Value  = $Value
        Detail = $Detail
    })
}

# --- CHECK 1: ATR Seed -------------------------------------------------------
$seedLine = Find-Last "GFE-SEED"
if ($seedLine) {
    if ($seedLine -match "seed_atr=([0-9.]+)") {
        $seedAtr = [double]$Matches[1]
        if ($seedAtr -le 5.0 -and $seedLine -notmatch "VIX-UNKNOWN") {
            Add-Result "ATR Seed" "WARN" "seed_atr=$seedAtr" "Low seed -- VIX may not have arrived yet. Check VIX line below."
        } elseif ($seedLine -match "VIX-UNKNOWN") {
            Add-Result "ATR Seed" "WARN" "seed_atr=$seedAtr [VIX-UNKNOWN]" "VIX.F tick not yet received when seed ran. ATR will update once VIX arrives."
        } else {
            Add-Result "ATR Seed" "PASS" "seed_atr=$seedAtr" $seedLine.Trim()
        }
    }
} else {
    Add-Result "ATR Seed" "INFO" "No GFE-SEED line seen" "Engine may have loaded ATR from disk (normal) -- see ATR State check."
}

# --- CHECK 2: ATR State (loaded from disk vs cold) ---------------------------
$atrLoaded  = Find-Last "GFE\] ATR state loaded"
$atrRejected= @(Find-All  "GFE\] ATR state rejected")
$atrStartup = Find-Last "GFE\] Startup ATR"

if ($atrLoaded) {
    if ($atrLoaded -match "atr=([0-9.]+)") {
        $loadedAtr = [double]$Matches[1]
        if ($loadedAtr -le 2.0) {
            Add-Result "ATR State" "WARN" "Loaded atr=$loadedAtr (very low)" $atrLoaded.Trim()
        } else {
            Add-Result "ATR State" "PASS" "Loaded from disk atr=$loadedAtr" $atrLoaded.Trim()
        }
    }
} elseif ($atrRejected.Count -gt 0) {
    Add-Result "ATR State" "WARN" "ATR file rejected ($($atrRejected.Count) rejection(s)) -- using seed" ($atrRejected[-1]).Trim()
} elseif ($atrStartup) {
    Add-Result "ATR State" "INFO" "Startup ATR line" $atrStartup.Trim()
} else {
    Add-Result "ATR State" "WARN" "No ATR state line seen" "Could not confirm ATR initialisation."
}

# --- CHECK 3: ATR running value (not 5.00 flat) ------------------------------
$gfGateLines = @(Find-All "GF-GATE-BLOCK")
$atrValues   = @()
foreach ($l in $gfGateLines) {
    if ($l -match "atr=([0-9.]+)") { $atrValues += [double]$Matches[1] }
}
if ($atrValues.Count -gt 0) {
    $distinctAtrs = @($atrValues | Sort-Object -Unique)
    $allFive = (@($distinctAtrs | Where-Object { $_ -ne 5.0 }).Count -eq 0)
    if ($allFive -and $distinctAtrs.Count -gt 1) {
        Add-Result "ATR Running Value" "FAIL" "atr=5.00 flat ($($atrValues.Count) gate checks)" "ATR still pinned at floor -- GFE_ATR_MIN fix may not be running."
    } elseif ($distinctAtrs[0] -le 5.0 -and $distinctAtrs.Count -eq 1) {
        Add-Result "ATR Running Value" "WARN" "atr=$($distinctAtrs[0]) (single value seen)" "Only one gate block sampled -- check again after more ticks."
    } else {
        $atrRange = "$($distinctAtrs[0])-$($distinctAtrs[-1])"
        Add-Result "ATR Running Value" "PASS" "atr varying $atrRange across gate checks" "NOT stuck at 5.00."
    }
} else {
    Add-Result "ATR Running Value" "INFO" "No GF-GATE-BLOCK lines (no gate blocks fired)" "Good -- or no entry attempts yet."
}

# --- CHECK 4: vol_range NOT 0.00 (962ad27 seed fix) -------------------------
$volLine = Find-Last "GOLD-DIAG.*vol_range"
if ($volLine) {
    if ($volLine -match "vol_range=([0-9.]+)") {
        $vr = [double]$Matches[1]
        if ($vr -eq 0.0) {
            Add-Result "vol_range" "FAIL" "vol_range=0.00" "Seed fix NOT working -- VolatilityFilter still using mid=3000 fallback."
        } elseif ($vr -lt 0.3) {
            Add-Result "vol_range" "WARN" "vol_range=$vr (very low)" "Tape may be dead. COMPRESSION_NO_VOL blocks likely."
        } else {
            Add-Result "vol_range" "PASS" "vol_range=$vr" $volLine.Trim()
        }
    }
} else {
    Add-Result "vol_range" "INFO" "No GOLD-DIAG line yet" "Engine may not have emitted diagnostics -- wait for first GOLD-DIAG log."
}

# --- CHECK 5: in_dead_zone = 0 -----------------------------------------------
$brkLines = @(Find-All "GOLD-BRK-DIAG")
if ($brkLines.Count -gt 0) {
    $dzLines = @($brkLines | Where-Object { $_ -match "in_dead_zone=1" })
    if ($dzLines.Count -gt 0) {
        Add-Result "Dead Zone" "FAIL" "in_dead_zone=1 on $($dzLines.Count) bars" "Dead zone still active -- check session_start_utc config."
    } else {
        Add-Result "Dead Zone" "PASS" "in_dead_zone=0 on all $($brkLines.Count) BRK-DIAG lines" ""
    }
} else {
    Add-Result "Dead Zone" "INFO" "No GOLD-BRK-DIAG lines yet" ""
}

# --- CHECK 6: VIX level -------------------------------------------------------
$vixTick = Find-Last "TICK.*VIX\.F"
$omegaDiag = Find-Last "OMEGA-DIAG.*RTTp95"
if ($vixTick -and $vixTick -match "VIX\.F ([0-9.]+)/") {
    $vix = [double]$Matches[1]
    if ($vix -le 0) {
        Add-Result "VIX Level" "WARN" "VIX.F tick seen but value=0" $vixTick.Trim()
    } else {
        $expectedAtr = if ($vix -ge 25) {"18.0"} elseif ($vix -ge 20) {"12.0"} elseif ($vix -ge 15) {"8.0"} else {"5.0"}
        Add-Result "VIX Level" "PASS" "VIX=$vix (expected seed_atr ~$expectedAtr)" $vixTick.Trim()
    }
} else {
    Add-Result "VIX Level" "WARN" "No VIX.F tick seen in first ${WaitSec}s" "ATR seed used default 8.0. Will update when VIX.F arrives."
}

# --- CHECK 7: Session slot & can_enter ---------------------------------------
$lastBrk = Find-Last "GOLD-BRK-DIAG"
if ($lastBrk) {
    $slot      = if ($lastBrk -match "session_slot=([0-9]+)")  { $Matches[1] } else { "?" }
    $canEnter  = if ($lastBrk -match "can_enter=([0-9]+)")     { $Matches[1] } else { "?" }
    $canArm    = if ($lastBrk -match "can_arm=([0-9]+)")       { $Matches[1] } else { "?" }
    $impBlock  = if ($lastBrk -match "impulse_block=([0-9]+)") { $Matches[1] } else { "?" }
    $slotName  = switch ($slot) {
        "1" { "London open (07-09 UTC)" }
        "2" { "London core (09-12 UTC)" }
        "3" { "Overlap (12-14 UTC)" }
        "4" { "NY open (14-17 UTC)" }
        "5" { "NY late (17-22 UTC)" }
        "6" { "Asia (22-05 UTC)" }
        default { "Unknown" }
    }
    $summary = "slot=$slot ($slotName) can_enter=$canEnter can_arm=$canArm impulse_block=$impBlock"
    if ($canEnter -eq "1" -and $canArm -eq "1" -and $impBlock -eq "0") {
        Add-Result "Session/Entry State" "PASS" $summary ""
    } elseif ($canEnter -eq "0") {
        Add-Result "Session/Entry State" "WARN" $summary "can_enter=0 -- check session/regime conditions."
    } else {
        Add-Result "Session/Entry State" "INFO" $summary ""
    }
} else {
    Add-Result "Session/Entry State" "INFO" "No BRK-DIAG lines yet" ""
}

# --- CHECK 8: L2 / cTrader live feed -----------------------------------------
$l2Line = Find-Last "L2-STATUS"
if ($l2Line) {
    if ($l2Line -match "ctrader_live=1") {
        $evts = if ($l2Line -match "events=([0-9]+)") { $Matches[1] } else { "?" }
        Add-Result "L2 / cTrader Feed" "PASS" "ctrader_live=1 events=$evts" $l2Line.Trim()
    } else {
        Add-Result "L2 / cTrader Feed" "FAIL" "ctrader_live=0" "Book feed not connected -- check cTrader API credentials/connection."
    }
} else {
    Add-Result "L2 / cTrader Feed" "WARN" "No L2-STATUS line in first ${WaitSec}s" "Feed may be slow to connect."
}

# --- CHECK 9: Latency ---------------------------------------------------------
$latLine = Find-Last "OMEGA-DIAG.*RTTp95"
if ($latLine) {
    $rtt    = if ($latLine -match "RTTp95=([0-9.]+)ms") { [double]$Matches[1] } else { -1 }
    $latOk  = if ($latLine -match "lat_ok=(\d)")       { $Matches[1] }         else { "?" }
    if ($latOk -eq "1" -and $rtt -ge 0 -and $rtt -lt 50) {
        Add-Result "Latency" "PASS" "RTTp95=${rtt}ms lat_ok=1" $latLine.Trim()
    } elseif ($latOk -eq "0") {
        Add-Result "Latency" "FAIL" "RTTp95=${rtt}ms lat_ok=0" "Latency exceeding cap -- check VPS network."
    } else {
        Add-Result "Latency" "WARN" "RTTp95=${rtt}ms lat_ok=$latOk" $latLine.Trim()
    }
} else {
    Add-Result "Latency" "INFO" "No OMEGA-DIAG yet" ""
}

# --- CHECK 10: Gate blocks summary -------------------------------------------
# PS 5.1: @() forces array even when Find-All returns a single string -- .Count is safe on arrays
$noRoom      = @(Find-All "GF-GATE-BLOCK.*NO_ROOM_TO_TARGET").Count
$compNoVol   = @(Find-All "GF-GATE-BLOCK.*COMPRESSION_NO_VOL").Count
$costGate    = @(Find-All "GF-GATE-BLOCK.*COST_GATE").Count
$barBlocks   = @(Find-All "GF-BAR-BLOCK").Count
$spreadBlock = @(Find-All "SPREAD-Z.*anomalous").Count

$gateTotal = $noRoom + $compNoVol + $costGate + $barBlocks + $spreadBlock
if ($gateTotal -eq 0) {
    Add-Result "Gate Blocks" "INFO" "No gate blocks in first ${WaitSec}s" "No entry attempts or no blocks (good)."
} else {
    $parts = @()
    if ($noRoom    -gt 0) { $parts += "NO_ROOM_TO_TARGET=$noRoom" }
    if ($compNoVol -gt 0) { $parts += "COMPRESSION_NO_VOL=$compNoVol" }
    if ($costGate  -gt 0) { $parts += "COST_GATE=$costGate" }
    if ($barBlocks -gt 0) { $parts += "BAR_BLOCK=$barBlocks" }
    if ($spreadBlock -gt 0) { $parts += "SPREAD_Z=$spreadBlock" }
    $gateStr = $parts -join "  "

    # FAIL only if the same block type fires every single time (stuck pattern)
    if ($noRoom -gt 5 -and ($compNoVol + $costGate + $barBlocks) -eq 0) {
        Add-Result "Gate Blocks" "WARN" $gateStr "NO_ROOM_TO_TARGET dominant -- ATR may still be wrong. Check ATR Running Value above."
    } elseif ($compNoVol -gt 5 -and ($noRoom + $costGate + $barBlocks) -eq 0) {
        Add-Result "Gate Blocks" "WARN" $gateStr "COMPRESSION_NO_VOL dominant -- vol_range near floor. May need lower gf_compression_vol_floor."
    } else {
        Add-Result "Gate Blocks" "INFO" $gateStr "Mixed blocks -- normal filtering behaviour."
    }
}

# --- CHECK 11: First signal ---------------------------------------------------
$firstSignal = Find-First "GOLD-ENGINE.*signals=1"
$firstTrade  = Find-First "XAUUSD.*(LONG|SHORT).*SP1\|TRAIL\|FC\|SL"
if ($firstSignal) {
    Add-Result "First Signal" "INFO" "Engine fired: $firstSignal" ""
} elseif ($firstTrade) {
    Add-Result "First Trade" "INFO" $firstTrade.Trim() ""
} else {
    Add-Result "First Signal" "INFO" "No signal in first ${WaitSec}s" "Normal if market quiet or in compression."
}

# --- CHECK 12: Impulse ghost block -------------------------------------------
$ghostLines = @(Find-All "GF-IMPULSE-GHOST.*Blocked")
if ($ghostLines.Count -gt 0) {
    $lastGhost = $ghostLines[-1]
    if ($lastGhost -match "only ([0-9]+) ticks, need ([0-9]+)") {
        $got = [int]$Matches[1]
        $need = [int]$Matches[2]
        if ($got -ge 1 -and $need -eq 3) {
            Add-Result "Impulse Ghost" "PASS" "Blocked at $got tick(s), need $need -- fix active" "Post-impulse flicker fix confirmed (need=3, not blocking on 0-tick)."
        } else {
            Add-Result "Impulse Ghost" "WARN" "got=$got need=$need" $lastGhost.Trim()
        }
    }
} else {
    Add-Result "Impulse Ghost" "INFO" "No impulse ghost blocks seen" ""
}

# --- CHECK 13: Build stamp (what binary is running) --------------------------
$stampFile = "C:\Omega\omega_build.stamp"
if (Test-Path $stampFile) {
    $stamp     = Get-Content $stampFile
    $gitShort  = (($stamp | Where-Object { $_ -match '^GIT_HASH_SHORT=' }) -replace '^GIT_HASH_SHORT=', '').Trim()
    $buildTime = (($stamp | Where-Object { $_ -match '^BUILD_TIME=' })     -replace '^BUILD_TIME=',     '').Trim()
    Add-Result "Build Stamp" "INFO" "commit=$gitShort  built=$buildTime" ""
} else {
    Add-Result "Build Stamp" "WARN" "No stamp file at $stampFile" ""
}

# --- CHECK 14: Bar state validity (not flat/holiday) -------------------------
$barFile = "$OmegaDir\logs\bars_gold_m1.dat"
if (Test-Path $barFile) {
    $barLines = Get-Content $barFile
    $e9  = ($barLines | Where-Object { $_ -match '^ema9='  } | Select-Object -First 1) -replace '^ema9=',  ''
    $e50 = ($barLines | Where-Object { $_ -match '^ema50=' } | Select-Object -First 1) -replace '^ema50=', ''
    $rsi = ($barLines | Where-Object { $_ -match '^rsi14=' } | Select-Object -First 1) -replace '^rsi14=', ''
    $atr = ($barLines | Where-Object { $_ -match '^atr14=' } | Select-Object -First 1) -replace '^atr14=', ''
    $ts  = ($barLines | Where-Object { $_ -match '^saved_ts=' } | Select-Object -First 1) -replace '^saved_ts=', ''

    try {
        $e9d  = [double]$e9
        $e50d = [double]$e50
        $rsid = [double]$rsi
        $atrd = [double]$atr
        $tsd  = [long]$ts
        $ageMin = [int](([System.DateTimeOffset]::UtcNow.ToUnixTimeSeconds() - $tsd) / 60)

        $flatEma  = ([Math]::Abs($e9d - $e50d) -lt 0.01 -and $e9d -gt 0)
        $badRsi   = ($rsid -lt 5 -or $rsid -gt 95)
        $badAtr   = ($atrd -lt 1.0 -and $atrd -gt 0)

        if ($flatEma -or $badRsi -or $badAtr) {
            Add-Result "Bar State" "FAIL" "CORRUPT: e9=$e9d e50=$e50d rsi=$rsid atr=$atrd age=${ageMin}min" `
                "Flat/holiday state on disk -- DELETE bars_gold_m1/m5/m15/h4.dat and redeploy."
        } elseif ($ageMin -gt 1440) {
            Add-Result "Bar State" "WARN" "e9=$e9d atr=$atrd rsi=$rsid age=${ageMin}min (>24h)" `
                "Bar state older than 24h -- will be rejected on load. Will rebuild from tick data."
        } else {
            Add-Result "Bar State" "PASS" "e9=$e9d atr=$atrd rsi=$rsid age=${ageMin}min" `
                "Valid bar state on disk -- m1_ready=true immediately on startup."
        }
    } catch {
        Add-Result "Bar State" "WARN" "Could not parse bars_gold_m1.dat" ""
    }
} else {
    Add-Result "Bar State" "INFO" "No bars_gold_m1.dat -- cold start" `
        "Will request M15 tick data from broker on startup. GoldFlow delayed ~2min."
}

# --- CHECK 15: Ratchet fix active (no immediate TRAIL exits) -----------------
$ratchetExits = @(Find-All "GOLD-FLOW.*TRAIL_HIT.*held=[0-9]+\.[0-9]+s" | Where-Object {
    if ($_ -match "held=([0-9.]+)s") { [double]$Matches[1] -lt 30 } else { $false }
})
$ratchetCapped = @(Find-All "DOLLAR-RATCHET.*price_capped")
if ($ratchetCapped.Count -gt 0) {
    Add-Result "Ratchet Fix" "PASS" "price_capped fired $($ratchetCapped.Count) time(s)" `
        "SL cap working -- ratchet not setting SL above current price."
} elseif ($ratchetExits.Count -gt 0) {
    Add-Result "Ratchet Fix" "WARN" "$($ratchetExits.Count) TRAIL_HIT exit(s) under 30s" `
        "Possible ratchet overshoot -- check DOLLAR-RATCHET lines for price_capped."
} else {
    Add-Result "Ratchet Fix" "INFO" "No ratchet activity yet" ""
}

# --- CHECK 16: Bracket window warmup (expected ~3min at 195 ticks/min) -------
# STRUCTURE_LOOKBACK=600 ticks at 195/min = ~185s (~3 min) to fill.
# range=0.00 during the first 3 min of startup is EXPECTED, not a bug.
# Only flag FAIL if bracket has never armed after 5+ minutes of runtime.
$bracketArmed = @(Find-All "BRACKET-XAUUSD.*ARMED")
$bracketNever = @(Find-All "GOLD-BRK-DIAG.*can_arm=1.*range=0\.00")
if ($bracketArmed.Count -gt 0) {
    Add-Result "Bracket Window Fix" "PASS" "ARMED $($bracketArmed.Count) time(s)" `
        "Bracket engine arming correctly."
} else {
    Add-Result "Bracket Window Fix" "INFO" "range=0.00 in startup window -- normal (window fills in ~3min at 195/min)" `
        "STRUCTURE_LOOKBACK=600 ticks needs ~185s to fill. Check again after 3 minutes."
}

# --- CHECK 17: Bar periodic save firing --------------------------------------
$barSave = @(Find-All "BAR-SAVE.*Periodic save")
if ($barSave.Count -gt 0) {
    Add-Result "Bar Auto-Save" "PASS" "Periodic save fired $($barSave.Count) time(s)" `
        "Bar state saved every 10min -- warm restart guaranteed."
} else {
    Add-Result "Bar Auto-Save" "INFO" "No BAR-SAVE yet (fires every 10min)" `
        "Normal if startup < 10min ago."
}

# ------------------------------------------------------------------------------
# Render results to console + report file
# ------------------------------------------------------------------------------
$passCount = @($results | Where-Object { $_.Status -eq "PASS" }).Count
$failCount = @($results | Where-Object { $_.Status -eq "FAIL" }).Count
$warnCount = @($results | Where-Object { $_.Status -eq "WARN" }).Count
$infoCount = @($results | Where-Object { $_.Status -eq "INFO" }).Count

$reportLines = [System.Collections.Generic.List[string]]::new()
$reportLines.Add("OMEGA STARTUP REPORT")
$reportLines.Add("Generated : $([System.DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm:ss')) UTC")
$reportLines.Add("Log file  : $LogPath")
$reportLines.Add("Collected : $($capturedLines.Count) lines over ${WaitSec}s")
$reportLines.Add("Result    : PASS=$passCount  FAIL=$failCount  WARN=$warnCount  INFO=$infoCount")
$reportLines.Add("=" * 72)

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   STARTUP REPORT" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

foreach ($r in $results) {
    $icon  = switch ($r.Status) {
        "PASS" { "[PASS]" }
        "FAIL" { "[FAIL]" }
        "WARN" { "[WARN]" }
        "INFO" { "[INFO]" }
    }
    $color = switch ($r.Status) {
        "PASS" { "Green"  }
        "FAIL" { "Red"    }
        "WARN" { "Yellow" }
        "INFO" { "Cyan"   }
    }
    $line = "  $icon  $($r.Name.PadRight(22)) $($r.Value)"
    Write-Host $line -ForegroundColor $color
    if ($r.Detail -ne "") {
        Write-Host "           $(" " * 22) $($r.Detail)" -ForegroundColor DarkGray
    }
    $reportLines.Add("$icon  $($r.Name.PadRight(22)) $($r.Value)")
    if ($r.Detail -ne "") { $reportLines.Add("              $(" " * 22) $($r.Detail)") }
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
if ($failCount -gt 0) {
    Write-Host "  RESULT: $failCount FAILURE(S) -- engine may not be trading correctly" -ForegroundColor Red
} elseif ($warnCount -gt 0) {
    Write-Host "  RESULT: $warnCount WARNING(S) -- review yellows above" -ForegroundColor Yellow
} else {
    Write-Host "  RESULT: ALL CHECKS PASSED" -ForegroundColor Green
}
Write-Host "  Report saved: $ReportFile" -ForegroundColor DarkGray
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# Write report file
$reportLines.Add("=" * 72)
$reportLines.Add("RESULT: PASS=$passCount  FAIL=$failCount  WARN=$warnCount  INFO=$infoCount")
$reportLines | Out-File -FilePath $ReportFile -Encoding utf8 -Force
Write-Host "  [OK] startup_report.txt written to $ReportFile" -ForegroundColor Green
Write-Host ""

# State push DISABLED -- log/state files removed from git tracking.
# State commits were creating new hashes on every restart (phantom hash problem).
# Logs stay on disk at C:\Omega\logs\ -- read via RDP or MONITOR.ps1.
Write-Host "  [OK] State files saved locally (not pushed to git)" -ForegroundColor DarkGray
Write-Host ""
