#Requires -Version 5.1
# ==============================================================================
#  OMEGA_STATUS.ps1  --  Complete system state in one command
#  Shows: running hash, all gate states, engine states, MCE state, P&L
#  Run any time. No restarts needed.
#  Usage: .\OMEGA_STATUS.ps1
# ==============================================================================

$LogFile  = "C:\Omega\logs\latest.log"
$VerFile  = "C:\Omega\include\version_generated.hpp"
$OmegaDir = "C:\Omega"

function Hdr($text) {
    Write-Host ""
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host "  $("=" * ($text.Length))" -ForegroundColor DarkCyan
}
function OK($label, $val)   { Write-Host ("  {0,-30} {1}" -f $label, $val) -ForegroundColor Green }
function WARN($label, $val) { Write-Host ("  {0,-30} {1}" -f $label, $val) -ForegroundColor Yellow }
function FAIL($label, $val) { Write-Host ("  {0,-30} {1}" -f $label, $val) -ForegroundColor Red }
function INFO($label, $val) { Write-Host ("  {0,-30} {1}" -f $label, $val) -ForegroundColor White }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  OMEGA STATUS  --  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss UTC' -AsUTC)" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

# ── SECTION 1: Binary verification ──────────────────────────────────────────
Hdr "1. BINARY VERIFICATION"

# Hash from version_generated.hpp (what was compiled)
$builtHash = "NOT FOUND"
if (Test-Path $VerFile) {
    $verContent = Get-Content $VerFile -Raw
    if ($verContent -match 'OMEGA_GIT_HASH\s+"([a-f0-9]+)"') { $builtHash = $Matches[1] }
}

# Hash from git HEAD (what should be running)
$gitHash = "UNKNOWN"
$gitMsg  = "UNKNOWN"
try {
    Set-Location $OmegaDir
    $gitHash = (git log --format="%h" -1 2>$null).Trim()
    $gitMsg  = (git log --format="%s" -1 2>$null).Trim()
} catch {}

# Hash from log (what is actually running)
$runningHash = "NOT IN LOG"
if (Test-Path $LogFile) {
    $hashLine = Get-Content $LogFile | Select-String "RUNNING COMMIT" | Select-Object -Last 1
    if ($hashLine) {
        if ($hashLine -match "RUNNING COMMIT:\s+([a-f0-9]+)") { $runningHash = $Matches[1] }
    }
}

if ($builtHash -eq $gitHash -and $gitHash -eq $runningHash) {
    OK "Git HEAD"     "$gitHash  $gitMsg"
    OK "Built hash"   "$builtHash  [MATCHES]"
    OK "Running hash" "$runningHash  [MATCHES]"
} else {
    FAIL "Git HEAD"     "$gitHash  $gitMsg"
    if ($builtHash -ne $gitHash) {
        FAIL "Built hash"  "$builtHash  [MISMATCH -- rebuild needed]"
    } else {
        OK   "Built hash"  "$builtHash"
    }
    if ($runningHash -ne $gitHash) {
        FAIL "Running hash" "$runningHash  [MISMATCH -- restart needed]"
    } else {
        OK   "Running hash" "$runningHash"
    }
}

# ── SECTION 2: Service state ────────────────────────────────────────────────
Hdr "2. SERVICE STATE"
$svc = Get-Service -Name "OmegaHFT" -ErrorAction SilentlyContinue
if ($svc) {
    if ($svc.Status -eq "Running") { OK "OmegaHFT service" "Running" }
    else { FAIL "OmegaHFT service" "$($svc.Status)  [NOT RUNNING -- run .\RESTART_OMEGA.ps1]" }
} else {
    FAIL "OmegaHFT service" "NOT INSTALLED"
}
$proc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($proc) { OK "Omega.exe process" "PID $($proc.Id)  CPU=$([math]::Round($proc.CPU,1))s  Mem=$([math]::Round($proc.WorkingSet64/1MB,0))MB" }
else       { FAIL "Omega.exe process" "NOT FOUND" }

# ── SECTION 3: Log freshness ─────────────────────────────────────────────────
Hdr "3. LOG FRESHNESS"
if (Test-Path $LogFile) {
    $logAge = (Get-Date) - (Get-Item $LogFile).LastWriteTime
    $logAgeS = [int]$logAge.TotalSeconds
    $lastLine = Get-Content $LogFile -Tail 1
    if ($logAgeS -lt 30) { OK "latest.log age"  "${logAgeS}s ago  [LIVE]" }
    elseif ($logAgeS -lt 120) { WARN "latest.log age" "${logAgeS}s ago  [STALE?]" }
    else { FAIL "latest.log age" "${logAgeS}s ago  [FROZEN -- engine may be dead]" }
    INFO "Last log line" "$lastLine"
} else {
    FAIL "latest.log" "NOT FOUND"
}

