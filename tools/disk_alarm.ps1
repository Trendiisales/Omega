# disk_alarm.ps1 — SAFE read-only health alarm (handoff item 0c/E-c).
# Writes a flag file when C: free < threshold. NEVER kills a process or task.
param([double]$MinFreePct = 8.0)
$d   = Get-PSDrive C
$pct = [math]::Round($d.Free/($d.Used+$d.Free)*100, 1)
$flag = 'C:\Omega\logs\DISK_ALARM.flag'
$log  = 'C:\Omega\logs\disk_alarm.log'
$ts   = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$nps  = (Get-Process powershell -ErrorAction SilentlyContinue | Measure-Object).Count
"$ts C-free=${pct}% powershell_procs=$nps" | Out-File -Append $log
if ($pct -lt $MinFreePct) {
    "$ts ALARM C: only ${pct}% free (<${MinFreePct}%); powershell_procs=$nps" | Out-File $flag
} elseif (Test-Path $flag) {
    Remove-Item $flag -Force -ErrorAction SilentlyContinue
}