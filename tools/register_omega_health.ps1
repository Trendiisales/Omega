# register_omega_health.ps1 -- (re)register OmegaHealthMonitor as a NON-RESIDENT
# --once task on a 60s periodic trigger (proven non-leaking pattern; mirrors
# register_aurora_snapshot.ps1 + Healthcheck + Gex).
#
# WHY THIS EXISTS (2026-07-15 recurring HEALTH RED root cause):
#   The task was created ad-hoc on the box as a RESIDENT `pythonw omega_health.py`
#   (infinite `while True` loop) with ExecutionTimeLimit = PT15M. Two failures
#   compounded into a phantom RED that beeped the operator for hours:
#     1. Task Scheduler killed the infinite loop 15 min after start (result
#        267014 = SCHED_S_TASK_TERMINATED). It happened to die ~2 min after
#        writing HEALTH_RED.flag on a transient nightly gateway blip, and no
#        daemon was left to clear the flag when the gateway recovered.
#     2. omega_health.py chk_gateway keyed on a process named "ibgateway.exe",
#        but this migrated ForexVPS box runs IB Gateway as java.exe (install4j
#        i4j_jres JRE) -> chk_gateway was RED on EVERY run regardless of the API
#        being live, manufacturing the flag in the first place.
#   Fix (2) shipped in omega_health.py (listener-based chk_gateway). Fix (1) is
#   this script: a --once run cannot be killed mid-flight the way a resident loop
#   is, cannot orphan the flag, and matches the non-leaking periodic pattern the
#   Aurora leak taught us. omega_health.py --once: collect() -> write_and_alert()
#   (writes health.json, heartbeats/clears HEALTH_RED.flag) -> exit.
#
# The standalone :7790 health page is dropped (only the resident loop served it);
# nothing consumes it -- the GUI reads /api/health (status.json via the C++ server)
# and the alarm reads HEALTH_STATUS.json. The load-bearing job (flag lifecycle +
# health.json) runs every 60s here.
#
# Register (interactive or over ssh):
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\register_omega_health.ps1

$TaskName = 'OmegaHealthMonitor'
$Py       = 'C:\Program Files\Python312\pythonw.exe'   # console-less; omega_health.py is stdlib-only
$Script   = 'C:\Omega\tools\omega_health.py'

if (-not (Test-Path $Py))     { Write-Error "pythonw not at $Py";     exit 1 }
if (-not (Test-Path $Script)) { Write-Error "omega_health.py not at $Script"; exit 1 }

# COMPUTERNAME (not USERDOMAIN): local WORKGROUP account, ssh-safe SID mapping.
$User = "$env:COMPUTERNAME\$env:USERNAME"
Write-Host "Registering '$TaskName' as $User"
Write-Host "Python  : $Py"
Write-Host "Script  : $Script  (--once, periodic 60s)"

$action = New-ScheduledTaskAction -Execute $Py `
                                  -Argument "`"$Script`" --once" `
                                  -WorkingDirectory 'C:\Omega'

# Periodic: one trigger repeating every 60s, effectively forever. No resident loop
# to orphan, no AtStartup loop, nothing for a 15min limit to kill mid-flight.
$repeat = New-ScheduledTaskTrigger -Once -At ([DateTime]::Now.AddMinutes(-1)) `
    -RepetitionInterval (New-TimeSpan -Minutes 1) `
    -RepetitionDuration (New-TimeSpan -Days 3650)

# ExecutionTimeLimit 4 min = reaper for a wedged --once pass (a normal pass takes
# a few seconds). This is SAFE here (unlike the old PT15M) because the process is
# short-lived by design -- the limit reaps a hang, it does not kill a healthy loop.
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 4) `
    -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

$principal = New-ScheduledTaskPrincipal -UserId $User `
                                        -LogonType S4U `
                                        -RunLevel Highest

if (Get-ScheduledTask -TaskName $TaskName -EA SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed existing '$TaskName'"
}

Register-ScheduledTask -TaskName $TaskName `
    -Action $action `
    -Trigger $repeat `
    -Settings $settings `
    -Principal $principal `
    -Description ("Omega health monitor -- runs omega_health.py --once every 60s: " +
                  "writes logs\health.json + manages HEALTH_RED.flag (heartbeat while " +
                  "RED, unlink when clear). NON-RESIDENT (--once periodic) so it cannot " +
                  "orphan the flag or be killed mid-loop -- 2026-07-15 phantom-RED fix. " +
                  "Do NOT re-register as a resident loop with an ExecutionTimeLimit.") | Out-Null

Write-Host "Registered '$TaskName' OK."
Start-ScheduledTask -TaskName $TaskName
Write-Host "Started '$TaskName'."