# ── SECTION 4: FIX connection ────────────────────────────────────────────────
Hdr "4. FIX / CTRADER CONNECTION"
if (Test-Path $LogFile) {
    $logon  = Get-Content $LogFile | Select-String "LOGON ACCEPTED" | Select-Object -Last 1
    $reconnect = Get-Content $LogFile | Select-String "reconnecting|Disconnected|SSL error" | Select-Object -Last 1
    $ctrader = Get-Content $LogFile | Select-String "Account 43014358 authorized" | Select-Object -Last 1

    if ($logon)    { OK   "FIX logon"     "$logon" }
    else           { FAIL "FIX logon"     "NOT SEEN IN LOG" }
    if ($ctrader)  { OK   "cTrader L2"    "$ctrader" }
    else           { WARN "cTrader L2"    "NOT SEEN IN LOG" }
    if ($reconnect){ WARN "Last reconnect" "$reconnect" }
}

# ── SECTION 5: Session & P&L ─────────────────────────────────────────────────
Hdr "5. SESSION STATE & P&L"
if (Test-Path $LogFile) {
    $diag = Get-Content $LogFile | Select-String "OMEGA-DIAG\] PnL" | Select-Object -Last 1
    if ($diag) {
        # Parse: [OMEGA-DIAG] PnL=-53.0 T=11 WR=36.3% RTTp95=12.9ms cap=120ms lat_ok=1 session=ACTIVE
        if ($diag -match "PnL=([\-\d\.]+)") { $pnl = $Matches[1] } else { $pnl = "?" }
        if ($diag -match "T=(\d+)")          { $trades = $Matches[1] } else { $trades = "?" }
        if ($diag -match "WR=([\d\.]+)%")    { $wr = $Matches[1] } else { $wr = "?" }
        if ($diag -match "RTTp95=([\d\.]+)ms") { $rtt = $Matches[1] } else { $rtt = "?" }
        if ($diag -match "lat_ok=(\d)")      { $latOk = $Matches[1] } else { $latOk = "?" }
        if ($diag -match "session=(\w+)")    { $sess = $Matches[1] } else { $sess = "?" }

        $pnlColor = if ([double]$pnl -ge 0) { "Green" } else { "Red" }
        Write-Host ("  {0,-30} {1}" -f "Daily P&L", "`$$pnl") -ForegroundColor $pnlColor
        INFO "Trades today" "$trades  WR=$wr%"
        if ($rtt -ne "?" -and [double]$rtt -lt 120) { OK "RTT p95" "${rtt}ms  lat_ok=$latOk" }
        else { WARN "RTT p95" "${rtt}ms  lat_ok=$latOk" }
        if ($sess -eq "ACTIVE") { OK "Session" "ACTIVE" } else { WARN "Session" "$sess" }
    } else {
        WARN "OMEGA-DIAG" "Not yet in log (engine still warming up)"
    }
}

