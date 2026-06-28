# prune_hung_ssh_queries.ps1 — SAFE guard against RAM exhaustion from leaked ssh queries.
# (operator-mandated 2026-06-27: "build a check that does not allow you to keep messing up my memory")
#
# THE PROBLEM: every `ssh omega-vps powershell -EncodedCommand <b64>` an AI session runs spawns a
# powershell on the VPS. If that command hangs (slow CIM on a paging box) the local ssh dies but the
# remote powershell lingers -- they pile up and starve the 3GB box's RAM until it freezes.
#
# THIS IS NOT THE POISON REAPER. The reaper that killed the feeds (reverted+deleted 2026-06-26) killed
# powershell BY AGE -- which hit the feed BRIDGES (long-lived `powershell -File`). This guard is the
# precise opposite: it kills ONLY `-EncodedCommand` powershells (interactive/ssh queries — never the
# bridges, which use `-File`) AND only when they've been running > MAX_MIN (truly hung, not an active
# query). It NEVER touches `-File` powershells, NEVER touches python.exe (the feeds). Hard-guaranteed.
$ErrorActionPreference = 'SilentlyContinue'
$MAX_MIN = 5
$cut = (Get-Date).AddMinutes(-$MAX_MIN)
$killed = @()
foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue)) {
    $cl = $p.CommandLine
    if (-not $cl) { continue }
    # HARD SAFETY: must be an EncodedCommand (ssh/interactive) AND must NOT be a -File launch (bridges/watchdog/tasks)
    if ($cl -notmatch '-EncodedCommand') { continue }
    if ($cl -match '\-File\b')           { continue }   # never a script-launched process
    if ($cl -match 'gateway_watchdog|bridge|MgcLiveBars|BigCap|IbkrBridge') { continue }  # belt-and-braces
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
