# omega_health_alarm.ps1 - UNIFIED staleness/health alarm (operator-mandated 2026-06-27).
# Monitors the dimension that was UNMONITORED: deploy-staleness (built hash vs origin/main).
# + HEALTH_RED.flag (was silent) + disk + process. Writes HEALTH_STATUS.json (GUI) and
# raises HEALTH_ALARM.flag when not-green. No git fetch (hang-safe), no process killing.
$ErrorActionPreference = 'SilentlyContinue'
Set-Location C:\Omega
$reasons = @(); $overall = 'GREEN'
$stamp = @{}
if (Test-Path C:\Omega\omega_build.stamp) {
    Get-Content C:\Omega\omega_build.stamp | ForEach-Object {
        if ($_ -match '^(\w+)=(.+)$') { $stamp[$matches[1]] = $matches[2].Trim() }
    }
}
$builtFull = $stamp['GIT_HASH']
$built = $stamp['GIT_HASH_SHORT']
if (-not $built -and $builtFull) { $built = $builtFull.Substring(0,9) }
$head = (git rev-parse --short origin/main 2>$null)
$behind = 0; $codeBehind = 0
if ($builtFull -and $head) {
    $behind = [int](git rev-list --count "$builtFull..origin/main" 2>$null)
    if ($behind -gt 0) {
        $files = git diff --name-only "$builtFull" origin/main 2>$null
        $codeBehind = ($files | Where-Object { $_ -match '\.(cpp|h|hpp|c|cc|cmake)$|CMakeLists' } | Measure-Object).Count
    }
}
# EXEC_RETIRED.flag (S-2026-07-07o): box retired from execution post-cutover (shadow reference
# only, pending decommission). Suppresses by-design alarms (gateway down, deploy-stale) that
# otherwise spam the operator; disk/RAM/process/Omega.exe checks stay live (shadow still runs).
$execRetired = Test-Path C:\Omega\EXEC_RETIRED.flag
if ($behind -gt 0 -and -not $execRetired) {
    $sev = if ($codeBehind -gt 0) { 'RED' } else { 'AMBER' }
    if ($sev -eq 'RED') { $overall = 'RED' } elseif ($overall -eq 'GREEN') { $overall = 'AMBER' }
    $reasons += "[$sev] DEPLOY-STALE: binary=$built behind origin/main=$head by $behind commit(s); code-touching=$codeBehind"
}
# HEALTH_RED.flag is a HEARTBEAT written by omega_health.py every loop (~45s) while RED and
# unlinked when it clears. Trust it ONLY if FRESH. If that daemon dies mid-RED (2026-07-15: the
# OmegaHealthMonitor task killed it via a 15min ExecutionTimeLimit right after it wrote the flag),
# the flag strands and this script would RED forever off a dead 4h-old snapshot -- the recurring
# phantom HEALTH RED. A stale flag = orphaned writer -> ignore it and delete it (self-heal).
$redFlagPath = 'C:\Omega\HEALTH_RED.flag'
if (Test-Path $redFlagPath) {
    $flagAgeMin = ((Get-Date) - (Get-Item $redFlagPath).LastWriteTime).TotalMinutes
    if ($flagAgeMin -le 5) {
        $hr = (Get-Content $redFlagPath -Raw).Trim() -replace "`r?`n", ' / '
        $overall = 'RED'; $reasons += "[RED] HEALTH_RED.flag: $hr"
    } else {
        Remove-Item $redFlagPath -Force -ErrorAction SilentlyContinue
        $reasons += "[INFO] cleared orphaned HEALTH_RED.flag ($([int]$flagAgeMin)min stale -- omega_health.py daemon not refreshing it)"
    }
}
$c = Get-PSDrive C
$pct = [math]::Round($c.Free/($c.Used+$c.Free)*100,1)
if ($pct -lt 8) { $overall = 'RED'; $reasons += "[RED] disk C: ${pct}% free" }
elseif ($pct -lt 12) { if ($overall -eq 'GREEN') { $overall = 'AMBER' }; $reasons += "[AMBER] disk C: ${pct}% free" }
if (-not (Get-Process Omega -ErrorAction SilentlyContinue)) { $overall = 'RED'; $reasons += "[RED] Omega.exe NOT running" }
# --- DEPLOY-HANG: a deploy that started but is stuck (operator-mandated 2026-06-27) ---
$di = Get-ScheduledTaskInfo -TaskName 'OmegaDeployNow' -ErrorAction SilentlyContinue
if ($di -and $di.LastTaskResult -eq 267009) {   # 267009 = TASK is currently running (robust; CIM State enum compare is unreliable)
    $drt = [int]((Get-Date) - $di.LastRunTime).TotalMinutes
    if ($drt -gt 18) { $overall = 'RED'; $reasons += "[RED] DEPLOY-HANG: OmegaDeployNow running ${drt}min (>18) -- stuck build" }
    else { $reasons += "[INFO] deploy in progress ${drt}min" }
}
# also catch a build that left compiler procs running absurdly long with no task (orphan build)
$cl = Get-Process cl,MSBuild,cmake -ErrorAction SilentlyContinue | Where-Object { ((Get-Date) - $_.StartTime).TotalMinutes -gt 25 }
if ($cl) { $overall = 'RED'; $reasons += "[RED] DEPLOY-HANG: build process older than 25min ($(($cl|%{$_.ProcessName}) -join ','))" }
# --- RAM (low free = thrash/freeze, froze RDP 2026-06-27 on the 3GB box) ---
# Thresholds scale with box size so ONE script is correct on both the 3GB (old) and 6GB
# (ForexVPS Edge) boxes during migration: RED < 8% of total, AMBER < 16% -- reproduces the
# proven 250/500MB lines on 3GB and lands ~490/980MB on 6GB. Floors keep small boxes safe.
$osm = Get-CimInstance Win32_OperatingSystem
$totalMB = [int]($osm.TotalVisibleMemorySize / 1024)
$freeMB = [int]($osm.FreePhysicalMemory / 1024)
$ramRedMB = [math]::Max(250, [int]($totalMB * 0.08))
$ramAmbMB = [math]::Max(500, [int]($totalMB * 0.16))
if ($freeMB -lt $ramRedMB) { $overall = 'RED'; $reasons += "[RED] RAM ${freeMB}MB free (<${ramRedMB}) -- thrash/freeze risk" }
elseif ($freeMB -lt $ramAmbMB) { if ($overall -eq 'GREEN') { $overall = 'AMBER' }; $reasons += "[AMBER] RAM ${freeMB}MB free (<${ramAmbMB})" }
# --- DUPLICATE-PROCESS LEAK (the Aurora 4-copy pile-up that exhausted RAM) ---
$byScript = @{}
foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='pythonw.exe' or Name='python.exe' or Name='powershell.exe'" -ErrorAction SilentlyContinue)) {
    if ($p.CommandLine -and $p.CommandLine -match '([\w\-]+\.(py|ps1))') { $s = $matches[1]; $byScript[$s] = 1 + [int]$byScript[$s] }
}
foreach ($s in $byScript.Keys) { if ($byScript[$s] -gt 2) { $overall = 'RED'; $reasons += "[RED] PROCESS-LEAK: $($byScript[$s]) copies of $s running (>2)" } }
# --- IB GATEWAY API LISTENER (operator-mandated 2026-07-01 after 2nd manual re-login) ---
# The gateway PROCESS can be alive but NOT logged in (IBC nightly restart lands it at the login
# screen) -> zero API listener on 4002 -> EVERY engine's order path is silently dead while data
# feeds still look fresh (the "built != running != working" trap). Alarm on the LISTENER, not the
# process. Verified 2026-07-01: logged-out gateway = proc up, blank window, no 4002 socket anywhere.
# S-2026-07-11: market-closed window (Omega trades gold/FX/index/stock -- ALL shut Fri 21:00 UTC ->
# Sun 22:00 UTC; crypto is a SEPARATE system). Inside it, broker gateway down + idle dispatch loop
# are BY DESIGN, not faults (operator: "should not alarm on weekend, only crypto is running").
# Matches SessionFlat.hpp is_weekend. Weekend-sensitive checks downgrade RED->AMBER (surfaced, no
# alarm); infra faults (disk/RAM/process-down/leak/deploy-hang) stay RED any day.
$nowU_ = [DateTime]::UtcNow; $dow_ = $nowU_.DayOfWeek; $h_ = $nowU_.Hour
$marketClosed = ($dow_ -eq 'Saturday') -or ($dow_ -eq 'Friday' -and $h_ -ge 21) -or ($dow_ -eq 'Sunday' -and $h_ -lt 22)
$gwPort = 4001
$gwListen = Get-NetTCPConnection -State Listen -LocalPort $gwPort -ErrorAction SilentlyContinue
$gwProc = Get-Process ibgateway -ErrorAction SilentlyContinue
$gwUp = [bool]$gwListen
if (-not $gwUp -and -not $execRetired) {
    if ($marketClosed) {
        if ($overall -eq 'GREEN') { $overall = 'AMBER' }
        $reasons += "[AMBER] IB GATEWAY down -- EXPECTED, broker closed (weekend); only crypto (separate system) runs. RED resumes at market open."
    } else {
        $overall = 'RED'
        if ($gwProc) { $reasons += "[RED] IB GATEWAY LOGGED OUT -- ibgateway alive (pid $($gwProc.Id)) but NO API listener on ${gwPort}; RE-LOGIN needed (orders dead)" }
        else         { $reasons += "[RED] IB GATEWAY DOWN -- ibgateway process not running, no API listener on ${gwPort}; restart+login (orders dead)" }
    }
}
# --- ACTIVITY / TRADE-RATE (operator-mandated 2026-07-02: "a live process that never orders passes
# every alarm -- where is the redundancy"). The dimension that was UNMONITORED: the book can be
# alive, warm-seeded, routing to 4002, feeds fresh -- and post ZERO order intents for days. Every
# liveness/health check stays GREEN while it silently never trades ("built != running != working").
# This block keys on the one counter that proves trading actually happens: ENG-DISPATCH-STATS
# posted_exec. HARD (RED): the current-UTC-date log is missing (logger dead / date-rollover -- the
# exact confusion that masked this) OR its dispatch stats are stale (dispatch loop frozen while the
# process is technically alive). SOFT (AMBER->RED): posted_exec has not advanced for a long
# market-open window. The 17 live entry engines are slow D1/H4/M30 (fastest ~weekly, several
# regime-gated), so the window is HOURS and generous by design -- a quiet compression stretch is
# legit and must NOT cry wolf. Self-tracks exec transitions in a state file so it survives reboots
# (posted_exec resets to 0 on boot). Rides the existing Mac off-box poll (omega_health_poll.sh) via
# HEALTH_STATUS.json -> the popup path that already fires on RED, now covers trade-rate too.
$actWinH     = if ($env:OMEGA_ACTIVITY_WINDOW_H) { [double]$env:OMEGA_ACTIVITY_WINDOW_H } else { 72 }
$actRedH     = if ($env:OMEGA_ACTIVITY_RED_H)    { [double]$env:OMEGA_ACTIVITY_RED_H }    else { 168 }
$actStaleMin = 20
$actToday = [DateTime]::UtcNow.ToString('yyyy-MM-dd')
$actLog   = "C:\Omega\logs\omega_$actToday.log"
$postedExec = $null; $statAgeMin = $null; $quietH = $null
# $marketClosed defined above (gateway block) -- weekend/market-shut => no ticks => dispatch idle
# by design; the stale/absent-stats check below downgrades to AMBER in-window.
if (-not (Test-Path $actLog)) {
    $overall = 'RED'
    $reasons += "[RED] ACTIVITY: current-UTC-date log missing ($actLog) -- logger dead or date-rollover unhandled (orders unobservable)"
} else {
    $lastStat = Get-Content $actLog -Tail 400 | Select-String 'ENG-DISPATCH-STATS' | Select-Object -Last 1
    if (-not $lastStat) {
        # S-2026-07-08: UTC-midnight rollover false-RED. A poll landing right after 00:00 UTC
        # reads the brand-new day's log before the first ENG-DISPATCH-STATS line lands ->
        # $statAgeMin stays null -> "dispatch stats stale (min old)" RED while the loop is fine.
        # Fall back to the prior day's log tail; the (-lt -60 -> +1440) wrap below dates it right.
        $prevLog = "C:\Omega\logs\omega_$([DateTime]::UtcNow.AddDays(-1).ToString('yyyy-MM-dd')).log"
        if (Test-Path $prevLog) {
            $lastStat = Get-Content $prevLog -Tail 400 | Select-String 'ENG-DISPATCH-STATS' | Select-Object -Last 1
        }
    }
    if ($lastStat) {
        if ($lastStat.Line -match 'posted_exec=(\d+)') { $postedExec = [int]$matches[1] }
        if ($lastStat.Line -match '(\d{2}):(\d{2}):(\d{2})') {
            $lt = [DateTime]::UtcNow.Date.AddHours([int]$matches[1]).AddMinutes([int]$matches[2]).AddSeconds([int]$matches[3])
            $statAgeMin = [int]((([DateTime]::UtcNow) - $lt).TotalMinutes)
            if ($statAgeMin -lt -60) { $statAgeMin += 1440 }   # last line was just before UTC midnight
        }
    }
    if ($null -eq $postedExec -or ($null -ne $statAgeMin -and $statAgeMin -ge $actStaleMin)) {
        # S-2026-07-14: two proven false-RED paths, both "no stats visible yet", not a frozen loop:
        #   (a) UTC-midnight: poll lands seconds after 00:00; first ENG-DISPATCH-STATS of the new
        #       day's log arrives ~1min in, AND the S-07-08 prev-day fallback is dead on arrival
        #       because OmegaLogRotate zips yesterday's .log to logs/archive/*.zip AT midnight.
        #   (b) deploy restart: service is down 10-25min during rebuild, so the last visible stats
        #       line is older than $actStaleMin at the first poll after boot.
        # Grace: if the Omega process OR today's log is younger than the staleness threshold, the
        # stats simply haven't landed yet -> INFO, no alarm. A GENUINE freeze outlives the grace
        # (process old + log old + stats still absent/stale) and REDs exactly as before.
        $omegaProc = Get-Process Omega -ErrorAction SilentlyContinue
        $upMin = if ($omegaProc) { [int]((Get-Date) - $omegaProc.StartTime).TotalMinutes } else { 99999 }
        $logAgeMin = [int]((Get-Date) - (Get-Item $actLog).CreationTime).TotalMinutes
        $ageTxt = if ($null -ne $statAgeMin) { "${statAgeMin}min old" } else { "none found (log created ${logAgeMin}min ago)" }
        if ($upMin -lt $actStaleMin -or $logAgeMin -lt $actStaleMin) {
            $reasons += "[INFO] ACTIVITY: dispatch stats $ageTxt -- within boot/rollover grace (proc up ${upMin}min, log ${logAgeMin}min); first stats land ~1min after boot"
        } elseif ($marketClosed) {
            # Weekend / market shut: no ticks -> dispatch loop idle by design (operator: "should not
            # alarm on weekend, only crypto is running"). AMBER = surfaced, NO alarm. RED resumes at
            # market open (Sun 22:00 UTC) so a genuine weekday freeze still fires.
            if ($overall -eq 'GREEN') { $overall = 'AMBER' }
            $reasons += "[AMBER] ACTIVITY: no/stale dispatch stats ($ageTxt) -- EXPECTED, Omega markets closed (weekend); only crypto (separate system) runs. RED resumes at market open."
        } else {
            $overall = 'RED'
            $reasons += "[RED] ACTIVITY: dispatch stats stale ($ageTxt) -- ENG-DISPATCH loop frozen while process alive"
        }
    }
}
if ($null -ne $postedExec) {
    $stFile = 'C:\Omega\logs\activity_state.json'
    $st = $null; if (Test-Path $stFile) { try { $st = Get-Content $stFile -Raw | ConvertFrom-Json } catch {} }
    $nowU = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $lastExecTs = $nowU
    if ($st) {
        $lastExecTs = [long]$st.last_exec_ts; $prevVal = [int]$st.last_val
        if ($postedExec -ne $prevVal) { $lastExecTs = $nowU }   # advanced (traded) OR reset (reboot) -> rebaseline
    }
    ([ordered]@{ last_exec_ts=$lastExecTs; last_val=$postedExec; updated=$nowU } | ConvertTo-Json) | Out-File -Encoding utf8 $stFile
    # market-open hours since last exec (skip whole weekend days, UTC)
    $sinceH = 0.0; $cur = [DateTimeOffset]::FromUnixTimeSeconds($lastExecTs).UtcDateTime; $endT = [DateTime]::UtcNow
    while ($cur -lt $endT) {
        $nxt = $cur.AddHours(1); if ($nxt -gt $endT) { $nxt = $endT }
        if ($cur.DayOfWeek -ne 'Saturday' -and $cur.DayOfWeek -ne 'Sunday') { $sinceH += ($nxt - $cur).TotalHours }
        $cur = $nxt
    }
    $quietH = [int]$sinceH
    $isWeekend = ([DateTime]::UtcNow.DayOfWeek -eq 'Saturday' -or [DateTime]::UtcNow.DayOfWeek -eq 'Sunday')
    if (-not $isWeekend -and -not $execRetired) {
        # S-2026-07-11: posted_exec-stale is NO LONGER a RED. The live entry engines are slow D1/H4
        # (fire ~weekly), gold is long-only + regime-gated (blocks the main real-exec candidate for
        # days at a time), and the book is paper_only -- so a multi-day quiet is LEGITIMATE, not a
        # fault. RED-alarming it every poll was crying wolf (operator: recurring false HEALTH RED).
        # It now escalates only to AMBER (visible + surfaced, no alarm). A GENUINE fault -- frozen
        # dispatch loop or dead logger -- still REDs via the checks above (log-missing / stats-stale).
        if ($sinceH -ge $actWinH) { if ($overall -eq 'GREEN') { $overall = 'AMBER' }; $reasons += "[AMBER] ACTIVITY: no real order intent in ${quietH}h market-open (posted_exec=$postedExec) -- legit while gold is regime-gated + engines slow/paper; RED only if you EXPECT live orders (raise engines to live / unblock gold)" }
    }
}
$ts = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$obj = [ordered]@{ ts=$ts; overall=$overall; reasons=$reasons; binary_hash=$built; head_hash=$head; behind=$behind; code_behind=$codeBehind; disk_free_pct=$pct; ram_free_mb=$freeMB; gateway_up=$gwUp; gateway_port=$gwPort; posted_exec=$postedExec; dispatch_age_min=$statAgeMin; activity_quiet_h=$quietH; exec_retired=$execRetired }
($obj | ConvertTo-Json -Depth 4) | Out-File -Encoding utf8 C:\Omega\logs\HEALTH_STATUS.json
"$ts overall=$overall " + ($reasons -join ' | ') | Out-File -Append C:\Omega\logs\health_alarm.log
$flag = 'C:\Omega\logs\HEALTH_ALARM.flag'
if ($overall -ne 'GREEN') {
    ($overall + " " + $ts + "`n" + ($reasons -join "`n")) | Out-File -Encoding utf8 $flag
} elseif (Test-Path $flag) { Remove-Item $flag -Force }
Write-Output ("overall=$overall " + ($reasons -join ' | '))

