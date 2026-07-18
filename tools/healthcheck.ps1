# ============================================================================
# healthcheck.ps1 - VPS invariant monitor for Omega + bracket-bot
#
# Runs every N minutes via Task Scheduler. Each run:
#   1. Evaluates a set of invariants (files exist + recent, service running,
#      ports listening, quote rate, disk space, etc).
#   2. Writes overall status to logs\health\status.json (atomic replace).
#   3. Appends any FAIL/WARN findings to logs\health\alerts.log.
#   4. Optionally POSTs alerts to a webhook (env var HEALTHCHECK_WEBHOOK,
#      e.g. Discord / Slack / Pushover URL).
#
# Dashboard reads status.json and shows a red banner + plays a sound on FAIL.
#
# Run manually (test):
#   powershell -ExecutionPolicy Bypass -File tools\healthcheck.ps1
#
# Schedule (Administrator):
#   $a = New-ScheduledTaskAction -Execute "powershell.exe" `
#        -Argument "-ExecutionPolicy Bypass -NoProfile -File C:\Omega\tools\healthcheck.ps1"
#   $t = New-ScheduledTaskTrigger -Once (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 2)
#   $p = New-ScheduledTaskPrincipal -UserId "SYSTEM" -RunLevel Highest
#   Register-ScheduledTask -TaskName "Omega Healthcheck" -Action $a -Trigger $t -Principal $p -Force
# ============================================================================
$ErrorActionPreference = 'Continue'

$Root      = 'C:\Omega'
$HealthDir = Join-Path $Root 'logs\health'
$StatusFile = Join-Path $HealthDir 'status.json'
$AlertsLog  = Join-Path $HealthDir 'alerts.log'
$Webhook    = $env:HEALTHCHECK_WEBHOOK

New-Item -ItemType Directory -Path $HealthDir -Force | Out-Null

$now = Get-Date
$checks = New-Object System.Collections.ArrayList

function Add-Check {
    param([string]$Name, [string]$Severity, [string]$Status, [string]$Detail)
    $null = $checks.Add([PSCustomObject]@{
        name = $Name; severity = $Severity; status = $Status; detail = $Detail
        ts = $now.ToString('o')
    })
}

# ---------- PER-MARKET SESSION BLOCKS (2026-07-10) ----------
# Operator: "ensure we have the blocks for each session so we dont get [false warnings]."
# The old single global FX-24x5 window flagged EVERY feed/book stale until Fri 22:00 UTC,
# so an equity/index shadow book quiet OVERNIGHT (US cash + EUREX both closed) tripped a
# WARN at e.g. 22:31 UTC Thu even though the writer was alive (boot heartbeat present) and
# there were simply no trades. Each market now has its OWN session so a stale feed/book is
# only judged when THAT market is actually open. All times UTC; DST-agnostic (uses the wider
# summer envelope -- a ~1h edge slop is harmless for a staleness gate).
function Test-MarketOpen {
    param([string]$Market, [datetime]$Utc = $now.ToUniversalTime())
    $d = [int]$Utc.DayOfWeek          # 0=Sun .. 6=Sat
    $h = $Utc.Hour + $Utc.Minute/60.0
    switch ($Market) {
        # US cash equities (NYSE/NASDAQ) 09:30-16:00 ET = 13:30-20:00 UTC, Mon-Fri.
        'US_EQUITY' { return ($d -ge 1 -and $d -le 5 -and $h -ge 13.5 -and $h -lt 20.0) }
        # EUREX index futures (FDAX/FESX) main+late ~07:00-20:00 UTC, Mon-Fri.
        'EUREX'     { return ($d -ge 1 -and $d -le 5 -and $h -ge 7.0  -and $h -lt 20.0) }
        # CME/COMEX futures (ES/NQ/YM/M2K/XAU) ~23h: Sun 22:00 -> Fri 21:00 UTC, daily 21-22 break.
        'CME'       { return -not (($d -eq 6) -or ($d -eq 0 -and $h -lt 22) -or ($d -eq 5 -and $h -ge 21) -or ($h -ge 21 -and $h -lt 22)) }
        # FX 24x5: Sun 22:00 -> Fri 22:00 UTC.
        'FX'        { return -not (($d -eq 6) -or ($d -eq 0 -and $h -lt 22) -or ($d -eq 5 -and $h -ge 22)) }
        # CORE = the window the SHADOW BOOK is meaningfully active (US equity + EUREX index +
        # the US-futures session overlap). Outside it the book is quiet -> staleness expected.
        'CORE'      { return ($d -ge 1 -and $d -le 5 -and $h -ge 13.0 -and $h -lt 21.0) }
        # GOLD_ACTIVE = London+NY gold session (~07:00-21:00 UTC weekdays) where the bracket range
        # SHOULD be non-zero. Overnight/Asian-thin + the 21-22 UTC CME break, range=0.00 is normal.
        'GOLD_ACTIVE' { return ($d -ge 1 -and $d -le 5 -and $h -ge 7.0 -and $h -lt 21.0) }
        default     { return $true }
    }
}

