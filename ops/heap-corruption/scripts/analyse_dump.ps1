# =============================================================================
# analyse_dump.ps1
# -----------------------------------------------------------------------------
# Wrapper that:
#   1. Finds the most recent Omega.exe crash dump (or one you specify).
#   2. Locates cdb.exe (WinDbg's command-line counterpart, headless and faster).
#   3. Runs analyse_dump.windbg against it with the right symbol path.
#   4. Saves the full output to a .txt next to the dump.
#
# OUTPUT
#   <DumpDir>\<dumpname>.analysis.txt
#
# USAGE
#   .\analyse_dump.ps1                           # latest dump
#   .\analyse_dump.ps1 -DumpPath C:\path\to.dmp  # specific dump
#   .\analyse_dump.ps1 -PdbPath C:\custom\pdb    # symbol override
# =============================================================================

param(
    [string]$DumpPath  = $null,
    [string]$DumpDir   = "C:\CrashDumps",
    [string]$PdbPath   = "C:\Omega",
    [string]$SymCache  = "C:\SymCache",
    [string]$ScriptDir = $PSScriptRoot
)

$ErrorActionPreference = "Stop"

# -----------------------------------------------------------------------------
# Step 1: locate cdb.exe
# -----------------------------------------------------------------------------
$cdb = Get-Command cdb.exe -ErrorAction SilentlyContinue
if (-not $cdb) {
    foreach ($c in @(
        "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
        "C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe",
        "C:\Program Files\Debugging Tools for Windows (x64)\cdb.exe"
    )) {
        if (Test-Path $c) { $cdb = $c; break }
    }
}
if (-not $cdb) {
    Write-Error "cdb.exe not found. Install Windows SDK Debugging Tools."
    Write-Error "  winget install Microsoft.WindowsSDK"
    exit 1
}
$cdbPath = if ($cdb -is [System.Management.Automation.CommandInfo]) { $cdb.Source } else { $cdb }
Write-Host "cdb located at: $cdbPath"

# -----------------------------------------------------------------------------
# Step 2: find the dump
# -----------------------------------------------------------------------------
if (-not $DumpPath) {
    if (-not (Test-Path $DumpDir)) {
        Write-Error "Dump directory $DumpDir does not exist. Has anything crashed?"
        exit 1
    }
    $dump = Get-ChildItem $DumpDir -Filter "Omega.exe.*.dmp" |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
    if (-not $dump) {
        Write-Error "No Omega.exe.*.dmp found in $DumpDir"
        exit 1
    }
    $DumpPath = $dump.FullName
}
if (-not (Test-Path $DumpPath)) {
    Write-Error "Dump file does not exist: $DumpPath"
    exit 1
}
Write-Host "Analysing dump: $DumpPath"
Write-Host "  Size: $([math]::Round((Get-Item $DumpPath).Length / 1MB, 1)) MB"
Write-Host "  Time: $((Get-Item $DumpPath).LastWriteTime)"

# -----------------------------------------------------------------------------
# Step 3: locate the WinDbg script
# -----------------------------------------------------------------------------
$windbgScript = Join-Path $ScriptDir "analyse_dump.windbg"
if (-not (Test-Path $windbgScript)) {
    Write-Error "analyse_dump.windbg not found at $windbgScript"
    exit 1
}

# -----------------------------------------------------------------------------
# Step 4: ensure symbol cache exists
# -----------------------------------------------------------------------------
if (-not (Test-Path $SymCache)) {
    New-Item -ItemType Directory -Path $SymCache -Force | Out-Null
    Write-Host "Created symbol cache: $SymCache"
}

# -----------------------------------------------------------------------------
# Step 5: build symbol path
# -----------------------------------------------------------------------------
$symPath = "srv*$SymCache*https://msdl.microsoft.com/download/symbols;$PdbPath"
Write-Host "Symbol path: $symPath"

# -----------------------------------------------------------------------------
# Step 6: run cdb with the script. -lines turns on source-line info,
# -c '$$<file' executes the script, q exits when done.
# -----------------------------------------------------------------------------
$outFile = "$DumpPath.analysis.txt"
Write-Host ""
Write-Host "Running analysis. Output -> $outFile"
Write-Host ""

# Build cdb command. Note: $$< is WinDbg's "include script file" command.
$cdbCommand = '$$<' + $windbgScript + ';q'

& $cdbPath -z $DumpPath -y $symPath -lines -c $cdbCommand 2>&1 | Tee-Object -FilePath $outFile

Write-Host ""
Write-Host "Analysis complete: $outFile"
Write-Host ""
Write-Host "Key sections to look at first:"
Write-Host "  - !analyze -v output (top of file): WinDbg's classifier"
Write-Host "  - 'Faulting thread stack': what we were doing when the trap fired"
Write-Host "  - '!heap -p -a' output: ALLOCATION BACKTRACE for the corrupted block"
Write-Host "  - 'Thread list': did corruption come from a different thread?"
Write-Host ""
Write-Host "Upload $outFile to the Claude session for joint analysis."
