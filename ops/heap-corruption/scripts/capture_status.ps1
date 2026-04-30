# =============================================================================
# capture_status.ps1
# -----------------------------------------------------------------------------
# Read-only health check for the diagnostic configuration. Run this any time
# to confirm AppVerif + gflags +ust + WER LocalDumps are still in place, and
# to see how many crash dumps have been captured since enable.
#
# Pure read-only -- safe to run any time, does not modify any state.
# =============================================================================

$Target = "Omega.exe"
$DumpDir = "C:\CrashDumps"
$ifeoPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$Target"
$werKey   = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$Target"

Write-Host ""
Write-Host "=== Omega Diagnostic Status ===" -ForegroundColor Cyan

# -----------------------------------------------------------------------------
# Service state
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "[Service]" -ForegroundColor Yellow
$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "  Omega service: $($svc.Status)"
    $proc = Get-Process Omega -ErrorAction SilentlyContinue
    if ($proc) {
        Write-Host "  PID: $($proc.Id)  StartTime: $($proc.StartTime)"
        Write-Host "  WorkingSet: $([math]::Round($proc.WorkingSet64 / 1MB, 1)) MB"
        Write-Host "  Threads: $($proc.Threads.Count)  Handles: $($proc.HandleCount)"
    }
} else {
    Write-Host "  Omega service NOT INSTALLED"
}

# -----------------------------------------------------------------------------
# IFEO key state (AppVerif + gflags)
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "[Image File Execution Options for $Target]" -ForegroundColor Yellow
if (Test-Path $ifeoPath) {
    $key = Get-Item $ifeoPath
    $gf = $key.GetValue("GlobalFlag")
    $vf = $key.GetValue("VerifierFlags")
    $vd = $key.GetValue("VerifierDlls")

    if ($null -ne $gf) {
        $gfHex = "0x{0:X8}" -f $gf
        $ust = if ($gf -band 0x1000) { "ON" } else { "off" }
        Write-Host "  GlobalFlag:    $gfHex   (+ust: $ust)"
    } else {
        Write-Host "  GlobalFlag:    not set"
    }

    if ($null -ne $vf) {
        $vfHex = "0x{0:X8}" -f $vf
        Write-Host "  VerifierFlags: $vfHex"
    } else {
        Write-Host "  VerifierFlags: not set"
    }

    if ($null -ne $vd) {
        Write-Host "  VerifierDlls:  $vd"
        $appverifActive = $true
    } else {
        Write-Host "  VerifierDlls:  not set (AppVerif inactive)"
        $appverifActive = $false
    }

    # Decode VerifierFlags to known checks (full decode is in appverif.exe headers,
    # this covers the relevant ones for our setup)
    if ($appverifActive -and $null -ne $vf) {
        $checks = @()
        if ($vf -band 0x00000001) { $checks += "Heaps(PageHeap)" }
        if ($vf -band 0x00000040) { $checks += "Locks" }
        if ($vf -band 0x00000020) { $checks += "Handles" }
        if ($vf -band 0x00000080) { $checks += "Memory" }
        Write-Host "  Active checks: $($checks -join ', ')"
        if ($vf -band 0x00000001) {
            Write-Host "  WARN: PageHeap-equivalent (Heaps) is ON. To stay light, run:" -ForegroundColor Yellow
            Write-Host "        appverif -disable Heaps -for $Target"
        }
    }
} else {
    Write-Host "  No IFEO key for $Target (no diagnostics enabled)"
}

# -----------------------------------------------------------------------------
# WER LocalDumps state
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "[WER LocalDumps for $Target]" -ForegroundColor Yellow
if (Test-Path $werKey) {
    $wer = Get-Item $werKey
    $df = $wer.GetValue("DumpFolder")
    $dt = $wer.GetValue("DumpType")
    $dc = $wer.GetValue("DumpCount")
    $dtName = switch ($dt) { 0 { "Custom" } 1 { "Mini" } 2 { "Full" } default { "?($dt)" } }
    Write-Host "  DumpFolder: $df"
    Write-Host "  DumpType:   $dt ($dtName)"
    Write-Host "  DumpCount:  $dc"
} else {
    Write-Host "  No WER LocalDumps configured for $Target"
}

# -----------------------------------------------------------------------------
# Recent crash dumps
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "[Recent crash dumps in $DumpDir]" -ForegroundColor Yellow
if (Test-Path $DumpDir) {
    $dumps = Get-ChildItem $DumpDir -Filter "Omega.exe.*.dmp" -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending
    if ($dumps) {
        Write-Host "  $($dumps.Count) dump(s) total"
        $dumps | Select-Object -First 5 | ForEach-Object {
            $sizeMB = [math]::Round($_.Length / 1MB, 1)
            Write-Host ("    {0,-40}  {1,8} MB  {2}" -f $_.Name, $sizeMB, $_.LastWriteTime)
        }
        if ($dumps.Count -gt 5) {
            Write-Host "    ... ($($dumps.Count - 5) older)"
        }
    } else {
        Write-Host "  No dumps captured yet."
    }
} else {
    Write-Host "  Dump directory $DumpDir does not exist"
}

# -----------------------------------------------------------------------------
# Recent crash events from Application log
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "[Recent Omega.exe crash events (last 5)]" -ForegroundColor Yellow
$events = Get-EventLog -LogName Application -Newest 50 -EntryType Error -ErrorAction SilentlyContinue |
          Where-Object { $_.Source -match "Application Error" -and $_.Message -match "Omega.exe" } |
          Select-Object -First 5
if ($events) {
    foreach ($e in $events) {
        # Extract exception code + fault offset from message
        $msg = $e.Message
        $excCode = ([regex]::Match($msg, "Exception code: (0x[0-9a-fA-F]+)")).Groups[1].Value
        $foff = ([regex]::Match($msg, "Fault offset: (0x[0-9a-fA-F]+)")).Groups[1].Value
        $fmod = ([regex]::Match($msg, "Faulting module name: ([^,\r\n]+)")).Groups[1].Value
        Write-Host ("  {0}  {1}  {2}+{3}" -f $e.TimeGenerated, $excCode, $fmod, $foff)
    }
} else {
    Write-Host "  No Omega.exe crash events found in last 50 Application log Errors"
}

Write-Host ""
