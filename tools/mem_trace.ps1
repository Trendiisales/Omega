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

# --- LEAK ANALYSIS (S-2026-07-03) --------------------------------------------
# The CSV named the leak only if a human read it. This stage reads the last ~2h
# of samples, least-squares the free-RAM slope + each process's RSS slope, and
# writes MEM_LEAK.json so omega_health_alarm.ps1 can (a) pre-emptively RED with
# an ETA BEFORE the freeze and (b) NAME the top climber in the alert instead of
# a bare "RAM low". Read-only + one JSON write; still SAFE.
try {
    $lines = @(Get-Content $out -Tail 13)          # 13 samples ~= 2h at 10min
    if ($lines.Count -ge 5) {
        $t0 = $null
        $freePts = @(); $procPts = @{}
        foreach ($ln in $lines) {
            $parts = $ln -split ','
            $ts2 = [DateTime]::ParseExact($parts[0], "yyyy-MM-dd'T'HH:mm:ss'Z'",
                       [Globalization.CultureInfo]::InvariantCulture,
                       ([Globalization.DateTimeStyles]::AssumeUniversal -bor
                        [Globalization.DateTimeStyles]::AdjustToUniversal))
            if ($null -eq $t0) { $t0 = $ts2 }
            $hrs = ($ts2 - $t0).TotalHours
            foreach ($kv in $parts[1..($parts.Count - 1)]) {
                if ($kv -match '^(\S+)=(\d+)$') {
                    $k = $matches[1]; $v = [double]$matches[2]
                    if ($k -eq 'free_mb') { $freePts += ,@($hrs, $v) }
                    else {
                        if (-not $procPts.ContainsKey($k)) { $procPts[$k] = @() }
                        $procPts[$k] += ,@($hrs, $v)
                    }
                }
            }
        }
        function Get-SlopeMBh($pts) {
            $n = $pts.Count; if ($n -lt 4) { return $null }
            $sx = 0.0; $sy = 0.0; $sxx = 0.0; $sxy = 0.0
            foreach ($q in $pts) { $sx += $q[0]; $sy += $q[1]; $sxx += $q[0]*$q[0]; $sxy += $q[0]*$q[1] }
            $den = $n * $sxx - $sx * $sx
            if ([math]::Abs($den) -lt 1e-9) { return $null }
            return ($n * $sxy - $sx * $sy) / $den
        }
        $fslope = Get-SlopeMBh $freePts
        $climbers = @()
        foreach ($k in $procPts.Keys) {
            $s2 = Get-SlopeMBh $procPts[$k]
            # only sustained growers matter; >20MB/h over 2h is not sample noise
            if ($null -ne $s2 -and $s2 -gt 20) {
                $climbers += [pscustomobject]@{ proc = $k; slope_mb_h = [math]::Round($s2, 0) }
            }
        }
        $climbers = @($climbers | Sort-Object slope_mb_h -Descending)
        $etaH = $null
        if ($null -ne $fslope -and $fslope -lt -1 -and $free -gt 250) {
            $etaH = [math]::Round(($free - 250) / (-$fslope), 1)   # hours to the 250MB RED line
        }
        $rep = [ordered]@{
            ts              = $ts
            free_mb         = $free
            free_slope_mb_h = $(if ($null -ne $fslope) { [math]::Round($fslope, 0) } else { $null })
            eta_freeze_h    = $etaH
            window_samples  = $lines.Count
            climbers        = $climbers
        }
        ($rep | ConvertTo-Json -Depth 4) | Out-File -Encoding utf8 'C:\Omega\logs\MEM_LEAK.json'
    }
    # rotation: cap the CSV at ~30 days (4320 samples) so the leak-finder can't
    # itself become the disk problem it is watching for
    $all = @(Get-Content $out)
    if ($all.Count -gt 4500) { $all | Select-Object -Last 4320 | Set-Content $out }
} catch {}
