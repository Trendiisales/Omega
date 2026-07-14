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
#    L2 live       -- l2_live=1 confirms FIX 264=0 book feed delivering updates
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

# ==============================================================================
# UTF-8 CONSOLE OUTPUT (FIX 2026-05-07, moved after param block)
# ------------------------------------------------------------------------------
# Was originally placed before the param() block which broke PS's param parsing
# ("InvalidLeftHandSide" on every default-value assignment). param() must be
# the first executable statement after #Requires/comments. This script's
# banners are plain ASCII so encoding doesn't matter for our own Write-Host
# output -- the setting below is kept for any child-process output piped
# through this script in the future. try/catch because ISE rejects direct
# assignment to [Console]::OutputEncoding.
# ==============================================================================
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding           = [System.Text.Encoding]::UTF8
} catch {
    # Some hosts (ISE, restricted runspaces) reject this -- safe to ignore.
}

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
# Reads the running binary hash by globbing every .log under C:\Omega and
# C:\Omega\logs for the "Git hash:" startup tag. If they don't match: RED
# BANNER, hard exit. Cannot verify a stale binary. This is the check that
# prevents stale binaries from ever going unnoticed.
#
# FIX 2026-05-07 (post-beccede deploy, third attempt):
#   Three failures led to the current implementation:
#     v1 read latest.log for "RUNNING COMMIT:"  -- wrong tag (binary
#         actually writes "[OK] Git hash:").
#     v2 read omega_service_stderr.log for "[Omega] Git hash:"  -- wrong
#         file (NSSM does not redirect to that name in this config) and
#         wrong prefix.
#     v3 (this code) globs every *.log under C:\Omega and C:\Omega\logs,
#         scans each in full via Select-String, takes the last "Git hash:"
#         match in the newest file. Confirmed line format:
#           "HH:MM:SS  [OK] Git hash: <sha7> -- verify against GitHub HEAD"
#         (from latest.log line 34, 2026-05-07).
#   Tail-only scanning was tried and rejected: latest.log writes the banner
#   near the top of the file and grows continuously, so any finite tail
#   misses the banner once the engine has been up >~5 minutes.
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

    # ------------------------------------------------------------------------
    # Find running-binary hash by globbing every .log under C:\Omega(\logs)
    # for the "Git hash" tag emitted at binary startup.
    #
    # Confirmed format (from C:\Omega\logs\latest.log line 34, 2026-05-07):
    #     "HH:MM:SS  [OK] Git hash: <sha7> -- verify against GitHub HEAD"
    # The "[OK]" prefix may change -- only the substring "Git hash" plus a
    # 7-12 char hex tag is required. Pattern is case-insensitive.
    #
    # NOTE: Banners use plain ASCII ('+', '|', '=') instead of box-drawing
    # chars to avoid the cp1252-vs-UTF-8 mojibake that PS 5.1 produces when
    # reading a .ps1 source file without a UTF-8 BOM (the script's box-draw
    # bytes get parsed as Windows-1252 and render as garbage). ASCII works
    # under any encoding.
    #
    # NOTE: not_found is a WARN, not a hard exit. Earlier versions exited
    # immediately, which left the operator unable to see any of the
    # subsequent diagnostics. Stale-binary detection (running != HEAD)
    # does still hard-exit -- that's the safety case.
    # ------------------------------------------------------------------------
    $runningHash    = "not_found"
    $hashSourceFile = ""
    $filesSearched  = @()

    $candidateFiles = @(
        Get-ChildItem -Path "C:\Omega\logs", "C:\Omega" -Filter "*.log" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending
    )

    Write-Host "  [DEBUG] $($candidateFiles.Count) .log file(s) to scan" -ForegroundColor DarkGray

    foreach ($f in $candidateFiles) {
        $filesSearched += $f.FullName
        $hits = Select-String -Path $f.FullName -Pattern "Git hash" -SimpleMatch -ErrorAction SilentlyContinue
        if ($hits) {
            $lastHit = $hits | Select-Object -Last 1
            Write-Host "  [DEBUG] hit in $($f.Name) line $($lastHit.LineNumber): $($lastHit.Line.Trim())" -ForegroundColor DarkGray
            if ($lastHit.Line -match "(?i)Git hash:?\s*([a-f0-9]{7,12})") {
                $runningHash    = $Matches[1]
                $hashSourceFile = "$($f.FullName):$($lastHit.LineNumber)"
                break
            } else {
                Write-Host "  [DEBUG] line matched 'Git hash' literal but regex failed to capture hex" -ForegroundColor Yellow
            }
        }
    }

    if ($runningHash -ne "not_found") {
        Write-Host "  [LOG] Running hash : $runningHash" -ForegroundColor Cyan
        Write-Host "        Source       : $hashSourceFile" -ForegroundColor DarkGray
    } else {
        Write-Host "  [LOG] Running hash : not_found" -ForegroundColor Red
        if ($filesSearched.Count -gt 0) {
            Write-Host "        Searched $($filesSearched.Count) log file(s) for 'Git hash' (no match):" -ForegroundColor DarkGray
            foreach ($p in $filesSearched) { Write-Host "          - $p" -ForegroundColor DarkGray }
        } else {
            Write-Host "        No .log files found under C:\Omega\ or C:\Omega\logs\." -ForegroundColor DarkGray
        }
    }

    if ($ghSha7 -eq "api_unreachable") {
        Write-Host "  [WARN] Cannot verify -- GitHub API unreachable" -ForegroundColor Yellow
    } elseif ($runningHash -eq "not_found") {
        # SOFTENED 2026-05-07: was a hard exit; now warn-and-continue so the
        # rest of the verifier still produces output.
        Write-Host ""
        Write-Host "  +======================================================+" -ForegroundColor Yellow
        Write-Host "  |  BINARY HASH NOT FOUND -- staleness check skipped    |" -ForegroundColor Yellow
        Write-Host "  |  No .log under C:\Omega\ contains 'Git hash:' yet.   |" -ForegroundColor Yellow
        Write-Host "  |  Engine may have just restarted -- recheck in 30s,   |" -ForegroundColor Yellow
        Write-Host "  |  or run: .\OMEGA.ps1 deploy                         |" -ForegroundColor Yellow
        Write-Host "  +======================================================+" -ForegroundColor Yellow
        Write-Host ""
    } elseif ($runningHash -eq $ghSha7) {
        Write-Host "  [OK] Binary hash MATCHES GitHub HEAD: $runningHash" -ForegroundColor Green
    } else {
        # ----------------------------------------------------------------------
        # SMART STALENESS CHECK (added 2026-05-07)
        # ----------------------------------------------------------------------
        # Bare "running != HEAD" produces too many false alarms because
        # script-only commits (PS hotfixes, doc edits, config tweaks) advance
        # HEAD without changing the binary. The C++ binary at $runningHash is
        # functionally identical to anything HEAD would build, so the
        # "STALE BINARY" red banner + DO NOT TRADE warning is unwarranted.
        #
        # Use the GitHub compare API to enumerate every file that changed
        # between $runningHash and $ghSha7. Bucket each path:
        #   - "binary"  : C/C++ source (.cpp/.hpp/.h/.c/.cc/.cxx) under
        #                 include/, src/, external/, OR build config
        #                 (CMakeLists*, *.cmake, build.ninja, Makefile)
        #   - "script"  : everything else (PS scripts, docs, .ini, .yml, .md)
        #
        # If $binaryDiffPaths is empty -> YELLOW warn-and-continue.
        # If $binaryDiffPaths is non-empty -> RED hard exit (real stale).
        # If the API call fails -> RED hard exit (fail safe).
        # ----------------------------------------------------------------------
        $binaryDiffPaths = @()
        $scriptDiffPaths = @()
        $compareError    = $null
        try {
            $compareUri = "https://api.github.com/repos/Trendiisales/Omega/compare/$runningHash...$ghSha7"
            $compare = Invoke-RestMethod -Uri $compareUri -Headers $apiHeaders -TimeoutSec 15 -ErrorAction Stop
            foreach ($cf in $compare.files) {
                $name = $cf.filename
                $isBinarySrc = ($name -match '\.(cpp|hpp|h|c|cc|cxx)$') -or
                               ($name -match '^(include|src|external)/')   -or
                               ($name -match '(CMakeLists|build\.ninja|Makefile)') -or
                               ($name -match '\.cmake$')
                if ($isBinarySrc) { $binaryDiffPaths += $name }
                else              { $scriptDiffPaths += $name }
            }
        } catch {
            $compareError = $_.ToString()
        }

        if ($compareError) {
            Write-Host ""
            Write-Host "  +======================================================+" -ForegroundColor Red
            Write-Host "  |  STALE BINARY -- DIFF UNVERIFIED (FAIL SAFE)        |" -ForegroundColor Red
            Write-Host ("  |  Running : {0,-43} |" -f $runningHash) -ForegroundColor Red
            Write-Host ("  |  GitHub  : {0,-43} |" -f $ghSha7)      -ForegroundColor Red
            Write-Host "  |  Compare API failed -- cannot tell if binary changed |" -ForegroundColor Red
            Write-Host "  |  Fix     : .\OMEGA.ps1 deploy                      |" -ForegroundColor Red
            Write-Host "  +======================================================+" -ForegroundColor Red
            Write-Host "  api_error: $compareError" -ForegroundColor DarkGray
            Write-Host ""
            exit 1
        } elseif ($binaryDiffPaths.Count -gt 0) {
            Write-Host ""
            Write-Host "  +======================================================+" -ForegroundColor Red
            Write-Host "  |  STALE BINARY DETECTED -- DO NOT TRADE              |" -ForegroundColor Red
            Write-Host ("  |  Running : {0,-43} |" -f $runningHash) -ForegroundColor Red
            Write-Host ("  |  GitHub  : {0,-43} |" -f $ghSha7)      -ForegroundColor Red
            Write-Host ("  |  Binary source changed in {0,-3} file(s):           |" -f $binaryDiffPaths.Count) -ForegroundColor Red
            foreach ($p in ($binaryDiffPaths | Select-Object -First 5)) {
                Write-Host ("  |    - {0,-49} |" -f $p) -ForegroundColor Red
            }
            if ($binaryDiffPaths.Count -gt 5) {
                Write-Host ("  |    ... and {0,-3} more                                  |" -f ($binaryDiffPaths.Count - 5)) -ForegroundColor Red
            }
            Write-Host "  |  Fix     : .\OMEGA.ps1 deploy                      |" -ForegroundColor Red
            Write-Host "  +======================================================+" -ForegroundColor Red
            Write-Host ""
            exit 1
        } else {
            # Hash differs but only PS / docs / configs changed -- binary OK
            Write-Host ""
            Write-Host "  +======================================================+" -ForegroundColor Yellow
            Write-Host "  |  HASH MISMATCH -- but binary source UNCHANGED       |" -ForegroundColor Yellow
            Write-Host ("  |  Running : {0,-43} |" -f $runningHash) -ForegroundColor Yellow
            Write-Host ("  |  GitHub  : {0,-43} |" -f $ghSha7)      -ForegroundColor Yellow
            Write-Host ("  |  {0,-3} script/doc file(s) changed -- safe to continue |" -f $scriptDiffPaths.Count) -ForegroundColor Yellow
            Write-Host "  +======================================================+" -ForegroundColor Yellow
            Write-Host "  Script-only changes (binary functionally identical):" -ForegroundColor DarkGray
            foreach ($p in ($scriptDiffPaths | Select-Object -First 10)) {
                Write-Host "    - $p" -ForegroundColor DarkGray
            }
            if ($scriptDiffPaths.Count -gt 10) {
                Write-Host "    ... and $($scriptDiffPaths.Count - 10) more" -ForegroundColor DarkGray
            }
            Write-Host ""
            # NOT a hard exit -- continue verifier
        }
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
    # ---- Locate the live log ----------------------------------------------
    # STALENESS CONTRACT:
    #   1. Prefer latest.log IF it exists AND LastWriteTime < 120s ago.
    #      latest.log is truncated on every restart -- if fresh it is guaranteed
    #      to be from THIS run. The engine emits [LOG-HEALTH] every 60s so any
    #      live file will always be < 120s old.
    #   2. If latest.log is stale or missing, fall back to today's dated log
    #      with the same 120s freshness check.
    #   3. If BOTH are stale or missing: hard FAIL with diagnostics.
    #      Never silently analyse a frozen file.
    # -----------------------------------------------------------------------
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
            Write-Host "  Run: .\OMEGA.ps1 deploy" -ForegroundColor Yellow
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

# ------------------------------------------------------------------------------
# Market-hours awareness helper (added 2026-04-25, S21a)
#
# Purpose: during weekends and Fri-close -> Sun-open gap, tick streams stop,
# l2_ticks CSVs stop updating, and bar state on disk ages normally. The
# diagnostic previously reported these as FAIL ("STALE 243s", "CORRUPT
# flat/holiday state") which created noise on every weekend restart.
#
# XAUUSD (BlackBull FIX) schedule:
#   - Closes approximately Fri 21:00 UTC (22:00-23:00 NZ DST)
#   - Reopens approximately Sun 22:00 UTC (10:00-11:00 NZ Mon morning)
# Indices (US500, USTEC) follow similar CME weekend maintenance windows.
#
# Conservative closed-window definition (UTC):
#   - Saturday: closed all day
#   - Sunday:   closed until 22:00 UTC
#   - Friday:   closed after 21:00 UTC
# Outside that window, market is assumed open.
#
# Holidays are NOT handled here -- we deliberately err on the side of
# reporting FAILs during likely-open hours. If you are verifying during
# a US holiday when the tape is thin, ignore specific FAILs manually.
#
# Returns $true if the market is OPEN, $false if CLOSED.
# Also emits a [MARKET-HOURS] banner line for visibility in the report.
# ------------------------------------------------------------------------------
function Test-MarketOpen {
    $nowUtc = (Get-Date).ToUniversalTime()
    $dow    = [int]$nowUtc.DayOfWeek   # Sunday=0, Monday=1, ..., Saturday=6
    $hour   = $nowUtc.Hour

    # Saturday: always closed
    if ($dow -eq 6) { return $false }
    # Sunday before 22:00 UTC: closed
    if ($dow -eq 0 -and $hour -lt 22) { return $false }
    # Friday after/including 21:00 UTC: closed
    if ($dow -eq 5 -and $hour -ge 21) { return $false }
    # All other times: open
    return $true
}

$MARKET_OPEN = Test-MarketOpen
$nowUtcLabel = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')
if ($MARKET_OPEN) {
    Write-Host "  [MARKET-HOURS] XAUUSD OPEN (UTC $nowUtcLabel) -- full checks enforced" -ForegroundColor Green
} else {
    Write-Host "  [MARKET-HOURS] XAUUSD CLOSED (UTC $nowUtcLabel) -- weekend/gap rules active" -ForegroundColor Yellow
    Write-Host "                 L2 CSV staleness and bar-state age checks softened" -ForegroundColor DarkGray
}
Write-Host ""


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

# --- CHECK 8: L2 feed (FIX 264=0) --------------------------------------------
# S13 cTrader cull 2026-05-08: L2 source flipped from cTrader Open API to FIX
# 264=0. The [L2-STATUS] line in on_tick.hpp emits l2_live=<0|1> + gold_age_ms.
# CTRADER-STATUS / CTRADER-EVTS / CTRADER-BOOK / CTRADER-L2-CHECK / "cTrader
# depth feed ACTIVE" are dead log tags now -- never check for them.
$l2StatusLine = Find-Last "L2-STATUS"
if ($l2StatusLine -and $l2StatusLine -match "l2_live=1") {
    $age = if ($l2StatusLine -match "gold_age_ms=([0-9]+)") { $Matches[1] } else { "?" }
    Add-Result "L2 Feed (FIX 264=0)" "PASS" "l2_live=1 gold_age_ms=$age" $l2StatusLine.Trim()
} elseif ($l2StatusLine -and $l2StatusLine -match "l2_live=0") {
    Add-Result "L2 Feed (FIX 264=0)" "FAIL" "l2_live=0" "FIX 264=0 not delivering L2 to AtomicL2 dispatch -- check fix_dispatch.hpp wiring."
} else {
    Add-Result "L2 Feed (FIX 264=0)" "INFO" "No L2-STATUS yet" "L2-STATUS fires every 60s -- check again after the next interval."
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
        if (-not $MARKET_OPEN) {
            Add-Result "L2 Tick CSV XAUUSD" "INFO" "STALE ${l2Age_XAU}s (MARKET CLOSED)" "Weekend/gap window -- no ticks expected. Will resume on next session open. Age will be verified again when market reopens."
        } else {
            Add-Result "L2 Tick CSV XAUUSD" "FAIL" "STALE ${l2Age_XAU}s" "l2_ticks_XAUUSD not updated in ${l2Age_XAU}s -- logger stopped or Omega crashed."
        }
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
                "imbalance=0.500 all rows. FIX 264=0 may be delivering equal bid/ask sizes (genuinely flat DOM) or fix_dispatch.hpp may be skipping the AtomicL2 write."
        } else {
            $l2NonNeutral_XAU = $l2TotalCount_XAU - $l2NeutralCount_XAU
            Add-Result "L2 Tick CSV XAUUSD" "PASS" "age=${l2Age_XAU}s imb=$([math]::Round($l2LastImb_XAU,3)) real=$l2NonNeutral_XAU/$l2TotalCount_XAU" `
                "L2 imbalance producing directional values from FIX 264=0."
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
        if (-not $MARKET_OPEN) {
            Add-Result "L2 Tick CSV US500" "INFO" "STALE ${l2Age_SP}s (MARKET CLOSED)" "Weekend/gap window -- no ticks expected. Will resume on next session open."
        } else {
            Add-Result "L2 Tick CSV US500" "FAIL" "STALE ${l2Age_SP}s" "l2_ticks_US500 not updated in ${l2Age_SP}s -- SP logger stopped or US500 tick stream dead."
        }
    } else {
        $l2RowCount_SP = (Get-Content $l2CsvFile_SP | Where-Object { $_ -match '^[0-9]' } | Measure-Object).Count
        if ($l2RowCount_SP -eq 0) {
            Add-Result "L2 Tick CSV US500" "INFO" "age=${l2Age_SP}s writing" "CSV writing -- no rows yet. Check again in 30s."
        } else {
            $l2FileSize_SP = (Get-Item $l2CsvFile_SP).Length
            Add-Result "L2 Tick CSV US500" "PASS" "age=${l2Age_SP}s rows=$l2RowCount_SP size=${l2FileSize_SP}b" `
                "US500 tick logger writing to disk. l2_imb is zeroed by design (FIX 264=0 indices feed only top-of-book)."
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
        if (-not $MARKET_OPEN) {
            Add-Result "L2 Tick CSV USTEC" "INFO" "STALE ${l2Age_NQ}s (MARKET CLOSED)" "Weekend/gap window -- no ticks expected. Will resume on next session open."
        } else {
            Add-Result "L2 Tick CSV USTEC" "FAIL" "STALE ${l2Age_NQ}s" "l2_ticks_USTEC not updated in ${l2Age_NQ}s -- NQ logger stopped or USTEC tick stream dead."
        }
    } else {
        $l2RowCount_NQ = (Get-Content $l2CsvFile_NQ | Where-Object { $_ -match '^[0-9]' } | Measure-Object).Count
        if ($l2RowCount_NQ -eq 0) {
            Add-Result "L2 Tick CSV USTEC" "INFO" "age=${l2Age_NQ}s writing" "CSV writing -- no rows yet. Check again in 30s."
        } else {
            $l2FileSize_NQ = (Get-Item $l2CsvFile_NQ).Length
            Add-Result "L2 Tick CSV USTEC" "PASS" "age=${l2Age_NQ}s rows=$l2RowCount_NQ size=${l2FileSize_NQ}b" `
                "USTEC tick logger writing to disk. l2_imb is zeroed by design (FIX 264=0 indices feed only top-of-book)."
        }
    }
}

# --- CHECK 8b2 REMOVED S13 2026-05-08 ----------------------------------------
# CTRADER-L2-CHECK was emitted by CTraderDepthClient on the first 20 depth
# events per symbol. The cTrader Open API surface was culled at S13, so this
# log tag is dead. L2 health is now covered by CHECK 8 (L2-STATUS line) and
# CHECK 8b (L2 tick CSV staleness + imbalance neutrality).

# --- CHECK 8d REMOVED S14: GoldFlow blocking gates --------------------------
# Engine culled S19 Stage 1B. GF-GATE-BLOCK ENGINE_CULLED, GFE-CONFIG,
# GFE-FADE-BLOCK are all dead log tags. GF Bar State merged into
# Bar State Load check below (bars still used by RSI/HybridBracket/TrendPB).

# ==============================================================================
# CHECK: RSI Reversal Engine
# RSIReversalEngine was TOMBSTONED / disabled 2026-05-01 (negative-EV: real-tick
# backtest 4320 trades / 2yr = -$3,800; g_rsi_reversal.enabled=false in
# engine_init.hpp ~L805-836). A disabled engine never logs an "RSI-REV configured"
# startup line, so the absence of that line is EXPECTED, not a failure. Emitting a
# hard FAIL here made every deploy report "1 FAILURE(S)" and trained operators to
# ignore the failure count -- masking real startup failures. Demoted to INFO
# 2026-06-22. The SHADOW/PASS branches are kept so that IF the engine is ever
# re-enabled, this check resumes reporting its live/shadow state correctly.
# ==============================================================================
$rsiConfigLine = Get-Content $LogPath -ErrorAction SilentlyContinue |
    Where-Object { $_ -match "RSI-REV.*configured" } | Select-Object -Last 1
if (!$rsiConfigLine) {
    Add-Result "RSI Reversal Active" "INFO" "RSIReversal intentionally tombstoned (disabled 2026-05-01, negative-EV)" `
        "Not an error -- engine deliberately disabled (enabled=false), never logs a startup line. Re-enabling would flip this to PASS/WARN."
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
        "load_indicators() never called OR binary is stale. Run .\OMEGA.ps1 deploy to rebuild."
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
$runningHash = "unknown"
# AUTHORITATIVE: the RUNNING binary emits "[Omega] Git hash: <sha7>" to its service
# log at boot (omega_main.cpp). That is the only true running-binary hash -- read it
# FIRST. version_generated.hpp is a SOURCE file that is NOT shipped to the VPS runtime
# (OMEGA.ps1 deletes it pre-build), so the old file-only read always WARNed "unknown".
# (S-2026-06-24p: bug fix -- #4a startup hash-check.)
$hashLogs = @("$OmegaDir\logs\omega_service_stderr.log",
              "$OmegaDir\logs\omega_service_stdout.log") +
            @(Get-ChildItem "$OmegaDir\logs\omega_*.log" -ErrorAction SilentlyContinue |
              Sort-Object LastWriteTime -Descending | ForEach-Object { $_.FullName })
foreach ($lf in $hashLogs) {
    if (-not (Test-Path $lf)) { continue }
    $hl = Select-String -Path $lf -Pattern 'Git hash:\s*([a-f0-9]{7,})' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($hl) { $runningHash = $hl.Matches[0].Groups[1].Value.Substring(0,7); break }
}
# Fallback: the build-time source stamp, only if no service log carried the boot hash.
if ($runningHash -eq "unknown") {
    $verFile2 = "$OmegaDir\include\version_generated.hpp"
    if (Test-Path $verFile2) {
        $vl = Select-String -Path $verFile2 -Pattern 'OMEGA_GIT_HASH' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($vl -and $vl.Line -match '"([a-f0-9]{7,})"') { $runningHash = $Matches[1] }
    }
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
    Add-Result "Hash vs HEAD" "WARN" "running=$runningHash" "no '[Omega] Git hash:' line in any service log AND version_generated.hpp absent -- did the binary boot + log?"
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
            if (-not $MARKET_OPEN) {
                # Weekend/gap window: flat EMAs and sub-1.0 ATR are the expected
                # state of a bar file saved at Friday close and untouched since.
                # The file will rebuild from live ticks when market reopens.
                # Do NOT recommend deleting -- that forces a cold start Monday
                # and loses the seeded context.
                Add-Result "Bar State" "INFO" "WEEKEND-SAVED: e9=$e9d e50=$e50d rsi=$rsid atr=$atrd age=${ageMin}min" `
                    "Bar state aged from Fri close (market currently CLOSED). Flat EMAs / low ATR expected. Recheck after market reopens -- do NOT delete bars_gold_*.dat."
            } else {
                Add-Result "Bar State" "FAIL" "CORRUPT: e9=$e9d e50=$e50d rsi=$rsid atr=$atrd age=${ageMin}min" `
                    "Flat/holiday state on disk -- DELETE bars_gold_m1/m5/m15/h4.dat and redeploy."
            }
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

# --- CHECK 18: Seed freshness (2026-06-23) -----------------------------------
#   A stale warm-seed = an engine/gate boots with a price view detached from
#   reality (the gold_regime() bear-gate silently went 83 days blind this way,
#   buying gold into a downtrend). This check runs the seed-freshness audit so a
#   stale seed can never again rot undetected -- it surfaces in startup_report.txt.
try {
    # 2026-06-24: gate on the audit's EXIT CODE (0=clean, 1=stale-on-enabled) -- robust to the
    # de-noised output. Guard .Line for null (Select-String returns nothing when a pattern misses
    # -> ".Line on null" was the prior 'property Line cannot be found' crash).
    $sfaOut = & python "C:\Omega\tools\seed_freshness_audit.py" --repo "C:\Omega" 2>&1
    $rc = $LASTEXITCODE
    $sumHit = ($sfaOut | Select-String "active seed CSVs found" | Select-Object -First 1)
    $sumLine = if ($sumHit) { $sumHit.Line } else { "" }
    $staleN = if ($sumLine -match 'STALE\(enabled\):\s*(\d+)') { $Matches[1] } else { "?" }
    if ($rc -eq 1) {
        Add-Result "Seed Freshness" "WARN" "$staleN stale ENABLED-engine warm-seed(s) -- gate boots blind to current price" `
            "Refresh: python tools\seed_refresh.py --only ibkr (needs IBKR gateway 4002 live)"
    } elseif ($rc -eq 2) {
        # S-2026-07-01: audit exit 2 = REQUIRED generated file missing. Today that means
        # data\risk_monitor_thresholds.csv is absent -> RiskMonitor loads 0 rows -> on_fire()
        # early-returns for every engine -> the auto-demote-to-shadow surveillance layer is OFF.
        Add-Result "Seed Freshness" "FAIL" "data\risk_monitor_thresholds.csv MISSING -- RiskMonitor surveillance layer OFF (no auto-demote)" `
            "Build+run backtest\calibrate_risk_thresholds (cl /O2 /std:c++17 /Iinclude ...) from repo root, then restart"
    } elseif ($sumLine) {
        Add-Result "Seed Freshness" "PASS" "all enabled-engine warm-seeds fresh" "$sumLine"
    } else {
        Add-Result "Seed Freshness" "INFO" "audit produced no summary" "check python + tools\seed_freshness_audit.py"
    }
} catch {
    Add-Result "Seed Freshness" "INFO" "audit not run ($_)" "ensure python + tools\seed_freshness_audit.py present on the VPS"
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
