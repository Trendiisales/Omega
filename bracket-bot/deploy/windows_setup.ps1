# ============================================================================
# windows_setup.ps1 - one-time environment setup for the Omega gold-bracket bot
#
# Run from the bracket-bot folder:
#   powershell -ExecutionPolicy Bypass -File deploy\windows_setup.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'

# bracket-bot/ is the parent of this script's deploy/ folder
$BotDir = Split-Path -Parent $PSScriptRoot
Set-Location $BotDir
Write-Host "Bot directory: $BotDir"

# --- 1. Verify Python -------------------------------------------------------
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
    Write-Error "Python not found on PATH. Install Python 3.12 from python.org (tick 'Add python.exe to PATH'), then re-run."
    exit 1
}
Write-Host ("Found: " + (python --version))

# --- 2. Create virtual environment -----------------------------------------
if (-not (Test-Path ".venv")) {
    Write-Host "Creating virtual environment (.venv)..."
    python -m venv .venv
} else {
    Write-Host "Virtual environment already exists - reusing it."
}

# --- 3. Install dependencies ------------------------------------------------
Write-Host "Installing dependencies from requirements.txt..."
& ".venv\Scripts\python.exe" -m pip install --upgrade pip
& ".venv\Scripts\python.exe" -m pip install -r requirements.txt

Write-Host ""
Write-Host "Setup complete."
Write-Host "Next: run  deploy\register_tasks.ps1  in an ADMINISTRATOR PowerShell to schedule the strategies."
