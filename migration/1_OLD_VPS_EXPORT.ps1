# =============================================================================
# 1_OLD_VPS_EXPORT.ps1 -- run ON THE OLD VPS (185.167.119.59) as trader
# =============================================================================
# Packages everything the new box (45.85.3.79) needs into one zip:
#   * C:\Omega working tree (incl. .git, Omega.exe, logs\trades shadow ledger,
#     data\, omega_config.ini as-deployed) -- build junk excluded
#   * Scheduled-task XML exports (Omega*, Aurora*, IBGateway*, Healthcheck)
#   * NSSM service dumps (Omega, IBGateway) + nssm.exe itself
#   * sshd_config + authorized_keys (so Mac key-auth carries over)
#   * C:\IBC (config.ini contains IB creds -- zip stays on your boxes only)
#     + C:\Jts settings (jts.ini, *.xml -- NOT logs)
#   * MANIFEST.txt with git hash + Omega.exe SHA256 for post-restore verify
#
# Usage (PowerShell as Administrator):
#   cd C:\Omega\migration ; .\1_OLD_VPS_EXPORT.ps1
# Output: C:\Omega_migration\omega_migration_<yyyyMMdd_HHmm>.zip
# =============================================================================
$ErrorActionPreference = 'Continue'
$Stamp   = Get-Date -Format 'yyyyMMdd_HHmm'
$Root    = 'C:\Omega_migration'
$Stage   = Join-Path $Root "stage_$Stamp"
$Zip     = Join-Path $Root "omega_migration_$Stamp.zip"
New-Item -ItemType Directory -Path $Stage -Force | Out-Null

Write-Host "=== [1/6] C:\Omega working tree (robocopy, excluding build junk) ===" -ForegroundColor Cyan
# /XD build .vs CMakeFiles: rebuildable; /XF *.obj *.pdb *.ilk: compiler junk.
# Omega.exe IS kept so the new box can run before the toolchain is installed.
robocopy 'C:\Omega' (Join-Path $Stage 'Omega') /E /R:1 /W:1 /NFL /NDL /NP `
    /XD 'C:\Omega\build' 'C:\Omega\.vs' /XF '*.obj' '*.pdb' '*.ilk' | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Host "ROBOCOPY FAILED ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }

Write-Host "=== [2/6] Scheduled tasks -> XML ===" -ForegroundColor Cyan
$taskDir = Join-Path $Stage '_system\tasks'
New-Item -ItemType Directory -Path $taskDir -Force | Out-Null
Get-ScheduledTask | Where-Object { $_.TaskName -match 'Omega|Aurora|IBGateway|Healthcheck|Gateway' } | ForEach-Object {
    $xml = Export-ScheduledTask -TaskName $_.TaskName -TaskPath $_.TaskPath
    $xml | Out-File (Join-Path $taskDir "$($_.TaskName).xml") -Encoding unicode
    Write-Host "  exported task: $($_.TaskName)"
}

Write-Host "=== [3/6] NSSM services ===" -ForegroundColor Cyan
$svcDir = Join-Path $Stage '_system\services'
New-Item -ItemType Directory -Path $svcDir -Force | Out-Null
$nssm = (Get-Command nssm -ErrorAction SilentlyContinue).Source
if (-not $nssm) { $nssm = (Get-ChildItem 'C:\' -Recurse -Filter nssm.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
if ($nssm) {
    Copy-Item $nssm (Join-Path $svcDir 'nssm.exe')
    foreach ($svc in 'Omega','IBGateway') {
        & $nssm dump $svc 2>$null | Out-File (Join-Path $svcDir "$svc.nssm.txt") -Encoding utf8
        sc.exe qc $svc      2>$null | Out-File (Join-Path $svcDir "$svc.scqc.txt") -Encoding utf8
    }
    Write-Host "  nssm found at $nssm; dumps written"
} else { Write-Host "  WARNING: nssm.exe not found -- service must be re-created by hand" -ForegroundColor Yellow }

Write-Host "=== [4/6] sshd config + authorized keys ===" -ForegroundColor Cyan
$sshDir = Join-Path $Stage '_system\ssh'
New-Item -ItemType Directory -Path $sshDir -Force | Out-Null
foreach ($f in 'C:\ProgramData\ssh\sshd_config',
               'C:\ProgramData\ssh\administrators_authorized_keys',
               "$env:USERPROFILE\.ssh\authorized_keys") {
    if (Test-Path $f) { Copy-Item $f $sshDir; Write-Host "  copied $f" }
}
# Host keys are NOT copied: the new box generates its own; the Mac accepts the
# new fingerprint on first connect.

Write-Host "=== [5/6] IBC + Gateway settings ===" -ForegroundColor Cyan
if (Test-Path 'C:\IBC') {
    robocopy 'C:\IBC' (Join-Path $Stage '_system\IBC') /E /R:1 /W:1 /NFL /NDL /NP /XD 'C:\IBC\logs' | Out-Null
    Write-Host "  C:\IBC copied (config.ini contains IB credentials -- keep this zip OFF third-party hosts)"
}
if (Test-Path 'C:\Jts') {
    robocopy 'C:\Jts' (Join-Path $Stage '_system\Jts') /E /R:1 /W:1 /NFL /NDL /NP `
        /XD 'C:\Jts\launcher.log' /XF '*.log' 'ibgateway.*.exe' | Out-Null
    Write-Host "  C:\Jts settings copied (installer re-downloaded on new box)"
}

Write-Host "=== [6/6] Manifest ===" -ForegroundColor Cyan
$manifest = Join-Path $Stage 'MANIFEST.txt'
Push-Location C:\Omega
@"
exported_utc = $((Get-Date).ToUniversalTime().ToString('s'))Z
old_box      = 185.167.119.59
new_box      = 45.85.3.79 (RDP :42014)
git_head     = $(git rev-parse HEAD 2>$null)
git_branch   = $(git rev-parse --abbrev-ref HEAD 2>$null)
omega_exe    = $((Get-FileHash C:\Omega\Omega.exe -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash)
exe_mtime    = $((Get-Item C:\Omega\Omega.exe -ErrorAction SilentlyContinue).LastWriteTime)
"@ | Out-File $manifest -Encoding utf8
Pop-Location
Get-Content $manifest | ForEach-Object { Write-Host "  $_" }

Write-Host "=== Zipping (this can take a while) ===" -ForegroundColor Cyan
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Compress-Archive -Path "$Stage\*" -DestinationPath $Zip -CompressionLevel Optimal
$sz = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host ""
Write-Host "DONE: $Zip ($sz MB)" -ForegroundColor Green
Write-Host ""
Write-Host "Transfer to the new box (AFTER 2_NEW_VPS_BOOTSTRAP.ps1 has run there):" -ForegroundColor Yellow
Write-Host "  scp -P 2222 `"$Zip`" trader@45.85.3.79:C:/omega_migration.zip"
Write-Host "(or copy via RDP drive redirection if scp is unavailable)"
