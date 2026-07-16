# ============================================================================
# omega_watchdog.ps1 -- SELF-HEALING restart watchdog for the Omega service.
#
# WHY (S-2026-07-16n, operator: "ensure the vps auto reconnects, we have it
# crashing and no restart, fix this"):
#   The Omega service runs under NSSM with AppExit=Restart, so NSSM restarts the
#   child Omega.exe when it *exits*. But NSSM only watches PROCESS LIVENESS -- it
#   cannot see a process that is ALIVE but HUNG/FROZEN or DISCONNECTED (broker/feed
#   thread dead while the process keeps running). The whole existing health stack
#   (omega_health.py / healthcheck.ps1 / omega_health_alarm.ps1) only OBSERVES +
#   ALARMS -- nothing ever RESTARTS. So a hang or a dropped connection was flagged
#   RED and then sat there. This watchdog closes that gap: it restarts the service
#   on sustained engine-down / engine-frozen, which reconnects the broker on boot.
#
# WHAT it restarts on (direct, independent checks -- NOT dependent on any other
# monitor's output, because a watchdog that depends on another daemon inherits that
# daemon's silent-death failure mode -- the exact 2026-07-07 class the operator is
# furious about):
#   * Omega service not Running (outside a deploy window)         -> restart
#   * service Running but Omega.exe child absent                  -> restart
#   * latest.log heartbeat stale > StaleSec (engine hung/frozen)  -> restart (sustained)
#
# What it deliberately does NOT restart on: data-feed REDs (bigcap delayed, FIX
# stale, gateway :4002 down). Restarting the engine does not fix an upstream feed
# and would mask it; those stay the alarm stack's job. This watchdog only fixes
# process death / hang, which is exactly the reported symptom.
#
# SAFETY:
#   * DEPLOY-AWARE: skips entirely if a deploy is in flight (fresh deploy_*.log or a
#     build process running) so it never fights OMEGA.ps1's own stop/build/restart.
#   * TRANSITIONAL: skips while the service is *Pending (let it settle).
#   * SUSTAINED: a soft hang needs 2 consecutive bad runs (~2 min) before acting, so
#     a transient IO blip does not trigger a restart. Hard-down (stopped / child gone)
#     acts on the first run (unambiguous, and the deploy window is already guarded).
#   * THROTTLED: at most MaxRestarts in ThrottleSec. If exceeded it does NOT restart --
#     it writes a LOUD escalation (HEALTH_RED.flag + watchdog_alerts.log) so a genuine
#     crash-loop from bad config is surfaced to the operator, not hammered. The restart
#     window is rolling, so once old restarts age out auto-healing resumes (no permanent
#     lockout).
#
# Runs every 60s via the OmegaWatchdog scheduled task (SYSTEM, --once periodic +
# AtStartup, 3-min ExecutionTimeLimit) registered by tools/install_omega_watchdog.ps1.
#
# Test WITHOUT touching the live engine:
#   powershell -File tools\omega_watchdog.ps1 -DryRun            # report only
#   powershell -File tools\omega_watchdog.ps1 -DryRun -ForceUnhealthy  # exercise the
#       full detect+decide+throttle path and log "WOULD RESTART" without restarting.
# ============================================================================
param(
    [int]    $StaleSec      = 300,     # latest.log heartbeat age that means "frozen"
    [int]    $MaxRestarts   = 3,       # max restarts allowed within ThrottleSec
    [int]    $ThrottleSec   = 1800,    # rolling throttle window (30 min)
    [int]    $DeploySkipMin = 15,      # skip if a deploy_*.log is younger than this
    [switch] $DryRun,                  # never actually restart; just log the decision
    [switch] $ForceUnhealthy           # (with -DryRun) pretend unhealthy to exercise logic
)
$ErrorActionPreference = 'Continue'

$Root      = 'C:\Omega'
$LogDir    = Join-Path $Root 'logs'
$LatestLog = Join-Path $LogDir 'latest.log'
$WdLog     = Join-Path $LogDir 'watchdog.log'
$WdAlerts  = Join-Path $LogDir 'watchdog_alerts.log'
$StateFile = Join-Path $LogDir 'watchdog_state.json'
$RedFlag   = Join-Path $Root  'HEALTH_RED.flag'
$nowU      = (Get-Date).ToUniversalTime()
$epoch     = [int64]([DateTimeOffset]$nowU).ToUnixTimeSeconds()

function Log([string]$msg) {
    $line = ('{0:yyyy-MM-ddTHH:mm:ssZ}  {1}' -f $nowU, $msg)
    try { Add-Content -Path $WdLog -Value $line -Encoding utf8 } catch {}
    Write-Output $line
}
function Alert([string]$msg) {
    Log $msg
    try { Add-Content -Path $WdAlerts -Value ('{0:yyyy-MM-ddTHH:mm:ssZ}  {1}' -f $nowU, $msg) -Encoding utf8 } catch {}
}

