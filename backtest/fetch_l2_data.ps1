# fetch_l2_data.ps1 -- Copy L2 tick logs from VPS to local machine
# Run from Mac: ssh trader@185.167.119.59 then run, or use scp directly
# 
# From Mac terminal:
#   scp -r trader@185.167.119.59:"C:/Omega/logs/l2_ticks_*.csv" ~/Tick/l2_data/

$LOG_DIR = "C:\Omega\logs"
$FILES = Get-ChildItem "$LOG_DIR\l2_ticks_*.csv" | Sort-Object Name

Write-Host "L2 tick files available:"
foreach ($f in $FILES) {
    $lines = (Get-Content $f | Measure-Object -Line).Lines
    Write-Host "  $($f.Name)  $lines rows  $([math]::Round($f.Length/1MB, 1)) MB"
}
Write-Host ""
Write-Host "Total: $($FILES.Count) files"
Write-Host ""
Write-Host "To copy to Mac:"
Write-Host '  scp trader@185.167.119.59:"C:/Omega/logs/l2_ticks_*.csv" ~/Tick/l2_data/'
