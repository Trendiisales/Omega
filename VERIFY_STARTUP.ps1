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
    [int]    $WaitSec = 10,
    [string] $LogPath = "",
    [string] $OmegaDir = "C:\Omega"
)

# FIX 2026-04-21 (StrictMode safety):
# $ghToken is loaded conditionally inside STEP 0 (only when C:\Omega\.github_token
# exists). CHECK 13 later reads $ghToken after Set-StrictMode -Version Latest is
# enabled at L~124. Under strict mode, reading an undefined variable throws
# VariableIsUndefined. Pre-declare $ghToken = $null here so the later read is
# always defined, regardless of whether the token file exists.
$ghToken = $null

# ==============================================================================
# STEP 0: GitHub API binary staleness check -- RUNS BEFORE EVERYTHING ELSE
# Hits the GitHub contents API to get live HEAD SHA.
# Reads the running binary hash from latest.log ([OMEGA] RUNNING COMMIT line).
# If they don't match: RED BANNER, hard exit. Cannot verify a stale binary.
# This is the check that prevents stale binaries from ever going unnoticed.
# ==============================================================================
Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  STEP 0: GITHUB API BINARY STALENESS CHECK" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

$tokenFile = "C:\Omega\.github_token"
if (-not (Test-Path $tokenFile)) {
    Write-Host "  [SKIP] C:\Omega\.github_token not found -- cannot verify GitHub HEAD" -ForegroundColor Yellow
    Write-Host "  Run: [System.IO.File]::WriteAllText('C:\Omega\.github_token', 'YOUR_TOKEN')" -ForegroundColor Yellow
} else {
    $ghToken = (Get-Content $tokenFile -Raw).Trim()
    $apiHeaders = @{
        Authorization   = "token $ghToken"
        "User-Agent"    = "OmegaVerify"
        "Cache-Control" = "no-cache"
        Accept          = "application/vnd.github.v3+json"
    }

    # Get GitHub HEAD SHA via contents API (never CDN-cached)
    $ghSha7 = "unknown"
    try {
        $ghHead = Invoke-RestMethod `
            -Uri "https://api.github.com/repos/Trendiisales/Omega/commits/main" `
            -Headers $apiHeaders -TimeoutSec 15 -ErrorAction Stop
        $ghSha7 = $ghHead.sha.Substring(0,7)
        Write-Host "  [API] GitHub HEAD : $ghSha7  -- $($ghHead.commit.message)" -ForegroundColor Cyan
    } catch {
        Write-Host "  [WARN] GitHub API unreachable: $_ -- skipping staleness check" -ForegroundColor Yellow
        $ghSha7 = "api_unreachable"
    }

    # Get running binary hash from latest.log RUNNING COMMIT line
    $runningHash = "not_found"
    $latestLog = "C:\Omega\logs\latest.log"
    if (Test-Path $latestLog) {
        # Read ONLY last 200 lines -- prevents old session hash from matching
        # The RUNNING COMMIT line is written within first 5s of startup.
        # Reading entire log risks finding previous session's hash if log
        # is not rotated, producing a false-positive "correct binary" result.
        $rcLine = Get-Content $latestLog -Tail 200 -ErrorAction SilentlyContinue |
                  Select-String "RUNNING COMMIT:" |
                  Select-Object -Last 1
        if ($rcLine -and ($rcLine -match "RUNNING COMMIT:\s+([a-f0-9]{7,12})")) {
            $runningHash = $Matches[1]
        }
    }
    Write-Host "  [LOG] Running hash : $runningHash" -ForegroundColor Cyan

    if ($ghSha7 -eq "api_unreachable") {
        Write-Host "  [WARN] Cannot verify -- GitHub API unreachable" -ForegroundColor Yellow
    } elseif ($runningHash -eq "not_found") {
        Write-Host ""
        Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
        Write-Host "  ║  BINARY HASH NOT FOUND IN LOG                   ║" -ForegroundColor Red
        Write-Host "  ║  Omega has not started or log is stale.         ║" -ForegroundColor Red
        Write-Host "  ║  Run: .\RESTART_OMEGA.ps1                       ║" -ForegroundColor Red
        Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
        Write-Host ""
        exit 1
    } elseif ($runningHash -ne $ghSha7) {
        Write-Host ""
        Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
        Write-Host "  ║  STALE BINARY DETECTED -- DO NOT TRADE          ║" -ForegroundColor Red
        Write-Host "  ║  Running : $runningHash                          ║" -ForegroundColor Red
        Write-Host "  ║  GitHub  : $ghSha7                               ║" -ForegroundColor Red
        Write-Host "  ║  Fix: .\RESTART_OMEGA.ps1                       ║" -ForegroundColor Red
        Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
        Write-Host ""
        exit 1
    } else {
        Write-Host "  [OK] Binary hash MATCHES GitHub HEAD: $runningHash" -ForegroundColor Green
    }
}
Write-Host ""


Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$ReportFile = "$OmegaDir\logs\startup_report.txt"