# ── SECTION 6: Gold state ────────────────────────────────────────────────────
Hdr "6. GOLD ENGINE STATE"
if (Test-Path $LogFile) {
    $goldDiag = Get-Content $LogFile | Select-String "\[GOLD-DIAG\]" | Select-Object -Last 1
    if ($goldDiag) {
        if ($goldDiag -match "regime=(\w+)")      { $regime = $Matches[1] } else { $regime = "?" }
        if ($goldDiag -match "vwap=([\d\.]+)")     { $vwap = $Matches[1] } else { $vwap = "?" }
        if ($goldDiag -match "vol_range=([\d\.]+)") { $volRange = $Matches[1] } else { $volRange = "?" }

        $regColor = switch ($regime) {
            "EXPANSION_BREAKOUT"  { "Green" }
            "TREND_CONTINUATION"  { "Green" }
            "MEAN_REVERSION"      { "Yellow" }
            "QUIET_COMPRESSION"   { "Yellow" }
            "COMPRESSION"         { "Yellow" }
            "IMPULSE"             { "Cyan" }
            "HIGH_RISK_NO_TRADE"  { "Red" }
            default               { "White" }
        }
        Write-Host ("  {0,-30} {1}" -f "Regime", $regime) -ForegroundColor $regColor
        INFO "VWAP" $vwap
        if ([double]$volRange -gt 2.0) { OK "Vol range" "${volRange}pt  [ACTIVE]" }
        elseif ([double]$volRange -gt 0.5) { WARN "Vol range" "${volRange}pt  [LOW]" }
        else { FAIL "Vol range" "${volRange}pt  [DEAD TAPE]" }
    } else {
        WARN "GOLD-DIAG" "Not yet in log"
    }

    # Bracket state
    $brkDiag = Get-Content $LogFile | Select-String "GOLD-BRK-DIAG" | Select-Object -Last 1
    if ($brkDiag) {
        if ($brkDiag -match "phase=(\w+)")        { $phase = $Matches[1] } else { $phase = "?" }
        if ($brkDiag -match "can_enter=(\d)")      { $canEnter = $Matches[1] } else { $canEnter = "?" }
        if ($brkDiag -match "drift=([\-\d\.]+)")   { $drift = $Matches[1] } else { $drift = "?" }
        if ($brkDiag -match "range=([\d\.]+)")     { $range = $Matches[1] } else { $range = "?" }
        if ($brkDiag -match "can_arm=(\d)")        { $canArm = $Matches[1] } else { $canArm = "?" }
        if ($brkDiag -match "session_slot=(\d)")   { $slot = $Matches[1] } else { $slot = "?" }
        if ($brkDiag -match "in_dead_zone=(\d)")   { $deadZone = $Matches[1] } else { $deadZone = "?" }
        if ($brkDiag -match "impulse_block=(\d)")  { $impulse = $Matches[1] } else { $impulse = "?" }
        if ($brkDiag -match "stack_open=(\d)")     { $stackOpen = $Matches[1] } else { $stackOpen = "?" }

        $phaseColor = switch ($phase) { "LIVE" { "Green" } "ARMED" { "Cyan" } "PENDING" { "Yellow" } default { "White" } }
        Write-Host ("  {0,-30} {1}" -f "Bracket phase", $phase) -ForegroundColor $phaseColor
        if ($canEnter -eq "1") { OK "can_enter" "1" } else { FAIL "can_enter" "0  [BLOCKED]" }
        if ($canArm -eq "1")   { OK "can_arm" "1" } else { WARN "can_arm" "0" }
        INFO "Drift" "${drift}pt  |  Range: ${range}pt  |  Slot: $slot"
        if ($deadZone -eq "1") { FAIL "Dead zone" "YES  [BLOCKED]" } else { OK "Dead zone" "no" }
        if ($impulse -eq "1")  { WARN "Impulse block" "YES  (20s post-impulse)" } else { OK "Impulse block" "no" }
        if ($stackOpen -eq "1"){ WARN "GoldStack open" "YES  (may block entry)" } else { OK "GoldStack open" "no" }
    }
}

# ── SECTION 7: MacroCrash state ──────────────────────────────────────────────
Hdr "7. MACROCRASH ENGINE STATE"
if (Test-Path $LogFile) {
    $mceNosig = Get-Content $LogFile | Select-String "MCE-NOSIG" | Select-Object -Last 1
    $mceTrigger = Get-Content $LogFile | Select-String "MCE-SHADOW\] TRIGGER|MCE\] TRIGGER" | Select-Object -Last 1
    $mceEntry = Get-Content $LogFile | Select-String "MCE-SHADOW\] ENTRY|MCE\] ENTRY" | Select-Object -Last 1
    $mceClose = Get-Content $LogFile | Select-String "MCE-SHADOW\] CLOSE|MCE\] CLOSE" | Select-Object -Last 1

    if ($mceNosig) {
        WARN "MCE-NOSIG (last)" "$mceNosig"
        # Parse which gate is blocking
        if ($mceNosig -match "cooldown=BLOCK") { FAIL "  >> Blocker" "COOLDOWN active" }
        if ($mceNosig -match "atr=[\d\.]+ \(need [\d\.]+ BLOCK\)") { FAIL "  >> Blocker" "ATR too low" }
        if ($mceNosig -match "vol=[\d\.]+ \(need [\d\.]+ BLOCK\)") { FAIL "  >> Blocker" "VOL_RATIO too low" }
        if ($mceNosig -match "exp=0\(regime=0 rsi=0\)") { FAIL "  >> Blocker" "NO EXPANSION (regime=COMPRESSION, RSI not extreme)" }
        if ($mceNosig -match "drift=[\d\.]+ \(need [\d\.]+ BLOCK\)") { FAIL "  >> Blocker" "DRIFT too low" }
    } else {
        OK "MCE-NOSIG" "Not firing  (all gates passing OR no move yet)"
    }
    if ($mceTrigger) { OK "Last MCE trigger" "$mceTrigger" }
    if ($mceEntry)   { OK "Last MCE entry"   "$mceEntry" }
    if ($mceClose)   { INFO "Last MCE close"  "$mceClose" }
    if (-not $mceTrigger -and -not $mceEntry) {
        WARN "MCE fires" "No triggers seen today  (waiting for ATR>6 + drift>5 + RSI extreme)"
    }
}

