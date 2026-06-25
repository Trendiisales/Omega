# =============================================================================
# bigcap_preflight.ps1 - G4 stale-data / health gate for the BigCap feed path.
#
# Hard-verifies the big-cap feed is SINGLE, LIVE, FRESH and CONSUMED before (and
# during) trading. Run it at session start and on the health loop (every 1-2 min).
# Exit 0 = GO (healthy). Exit 1 = NO-GO (a problem the operator must see).
#
# Closes the 2026-06-25 gaps: a 2nd bridge fighting for ports, a frozen overnight
# feed reported as "tradeable", a silent IBKR mkt-data downgrade, and Omega not
# consuming :7784. Pairs with the in-bridge guards (G1 lock, G2 freshness/RTH,
# health flag) and the in-engine guards (G3 consumer age-gate, G5 bar-age gate).
#
# Run:  powershell -NoProfile -File C:\Omega\pump\bigcap_preflight.ps1
# =============================================================================
$ErrorActionPreference = 'SilentlyContinue'
$log   = 'C:\Omega\logs\bigcap_bridge.log'
$flag  = 'C:\Omega\logs\bigcap_feed_unhealthy.flag'
$fails = @()
$warns = @()

# ---- is it RTH now? (13:30-20:00 UTC, Mon-Fri) -- only enforce live-ness in RTH ----
$utc   = (Get-Date).ToUniversalTime()
$mins  = $utc.Hour*60 + $utc.Minute
$isRTH = ($utc.DayOfWeek -ne 'Saturday') -and ($utc.DayOfWeek -ne 'Sunday') -and ($mins -ge 810) -and ($mins -lt 1200)

# ---- 1. EXACTLY ONE bridge child bound to :7783 AND :7784 (no double-launch) ----
$own = @{}
foreach($p in 7783,7784,7785){
    $c = Get-NetTCPConnection -State Listen -LocalPort $p -ErrorAction SilentlyContinue
    if($c){ $own[$p] = ($c.OwningProcess | Select-Object -Unique) }
}
if(-not $own[7784]){ $fails += "no listener on :7784 (bridge serve port down)" }
if(-not $own[7783]){ $warns += "no listener on :7783 (scanner page down)" }
if(-not $own[7785]){ $warns += "no listener on :7785 (G1 singleton lock not held -- old bridge build?)" }
$pidset = @($own[7783]; $own[7784]; $own[7785]) | Where-Object { $_ } | Select-Object -Unique
if($pidset.Count -gt 1){ $fails += "ports 7783/7784/7785 owned by MULTIPLE pids ($($pidset -join ',')) = double bridge" }

# ---- 2. health flag from the bridge (G2: set when RTH + all names stale) ----
if(Test-Path $flag){ $fails += "bridge UNHEALTHY flag present: $((Get-Content $flag -Tail 1))" }

# ---- 3. last heartbeat: fresh, consumed, live mdtype, not all-stale ----
$hb = (Select-String -Path $log -Pattern '\[HB ' -ErrorAction SilentlyContinue | Select-Object -Last 1).Line
if(-not $hb){ $fails += "no [HB] heartbeat in bridge log" }
else {
    if($hb -match 'consumer=N'   -and $isRTH){ $fails += "Omega NOT consuming :7784 during RTH (consumer=N)" }
    if($hb -match 'mdtype=(\d)'  -and $Matches[1] -ne '1'){ $fails += "IBKR mdtype=$($Matches[1]) (not live=1) -- delayed/frozen data" }
    if($isRTH -and ($hb -match 'subs=(\d+)') ){
        $subs = [int]$Matches[1]
        if($hb -match 'stale=(\d+)'){ $stale=[int]$Matches[1]; if($subs -gt 0 -and $stale -eq $subs){ $fails += "RTH but ALL $subs names stale (feed frozen)" } }
    }
    Write-Output "HB: $hb"
}

# ---- 4. in-process engine: last_data fresh during RTH ----
$alive = (Select-String -Path 'C:\Omega\logs\omega_service_stdout.log' -Pattern '\[BigCapMomo\] ALIVE' -ErrorAction SilentlyContinue | Select-Object -Last 1).Line
if($alive -and $isRTH -and ($alive -match 'last_data=(\d+)s')){
    $ld=[int]$Matches[1]; if($ld -gt 120){ $fails += "in-process engine last_data=${ld}s stale during RTH" }
}

# ---- verdict ----
$rthTag = if($isRTH){'RTH'}else{'non-RTH'}
if($fails.Count -gt 0){
    Write-Output "=== BIGCAP PREFLIGHT: NO-GO ($rthTag) ==="
    $fails | ForEach-Object { Write-Output "  FAIL: $_" }
    $warns | ForEach-Object { Write-Output "  warn: $_" }
    try { [console]::beep(880,400) } catch {}
    exit 1
} else {
    Write-Output "=== BIGCAP PREFLIGHT: GO ($rthTag) ==="
    $warns | ForEach-Object { Write-Output "  warn: $_" }
    exit 0
}