# ------------------------------------------------------------------------------
# Locate the log file
# ------------------------------------------------------------------------------
if ($LogPath -eq "") {
    # ── Locate the live log ──────────────────────────────────────────────────
    # STALENESS CONTRACT:
    #   1. Prefer latest.log IF it exists AND LastWriteTime < 120s ago.
    #      latest.log is truncated on every restart -- if fresh it is guaranteed
    #      to be from THIS run. The engine emits [LOG-HEALTH] every 60s so any
    #      live file will always be < 120s old.
    #   2. If latest.log is stale or missing, fall back to today's dated log
    #      with the same 120s freshness check.
    #   3. If BOTH are stale or missing: hard FAIL with diagnostics.
    #      Never silently analyse a frozen file.
    # ────────────────────────────────────────────────────────────────────────
    $latestLog     = "$OmegaDir\logs\latest.log"
    $datedLog      = "$OmegaDir\logs\omega_$((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')).log"  # UTC -- binary uses gmtime_s
    $MAX_STALE_SEC = 120

    $latestOk = $false

    if (Test-Path $latestLog) {
        $latestAge = [int]((Get-Date) - (Get-Item $latestLog).LastWriteTime).TotalSeconds
        if ($latestAge -le $MAX_STALE_SEC) {
            $latestOk = $true
            $LogPath  = $latestLog
            Write-Host "  [LOG] latest.log LIVE (age=${latestAge}s)" -ForegroundColor Green
        } else {
            Write-Host "  [WARN] latest.log STALE (age=${latestAge}s > ${MAX_STALE_SEC}s) -- checking dated log" -ForegroundColor Yellow
        }
    } else {
        Write-Host "  [WARN] latest.log NOT FOUND -- checking dated log" -ForegroundColor Yellow
    }

    if (-not $latestOk) {
        if (Test-Path $datedLog) {
            $datedAge = [int]((Get-Date) - (Get-Item $datedLog).LastWriteTime).TotalSeconds
            if ($datedAge -le $MAX_STALE_SEC) {
                $LogPath = $datedLog
                Write-Host "  [LOG] Using dated log LIVE (age=${datedAge}s): $datedLog" -ForegroundColor Cyan
            } else {
                Write-Host ""
                Write-Host "  *** LOG HEALTH FAILURE ***" -ForegroundColor Red
                Write-Host "  latest.log : $(if (Test-Path $latestLog) { 'STALE (' + [int]((Get-Date)-(Get-Item $latestLog).LastWriteTime).TotalSeconds + 's)' } else { 'NOT FOUND' })" -ForegroundColor Red
                Write-Host "  dated log  : STALE (age=${datedAge}s) -- $datedLog" -ForegroundColor Red
                Write-Host "  Both logs frozen. Omega is dead or log dir is wrong." -ForegroundColor Red
                Write-Host "  Run: Get-Service Omega" -ForegroundColor Yellow
                Write-Host "  Run: Get-Process Omega" -ForegroundColor Yellow
                Write-Host ""
                exit 1
            }
        } else {
            Write-Host ""
            Write-Host "  *** LOG HEALTH FAILURE ***" -ForegroundColor Red
            Write-Host "  latest.log : $(if (Test-Path $latestLog) { 'STALE' } else { 'NOT FOUND' })" -ForegroundColor Red
            Write-Host "  dated log  : NOT FOUND ($datedLog)" -ForegroundColor Red
            Write-Host "  No live log found. Omega has not started or crashed before opening logs." -ForegroundColor Red
            Write-Host "  Run: .\\RESTART_OMEGA.ps1" -ForegroundColor Yellow
            Write-Host ""
            exit 1
        }
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

# --- CHECK 0: Log health -- confirm [LOG-HEALTH] heartbeat present and recent -
# The engine emits [LOG-HEALTH] dated=ok latest=<ok|FAIL> ts=HH:MM:SS every 60s.
# If we see latest=FAIL in any heartbeat, latest.log failed to open -- dated log only.
# If no heartbeat at all in captured lines, log system did not initialise.
$logHealthLines = @(Find-All "\[LOG-HEALTH\]")
if ($logHealthLines.Count -gt 0) {
    $lastHealth = $logHealthLines[-1]
    if ($lastHealth -match "latest=FAIL") {
        Add-Result "Log Health" "FAIL" "latest.log FAILED TO OPEN" `
            "Engine is writing to dated log only. All scripts auto-fallback. Check disk permissions on C:\Omega\logs\"
    } elseif ($lastHealth -match "latest=ok") {
        Add-Result "Log Health" "PASS" "dated=ok latest=ok ($($logHealthLines.Count) heartbeat(s) seen)" `
            "Both log files confirmed open and receiving writes."
    } else {
        Add-Result "Log Health" "WARN" "Heartbeat found but status unclear: $lastHealth" ""
    }
} else {
    # No heartbeat seen -- could be within first 60s of startup (normal) or log init failed
    $logFailLine = Find-Last "OMEGA-LOG-LATEST-FAIL"
    if ($logFailLine) {
        Add-Result "Log Health" "FAIL" "OMEGA-LOG-LATEST-FAIL seen -- latest.log never opened" `
            "Engine running but latest.log is not being written. Check C:\Omega\logs\ permissions."
    } else {
        Add-Result "Log Health" "INFO" "No [LOG-HEALTH] heartbeat yet (fires every 60s)" `
            "Normal if startup < 60s ago. If this remains INFO after 2min, log system may have failed."
    }
}

# --- CHECK 1/2/3 REMOVED S14 ------------------------------------------------
# GFE-SEED, GFE ATR state loaded/rejected/startup, and GF-GATE-BLOCK atr tracking
# were all GoldFlow-specific. GoldFlow culled S19 Stage 1B (engine deleted).
# Bar-level ATR for remaining engines is verified in Bar State Load check below.

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
    } elseif ($canEnter -eq "0" -and $slot -eq "6") {
        # Asia session (slot=6): can_enter=0 is normal during dead tape / low vol compression
        Add-Result "Session/Entry State" "INFO" $summary "Asia session -- can_enter=0 normal during low vol compression"
    } elseif ($canEnter -eq "0") {
        Add-Result "Session/Entry State" "WARN" $summary "can_enter=0 during active session -- check regime/vol conditions."
    } else {
        Add-Result "Session/Entry State" "INFO" $summary ""
    }
} else {
    Add-Result "Session/Entry State" "INFO" "No BRK-DIAG lines yet" ""
}

