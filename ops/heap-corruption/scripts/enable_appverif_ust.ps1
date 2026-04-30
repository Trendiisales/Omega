# =============================================================================
# enable_appverif_ust.ps1
# -----------------------------------------------------------------------------
# Configures heap-corruption diagnostics for Omega.exe WITHOUT enabling
# PageHeap. Three things get turned on:
#
#   1. Application Verifier  -> Locks + Memory + Handles checks ONLY.
#                               NO Heaps stop -- that path is PageHeap-equivalent
#                               and we want to stay lighter than that for the
#                               first attempt.
#   2. GFlags +ust            -> Records allocation backtraces inside every
#                               heap block. When the existing 0xc0000374 trap
#                               fires (segment heap fail-fast detection,
#                               normal Windows behaviour, NOT PageHeap), the
#                               dump now has allocation history -- WinDbg's
#                               !heap -p -a <addr> will tell us who allocated
#                               the corrupted block.
#   3. WER LocalDumps        -> Writes full minidumps to C:\CrashDumps\ on
#                               every crash, indexed by PID and timestamp.
#                               Survives watchdog restarts.
#
# Memory overhead: ~30-50% over baseline. Acceptable on the 3 GB VPS provided
# you reboot first to free RAM (or run on a non-prod replica).
#
# This script is REVERSIBLE -- run disable_appverif_ust.ps1 to undo every
# change made here.
#
# REQUIREMENTS
#   - Run as Administrator (writes HKLM keys + AppVerif modifies process state)
#   - Windows SDK Debugging Tools installed (provides gflags.exe). Usually
#     ships with Visual Studio. If gflags is missing, install via:
#       winget install Microsoft.WindowsSDK
#     (or download from https://learn.microsoft.com/windows-hardware/drivers/debugger/)
#   - Application Verifier installed (built into Win10/11; if missing,
#     install Windows SDK Debugging Tools).
# =============================================================================

#requires -RunAsAdministrator

$ErrorActionPreference = "Stop"
$Target = "Omega.exe"
$DumpDir = "C:\CrashDumps"
$LogFile = "C:\Omega\logs\appverif_ust_enable.log"

function Log {
    param([string]$msg)
    $ts = Get-Date -Format "yyyy-MM-ddTHH:mm:ss"
    $line = "[$ts] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line -ErrorAction SilentlyContinue
}

# Ensure log directory exists
$logDir = Split-Path $LogFile -Parent
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

Log "==== enable_appverif_ust.ps1 START ===="
Log "Target: $Target"
Log "Dump directory: $DumpDir"

# -----------------------------------------------------------------------------
# Step 1: locate gflags and appverif
# -----------------------------------------------------------------------------
$gflags = Get-Command gflags.exe -ErrorAction SilentlyContinue
if (-not $gflags) {
    # Common SDK install paths
    $candidates = @(
        "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe",
        "C:\Program Files\Windows Kits\10\Debuggers\x64\gflags.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $gflags = $c; break }
    }
}
if (-not $gflags) {
    Log "ERROR: gflags.exe not found. Install Windows SDK Debugging Tools."
    Log "  winget install Microsoft.WindowsSDK"
    Log "  or https://learn.microsoft.com/windows-hardware/drivers/debugger/"
    exit 1
}
$gflagsPath = if ($gflags -is [System.Management.Automation.CommandInfo]) { $gflags.Source } else { $gflags }
Log "gflags found at: $gflagsPath"

$appverif = Get-Command appverif.exe -ErrorAction SilentlyContinue
if (-not $appverif) {
    $candidates = @(
        "C:\Windows\System32\appverif.exe",
        "C:\Program Files (x86)\Application Verifier\appverif.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $appverif = $c; break }
    }
}
if (-not $appverif) {
    Log "ERROR: appverif.exe not found. Install Application Verifier."
    Log "  Built into Win10/11 by default; if missing, install Windows SDK Debugging Tools."
    exit 1
}
$appverifPath = if ($appverif -is [System.Management.Automation.CommandInfo]) { $appverif.Source } else { $appverif }
Log "appverif found at: $appverifPath"

# -----------------------------------------------------------------------------
# Step 2: stop the Omega service before flipping IFEO bits
# -----------------------------------------------------------------------------
$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Log "Stopping Omega service before configuration..."
    Stop-Service -Name "Omega" -Force
    Start-Sleep -Seconds 3
    Log "Omega service stopped."
} else {
    Log "Omega service not running (status=$($svc.Status))."
}

