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
$osm = Get-CimInstance Win32_OperatingSystem
$freeMB = [int]($osm.FreePhysicalMemory / 1024)
if ($freeMB -lt 250) { $overall = 'RED'; $reasons += "[RED] RAM ${freeMB}MB free (<250) -- thrash/freeze risk" }
elseif ($freeMB -lt 500) { if ($overall -eq 'GREEN') { $overall = 'AMBER' }; $reasons += "[AMBER] RAM ${freeMB}MB free" }
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
$ts = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$obj = [ordered]@{ ts=$ts; overall=$overall; reasons=$reasons; binary_hash=$built; head_hash=$head; behind=$behind; code_behind=$codeBehind; disk_free_pct=$pct; ram_free_mb=$freeMB; gateway_up=$gwUp; gateway_port=$gwPort }
($obj | ConvertTo-Json -Depth 4) | Out-File -Encoding utf8 C:\Omega\logs\HEALTH_STATUS.json
"$ts overall=$overall " + ($reasons -join ' | ') | Out-File -Append C:\Omega\logs\health_alarm.log
$flag = 'C:\Omega\logs\HEALTH_ALARM.flag'
if ($overall -ne 'GREEN') {
    ($overall + " " + $ts + "`n" + ($reasons -join "`n")) | Out-File -Encoding utf8 $flag
} elseif (Test-Path $flag) { Remove-Item $flag -Force }
Write-Output ("overall=$overall " + ($reasons -join ' | '))

