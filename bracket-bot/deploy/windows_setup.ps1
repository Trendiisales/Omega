# ============================================================================
# windows_setup.ps1 - one-time environment setup for the Omega gold-bracket bot
#
# Run from the bracket-bot folder:
#   cd C:\Omega\bracket-bot
#   powershell -ExecutionPolicy Bypass -File deploy\windows_setup.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'

# bracket-bot/ is the parent of this script's deploy/ folder
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

# --- 2. Create virtual environment -----------------------------------------
if (-not (Test-Path ".venv")) {
    Write-Host "Creating virtual environment (.venv)..."
    & $Python -m venv .venv
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
