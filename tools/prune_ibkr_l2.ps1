# prune_ibkr_l2.ps1 — SAFE L2 disk rotation (handoff 2026-06-26 item 0a/E-a).
# FILE-prune ONLY of C:\Omega\logs\ibkr_l2 older than N days. Kills NO process.
# Explicitly NOT a process-reaper (the reaper that killed feeds is quarantined).
param([int]$Days = 2)
$dir = 'C:\Omega\logs\ibkr_l2'
$log = 'C:\Omega\logs\l2_prune.log'
$ts  = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
if (-not (Test-Path $dir)) { "$ts dir missing $dir" | Out-File -Append $log; exit 0 }
$cut = (Get-Date).AddDays(-$Days)
$old = Get-ChildItem $dir -File -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $cut }
$gb  = [math]::Round((($old | Measure-Object Length -Sum).Sum)/1GB, 2)
$n   = ($old | Measure-Object).Count
$old | Remove-Item -Force -ErrorAction SilentlyContinue
"$ts pruned $n files (${gb}GB) older than ${Days}d" | Out-File -Append $log