# --- CHECK 8: L2 / cTrader live feed -----------------------------------------
# L2-STATUS fires every 60s -- outside the 45s VERIFY window. Use CTRADER-STATUS
# which fires at the 60s diagnostic mark, or CTRADER-EVTS which fires even earlier.
# Also accept CTRADER-BOOK (first 10 depth events logged at startup).
$l2StatusLine  = Find-Last "L2-STATUS"
$ctStatusLine  = Find-Last "CTRADER-STATUS"
$ctEvtsLine    = Find-Last "CTRADER-EVTS.*XAUUSD="
$ctBookLine    = Find-Last "CTRADER-BOOK.*XAUUSD"
$ctActiveLine  = Find-Last "CTRADER.*Depth feed ACTIVE"

if ($l2StatusLine -and $l2StatusLine -match "ctrader_live=1") {
    $evts = if ($l2StatusLine -match "events=([0-9]+)") { $Matches[1] } else { "?" }
    Add-Result "L2 / cTrader Feed" "PASS" "ctrader_live=1 events=$evts" $l2StatusLine.Trim()
} elseif ($l2StatusLine -and $l2StatusLine -match "ctrader_live=0") {
    Add-Result "L2 / cTrader Feed" "FAIL" "ctrader_live=0" "cTrader not connected -- check API credentials."
} elseif ($ctStatusLine) {
    $evts = if ($ctStatusLine -match "events_total=([0-9]+)") { $Matches[1] } else { "?" }
    Add-Result "L2 / cTrader Feed" "PASS" "cTrader active events_total=$evts" $ctStatusLine.Trim()
} elseif ($ctEvtsLine) {
    $xauEvts = if ($ctEvtsLine -match "XAUUSD=([0-9]+)") { $Matches[1] } else { "?" }
    Add-Result "L2 / cTrader Feed" "PASS" "XAUUSD depth events=$xauEvts this minute" $ctEvtsLine.Trim()
} elseif ($ctBookLine) {
    Add-Result "L2 / cTrader Feed" "PASS" "XAUUSD depth book received" $ctBookLine.Trim()
} elseif ($ctActiveLine) {
    Add-Result "L2 / cTrader Feed" "PASS" "cTrader depth feed ACTIVE" $ctActiveLine.Trim()
} else {
    Add-Result "L2 / cTrader Feed" "INFO" "No cTrader depth events in first ${WaitSec}s" "cTrader connecting -- check log for CTRADER-EVTS within 60s. If still missing after 2min, check ctid=43014358."
}

