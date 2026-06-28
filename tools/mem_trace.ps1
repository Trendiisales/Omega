# =============================================================================
# mem_trace.ps1 — lightweight per-process RAM snapshot (the LEAK finder).
#
# WHY: VPS frees ~770-930MB after reboot, climbs to <40MB and FREEZES in 1-2h.
# That climb is ON TOP of baseline (Omega+Gateway+Windows already counted in the
# post-reboot free figure) => something LEAKS ~400-800MB/h. More RAM only delays
# the same freeze. This task names the climber: read the CSV after 1-2h and the
# row whose MB column grows monotonically IS the leak.
#
# SAFE: read-only Get-Process + one append. No kills, no feed touch. Tiny.
# Appends top-8 working-set processes + total free MB, timestamped (UTC).
#
# Registered every 10min by register_mem_trace.ps1. Output:
#   C:\Omega\logs\mem_trace.csv   (ts, total_free_mb, then name=MB pairs)
# =============================================================================
$ErrorActionPreference = 'SilentlyContinue'
$out = 'C:\Omega\logs\mem_trace.csv'

$ts   = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$os   = Get-CimInstance Win32_OperatingSystem
$free = [int]($os.FreePhysicalMemory / 1KB)   # FreePhysicalMemory is in KB

$top = Get-Process |
       Sort-Object WS -Descending |
       Select-Object -First 8 |
       ForEach-Object { "{0}={1}" -f $_.ProcessName, [int]($_.WS / 1MB) }

$line = ($ts, "free_mb=$free") + $top -join ','
Add-Content -Path $out -Value $line