# -----------------------------------------------------------------------------
# Step 3: snapshot current IFEO state for rollback
# -----------------------------------------------------------------------------
$ifeoPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$Target"
$snapshotFile = "C:\Omega\logs\appverif_ust_ifeo_snapshot.json"

$snapshot = @{
    Target  = $Target
    Existed = $false
    Values  = @{}
}
if (Test-Path $ifeoPath) {
    $snapshot.Existed = $true
    $key = Get-Item $ifeoPath
    foreach ($name in $key.GetValueNames()) {
        $snapshot.Values[$name] = $key.GetValue($name)
    }
    Log "Snapshotted existing IFEO key for $Target ($($snapshot.Values.Count) values)"
} else {
    Log "No existing IFEO key for $Target; will be created."
}
$snapshot | ConvertTo-Json -Depth 5 | Set-Content -Path $snapshotFile
Log "IFEO snapshot saved: $snapshotFile"

# -----------------------------------------------------------------------------
# Step 4: enable Application Verifier checks (Locks + Memory + Handles ONLY)
# -----------------------------------------------------------------------------
Log "Enabling Application Verifier (Locks + Memory + Handles, NO Heaps)..."
& $appverifPath -enable Locks Memory Handles -for $Target | Out-String | ForEach-Object { Log $_ }

# Verify it stuck
if (Test-Path $ifeoPath) {
    $vf = (Get-ItemProperty -Path $ifeoPath -Name "VerifierFlags" -ErrorAction SilentlyContinue).VerifierFlags
    Log "VerifierFlags after enable: 0x$([Convert]::ToString($vf, 16))"
} else {
    Log "WARN: IFEO key not found after appverif enable -- check appverif output above"
}

# -----------------------------------------------------------------------------
# Step 5: enable gflags +ust (user-mode stack tracing for heap allocations)
# -----------------------------------------------------------------------------
Log "Enabling gflags +ust on $Target..."
& $gflagsPath /i $Target +ust | Out-String | ForEach-Object { Log $_ }

# Verify
if (Test-Path $ifeoPath) {
    $gf = (Get-ItemProperty -Path $ifeoPath -Name "GlobalFlag" -ErrorAction SilentlyContinue).GlobalFlag
    Log "GlobalFlag after gflags +ust: $gf (expect 0x1000 bit set = ust)"
}

# -----------------------------------------------------------------------------
# Step 6: configure WER LocalDumps for Omega.exe -> C:\CrashDumps\
# -----------------------------------------------------------------------------
Log "Configuring WER LocalDumps for $Target..."
if (-not (Test-Path $DumpDir)) {
    New-Item -ItemType Directory -Path $DumpDir -Force | Out-Null
    Log "Created dump directory: $DumpDir"
}

$werRoot = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps"
$werKey  = "$werRoot\$Target"
if (-not (Test-Path $werRoot)) { New-Item -Path $werRoot -Force | Out-Null }
if (-not (Test-Path $werKey))  { New-Item -Path $werKey  -Force | Out-Null }

# DumpFolder = where to write dumps
New-ItemProperty -Path $werKey -Name "DumpFolder"  -Value $DumpDir -PropertyType ExpandString -Force | Out-Null
# DumpType = 2 -> full minidump (memory + handles + threads). 1 = mini, 2 = full.
New-ItemProperty -Path $werKey -Name "DumpType"    -Value 2        -PropertyType DWord       -Force | Out-Null
# DumpCount = max dumps retained (FIFO). 20 covers ~10 hours of crash-loop.
New-ItemProperty -Path $werKey -Name "DumpCount"   -Value 20       -PropertyType DWord       -Force | Out-Null
Log "WER LocalDumps configured: type=Full, count=20, folder=$DumpDir"

# -----------------------------------------------------------------------------
# Step 7: print summary + next steps
# -----------------------------------------------------------------------------
Log "==== enable_appverif_ust.ps1 COMPLETE ===="
Log ""
Log "Configuration applied. Memory overhead expected: 30-50% over baseline."
Log "Rollback any time: .\disable_appverif_ust.ps1"
Log ""
Log "NEXT STEPS:"
Log "  1. Free RAM if running on the 3 GB VPS:"
Log "       Restart-Computer -Force   (during quiet market window)"
Log "  2. Start the Omega service:"
Log "       Start-Service Omega"
Log "  3. Watch for crash dumps:"
Log "       Get-ChildItem $DumpDir -Filter Omega.exe.*.dmp | Sort-Object LastWriteTime -Descending"
Log "  4. When a dump appears, analyse it:"
Log "       .\analyse_dump.ps1"