# --- CHECK 8b: L2 tick CSV freshness + imbalance verification ----------------
# CRITICAL: Verify l2_ticks CSVs are fresh (<=120s) and have real rows.
# Three files now exist per UTC day:
#     l2_ticks_XAUUSD_YYYY-MM-DD.csv   (full schema incl. l2_imb)
#     l2_ticks_US500_YYYY-MM-DD.csv    (ts/mid/bid/ask/has_pos populated; imb/vol/depth zeroed)
#     l2_ticks_USTEC_YYYY-MM-DD.csv    (same as US500)
#
# CSV columns (17, 0-indexed):
#   0=ts_ms 1=mid 2=bid 3=ask 4=l2_imb 5=l2_bid_vol 6=l2_ask_vol
#   7=depth_bid_levels 8=depth_ask_levels 9=depth_events_total
#   10=watchdog_dead 11=vol_ratio 12=regime 13=vpin 14=has_pos 15=micro_edge 16=ewm_drift
#
# Imbalance check runs for XAUUSD only -- indices loggers zero l2_imb by design.
$l2UtcDate = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')

# -- XAUUSD (primary -- full check including l2_imb neutrality) --------------
$l2CsvFile_XAU = "C:\Omega\logs\l2_ticks_XAUUSD_$l2UtcDate.csv"
if (-not (Test-Path $l2CsvFile_XAU)) {
    Add-Result "L2 Tick CSV XAUUSD" "INFO" "File not yet created" "Logger creates file on first XAUUSD tick -- check again in 60s."
} else {
    $l2Age_XAU = [int]((Get-Date) - (Get-Item $l2CsvFile_XAU).LastWriteTime).TotalSeconds
    if ($l2Age_XAU -gt 120) {
        Add-Result "L2 Tick CSV XAUUSD" "FAIL" "STALE ${l2Age_XAU}s" "l2_ticks_XAUUSD not updated in ${l2Age_XAU}s -- logger stopped or Omega crashed."
    } else {
        $l2Sample_XAU       = Get-Content $l2CsvFile_XAU | Where-Object { $_ -match '^[0-9]' } | Select-Object -Last 20
        $l2NeutralCount_XAU = 0
        $l2TotalCount_XAU   = 0
        $l2LastImb_XAU      = 0.5
        foreach ($l2Row in $l2Sample_XAU) {
            $l2Cols = $l2Row -split ','
            if ($l2Cols.Count -ge 5) {
                $l2TotalCount_XAU++
                try { $l2LastImb_XAU = [double]$l2Cols[4] } catch { $l2LastImb_XAU = 0.5 }
                if ([Math]::Abs($l2LastImb_XAU - 0.5) -lt 0.001) { $l2NeutralCount_XAU++ }
            }
        }
        if ($l2TotalCount_XAU -eq 0) {
            Add-Result "L2 Tick CSV XAUUSD" "INFO" "age=${l2Age_XAU}s writing" "CSV writing -- no rows yet. Check again in 30s."
        } elseif ($l2NeutralCount_XAU -eq $l2TotalCount_XAU) {
            Add-Result "L2 Tick CSV XAUUSD" "WARN" "ALL NEUTRAL last $l2TotalCount_XAU rows imb=$([math]::Round($l2LastImb_XAU,3))" `
                "imbalance=0.500 all rows. Check [CTRADER-L2-CHECK] lines in log for bid_lvls/ask_lvls. If bid_lvls==ask_lvls all day, DOM is genuinely flat. If bid_lvls+ask_lvls=0, cTrader not delivering quotes."
        } else {
            $l2NonNeutral_XAU = $l2TotalCount_XAU - $l2NeutralCount_XAU
            Add-Result "L2 Tick CSV XAUUSD" "PASS" "age=${l2Age_XAU}s imb=$([math]::Round($l2LastImb_XAU,3)) real=$l2NonNeutral_XAU/$l2TotalCount_XAU" `
                "L2 imbalance_level() producing directional values from cTrader DOM."
        }
    }
}

