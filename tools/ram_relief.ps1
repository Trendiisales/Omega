# ram_relief.ps1 -- AUTOMATIC RAM-relief for the 3GB omega-vps (no manual reset).
# Runs every 10 min (OmegaRamRelief task). If free RAM stays low for 30 min it
# restarts IB Gateway -- the JVM is the chief slow-creep consumer -- which CLEARS
# the heap WITHOUT a Windows reboot. Gateway auto-relogins via saved credentials
# (the same IbkrGateway task->gateway_watchdog.ps1 path proven on every boot).
#
# Safety:
#  * THRESHOLD 280MB, 3 consecutive low samples (=30min sustained) -> not twitchy.
#  * COOLDOWN 2h between restarts. If RAM is STILL low <2h after a relief restart,
#    the cause is NOT the JVM (standby/other) -> it does NOT thrash-restart; it
#    raises HEALTH_RED.flag so the operator alarm escalates instead.
#  * SELF-CLEAR (2026-07-01): once RAM genuinely recovers it DELETES a RED flag it
#    raised, so a stale flag can't latch the health alarm RED for hours after the
#    box recovered. Only clears a flag WE raised (content marked RAM-RELIEF).
#  * LIVE-PATH GUARD (2026-07-02): the restart is auto-SUPPRESSED whenever a loopback
#    API client is Established to :4001 (live). On PAPER (:4002) a gateway blip = brief
#    shadow-data gap only, so the restart still runs there. On live it escalates instead
#    of restarting -- so it can never drop a live order.
$ErrorActionPreference = 'SilentlyContinue'
$log   = 'C:\Omega\logs\ram_relief.log'
$state = 'C:\Omega\logs\ram_relief_state.txt'
$flagP = 'C:\Omega\HEALTH_RED.flag'
$THRESH_MB   = 280
$CLEAR_MB    = 320     # require comfortable recovery (not edge-hover) before self-clear
$NEED_LOW    = 3       # consecutive low samples before acting (10min each = 30min)
$COOLDOWN_S  = 7200    # 2h between relief restarts

$ts   = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$free = [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)

# load state: "consecutiveLow,lastReliefEpoch"
$consec = 0; $lastRelief = 0
if (Test-Path $state) {
    $p = (Get-Content $state -Raw).Trim() -split ','
    if ($p.Count -ge 2) { $consec = [int]$p[0]; $lastRelief = [long]$p[1] }
}
$nowEpoch = [long][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

if ($free -lt $THRESH_MB) { $consec++ } else { $consec = 0 }

$action = 'ok'
# LIVE-PATH guard: a gateway restart is safe on PAPER (:4002) but DROPS LIVE ORDERS on
# the real path (:4001). Treat as LIVE if any LOOPBACK API client is Established to
# RemotePort 4001 (the gateway's own public uplink to IB infra is NOT loopback -> excluded).
# Fail-closed: when live, SUPPRESS the restart and escalate to the operator instead.
$liveClients = @(Get-NetTCPConnection -State Established -ErrorAction SilentlyContinue |
    Where-Object { $_.RemotePort -eq 4001 -and ($_.LocalAddress -eq '127.0.0.1' -or $_.LocalAddress -eq '::1') })
$isLive = $liveClients.Count -gt 0

if ($consec -ge $NEED_LOW) {
    if ($isLive) {
        # NEVER restart the gateway while trading live -- it would drop open live orders.
        $msg = "RAM-RELIEF: free=${free}MB sustained-low but LIVE :4001 client connected (n=$($liveClients.Count)) -> gateway restart SUPPRESSED (would drop live orders). Use standby-purge / market-closed reboot / RAM bump instead."
        Add-Content $log "$ts $msg"
        Set-Content $flagP "$ts OVERALL=RED`n  [RED] $msg"
        $action = 'live-suppressed'
    }
    elseif (($nowEpoch - $lastRelief) -ge $COOLDOWN_S) {
        Add-Content $log "$ts RAM-RELIEF free=${free}MB consec=$consec -> restarting ibgateway (clear JVM)"
        Stop-Process -Name ibgateway -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
        Start-ScheduledTask -TaskName IbkrGateway   # relaunch + auto-login (saved creds)
        $lastRelief = $nowEpoch; $consec = 0; $action = 'restarted-gateway'
    } else {
        # still low shortly after a restart => NOT the JVM => escalate, do NOT loop
        $mins = [int](($nowEpoch - $lastRelief)/60)
        $msg  = "RAM-RELIEF: free=${free}MB STILL low ${mins}min after a gateway restart -- cause is NOT the JVM (standby/other). Manual investigation needed."
        Add-Content $log "$ts $msg"
        Set-Content $flagP "$ts OVERALL=RED`n  [RED] $msg"
        $action = 'escalated'
    }
}

# SELF-CLEAR: RAM has genuinely recovered ($free >= CLEAR_MB) and a RED flag WE
# raised is still present -> delete it so the health alarm doesn't latch RED on a
# stale flag. Scoped to our own flag (content marked RAM-RELIEF); never touches a
# RED raised by another component. Cannot run in the same tick as an escalation
# (that path only fires when $free < THRESH_MB, well below CLEAR_MB).
if ($free -ge $CLEAR_MB -and (Test-Path $flagP)) {
    $flagTxt = (Get-Content $flagP -Raw)
    if ($flagTxt -match 'RAM-RELIEF') {
        Remove-Item $flagP -Force -ErrorAction SilentlyContinue
        Add-Content $log "$ts SELF-CLEAR free=${free}MB >= ${CLEAR_MB}MB -> deleted stale RAM-RELIEF HEALTH_RED.flag"
        $action = 'self-cleared'
    }
}

Set-Content $state "$consec,$lastRelief"
Add-Content $log "$ts free=${free}MB consec=$consec action=$action"
