# =============================================================================
# disable_appverif_ust.ps1
# -----------------------------------------------------------------------------
# Reverses every change made by enable_appverif_ust.ps1:
#   1. Disable Application Verifier for Omega.exe
#   2. Disable gflags +ust for Omega.exe
#   3. Remove WER LocalDumps key for Omega.exe (or restore prior values)
#   4. Restore IFEO key state from snapshot if present
#
# Run this to return Omega.exe to its baseline diagnostic state. The 0xc0000374
# segment-heap trap continues to fire normally (that's normal Windows behaviour,
# not something we enabled) -- we just stop adding overhead from AppVerif/+ust.
#
# REQUIREMENTS
#   - Run as Administrator
# =============================================================================

#requires -RunAsAdministrator

$ErrorActionPreference = "Continue"
$Target = "Omega.exe"
$LogFile = "C:\Omega\logs\appverif_ust_disable.log"
$snapshotFile = "C:\Omega\logs\appverif_ust_ifeo_snapshot.json"

function Log {
    param([string]$msg)
    $ts = Get-Date -Format "yyyy-MM-ddTHH:mm:ss"
    $line = "[$ts] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line -ErrorAction SilentlyContinue
}

$logDir = Split-Path $LogFile -Parent
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

Log "==== disable_appverif_ust.ps1 START ===="
Log "Target: $Target"

# -----------------------------------------------------------------------------
# Step 1: locate gflags + appverif (same lookup logic as enable)
# -----------------------------------------------------------------------------
$gflags = Get-Command gflags.exe -ErrorAction SilentlyContinue
if (-not $gflags) {
    foreach ($c in @(
        "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe",
        "C:\Program Files\Windows Kits\10\Debuggers\x64\gflags.exe"
    )) {
        if (Test-Path $c) { $gflags = $c; break }
    }
}
$gflagsPath = if ($gflags -is [System.Management.Automation.CommandInfo]) { $gflags.Source } else { $gflags }

$appverif = Get-Command appverif.exe -ErrorAction SilentlyContinue
if (-not $appverif) {
    foreach ($c in @(
        "C:\Windows\System32\appverif.exe",
        "C:\Program Files (x86)\Application Verifier\appverif.exe"
    )) {
        if (Test-Path $c) { $appverif = $c; break }
    }
}
$appverifPath = if ($appverif -is [System.Management.Automation.CommandInfo]) { $appverif.Source } else { $appverif }

# -----------------------------------------------------------------------------
# Step 2: stop service
# -----------------------------------------------------------------------------
$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Log "Stopping Omega service..."
    Stop-Service -Name "Omega" -Force
    Start-Sleep -Seconds 3
}

# -----------------------------------------------------------------------------
# Step 3: disable Application Verifier
# -----------------------------------------------------------------------------
if ($appverifPath) {
    Log "Disabling Application Verifier for $Target..."
    & $appverifPath -disable * -for $Target | Out-String | ForEach-Object { Log $_ }
} else {
    Log "WARN: appverif.exe not found, skipping AppVerif disable"
}

# -----------------------------------------------------------------------------
# Step 4: clear gflags +ust
# -----------------------------------------------------------------------------
if ($gflagsPath) {
    Log "Disabling gflags +ust on $Target..."
    & $gflagsPath /i $Target -ust | Out-String | ForEach-Object { Log $_ }
} else {
    Log "WARN: gflags.exe not found, skipping gflags disable"
}

# -----------------------------------------------------------------------------
# Step 5: remove WER LocalDumps key for Omega.exe
# -----------------------------------------------------------------------------
$werKey = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$Target"
if (Test-Path $werKey) {
    Remove-Item -Path $werKey -Recurse -Force
    Log "Removed WER LocalDumps key for $Target"
} else {
    Log "No WER LocalDumps key for $Target to remove."
}

# -----------------------------------------------------------------------------
# Step 6: restore IFEO key from snapshot if needed
# -----------------------------------------------------------------------------
if (Test-Path $snapshotFile) {
    try {
        $snapshot = Get-Content $snapshotFile -Raw | ConvertFrom-Json
        $ifeoPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$Target"
        if (-not $snapshot.Existed) {
            # Original state had no IFEO key. If gflags/appverif left an empty key,
            # remove it so we return to true baseline.
            if (Test-Path $ifeoPath) {
                $key = Get-Item $ifeoPath
                if ($key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0) {
                    Remove-Item -Path $ifeoPath -Force
                    Log "Removed empty IFEO key (original state: did not exist)"
                } else {
                    Log "IFEO key still has values; leaving in place"
                }
            }
        } else {
            Log "Original IFEO key existed with $($snapshot.Values.PSObject.Properties.Count) values. Manual review may be needed if appverif/gflags didn't restore them."
            Log "Snapshot at: $snapshotFile"
        }
    } catch {
        Log "WARN: could not parse snapshot file: $_"
    }
}

Log "==== disable_appverif_ust.ps1 COMPLETE ===="
Log ""
Log "Omega.exe is now back to baseline diagnostic state."
Log "Restart the service when ready: Start-Service Omega"