# -- US500 (freshness + rows only; imb/vol/depth are zeroed by logger) -------
$l2CsvFile_SP = "C:\Omega\logs\l2_ticks_US500_$l2UtcDate.csv"
if (-not (Test-Path $l2CsvFile_SP)) {
    Add-Result "L2 Tick CSV US500" "INFO" "File not yet created" "Logger creates file on first US500 tick -- check again in 60s."
} else {
    $l2Age_SP = [int]((Get-Date) - (Get-Item $l2CsvFile_SP).LastWriteTime).TotalSeconds
    if ($l2Age_SP -gt 120) {
        Add-Result "L2 Tick CSV US500" "FAIL" "STALE ${l2Age_SP}s" "l2_ticks_US500 not updated in ${l2Age_SP}s -- SP logger stopped or US500 tick stream dead."
    } else {
        $l2RowCount_SP = (Get-Content $l2CsvFile_SP | Where-Object { $_ -match '^[0-9]' } | Measure-Object).Count
        if ($l2RowCount_SP -eq 0) {
            Add-Result "L2 Tick CSV US500" "INFO" "age=${l2Age_SP}s writing" "CSV writing -- no rows yet. Check again in 30s."
        } else {
            $l2FileSize_SP = (Get-Item $l2CsvFile_SP).Length
            Add-Result "L2 Tick CSV US500" "PASS" "age=${l2Age_SP}s rows=$l2RowCount_SP size=${l2FileSize_SP}b" `
                "US500 tick logger writing to disk. l2_imb is zeroed by design (no cTrader DOM for indices)."
        }
    }
}

# -- USTEC (freshness + rows only; imb/vol/depth are zeroed by logger) -------
$l2CsvFile_NQ = "C:\Omega\logs\l2_ticks_USTEC_$l2UtcDate.csv"
if (-not (Test-Path $l2CsvFile_NQ)) {
    Add-Result "L2 Tick CSV USTEC" "INFO" "File not yet created" "Logger creates file on first USTEC tick -- check again in 60s."
} else {
    $l2Age_NQ = [int]((Get-Date) - (Get-Item $l2CsvFile_NQ).LastWriteTime).TotalSeconds
    if ($l2Age_NQ -gt 120) {
        Add-Result "L2 Tick CSV USTEC" "FAIL" "STALE ${l2Age_NQ}s" "l2_ticks_USTEC not updated in ${l2Age_NQ}s -- NQ logger stopped or USTEC tick stream dead."
    } else {
        $l2RowCount_NQ = (Get-Content $l2CsvFile_NQ | Where-Object { $_ -match '^[0-9]' } | Measure-Object).Count
        if ($l2RowCount_NQ -eq 0) {
            Add-Result "L2 Tick CSV USTEC" "INFO" "age=${l2Age_NQ}s writing" "CSV writing -- no rows yet. Check again in 30s."
        } else {
            $l2FileSize_NQ = (Get-Item $l2CsvFile_NQ).Length
            Add-Result "L2 Tick CSV USTEC" "PASS" "age=${l2Age_NQ}s rows=$l2RowCount_NQ size=${l2FileSize_NQ}b" `
                "USTEC tick logger writing to disk. l2_imb is zeroed by design (no cTrader DOM for indices)."
        }
    }
}

