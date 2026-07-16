# ============================================================================
# install_omega_watchdog.ps1 -- idempotent installer for the Omega auto-restart
# resilience layer (S-2026-07-16n). Run ONCE on omega-new (LIVE box) as admin/SYSTEM.
# Re-running is safe (Register-ScheduledTask -Force + declarative sc/nssm settings).
#
# Installs THREE independent safety nets so "crashing / hung / disconnected with no
# restart" cannot recur:
#   1. OmegaWatchdog scheduled task -- runs omega_watchdog.ps1 every 60s (+ AtStartup).
#      Restarts the service on sustained process-death / hang. (The layer NSSM can't do.)
#   2. Native SC failure recovery on the Omega service -- Windows SCM restarts the
#      service if nssm.exe itself dies / the service enters STOPPED. (Was EMPTY.)
#   3. NSSM AppRestartDelay -- small delay so a fast child crash-loop is not hammered.
#
# Per operator rule feedback-crontab-edit-via-script: task/schedule mutations go
# through THIS committed idempotent script, never ad-hoc inline schtasks pastes.
# ============================================================================
$ErrorActionPreference = 'Stop'
$Root   = 'C:\Omega'
$Wd     = Join-Path $Root 'tools\omega_watchdog.ps1'
$nssm   = 'C:\nssm\nssm.exe'

if (-not (Test-Path $Wd)) { throw "watchdog script missing at $Wd -- deploy it first (git pull on VPS)" }

Write-Output '=== 1/3  OmegaWatchdog scheduled task ==='
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$Wd`""
# Once-now + 60s repetition (indefinite) mirrors the resilient OmegaHealthMonitor pattern;
# plus an AtStartup trigger so the watchdog is armed the instant the box boots.
$tNow   = New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 1)
$tBoot  = New-ScheduledTaskTrigger -AtStartup
$princ  = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
$set    = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 3) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'OmegaWatchdog' -Action $action -Trigger $tNow, $tBoot `
    -Principal $princ -Settings $set -Description 'Self-healing restart watchdog for the Omega service (process death / hang / disconnect). S-2026-07-16n.' -Force | Out-Null
Write-Output '  registered OmegaWatchdog (SYSTEM, 60s + AtStartup, 3min limit)'

Write-Output '=== 2/3  native SC failure recovery on Omega service ==='
# If nssm.exe itself dies or the service enters STOPPED, SCM auto-restarts it.
# reset window 24h; escalating delays 5s / 15s / 60s.
& sc.exe failure Omega reset= 86400 actions= restart/5000/restart/15000/restart/60000 | Out-Null
& sc.exe failureflag Omega 1 | Out-Null   # apply failure actions on any stop, not only crashes
Write-Output '  sc failure actions set (restart 5s/15s/60s, reset 24h, failureflag=1)'

Write-Output '=== 3/3  NSSM harden ==='
if (Test-Path $nssm) {
    & $nssm set Omega AppExit Default Restart | Out-Null
    & $nssm set Omega AppRestartDelay 2000     | Out-Null   # 2s between child restarts
    Write-Output '  nssm AppExit=Restart, AppRestartDelay=2000ms'
} else {
    Write-Output "  [warn] $nssm not found -- skipped nssm harden (service may not be nssm-managed)"
}

Write-Output ''
Write-Output '=== VERIFY ==='
Get-ScheduledTask -TaskName 'OmegaWatchdog' | Select-Object TaskName, State | Format-Table -Auto
& sc.exe qfailure Omega
Write-Output '--- watchdog dry-run self-test (exercises detect+decide, no restart) ---'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Wd -DryRun -ForceUnhealthy
Write-Output 'DONE. Watchdog live. tail: Get-Content C:\Omega\logs\watchdog.log -Tail 20 -Wait'