# ── SECTION 8: Recent trades ─────────────────────────────────────────────────
Hdr "8. RECENT TRADES (last 10)"
$tradeFile = "C:\Omega\logs\trades\omega_trade_closes.csv"
if (Test-Path $tradeFile) {
    $trades = Get-Content $tradeFile | Select-Object -Last 10
    foreach ($t in $trades) {
        $fields = $t -split ","
        if ($fields.Count -ge 6) {
            $side   = if ($fields.Count -gt 2) { $fields[2] } else { "?" }
            $pnlVal = if ($fields.Count -gt 5) { $fields[5] } else { "0" }
            $color  = try { if ([double]$pnlVal -ge 0) { "Green" } else { "Red" } } catch { "White" }
            Write-Host "  $t" -ForegroundColor $color
        } else {
            Write-Host "  $t" -ForegroundColor White
        }
    }
} else {
    # Fall back to log
    if (Test-Path $LogFile) {
        $recentTrades = Get-Content $LogFile | Select-String "TRAIL|SL_HIT|BE_HIT|TP_HIT|CLOSE" | Select-Object -Last 8
        foreach ($t in $recentTrades) { INFO "  Trade" "$t" }
    }
}

# ── SECTION 9: Session gate ──────────────────────────────────────────────────
Hdr "9. SESSION GATE"
# session_tradeable() is 24h (session_start_utc=0, session_end_utc=0).
# ASIA-GATE in trade_lifecycle.hpp is a VOL QUALITY gate only -- not a session block.
# Fires only on genuinely dead tape: vol_ratio<2.0 AND drift<0.5 AND RSI not extreme.
# If you see BLOCKED here it means flat tape, not a session restriction.
if (Test-Path $LogFile) {
    $asiaGate = Get-Content $LogFile | Select-String "ASIA-GATE" | Select-Object -Last 1
    if ($asiaGate) {
        if ($asiaGate -match "BLOCKED") {
            WARN "Asia vol gate" "BLOCKED (dead tape -- vol/drift too low, NOT a session block)"
            INFO "  Detail" "$asiaGate"
        } else {
            OK "Asia vol gate" "OPEN  $asiaGate"
        }
    } else {
        OK "Session gate" "24h mode active -- no session blocks"
    }
}

# ── SECTION 10: What to watch ────────────────────────────────────────────────
Hdr "10. WHAT TO WATCH"
$utcHour = [int](Get-Date -AsUTC -Format "HH")
if ($utcHour -ge 5 -and $utcHour -lt 7) {
    WARN "Session" "Pre-London (05-07 UTC) -- RSIReversal + MicroMomentum active, MacroCrash needs ATR>6"
} elseif ($utcHour -ge 7 -and $utcHour -lt 9) {
    OK "Session" "LONDON OPEN (07-09 UTC) -- all engines active, best opportunity window"
} elseif ($utcHour -ge 13 -and $utcHour -lt 17) {
    OK "Session" "NY SESSION (13-17 UTC) -- high vol, MacroCrash prime window"
} elseif ($utcHour -ge 22 -or $utcHour -lt 5) {
    WARN "Session" "ASIA (22-05 UTC) -- GoldStack + lower thresholds active"
} else {
    OK "Session" "LONDON/NY CORE (09-22 UTC)"
}
Write-Host ""
INFO "Live log stream" "Get-Content C:\Omega\logs\latest.log -Wait | Select-String 'MCE|GOLD-DIAG|BRACKET|TRADE'"
INFO "Full log tail"   "Get-Content C:\Omega\logs\latest.log -Tail 50"
Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