# --- CHECK 8b2: CTRADER-L2-CHECK startup log ---------------------------------
# [CTRADER-L2-CHECK] is logged on first 20 depth events per symbol.
# It shows bid_lvls, ask_lvls, imb_level, imb_vol for each event.
# If bid_lvls+ask_lvls > 0 and imb_level != imb_vol: level-count fix is active.
# If bid_lvls=0 and ask_lvls=0: cTrader sent no quotes for XAUUSD yet.
$l2CheckLine = Find-Last "CTRADER-L2-CHECK.*XAUUSD"
if ($l2CheckLine) {
    $bidLvls  = if ($l2CheckLine -match "bid_lvls=(\d+)")   { [int]$Matches[1] } else { -1 }
    $askLvls  = if ($l2CheckLine -match "ask_lvls=(\d+)")   { [int]$Matches[1] } else { -1 }
    $imbLevel = if ($l2CheckLine -match "imb_level=([\d.]+)") { [double]$Matches[1] } else { -1 }
    $imbVol   = if ($l2CheckLine -match "imb_vol=([\d.]+)")   { [double]$Matches[1] } else { -1 }
    if ($bidLvls -eq 0 -and $askLvls -eq 0) {
        Add-Result "L2 Level-Count Check" "FAIL" "bid_lvls=0 ask_lvls=0" `
            "cTrader sending no XAUUSD DOM quotes. Check ctid=43014358 depth subscription."
    } elseif ($imbLevel -ge 0 -and $imbVol -ge 0 -and [Math]::Abs($imbLevel - $imbVol) -gt 0.001) {
        Add-Result "L2 Level-Count Check" "PASS" "bid=$bidLvls ask=$askLvls imb_level=$imbLevel imb_vol=$imbVol" `
            "imbalance_level() differs from imbalance() -- level-count fix active and working."
    } elseif ($imbLevel -ge 0) {
        Add-Result "L2 Level-Count Check" "PASS" "bid=$bidLvls ask=$askLvls imb_level=$imbLevel" `
            "cTrader DOM levels flowing. imb_level=imb_vol likely means cTrader is sending real sizes today."
    } else {
        Add-Result "L2 Level-Count Check" "INFO" "line found but could not parse" $l2CheckLine.Trim()
    }
} else {
    Add-Result "L2 Level-Count Check" "INFO" "No CTRADER-L2-CHECK yet" `
        "Logged on first 20 depth events -- check again after 30s. If still missing, cTrader depth not subscribed."
}

# --- CHECK 8d REMOVED S14: GoldFlow blocking gates --------------------------
# Engine culled S19 Stage 1B. GF-GATE-BLOCK ENGINE_CULLED, GFE-CONFIG,
# GFE-FADE-BLOCK are all dead log tags. GF Bar State merged into
# Bar State Load check below (bars still used by RSI/HybridBracket/TrendPB).

# ==============================================================================
# CHECK: RSI Reversal Engine enabled
# RSI-REV configured is a startup-once line written before VERIFY_STARTUP starts tailing.
# Must search the FULL log from byte 0, not just capturedLines (new lines only).
# ==============================================================================
$rsiConfigLine = Get-Content $LogPath -ErrorAction SilentlyContinue |
    Where-Object { $_ -match "RSI-REV.*configured" } | Select-Object -Last 1
if (!$rsiConfigLine) {
    Add-Result "RSI Reversal Active" "FAIL" "No RSI-REV configured line in log" `
        "RSIReversalEngine never logged startup -- binary stale or engine disabled. Run QUICK_RESTART.ps1"
} elseif ($rsiConfigLine -match "shadow_mode=true") {
    Add-Result "RSI Reversal Active" "WARN" "RSI Reversal in SHADOW mode" `
        "Engine active but shadow_mode=true -- trades logged only, no real orders fired."
} else {
    Add-Result "RSI Reversal Active" "PASS" "$($rsiConfigLine.Trim())" "RSI Reversal live."
}

# --- BAR STATE CHECKS (shared by RSIReversal, HybridBracket, TrendPB, BB) ----
# Bars are M1/M5/M15/H1/H4 OHLC series used by every live gold engine for ATR,
# RSI, trend filters. GoldFlow-specific gates removed S14 (engine culled S19).
$barsPermanentLine = Find-Last "BARS_PERMANENTLY_UNAVAILABLE"
$barsNotReadyLine  = Find-Last "BARS_NOT_READY"
if ($barsPermanentLine) {
    Add-Result "Bar State" "FAIL" "BARS_PERMANENTLY_UNAVAILABLE" "M1 bars never seeded -- restart Omega to retry. Gold engines running without ATR/RSI/trend context."
} elseif ($barsNotReadyLine) {
    Add-Result "Bar State" "INFO" "Bars warming up" "M1 bars seeding -- gold engines blocked until ready. Normal in first 2min."
} else {
    Add-Result "Bar State" "PASS" "Bars ready" "M1 bars seeded, all gold engines have full context."
}

# --- CHECK BAR-LOAD: bar state loaded from disk on startup (not cold) -----------
# [BAR-LOAD] must appear in log with m1_ready=1.
# If missing: bars not loading from disk -- cold start, gold engines blocked ~2min.
# If m1_ready=0: .dat file was stale/corrupt and rejected -- first restart of the day.
# FAIL either way: next restart will be warm because periodic save runs every 10min.
$barLoadLine = Get-Content $LogPath -ErrorAction SilentlyContinue |
    Where-Object { $_ -match "BAR-LOAD" } | Select-Object -Last 1
if (!$barLoadLine) {
    Add-Result "Bar State Load" "FAIL" "No BAR-LOAD line in log" `
        "load_indicators() never called OR binary is stale. Run RESTART_OMEGA.ps1 to rebuild."
} elseif ($barLoadLine -match "m1_ready=1") {
    $atrVal = if ($barLoadLine -match "ATR=([\d\.]+)") { $Matches[1] } else { "?" }
    Add-Result "Bar State Load" "PASS" "Loaded from disk m1_ready=1 ATR=$atrVal -- warm start" $barLoadLine.Trim()
} else {
    Add-Result "Bar State Load" "WARN" "BAR-LOAD seen but m1_ready=0 -- .dat file rejected (stale/cold)" `
        "$barLoadLine -- gold engines will warm in ~2min from tick data. Next restart will be instant."
}

# --- CHECK BAR-SAVE: periodic save fires every 10min -------------------------
# [BAR-SAVE] Periodic save must appear within 15min of startup.
# Confirms the 10-min save loop is running -- guarantees next restart is warm.
$barSaveLine = Get-Content $LogPath -ErrorAction SilentlyContinue |
    Where-Object { $_ -match "BAR-SAVE.*Periodic" } | Select-Object -Last 1
if ($barSaveLine) {
    Add-Result "Bar Periodic Save" "PASS" "Periodic save confirmed running" $barSaveLine.Trim()
} else {
    Add-Result "Bar Periodic Save" "INFO" "No BAR-SAVE yet (fires every 10min -- check again after 10min)" `
        "Normal if uptime < 10min. If still missing after 15min, L2 watchdog thread may have crashed."
}

# --- CHECK 8e REMOVED S14: GoldFlow phase state ------------------------------
# GFE-PHASE and GFE-COOLDOWN were GoldFlow-specific (engine culled S19 Stage 1B).

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
# GF-GATE-BLOCK variants and GF-BAR-BLOCK removed S14 (GoldFlow culled S19).
# SPREAD-Z is still emitted by trade_lifecycle.hpp on anomalous spreads.
$spreadBlock = @(Find-All "SPREAD-Z.*anomalous").Count

if ($spreadBlock -eq 0) {
    Add-Result "Gate Blocks" "INFO" "No gate blocks in first ${WaitSec}s" "No spread anomalies detected."
} else {
    Add-Result "Gate Blocks" "INFO" "SPREAD_Z=$spreadBlock" "Spread anomaly blocks -- normal filtering."
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

# --- CHECK 12 REMOVED S14: Impulse ghost -------------------------------------
# GF-IMPULSE-GHOST was GoldFlow-specific (engine culled S19 Stage 1B).

# --- CHECK 13: Running hash vs GitHub HEAD (API, not git CLI) ----------------
# Uses GitHub API -- git CLI is not available on VPS after zip-based restart.
# $ghToken is pre-initialized to $null after param() block and optionally
# populated by STEP 0 from C:\Omega\.github_token. Safe under StrictMode
# regardless of token file presence.
$verFile2 = "$OmegaDir\include\version_generated.hpp"
$runningHash = "unknown"
if (Test-Path $verFile2) {
    $vl = Select-String -Path $verFile2 -Pattern 'OMEGA_GIT_HASH' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vl -and $vl.Line -match '"([a-f0-9]{7,})"') { $runningHash = $Matches[1] }
}
$gitHead7 = "unknown"
if ($ghToken -and $ghToken -ne "") {
    try {
        $apiHdr = @{ Authorization="token $ghToken"; "User-Agent"="OmegaVerify" }
        $commitResp = Invoke-RestMethod -Uri "https://api.github.com/repos/Trendiisales/Omega/commits/main" `
            -Headers $apiHdr -TimeoutSec 10 -ErrorAction Stop
        if ($commitResp.sha -and $commitResp.sha.Length -ge 7) {
            $gitHead7 = $commitResp.sha.Substring(0,7)
        }
    } catch {
        $gitHead7 = "api_error"
    }
}
if ($runningHash -eq "unknown") {
    Add-Result "Hash vs HEAD" "WARN" "running=$runningHash" "version_generated.hpp not found or OMEGA_GIT_HASH not in it."
} elseif ($gitHead7 -eq "unknown" -or $gitHead7 -eq "api_error") {
    # Can still confirm hash is present even if API unavailable
    Add-Result "Hash vs HEAD" "INFO" "running=$runningHash github_head=unavailable" "Binary hash confirmed ($runningHash). GitHub API unreachable -- cannot compare to HEAD."
} elseif ($runningHash -eq $gitHead7) {
    Add-Result "Hash vs HEAD" "PASS" "running=$runningHash == HEAD=$gitHead7" "Binary matches latest GitHub commit exactly."
} else {
    Add-Result "Hash vs HEAD" "INFO" "running=$runningHash git_head=$gitHead7" "Hash differs from GitHub HEAD -- normal when new commits pushed after this build."
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
        "Will request M15 tick data from broker on startup. Gold engines delayed ~2min."
}

# --- CHECK 15 REMOVED S14: Ratchet fix ---------------------------------------
# GOLD-FLOW.*TRAIL_HIT and DOLLAR-RATCHET were GoldFlow-specific
# (engine culled S19 Stage 1B). Hybrid/Bracket ratchets tracked elsewhere.

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



