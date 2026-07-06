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
if ($behind -gt 0) {
    $sev = if ($codeBehind -gt 0) { 'RED' } else { 'AMBER' }
    if ($sev -eq 'RED') { $overall = 'RED' } elseif ($overall -eq 'GREEN') { $overall = 'AMBER' }
    $reasons += "[$sev] DEPLOY-STALE: binary=$built behind origin/main=$head by $behind commit(s); code-touching=$codeBehind"
}
if (Test-Path C:\Omega\HEALTH_RED.flag) {
    $hr = (Get-Content C:\Omega\HEALTH_RED.flag -Raw).Trim() -replace "`r?`n", ' / '
    $overall = 'RED'; $reasons += "[RED] HEALTH_RED.flag: $hr"
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
# --- RAM (3GB box -> low free = thrash/freeze, froze RDP 2026-06-27) ---
# S-2026-07-03 LEAK-AWARE upgrade (operator: "fix the Omega alerts/warning/memory
# issue"). The old check was static thresholds only: (a) chronic-low-but-STABLE
# free is this 3GB box's NORMAL (Omega+Gateway+feeds resident) so "<500MB" AMBER
# fired nearly every cycle = permanent warning noise = alert fatigue; (b) a real
# 400-800MB/h leak only went RED at <250MB -- minutes before the freeze, and the
# alert never said WHO was leaking even though mem_trace.csv had the answer.
# Now: mem_trace.ps1 writes MEM_LEAK.json (free-RAM slope + named top climber);
# this block warns on the DYNAMIC (falling free / projected freeze / named proc)
# and stays quiet on the static. RED <250MB kept as the last line.
$osm = Get-CimInstance Win32_OperatingSystem
$freeMB = [int]($osm.FreePhysicalMemory / 1024)
$leak = $null
if (Test-Path C:\Omega\logs\MEM_LEAK.json) {
    try { $leak = Get-Content C:\Omega\logs\MEM_LEAK.json -Raw | ConvertFrom-Json } catch {}
}
$ramSlope = $null; $ramEtaH = $null; $climber = ''
if ($leak) {
    if ($null -ne $leak.free_slope_mb_h) { $ramSlope = [double]$leak.free_slope_mb_h }
    if ($null -ne $leak.eta_freeze_h)    { $ramEtaH  = [double]$leak.eta_freeze_h }
    if ($leak.climbers -and @($leak.climbers).Count -gt 0) {
        $c0 = @($leak.climbers)[0]
        $climber = " -- top climber: $($c0.proc) +$($c0.slope_mb_h)MB/h"
    }
}
if ($freeMB -lt 250) {
    $overall = 'RED'; $reasons += "[RED] RAM ${freeMB}MB free (<250) -- thrash/freeze risk${climber}"
} elseif ($null -ne $ramSlope -and $ramSlope -le -300 -and $null -ne $ramEtaH -and $ramEtaH -le 2) {
    # pre-emptive: the leak is running NOW; fire 1-2h before the old check could
    $overall = 'RED'
    $reasons += "[RED] RAM LEAK: ${freeMB}MB free falling ${ramSlope}MB/h -> ~${ramEtaH}h to freeze threshold${climber}"
} elseif ($freeMB -lt 500) {
    if ($null -ne $ramSlope -and $ramSlope -le -50) {
        if ($overall -eq 'GREEN') { $overall = 'AMBER' }
        $reasons += "[AMBER] RAM ${freeMB}MB free AND falling ${ramSlope}MB/h${climber}"
    } else {
        # stable-low = this box's baseline; report, do NOT warn (kills the AMBER spam)
        $reasons += "[INFO] RAM ${freeMB}MB free (stable -- no leak slope)"
    }
}
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
$gwPort = 4002
$gwListen = Get-NetTCPConnection -State Listen -LocalPort $gwPort -ErrorAction SilentlyContinue
$gwProc = Get-Process ibgateway -ErrorAction SilentlyContinue
$gwUp = [bool]$gwListen
if (-not $gwUp) {
    $overall = 'RED'
    if ($gwProc) { $reasons += "[RED] IB GATEWAY LOGGED OUT -- ibgateway alive (pid $($gwProc.Id)) but NO API listener on ${gwPort}; RE-LOGIN needed (orders dead)" }
    else         { $reasons += "[RED] IB GATEWAY DOWN -- ibgateway process not running, no API listener on ${gwPort}; restart+login (orders dead)" }
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
if (-not (Test-Path $actLog)) {
    $overall = 'RED'
    $reasons += "[RED] ACTIVITY: current-UTC-date log missing ($actLog) -- logger dead or date-rollover unhandled (orders unobservable)"
} else {
    $lastStat = Get-Content $actLog -Tail 400 | Select-String 'ENG-DISPATCH-STATS' | Select-Object -Last 1
    if ($lastStat) {
        if ($lastStat.Line -match 'posted_exec=(\d+)') { $postedExec = [int]$matches[1] }
        if ($lastStat.Line -match '(\d{2}):(\d{2}):(\d{2})') {
            $lt = [DateTime]::UtcNow.Date.AddHours([int]$matches[1]).AddMinutes([int]$matches[2]).AddSeconds([int]$matches[3])
            $statAgeMin = [int]((([DateTime]::UtcNow) - $lt).TotalMinutes)
            if ($statAgeMin -lt -60) { $statAgeMin += 1440 }   # last line was just before UTC midnight
        }
    }
    if ($null -eq $postedExec -or ($null -ne $statAgeMin -and $statAgeMin -ge $actStaleMin)) {
        $overall = 'RED'
        $reasons += "[RED] ACTIVITY: dispatch stats stale (${statAgeMin}min old) -- ENG-DISPATCH loop frozen while process alive"
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
    if (-not $isWeekend) {
        if ($sinceH -ge $actRedH) { $overall = 'RED'; $reasons += "[RED] ACTIVITY: posted_exec has not advanced in ${quietH}h market-open (>= ${actRedH}h) -- book not trading; verify route+engines" }
        elseif ($sinceH -ge $actWinH) { if ($overall -eq 'GREEN') { $overall = 'AMBER' }; $reasons += "[AMBER] ACTIVITY: no order intent in ${quietH}h market-open (>= ${actWinH}h); posted_exec=$postedExec -- may be legit (compression); verify intended" }
    }
}
$ts = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$obj = [ordered]@{ ts=$ts; overall=$overall; reasons=$reasons; binary_hash=$built; head_hash=$head; behind=$behind; code_behind=$codeBehind; disk_free_pct=$pct; ram_free_mb=$freeMB; ram_slope_mb_h=$ramSlope; ram_eta_freeze_h=$ramEtaH; gateway_up=$gwUp; gateway_port=$gwPort; posted_exec=$postedExec; dispatch_age_min=$statAgeMin; activity_quiet_h=$quietH }
($obj | ConvertTo-Json -Depth 4) | Out-File -Encoding utf8 C:\Omega\logs\HEALTH_STATUS.json
# rotate the alarm log (S-2026-07-03): unbounded append on a disk-alarmed box
$halog = 'C:\Omega\logs\health_alarm.log'
try {
    if ((Test-Path $halog) -and (@(Get-Content $halog).Count -gt 5000)) {
        Get-Content $halog -Tail 4000 | Set-Content $halog
    }
} catch {}
"$ts overall=$overall " + ($reasons -join ' | ') | Out-File -Append $halog
$flag = 'C:\Omega\logs\HEALTH_ALARM.flag'
if ($overall -ne 'GREEN') {
    ($overall + " " + $ts + "`n" + ($reasons -join "`n")) | Out-File -Encoding utf8 $flag
} elseif (Test-Path $flag) { Remove-Item $flag -Force }
Write-Output ("overall=$overall " + ($reasons -join ' | '))

