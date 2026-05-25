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
Check-File-Fresh "$Root\logs\watchdog.log" 10 "log.watchdog_fresh" "WARN"

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
        if ($f.Length -eq 0) {
            Add-Check $kv.Key "FAIL" "empty" "$p is 0 bytes"
        } elseif ($ageMin -gt 60) {
            Add-Check $kv.Key "WARN" "stale" ("{0:N0}m since last write" -f $ageMin)
        } else {
            Add-Check $kv.Key "OK" "ok" ("{0} bytes, {1:N1}m old" -f $f.Length, $ageMin)
        }
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
foreach ($port in @(4001, 4002)) {
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
        Add-Check "quote.recent_rate" "FAIL" "zero" "0 tick/quote lines in last 2000 log lines (feed dead?)"
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
        Add-Check "supervisor.recent_decisions" "FAIL" "zero" "No supervisor decisions in last 2000 log lines"
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
            Add-Check "gold_bracket.range_alive" "WARN" "idle" "Last $($diag.Count) GOLD-BRK-DIAG lines show range=0.00 (engine permanently IDLE; expected in QUIET_COMPRESSION, suspicious otherwise)"
        } else {
            Add-Check "gold_bracket.range_alive" "OK" "ok" "$nonZero of $($diag.Count) recent diags show non-zero range"
        }
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
