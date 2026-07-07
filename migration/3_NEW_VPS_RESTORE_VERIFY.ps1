# =============================================================================
# 3_NEW_VPS_RESTORE_VERIFY.ps1 -- run ON THE NEW VPS (45.85.3.79) as Admin,
# AFTER 2_NEW_VPS_BOOTSTRAP.ps1 and after the export zip has been copied here.
# =============================================================================
# Restores C:\Omega + scheduled tasks + NSSM services + ssh keys + IBC/Jts,
# starts the Omega service from the carried-over binary, then runs the
# verification block (Deploy Hygiene three-way hash check, listeners, tasks).
#
# Usage:  powershell -File 3_NEW_VPS_RESTORE_VERIFY.ps1 [-Zip C:\omega_migration.zip]
# =============================================================================
param([string]$Zip = 'C:\omega_migration.zip')
$ErrorActionPreference = 'Continue'
$Stage = 'C:\omega_migration_unpacked'

if (-not (Test-Path $Zip)) { Write-Host "FATAL: $Zip not found" -ForegroundColor Red; exit 1 }
Write-Host "=== [1/7] Unpacking $Zip ===" -ForegroundColor Cyan
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
Expand-Archive -Path $Zip -DestinationPath $Stage
Get-Content (Join-Path $Stage 'MANIFEST.txt') | ForEach-Object { Write-Host "  $_" }

Write-Host "=== [2/7] Restoring C:\Omega ===" -ForegroundColor Cyan
robocopy (Join-Path $Stage 'Omega') 'C:\Omega' /E /R:1 /W:1 /NFL /NDL /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Host "ROBOCOPY FAILED ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
Write-Host "  restored ($((Get-ChildItem C:\Omega -Recurse -File | Measure-Object).Count) files)"