# ---------- load rolling state ----------
$state = [PSCustomObject]@{ consec_bad = 0; restarts = @() }
if (Test-Path $StateFile) {
    try {
        $s = Get-Content $StateFile -Raw | ConvertFrom-Json
        if ($null -ne $s.consec_bad) { $state.consec_bad = [int]$s.consec_bad }
        if ($null -ne $s.restarts)   { $state.restarts   = @($s.restarts | ForEach-Object { [int64]$_ }) }
    } catch {}
}
function Save-State {
    try { $state | ConvertTo-Json -Compress | Set-Content -Path $StateFile -Encoding utf8 } catch {}
}

# ---------- GUARD: deploy in flight -> hands off ----------
$deployFresh = Get-ChildItem (Join-Path $LogDir 'deploy_*.log') -EA SilentlyContinue |
    Where-Object { ($nowU - $_.LastWriteTime.ToUniversalTime()).TotalMinutes -lt $DeploySkipMin }
if ($deployFresh) {
    Log ('SKIP: deploy in flight (fresh {0})' -f ($deployFresh | Select-Object -First 1).Name)
    $state.consec_bad = 0; Save-State; exit 0
}
$buildProc = Get-Process cmake, MSBuild, cl, link, git -EA SilentlyContinue
if ($buildProc) { Log ('SKIP: build/deploy process running ({0})' -f (($buildProc | Select-Object -Expand Name -Unique) -join ',')); $state.consec_bad = 0; Save-State; exit 0 }

# ---------- GUARD: service transitional -> let it settle ----------
$svc = Get-Service Omega -EA SilentlyContinue
if ($null -eq $svc) { Alert 'FATAL: Omega service NOT REGISTERED -- watchdog cannot restart a missing service (manual: install nssm service).'; exit 1 }
if ($svc.Status -match 'Pending') { Log ('SKIP: service transitional (Status={0})' -f $svc.Status); Save-State; exit 0 }

# ---------- HEALTH: direct, independent checks ----------
$down = $false; $hard = $false; $reason = ''
if ($svc.Status -ne 'Running') {
    $down = $true; $hard = $true; $reason = "service Status=$($svc.Status)"
} elseif (-not (Get-Process Omega -EA SilentlyContinue)) {
    $down = $true; $hard = $true; $reason = 'service Running but Omega.exe child ABSENT'
} else {
    $lf = Get-Item $LatestLog -EA SilentlyContinue
    if ($null -eq $lf) {
        $down = $true; $hard = $false; $reason = 'latest.log MISSING'
    } else {
        $age = [int]($nowU - $lf.LastWriteTime.ToUniversalTime()).TotalSeconds
        if ($age -gt $StaleSec) { $down = $true; $hard = $false; $reason = "latest.log heartbeat stale ${age}s (>$StaleSec -- engine frozen/hung)" }
    }
}
if ($ForceUnhealthy) { $down = $true; $reason = 'FORCED (dry-run self-test): ' + $reason }

# ---------- HEALTHY: reset + exit ----------
if (-not $down) {
    if ($state.consec_bad -gt 0) { Log 'RECOVERED: healthy again, clearing consec_bad' }
    $state.consec_bad = 0; Save-State; exit 0
}

# ---------- DOWN: sustained gate ----------
$state.consec_bad = [int]$state.consec_bad + 1
# soft hang must persist 2 runs; hard-down acts immediately (deploy window already ruled out)
$need = if ($hard) { 1 } else { 2 }
if ($state.consec_bad -lt $need) {
    Log ("UNHEALTHY ($reason) consec=$($state.consec_bad)/$need -- waiting to confirm")
    Save-State; exit 0
}

# ---------- THROTTLE ----------
$recent = @($state.restarts | Where-Object { ($epoch - $_) -lt $ThrottleSec })
if ($recent.Count -ge $MaxRestarts) {
    $oldest = ($recent | Measure-Object -Minimum).Minimum
    $mins   = [int](($ThrottleSec - ($epoch - $oldest)) / 60)
    Alert ("ESCALATE: $($recent.Count) restarts in last $([int]($ThrottleSec/60))min did NOT heal ($reason). NOT restarting -- MANUAL INTERVENTION NEEDED. Auto-heal resumes in ~${mins}min.")
    try { Set-Content -Path $RedFlag -Value ("watchdog-escalate {0:o} $reason" -f $nowU) -Encoding utf8 } catch {}
    $state.restarts = $recent; Save-State; exit 2
}

# ---------- RESTART ----------
$n = $recent.Count + 1
if ($DryRun) {
    Log ("WOULD RESTART Omega (dry-run): reason=$reason  [restart #$n/$MaxRestarts in window]")
    exit 0
}
Alert ("RESTARTING Omega: reason=$reason  [restart #$n/$MaxRestarts in ${ThrottleSec}s window]")
try {
    Restart-Service Omega -Force -EA Stop
    Start-Sleep -Seconds 3
    $svc2 = Get-Service Omega -EA SilentlyContinue
    Log ("post-restart service Status=$($svc2.Status)")
} catch {
    Alert ("RESTART FAILED: $($_.Exception.Message)")
    $state.restarts = @($recent + $epoch); Save-State; exit 3
}
$state.restarts = @($recent + $epoch)
$state.consec_bad = 0
Save-State
exit 0
