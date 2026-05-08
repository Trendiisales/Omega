# =============================================================================
# FIX_SSH.ps1 -- One-shot Windows OpenSSH key-auth repair
# =============================================================================
#
# 2026-05-08 S19: Stop the recurring "key works once then breaks" cycle on the
#   Omega VPS. Single elevated-PowerShell run fixes the actual root cause:
#
#     1. For admin accounts, sshd ignores ~/.ssh/authorized_keys and only
#        reads C:\ProgramData\ssh\administrators_authorized_keys. Most fixes
#        update the wrong file and "appear" to work for unrelated reasons.
#
#     2. StrictModes (default ON) silently rejects keys when NTFS ACLs grant
#        any access to anyone other than the owner + SYSTEM + Administrators.
#        Notepad, Explorer copy/paste, AV, Group Policy, and deploy scripts
#        all routinely re-add inheritance and break this.
#
#     3. The PARENT directory's ACL also has to be locked down -- many fixes
#        only stamp the file and miss the directory, so sshd still rejects.
#
# WHAT THIS DOES (idempotent -- safe to re-run any time):
#   * Detects whether the current user is an Administrator and picks the
#     correct authorized_keys path accordingly.
#   * Reads the public key from -PublicKey arg, clipboard, or fails clean.
#   * Backs up any existing authorized_keys to .bak_<timestamp>.
#   * Writes the key (de-duplicated -- won't grow on re-run).
#   * Resets inheritance + grants only Administrators / SYSTEM / (user) on
#     BOTH the authorized_keys file AND its parent directory.
#   * Restarts sshd to pick up any sshd_config or ACL changes.
#   * Prints sshd's resolved AuthorizedKeysFile setting so you can verify
#     the file we just wrote is the one sshd will actually read.
#
# USAGE
#   On the Mac:
#       cat ~/.ssh/id_ed25519.pub | pbcopy
#   RDP into the VPS as 'trader' (RDP clipboard sync carries it across).
#   In ELEVATED PowerShell:
#       cd C:\Omega
#       git pull
#       .\FIX_SSH.ps1                                 # reads from clipboard
#       # OR:
#       .\FIX_SSH.ps1 -PublicKey "ssh-ed25519 AAAA... jo@Mac"
#
# =============================================================================
[CmdletBinding()]
param(
    [string]$PublicKey = $null
)

$ErrorActionPreference = 'Stop'

function Write-Section($msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

# -----------------------------------------------------------------------------
# 0. Sanity: must be elevated.
# -----------------------------------------------------------------------------
$me = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($me)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "ERROR: must run from an elevated PowerShell window." -ForegroundColor Red
    exit 1
}
$isAdmin = $true   # by definition we got past the elevation check

# -----------------------------------------------------------------------------
# 1. Get the public key.
# -----------------------------------------------------------------------------
Write-Section "Loading public key"
if (-not $PublicKey) {
    try {
        $PublicKey = (Get-Clipboard -Raw).Trim()
    } catch {
        $PublicKey = $null
    }
}
if (-not $PublicKey) {
    Write-Host "ERROR: no -PublicKey provided and clipboard is empty." -ForegroundColor Red
    Write-Host "Usage: .\FIX_SSH.ps1 -PublicKey 'ssh-ed25519 AAAA... user@host'" -ForegroundColor Yellow
    exit 1
}
$PublicKey = ($PublicKey -split "`r?`n" | Where-Object { $_ -match '^(ssh-|ecdsa-)' } | Select-Object -First 1).Trim()
if ($PublicKey -notmatch '^(ssh-(rsa|ed25519|dss)|ecdsa-sha2-\S+)\s+\S+') {
    Write-Host "ERROR: PublicKey does not look like a valid OpenSSH public key:" -ForegroundColor Red
    Write-Host "  $PublicKey" -ForegroundColor Red
    exit 1
}
Write-Host "Key OK: $($PublicKey.Substring(0, [Math]::Min(80, $PublicKey.Length)))..."

# -----------------------------------------------------------------------------
# 2. Decide which authorized_keys file sshd will actually read.
#    For admin accounts on Windows OpenSSH the default sshd_config has:
#      Match Group administrators
#          AuthorizedKeysFile __PROGRAMDATA__/ssh/administrators_authorized_keys
#    so the user-folder file is ignored. We replicate that decision here.
# -----------------------------------------------------------------------------
Write-Section "Resolving authorized_keys path"
$progSshDir = Join-Path $env:ProgramData 'ssh'
if ($isAdmin) {
    $akDir  = $progSshDir
    $akFile = Join-Path $akDir 'administrators_authorized_keys'
    Write-Host "Account '$env:USERNAME' is admin -> $akFile"
} else {
    $akDir  = Join-Path $env:USERPROFILE '.ssh'
    $akFile = Join-Path $akDir 'authorized_keys'
    Write-Host "Account '$env:USERNAME' is non-admin -> $akFile"
}

