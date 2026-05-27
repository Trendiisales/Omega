# =============================================================================
# register_gateway_watchdog.ps1 - (re-)create the IbkrGateway scheduled task
#
# Replaces any existing "IbkrGateway" task with a properly-triggered one that
# invokes scripts\gateway_watchdog.ps1 at logon + every 5 min thereafter.
#
# MUST be run in an ADMINISTRATOR PowerShell.
#
# The task runs as the *currently logged-on user* (NOT SYSTEM) because IB
# Gateway is a GUI app and needs an interactive desktop. Keep an RDP session
# left in "disconnected" state so the user stays logged in across reboots.
# =============================================================================
$ErrorActionPreference = 'Stop'

$TaskName = 'IbkrGateway'
$BotDir   = Split-Path -Parent $PSScriptRoot
$Script   = Join-Path $BotDir 'scripts\gateway_watchdog.ps1'
$PSExe    = (Get-Command powershell.exe).Source

if (-not (Test-Path $Script)) {
    Write-Error "watchdog script not found at $Script"
    exit 1
}

$User = "$env:USERDOMAIN\$env:USERNAME"
Write-Host "Registering '$TaskName' to run as $User"
Write-Host "Action: $PSExe -ExecutionPolicy Bypass -NoProfile -File $Script"

# Triggers: at logon (for the running user) + repeat every 5 min indefinitely.
# StartBoundary set 1 min in the past so the trigger is active immediately
# after registration. Repetition pattern fires the action every 5 min.
$logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $User

$repeat = New-ScheduledTaskTrigger -Once -At ([DateTime]::Now.AddMinutes(-1)) `
    -RepetitionInterval (New-TimeSpan -Minutes 5) `
    -RepetitionDuration (New-TimeSpan -Days 3650)

$action = New-ScheduledTaskAction -Execute $PSExe `
    -Argument "-ExecutionPolicy Bypass -NoProfile -File `"$Script`"" `
    -WorkingDirectory $BotDir

$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 5) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed existing '$TaskName' task"
}

Register-ScheduledTask -TaskName $TaskName `
    -Action $action `
    -Trigger @($logonTrigger, $repeat) `
    -Settings $settings `
    -Principal $principal `
    -Description "Keep IB Gateway running on the VPS (watchdog every 5 min)" | Out-Null

Write-Host "Registered '$TaskName' OK. Kick it off now with:"
Write-Host "    Start-ScheduledTask -TaskName '$TaskName'"
Write-Host ""
Write-Host "Then tail:  Get-Content C:\Omega\bracket-bot\logs\gateway_watchdog.log -Wait"
