# prune_hung_ssh_queries.ps1 — SAFE guard against RAM exhaustion from leaked ssh queries.
# (operator-mandated 2026-06-27: "build a check that does not allow you to keep messing up my memory")
#
# THE PROBLEM: every `ssh omega-new powershell ...` an AI session runs spawns a powershell on the VPS.
# If that command hangs (slow CIM on a paging box) the local ssh dies but the remote powershell lingers
# -- they pile up and starve the 3GB box's RAM until it freezes.
#
# 2026-06-29 FILTER WIDENED (operator livid, leak recurred): the original guard only matched
# `-EncodedCommand`, but ad-hoc AI queries use `powershell -NoProfile -Command "..."` (NOT encoded),
# so the guard NEVER caught them -- they piled to 10 shells, RAM 63->22MB. Now it also reaps
# `-Command` query shells. STILL hard-safe: never `-File`, never python, never a `.ps1` invocation
# (deploy/build/health-writer run via `-Command "& C:\...\X.ps1"` -- those are spared), age-gated > MAX_MIN.
#
# THIS IS NOT THE POISON REAPER. The reaper that killed the feeds (reverted+deleted 2026-06-26) killed
# powershell BY AGE alone -- which hit the feed BRIDGES (long-lived `powershell -File`). This guard kills
# ONLY hung interactive/ssh QUERY shells and NEVER touches `-File`, python.exe, .ps1 launches, or feeds.
$ErrorActionPreference = 'SilentlyContinue'
$MAX_MIN = 5
$cut = (Get-Date).AddMinutes(-$MAX_MIN)
$killed = @()
foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue)) {
    $cl = $p.CommandLine
    if (-not $cl) { continue }
    # MUST be an ad-hoc query shell: EncodedCommand (encoded ssh) OR -Command (plain ssh/interactive query).
    if (($cl -notmatch '-EncodedCommand') -and ($cl -notmatch '-Command')) { continue }
    # HARD SAFETY -- spare anything that is a real workload, not an ad-hoc query:
    if ($cl -match '\-File\b')  { continue }   # never a script-launched process (bridges/watchdog/tasks)
    if ($cl -match '\.ps1')     { continue }   # never a script invoked via -Command (deploy/build/health-writer)
    if ($cl -match 'gateway_watchdog|bridge|MgcLiveBars|BigCap|IbkrBridge|OMEGA\.ps1|DEPLOY|cmake|MSBuild|health_alarm|Restart-Computer') { continue }  # belt-and-braces workload names
    $proc = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
    if ($proc -and $proc.StartTime -lt $cut) {
        $age = [int]((Get-Date) - $proc.StartTime).TotalMinutes
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        $killed += "pid=$($p.ProcessId) age=${age}min"
    }
}
$ts = (Get-Date).ToString('o')
$os = Get-CimInstance Win32_OperatingSystem
$freeMB = [int]($os.FreePhysicalMemory / 1024)
"$ts reaped $($killed.Count) hung ssh-query powershell(s) [$($killed -join '; ')] | RAM free=${freeMB}MB" | Out-File -Append C:\Omega\logs\prune_hung_ssh.log
if ($killed.Count) { Write-Output "reaped $($killed.Count): $($killed -join '; ')" }