# ---------- 1. Service running ----------
$svc = Get-Service Omega -ErrorAction SilentlyContinue
if (-not $svc) {
    Add-Check "service.exists"  "FAIL" "missing"  "Omega service not registered"
} elseif ($svc.Status -ne 'Running') {
    Add-Check "service.running"  "FAIL" "stopped"  "Omega service status=$($svc.Status)"
} else {
    Add-Check "service.running"  "OK"   "running"  "Omega service Running"
}

# ---------- 2. Omega exe matches origin/main hash ----------
try {
    Push-Location $Root
    $head    = (git rev-parse HEAD 2>$null)
    $origin  = (git rev-parse origin/main 2>$null)
    Pop-Location
    if ($head -and $origin -and $head -ne $origin) {
        Add-Check "git.head_matches_origin" "WARN" "drift" "HEAD=$head origin/main=$origin"
    } else {
        Add-Check "git.head_matches_origin" "OK" "aligned" "HEAD == origin/main"
    }
} catch { Add-Check "git.head_matches_origin" "WARN" "error" $_.Exception.Message }

# ---------- 3. Log freshness ----------
function Check-File-Fresh {
    param([string]$Path, [int]$MaxAgeMin, [string]$Name, [string]$Severity = 'FAIL')
    if (-not (Test-Path $Path)) {
        Add-Check $Name $Severity "missing" "$Path does not exist"
        return
    }
    $f = Get-Item $Path
    $ageMin = ($now - $f.LastWriteTime).TotalMinutes
    if ($ageMin -gt $MaxAgeMin) {
        Add-Check $Name $Severity "stale" ("Last write {0:N1}m ago (limit {1}m): $Path" -f $ageMin, $MaxAgeMin)
    } else {
        Add-Check $Name "OK" "fresh" ("{0:N1}m old" -f $ageMin)
    }
}

Check-File-Fresh "$Root\logs\latest.log"  5   "log.latest_fresh"
# RUNNING-BINARY HASH == HEAD (2026-07-10) -- absorbs the retired python watchdog.
# The old watchdog (tools/omega_health.py -> logs/watchdog.log) verified running==github but
# DIED 2026-07-07 with NO scheduled task = no restart/backup -> silently stale for 3 days
# (operator: "watchdogs supposed to be watchdogs, how can they fail with no fix/backup?"). Its
# job is now folded into THIS check. The healthcheck is a robust 2-min + AtStartup Task Scheduler
# job, so the SCHEDULER is the backup -- it cannot silently die the way a lone python loop did.
# Reads the running binary's embedded Git hash from stderr; compares to HEAD ($head from step 2).
$stderrLog = Join-Path $Root 'logs\omega_service_stderr.log'
if ((Test-Path $stderrLog) -and $head) {
    $hline = Select-String -Path $stderrLog -Pattern 'Git hash:\s*([0-9a-f]{7,})' | Select-Object -Last 1
    if ($hline) {
        $runHash = ([regex]::Match($hline.Line, 'Git hash:\s*([0-9a-f]{7,})')).Groups[1].Value
        if ($head.StartsWith($runHash)) {
            Add-Check "binary.hash_matches_head" "OK" "aligned" "running=$runHash == HEAD"
        } else {
            # Drift is only real if the commits since the running binary touch BINARY-affecting
            # paths (include/ src/ CMakeLists / *.hpp / *.cpp). A tools/docs/.ps1-only advance
            # does NOT need a rebuild -> not a warning (avoids a false "redeploy owed" after every
            # ops commit -- 2026-07-10).
            Push-Location $Root
            $changed = @(git diff --name-only "$runHash..HEAD" 2>$null)
            Pop-Location
            $binChanged = $changed | Where-Object { $_ -match '^(include/|src/|CMakeLists|.*\.hpp|.*\.cpp)' }
            if ($binChanged) {
                Add-Check "binary.hash_matches_head" "WARN" "drift" ("running binary={0} != HEAD={1}; binary code changed since -> rebuild/redeploy owed" -f $runHash, $head.Substring(0,7))
            } else {
                Add-Check "binary.hash_matches_head" "OK" "aligned_nonbinary" ("running={0}; HEAD={1} advanced but only non-binary (tools/docs) -> no rebuild needed" -f $runHash, $head.Substring(0,7))
            }
        }
    } else {
        Add-Check "binary.hash_matches_head" "WARN" "no_hash" "no 'Git hash:' line in stderr log"
    }
} else {
    Add-Check "binary.hash_matches_head" "WARN" "no_stderr" "stderr log or HEAD unavailable"
}

