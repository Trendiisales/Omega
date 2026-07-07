# =============================================================================
# 2_NEW_VPS_BOOTSTRAP.ps1 -- run ON THE NEW VPS (45.85.3.79) FIRST, via RDP
# =============================================================================
# RDP in: 45.85.3.79:42014 as trader (port 42014 is the provider's RDP port).
# This script makes the box reachable by all existing Mac-side tooling:
#   * OpenSSH Server on port 2222 (our convention -- every script assumes it)
#   * Firewall: inbound TCP 2222 (ssh), 7779 (GUI), 7781 (omega-terminal)
#   * git (via winget, if missing)
# Run in PowerShell AS ADMINISTRATOR. Then run 1_OLD_VPS_EXPORT.ps1 on the old
# box and transfer the zip here, then 3_NEW_VPS_RESTORE_VERIFY.ps1.
# =============================================================================
$ErrorActionPreference = 'Stop'

Write-Host "=== [1/4] OpenSSH Server ===" -ForegroundColor Cyan
$cap = Get-WindowsCapability -Online -Name 'OpenSSH.Server*'
if ($cap.State -ne 'Installed') {
    Add-WindowsCapability -Online -Name $cap.Name | Out-Null
    Write-Host "  installed $($cap.Name)"
} else { Write-Host "  already installed" }
Start-Service sshd -ErrorAction SilentlyContinue   # first start generates C:\ProgramData\ssh
Stop-Service  sshd -ErrorAction SilentlyContinue

Write-Host "=== [2/4] sshd on port 2222 ===" -ForegroundColor Cyan
$conf = 'C:\ProgramData\ssh\sshd_config'
$c = Get-Content $conf
$c = $c -replace '^\s*#?\s*Port\s+\d+\s*$', 'Port 2222'
if (-not ($c -match '^Port 2222')) { $c = ,'Port 2222' + $c }
# Password auth stays ON until FIX_SSH.ps1 establishes key auth, then it can
# be disabled. Pubkey explicitly on so FIX_SSH.ps1 finds a sane baseline.
$c = $c -replace '^\s*#?\s*PubkeyAuthentication\s+\w+\s*$', 'PubkeyAuthentication yes'
Set-Content $conf $c -Encoding ascii
Set-Service sshd -StartupType Automatic
Restart-Service sshd
Write-Host "  sshd listening check:"
Start-Sleep 2
Get-NetTCPConnection -State Listen -LocalPort 2222 -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "  OK: listening on $($_.LocalAddress):2222" -ForegroundColor Green }

Write-Host "=== [3/4] Firewall rules ===" -ForegroundColor Cyan
$rules = @(
    @{Name='Omega SSH 2222';          Port=2222},
    @{Name='Omega GUI 7779';          Port=7779},
    @{Name='Omega Terminal UI 7781';  Port=7781}
)
foreach ($r in $rules) {
    if (-not (Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Protocol TCP `
            -LocalPort $r.Port -Action Allow | Out-Null
        Write-Host "  added: $($r.Name)"
    } else { Write-Host "  exists: $($r.Name)" }
}

Write-Host "=== [4/4] git ===" -ForegroundColor Cyan
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    winget install --id Git.Git -e --accept-source-agreements --accept-package-agreements
    Write-Host "  git installed -- reopen PowerShell for PATH refresh"
} else { Write-Host "  git present: $(git --version)" }

Write-Host ""
Write-Host "BOOTSTRAP DONE." -ForegroundColor Green
Write-Host @"

Next steps:
  1. On the OLD box:  cd C:\Omega\migration ; .\1_OLD_VPS_EXPORT.ps1
  2. Transfer:        scp -P 2222 C:\Omega_migration\omega_migration_*.zip trader@45.85.3.79:C:/omega_migration.zip
                      (run on the old box; it will ask for THIS box's trader password)
  3. Here:            powershell -File C:\omega_migration_kit\3_NEW_VPS_RESTORE_VERIFY.ps1
     (copy migration\*.ps1 here first, or run it out of the restored C:\Omega\migration)
  4. From the Mac:    bash -c 'ssh-keygen -R "[45.85.3.79]:2222"; ssh -p 2222 trader@45.85.3.79 exit'
                      then re-run FIX_SSH.ps1 on this box for key auth.
"@ -ForegroundColor Yellow