# -----------------------------------------------------------------------------
# 3. Make the directory + file exist; back up any prior content.
# -----------------------------------------------------------------------------
Write-Section "Preparing file"
if (-not (Test-Path $akDir)) {
    New-Item -ItemType Directory -Force -Path $akDir | Out-Null
    Write-Host "Created $akDir"
}
if (Test-Path $akFile) {
    $bak = "$akFile.bak_$(Get-Date -Format yyyyMMdd_HHmmss)"
    Copy-Item -Path $akFile -Destination $bak -Force
    Write-Host "Backed up existing -> $bak"
}
if (-not (Test-Path $akFile)) {
    New-Item -ItemType File -Force -Path $akFile | Out-Null
}

# Append (deduped) the supplied key.
$existing = @()
try { $existing = Get-Content $akFile -ErrorAction Stop } catch { $existing = @() }
if ($existing -notcontains $PublicKey) {
    Add-Content -Path $akFile -Value $PublicKey -Encoding ascii
    Write-Host "Key appended."
} else {
    Write-Host "Key already present -- not duplicated."
}

# -----------------------------------------------------------------------------
# 4. Stamp ACLs. THIS is what fixes StrictModes for good. Both file AND parent
#    directory must be locked down to owner + SYSTEM + Administrators only.
# -----------------------------------------------------------------------------
Write-Section "Stamping ACLs"

function Lock-Acl([string]$path, [bool]$grantUser) {
    if (-not (Test-Path $path)) { return }
    Write-Host "  $path"
    icacls $path /inheritance:r | Out-Null
    icacls $path /grant:r "Administrators:(F)" "SYSTEM:(F)" | Out-Null
    if ($grantUser) {
        icacls $path /grant:r "${env:USERNAME}:(F)" | Out-Null
    }
    # Strip everything else by removing all unknown principals.
    $acl = Get-Acl $path
    $changed = $false
    foreach ($r in @($acl.Access)) {
        $idn = $r.IdentityReference.Value
        if ($idn -notmatch '\\(Administrators|SYSTEM)$' -and
            $idn -notmatch "\\$([Regex]::Escape($env:USERNAME))$") {
            $acl.RemoveAccessRule($r) | Out-Null
            $changed = $true
        }
    }
    if ($changed) { Set-Acl -Path $path -AclObject $acl }
}

# Non-admin file: owner needs access. Admin file: only Administrators+SYSTEM.
Lock-Acl -path $akFile -grantUser (-not $isAdmin)
Lock-Acl -path $akDir  -grantUser $true   # directory always allows current user to traverse

# -----------------------------------------------------------------------------
# 5. Restart sshd so it re-reads any config / picks up directory ACL refresh.
# -----------------------------------------------------------------------------
Write-Section "Restarting sshd"
try {
    Restart-Service sshd -Force
    Write-Host "sshd restarted."
} catch {
    Write-Host "WARN: Restart-Service sshd failed: $_" -ForegroundColor Yellow
    Write-Host "Trying Stop+Start..." -ForegroundColor Yellow
    Stop-Service sshd -Force -ErrorAction SilentlyContinue
    Start-Service sshd
    Write-Host "sshd restarted via Stop+Start."
}

# -----------------------------------------------------------------------------
# 6. Verification: print what sshd believes its config is, and the file content.
# -----------------------------------------------------------------------------
Write-Section "Verification"

Write-Host "Resolved sshd config (authorizedkeysfile / match):"
& sshd.exe -T 2>$null | Select-String -Pattern 'authorizedkeysfile|^match' | ForEach-Object {
    Write-Host "  $_"
}

Write-Host ""
Write-Host "Final ACL on $akFile :"
icacls $akFile | ForEach-Object { Write-Host "  $_" }

Write-Host ""
Write-Host "Final ACL on $akDir :"
icacls $akDir | ForEach-Object { Write-Host "  $_" }

Write-Host ""
Write-Host "Final $akFile contents:"
Get-Content $akFile | ForEach-Object { Write-Host "  $_" }

# -----------------------------------------------------------------------------
# 7. Tell the operator how to test.
# -----------------------------------------------------------------------------
Write-Section "Done. Test from your Mac"
Write-Host @"
On the Mac, run:

    ssh -p 2222 trader@185.167.119.59 exit

If it returns to the prompt without asking for a password, key auth is working.

If it still prompts, tail the sshd log on the VPS while reconnecting:

    Get-Content "$progSshDir\logs\sshd.log" -Wait -Tail 50

The exact rejection reason will print there. The most common remaining
causes after this script:
  * sshd_config has 'PubkeyAuthentication no'  (check with sshd -T | findstr pubkey)
  * StrictModes is being tripped by a parent directory ACL (rare; this script
    handled the immediate parent, but if the disk root has odd ACLs that
    can also bite -- run icacls C:\ to inspect)
  * The 'trader' account isn't actually a member of Administrators, but
    sshd_config still has the Match Group administrators redirect -- in
    that case rerun this script as 'trader' (not as the elevated session)
    so the user-folder authorized_keys is what gets written.
"@