# Today's per-day log must exist and be non-empty
$today = $now.ToString('yyyy-MM-dd')
$todayLog = "$Root\logs\omega_$today.log"
if (-not (Test-Path $todayLog)) {
    Add-Check "log.today_exists" "FAIL" "missing" "No log file for $today"
} else {
    $sz = (Get-Item $todayLog).Length
    if ($sz -eq 0) {
        Add-Check "log.today_nonempty" "FAIL" "empty" "$todayLog is 0 bytes"
    } else {
        Add-Check "log.today_nonempty" "OK" "ok" "$sz bytes"
    }
}

# ---------- 4. Shadow CSVs (the silent-loss case from 2026-05-25) ----------
# 2026-05-26 ESCALATION: omega_shadow.csv went silent on 2026-05-05 and the
# previous WARN-at-60min check fired 15,000+ times over 3 weeks but the
# WARN-amber-badge was not actioned. Lessons:
#   1. STALE > 2h during market hours is FAIL not WARN.
#   2. EMPTY file is FAIL regardless of file existence.
#   3. We also stat the DAILY rotated shadow file -- cumulative can legitimately
#      go stale on a server restart, but daily file MUST be fresh in-session.
# Market hours = Sun 22:00 UTC -> Fri 22:00 UTC (FX 24x5). Outside that, stale
# is tolerated (WARN only).
$utcNow = $now.ToUniversalTime()
$dayOfWeek = [int]$utcNow.DayOfWeek  # 0=Sun, 6=Sat
$hourUtc   = $utcNow.Hour
$isMarketHours = -not (
    ($dayOfWeek -eq 6) -or                        # all Sat
    ($dayOfWeek -eq 0 -and $hourUtc -lt 22) -or   # Sun before 22 UTC
    ($dayOfWeek -eq 5 -and $hourUtc -ge 22)       # Fri after 22 UTC
)
# Off-hours a stale shadow CSV is EXPECTED (no trades to write) -> OK, not WARN.
# 2026-07-10 FIX (operator: "why did these fire when the NY session is not current"): the
# shadow book is equity/index-dominated, so its staleness must be judged against the CORE
# trading session (US equity + EUREX + US-futures overlap, weekdays 13:00-21:00 UTC), NOT the
# 24x5 FX window. Overnight (e.g. 22:31 UTC Thu) the writer is alive (boot heartbeat) but there
# are simply no trades -> stale is EXPECTED, must not warn. Use CORE, not $isMarketHours.
$shadowActive  = Test-MarketOpen 'CORE'
$staleSeverity = if ($shadowActive) { 'FAIL' } else { 'OK' }

