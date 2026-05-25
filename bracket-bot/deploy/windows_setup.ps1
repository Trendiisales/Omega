# ============================================================================
# windows_setup.ps1 - one-time environment setup for the Omega gold-bracket bot
#
# Run from the bracket-bot folder:
#   cd C:\Omega\bracket-bot
#   powershell -ExecutionPolicy Bypass -File deploy\windows_setup.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'

$BotDir = Split-Path -Parent $PSScriptRoot
Set-Location $BotDir
Write-Host "Bot directory: $BotDir"

# --- 1. Locate Python (PATH, then common install paths, then the py launcher) ---
function Find-Python {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        "C:\Program Files\Python312\python.exe",
        "C:\Program Files\Python313\python.exe",
        "C:\Program Files\Python311\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) { return $py.Source }
    return $null
}

$Python = Find-Python
if (-not $Python) {
    Write-Error "Python not found. Install Python 3.12 from https://www.python.org/downloads/ (tick 'Add python.exe to PATH'), then re-run."
    exit 1
}
Write-Host ("Using Python: " + $Python)

# --- 2. Create a fresh virtual environment ---------------------------------
if (Test-Path ".venv") {
    Write-Host "Removing existing .venv for a clean install..."
    Remove-Item -Recurse -Force ".venv"
}
Write-Host "Creating virtual environment (.venv)..."
& $Python -m venv .venv
$VenvPy = Join-Path $BotDir ".venv\Scripts\python.exe"

# --- 3. Install dependencies -----------------------------------------------
# NOTE: we deliberately do NOT run "pip install --upgrade pip". Upgrading pip
# regenerates console-script launchers and can fail on locked-down Windows
# hosts (the t64.exe error); the bundled pip installs these packages fine.

Write-Host "Installing ib_insync (required by the live strategies)..."
& $VenvPy -m pip install --disable-pip-version-check --no-input ib_insync==0.9.86
if ($LASTEXITCODE -ne 0) {
    Write-Error "ib_insync install failed - the strategies cannot run without it."
    exit 1
}

Write-Host "Installing flask (optional - only the monitoring dashboard needs it)..."
& $VenvPy -m pip install --disable-pip-version-check --no-input flask==3.1.3
if ($LASTEXITCODE -ne 0) {
    Write-Warning "flask install failed. The dashboard (server.py) will be unavailable,"
    Write-Warning "but the trading strategies do NOT need it - this is not fatal."
}

Write-Host ""
Write-Host "Setup complete."
Write-Host "Next: run  deploy\register_tasks.ps1  in an ADMINISTRATOR PowerShell."