Write-Host "=== [3/7] ssh authorized keys ===" -ForegroundColor Cyan
$sshSrc = Join-Path $Stage '_system\ssh'
if (Test-Path "$sshSrc\administrators_authorized_keys") {
    Copy-Item "$sshSrc\administrators_authorized_keys" 'C:\ProgramData\ssh\' -Force
    icacls 'C:\ProgramData\ssh\administrators_authorized_keys' /inheritance:r `
        /grant 'Administrators:F' /grant 'SYSTEM:F' | Out-Null
    Write-Host "  administrators_authorized_keys restored + ACL locked"
}
if (Test-Path "$sshSrc\authorized_keys") {
    New-Item -ItemType Directory -Path "$env:USERPROFILE\.ssh" -Force | Out-Null
    Copy-Item "$sshSrc\authorized_keys" "$env:USERPROFILE\.ssh\" -Force
    Write-Host "  user authorized_keys restored"
}
Restart-Service sshd -ErrorAction SilentlyContinue

Write-Host "=== [4/7] IBC + Jts ===" -ForegroundColor Cyan
if (Test-Path (Join-Path $Stage '_system\IBC')) {
    robocopy (Join-Path $Stage '_system\IBC') 'C:\IBC' /E /R:1 /W:1 /NFL /NDL /NP | Out-Null
    Write-Host "  C:\IBC restored (Gateway itself: install per docs\IBC_GATEWAY_AUTOSTART.md, then C:\Jts settings below apply)"
}
if (Test-Path (Join-Path $Stage '_system\Jts')) {
    robocopy (Join-Path $Stage '_system\Jts') 'C:\Jts' /E /R:1 /W:1 /NFL /NDL /NP | Out-Null
    Write-Host "  C:\Jts settings restored"
}

Write-Host "=== [5/7] NSSM services ===" -ForegroundColor Cyan
$svcSrc = Join-Path $Stage '_system\services'
$nssm = Join-Path $svcSrc 'nssm.exe'
if (Test-Path $nssm) {
    New-Item -ItemType Directory -Path 'C:\nssm' -Force | Out-Null
    Copy-Item $nssm 'C:\nssm\nssm.exe' -Force
    $nssm = 'C:\nssm\nssm.exe'
    if (-not (Get-Service Omega -ErrorAction SilentlyContinue)) {
        & $nssm install Omega 'C:\Omega\Omega.exe'
        & $nssm set Omega AppDirectory 'C:\Omega'
        & $nssm set Omega AppStdout 'C:\Omega\logs\omega_service_stdout.log'
        & $nssm set Omega AppStderr 'C:\Omega\logs\omega_service_stderr.log'
        & $nssm set Omega AppExit Default Restart
        & $nssm set Omega Start SERVICE_AUTO_START
        Write-Host "  service 'Omega' installed (compare against $svcSrc\Omega.nssm.txt for custom flags)"
    } else { Write-Host "  service 'Omega' already exists -- left untouched" }
    Write-Host "  IBGateway service: re-create manually per docs\IBC_GATEWAY_AUTOSTART.md once Gateway is installed"
} else { Write-Host "  WARNING: no nssm.exe in payload -- create services by hand" -ForegroundColor Yellow }

Write-Host "=== [6/7] Scheduled tasks ===" -ForegroundColor Cyan
Get-ChildItem (Join-Path $Stage '_system\tasks') -Filter *.xml -ErrorAction SilentlyContinue | ForEach-Object {
    $name = $_.BaseName
    try {
        Register-ScheduledTask -TaskName $name -Xml (Get-Content $_.FullName -Raw) -Force | Out-Null
        Write-Host "  registered: $name"
    } catch { Write-Host "  FAILED: $name -- $($_.Exception.Message)" -ForegroundColor Yellow }
}

Write-Host "=== [7/7] Start + VERIFY ===" -ForegroundColor Cyan
Start-Service Omega -ErrorAction SilentlyContinue
Start-Sleep 8

$pass = $true
function Check($label, $ok, $detail) {
    if ($ok) { Write-Host ("  PASS  {0}  {1}" -f $label, $detail) -ForegroundColor Green }
    else     { Write-Host ("  FAIL  {0}  {1}" -f $label, $detail) -ForegroundColor Red; $script:pass = $false }
}

# -- binary integrity vs manifest
$man = Get-Content (Join-Path $Stage 'MANIFEST.txt') -Raw
$wantExe = if ($man -match 'omega_exe\s*=\s*(\S+)') { $Matches[1] } else { '' }
$haveExe = (Get-FileHash C:\Omega\Omega.exe -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
Check 'Omega.exe SHA256 == manifest' ($wantExe -and $haveExe -eq $wantExe) $haveExe

# -- Deploy Hygiene three-way hash check
Push-Location C:\Omega
git fetch origin main 2>$null | Out-Null
$head = git rev-parse HEAD 2>$null
$main = git rev-parse origin/main 2>$null
Check 'git HEAD == origin/main' ($head -and $head -eq $main) "HEAD=$head"
$stderrHash = Select-String -Path 'C:\Omega\logs\omega_service_stderr.log' -Pattern 'Git hash' -ErrorAction SilentlyContinue |
              Select-Object -Last 1
Check 'running binary logs Git hash' ($null -ne $stderrHash) "$stderrHash"
Pop-Location

# -- service + listeners
Check "service 'Omega' RUNNING" ((Get-Service Omega -ErrorAction SilentlyContinue).Status -eq 'Running') ''
foreach ($p in 2222, 7779, 7781) {
    Check "port $p listening" ([bool](Get-NetTCPConnection -State Listen -LocalPort $p -ErrorAction SilentlyContinue)) ''
}
Check 'port 4002 (IB Gateway) listening' ([bool](Get-NetTCPConnection -State Listen -LocalPort 4002 -ErrorAction SilentlyContinue)) '(expected FAIL until Gateway installed + logged in)'

# -- warm-seed lines (engine warm-seed mandate: one [SEED] per seeded engine)
$seed = Select-String -Path 'C:\Omega\logs\omega_service_stderr.log' -Pattern '\[SEED\]' -ErrorAction SilentlyContinue
Check '[SEED] lines present' ($seed.Count -gt 0) "$($seed.Count) seed lines"

# -- box resources (the reason for this migration)
$os  = Get-CimInstance Win32_OperatingSystem
$cpu = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
$ram = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
Write-Host ("  INFO  box: {0} logical CPUs, {1} GB RAM ({2} MB free)" -f $cpu, $ram, [int]($os.FreePhysicalMemory/1024))
Check 'RAM > 4 GB (upgrade took effect)' ($ram -gt 4.5) "$ram GB"

Write-Host ""
if ($pass) { Write-Host "RESTORE VERIFIED -- see runbook for remaining manual steps (Gateway login, broker IP whitelist, Mac known_hosts)." -ForegroundColor Green }
else       { Write-Host "SOME CHECKS FAILED -- fix before decommissioning the old box." -ForegroundColor Red }