$shadowFiles = @{
    'shadow.omega_shadow_csv'          = 'logs\shadow\omega_shadow.csv'
    'shadow.omega_shadow_signals_csv'  = 'logs\shadow\omega_shadow_signals.csv'
}
foreach ($kv in $shadowFiles.GetEnumerator()) {
    $p = Join-Path $Root $kv.Value
    if (-not (Test-Path $p)) {
        Add-Check $kv.Key "FAIL" "missing" "$p does not exist"
    } else {
        $f = Get-Item $p
        $ageMin = ($now - $f.LastWriteTime).TotalMinutes
        $staleStatus = if ($shadowActive) { "stale" } else { "off_session" }
        if ($f.Length -eq 0) {
            Add-Check $kv.Key "FAIL" "empty" "$p is 0 bytes"
        } elseif ($ageMin -gt 120) {
            # 2h+ stale: FAIL only during the CORE session; off-session = OK (quiet, no trades).
            Add-Check $kv.Key $staleSeverity $staleStatus ("{0:N0}m since last write (core_session={1})" -f $ageMin, $shadowActive)
        } elseif ($ageMin -gt 60) {
            # 1-2h stale: WARN in the CORE session, OK off-session.
            $sev60 = if ($shadowActive) { "WARN" } else { "OK" }
            Add-Check $kv.Key $sev60 $staleStatus ("{0:N0}m since last write (core_session={1})" -f $ageMin, $shadowActive)
        } else {
            Add-Check $kv.Key "OK" "ok" ("{0} bytes, {1:N1}m old" -f $f.Length, $ageMin)
        }
    }
}

# 4b. Daily rotated shadow trade file (omega_shadow_trades_YYYY-MM-DD.csv).
# Cumulative file can lag; daily file is the authoritative real-time signal.
$todayUtc = $utcNow.ToString('yyyy-MM-dd')
$dailyShadowCandidates = @(
    "logs\shadow\daily\omega_shadow_trades_$todayUtc.csv",
    "logs\shadow\omega_shadow_trades_$todayUtc.csv",
    "logs\daily\omega_shadow_trades_$todayUtc.csv"
)
$dailyFound = $false
foreach ($rel in $dailyShadowCandidates) {
    $p = Join-Path $Root $rel
    if (Test-Path $p) {
        $dailyFound = $true
        $f = Get-Item $p
        $ageMin = ($now - $f.LastWriteTime).TotalMinutes
        if ($f.Length -eq 0) {
            Add-Check "shadow.daily_today" "WARN" "empty" "Daily shadow trades file exists but is 0 bytes ($p)"
        } elseif ($isMarketHours -and $ageMin -gt 240) {
            Add-Check "shadow.daily_today" "WARN" "stale" ("Daily file {0:N0}m old during market hours: $p" -f $ageMin)
        } else {
            Add-Check "shadow.daily_today" "OK" "ok" ("{0} bytes, {1:N1}m old" -f $f.Length, $ageMin)
        }
        break
    }
}
if (-not $dailyFound) {
    # No daily file for today yet -- only fail during deep-session market hours
    if ($isMarketHours -and $hourUtc -ge 8 -and $hourUtc -le 21) {
        Add-Check "shadow.daily_today" "WARN" "missing" "No omega_shadow_trades_$todayUtc.csv in any expected location"
    } else {
        Add-Check "shadow.daily_today" "OK" "ok" "No daily file yet (off-hours or early-session)"
    }
}

# ---------- 5. Bracket-bot data file ----------
$btData = "$Root\bracket-bot\data\trades.ndjson"
if (Test-Path $btData) {
    $f = Get-Item $btData
    Add-Check "bracketbot.trades_ndjson" "OK" "ok" ("{0} bytes" -f $f.Length)
} else {
    Add-Check "bracketbot.trades_ndjson" "WARN" "missing" "No trades.ndjson yet"
}

# ---------- 6. IB Gateway ports ----------
foreach ($port in @(4001)) {
    try {
        $r = Test-NetConnection -ComputerName 127.0.0.1 -Port $port -WarningAction SilentlyContinue
        if ($r.TcpTestSucceeded) {
            Add-Check "ib.port_$port" "OK" "listening" "127.0.0.1:$port"
        } else {
            Add-Check "ib.port_$port" "WARN" "closed" "127.0.0.1:$port not listening"
        }
    } catch { Add-Check "ib.port_$port" "WARN" "error" $_.Exception.Message }
}

