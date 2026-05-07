# =============================================================================
# analyze_s58_s59.ps1
# =============================================================================
# Wrapper around scripts/s58_s59_impact.py.
# Reason: chat UIs auto-link bare `.py` filenames as hyperlinks, breaking
# clipboard round-trips. PowerShell .ps1 extension is not auto-linked, so the
# command line stays paste-safe.
#
# USAGE:
#   cd C:\Omega\scripts
#   .\analyze_s58_s59.ps1 ..\logs\trades\
#
# Or with explicit args:
#   .\analyze_s58_s59.ps1 C:\Omega\logs\trades\omega_trade_closes_2026-05-07.csv
# =============================================================================

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$pyFile = Join-Path $scriptDir "s58_s59_impact.py"

if (-not (Test-Path $pyFile)) {
    Write-Host "ERROR: Cannot find $pyFile" -ForegroundColor Red
    Write-Host "Did you run 'git pull' on this machine?" -ForegroundColor Yellow
    exit 1
}

& python $pyFile @args
exit $LASTEXITCODE