# ---------- 7. Disk space ----------
$drive = Get-PSDrive C
$freeGB = [math]::Round($drive.Free / 1GB, 1)
if ($freeGB -lt 5) {
    Add-Check "disk.free_gb" "FAIL" "low" "$freeGB GB free"
} elseif ($freeGB -lt 15) {
    Add-Check "disk.free_gb" "WARN" "tight" "$freeGB GB free"
} else {
    Add-Check "disk.free_gb" "OK" "ok" "$freeGB GB free"
}

# ---------- 8. Quote rate from latest.log (last 2000 lines) ----------
# 2026-05-25 first-version regex `tick|on_tick|md_update` was a false-positive
# generator -- the engine actually logs `[TICK] SYMBOL bid/ask` and `[GOLD-L2-LIVE]`
# /`[GOLD-VOL]`. Match those literally (-SimpleMatch is per-pattern; use regex).
try {
    $recent = Get-Content "$Root\logs\latest.log" -Tail 2000 -ErrorAction SilentlyContinue |
              Select-String -Pattern '\[TICK\]|\[GOLD-L2-LIVE\]|\[GOLD-VOL\]'
    $rate = if ($recent) { $recent.Count } else { 0 }
    if ($rate -lt 1) {
        # Off-hours (weekend / market closed) a zero quote rate is EXPECTED -- OK, not WARN, so
        # overall HEALTH stays green on weekends (operator 2026-07-04: no weekend-nonsense warnings).
        # In-session a dead feed is still FAIL.
        $qsev = if ($isMarketHours) { 'FAIL' } else { 'OK' }
        $qsta = if ($isMarketHours) { 'zero' } else { 'off_hours' }
        $qdet = if ($isMarketHours) { "0 tick/quote lines in last 2000 log lines (feed dead?)" } else { "0 tick/quote lines (market closed -- expected)" }
        Add-Check "quote.recent_rate" $qsev $qsta $qdet
    } elseif ($rate -lt 20) {
        Add-Check "quote.recent_rate" "WARN" "low" "$rate tick/quote lines in last 2000 log lines"
    } else {
        Add-Check "quote.recent_rate" "OK" "ok" "$rate tick/quote lines in last 2000 log lines"
    }
} catch { Add-Check "quote.recent_rate" "WARN" "error" $_.Exception.Message }

# ---------- 9. Supervisor signal liveness (NEW 2026-05-25) ----------
# Counts supervisor decision changes in the last 2000 log lines. The 2026-05-25
# silent-loss episode was the trigger -- supervisor was firing but downstream
# (omega_shadow_signals.csv) wasn't recording. Now that the writer exists, we
# can also assert the supervisor itself is alive (regardless of the CSV writer).
try {
    $sup = Get-Content "$Root\logs\latest.log" -Tail 2000 -ErrorAction SilentlyContinue |
           Select-String -Pattern '\[SUPERVISOR-'
    $supn = if ($sup) { $sup.Count } else { 0 }
    if ($supn -lt 1) {
        # Off-hours the supervisor has nothing to decide (no quotes) -- expected, OK not WARN so
        # weekend HEALTH stays green (operator 2026-07-04). In-session silence is still FAIL.
        $ssev = if ($isMarketHours) { 'FAIL' } else { 'OK' }
        $ssta = if ($isMarketHours) { 'zero' } else { 'off_hours' }
        $sdet = if ($isMarketHours) { "No supervisor decisions in last 2000 log lines" } else { "No supervisor decisions (market closed -- expected)" }
        Add-Check "supervisor.recent_decisions" $ssev $ssta $sdet
    } elseif ($supn -lt 5) {
        Add-Check "supervisor.recent_decisions" "WARN" "low" "$supn supervisor decisions in last 2000 log lines"
    } else {
        Add-Check "supervisor.recent_decisions" "OK" "ok" "$supn supervisor decisions in last 2000 log lines"
    }
} catch { Add-Check "supervisor.recent_decisions" "WARN" "error" $_.Exception.Message }

# ---------- 10. Gold bracket arm liveness ----------
# brk_hi=0.00 brk_lo=0.00 range=0.00 indefinitely means engine is permanently
# IDLE -- either correct (QUIET_COMPRESSION) or a config mismatch. Warn if the
# last 200 [GOLD-BRK-DIAG] lines all show range<eff_min.
try {
    $diag = Get-Content "$Root\logs\latest.log" -Tail 2000 -ErrorAction SilentlyContinue |
            Select-String -Pattern '\[GOLD-BRK-DIAG\]' | Select-Object -Last 200
    if ($diag) {
        $nonZero = @($diag | Where-Object { $_.Line -match 'range=([0-9.]+)' -and [double]$matches[1] -gt 0 }).Count
        if ($nonZero -eq 0) {
            # 2026-07-10: range=0.00 is EXPECTED overnight / in the 21-22 UTC CME break / QUIET_COMPRESSION.
            # Only "suspicious" (WARN) during the ACTIVE gold session (London+NY); off-session it's a
            # normal quiet tape -> OK, so the badge stops crying wolf at night (operator: build it properly).
            if (Test-MarketOpen 'GOLD_ACTIVE') {
                Add-Check "gold_bracket.range_alive" "WARN" "idle" "range=0.00 across last $($diag.Count) GOLD-BRK-DIAG lines DURING the active gold session -- investigate (config/feed)"
            } else {
                Add-Check "gold_bracket.range_alive" "OK" "quiet_off_session" "range=0.00 (last $($diag.Count) diags) -- expected: gold off-session / CME break / QUIET_COMPRESSION"
            }
        } else {
            Add-Check "gold_bracket.range_alive" "OK" "ok" "$nonZero of $($diag.Count) recent diags show non-zero range"
        }
    } elseif (-not $isMarketHours) {
        Add-Check "gold_bracket.range_alive" "OK" "off_hours" "No [GOLD-BRK-DIAG] lines (market closed -- expected)"
    } else {
        Add-Check "gold_bracket.range_alive" "WARN" "no_diag" "No [GOLD-BRK-DIAG] lines in last 2000 log lines"
    }
} catch { Add-Check "gold_bracket.range_alive" "WARN" "error" $_.Exception.Message }

# ---------- Summary + write status.json (atomic) ----------
$fails = @($checks | Where-Object { $_.severity -eq 'FAIL' })
$warns = @($checks | Where-Object { $_.severity -eq 'WARN' })
$overall = if ($fails.Count -gt 0) { 'FAIL' } elseif ($warns.Count -gt 0) { 'WARN' } else { 'OK' }

$payload = [PSCustomObject]@{
    ts        = $now.ToString('o')
    overall   = $overall
    fail_count = $fails.Count
    warn_count = $warns.Count
    checks    = $checks
}

$json = $payload | ConvertTo-Json -Depth 5
$tmp  = "$StatusFile.tmp"
Set-Content -Path $tmp -Value $json -Encoding UTF8
Move-Item -Path $tmp -Destination $StatusFile -Force

# Append failures + warnings to alerts.log
if ($fails.Count -gt 0 -or $warns.Count -gt 0) {
    $line = "[$($now.ToString('o'))] overall=$overall fail=$($fails.Count) warn=$($warns.Count)"
    Add-Content -Path $AlertsLog -Value $line
    foreach ($c in ($fails + $warns)) {
        Add-Content -Path $AlertsLog -Value ("  - {0} [{1}] {2}: {3}" -f $c.severity, $c.status, $c.name, $c.detail)
    }
}

# Optional webhook (POST JSON). Suppress on first transition handled by webhook itself.
if ($Webhook -and ($fails.Count -gt 0 -or $warns.Count -gt 0)) {
    try {
        Invoke-RestMethod -Uri $Webhook -Method Post -Body $json -ContentType 'application/json' -TimeoutSec 5 | Out-Null
    } catch {
        Add-Content -Path $AlertsLog -Value "[webhook] POST failed: $($_.Exception.Message)"
    }
}

# Exit code reflects severity for any caller that cares
if ($overall -eq 'FAIL') { exit 2 }
elseif ($overall -eq 'WARN') { exit 1 }
else { exit 0 }
