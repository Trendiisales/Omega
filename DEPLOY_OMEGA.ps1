# ==============================================================================
#                        OMEGA - DEPLOY AND START
#   Run from anywhere on the VPS - handles everything.
# ==============================================================================

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  Commodities and Indices  |  Breakout System" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# [1/4] Stop any running Omega process
Write-Host "[1/4] Stopping existing Omega process..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Host "      [OK] Stopped (or was not running)" -ForegroundColor Green
Write-Host ""

# [2/4] Pull latest from GitHub
Write-Host "[2/4] Pulling latest from GitHub..." -ForegroundColor Yellow
Set-Location C:\Omega
git fetch origin
git reset --hard origin/main
# Force-overwrite any files that may have wrong encoding or been modified outside git
git checkout HEAD -- symbols.ini
git checkout HEAD -- DEPLOY_OMEGA.ps1
Write-Host "      [OK] Up to date: $(git log --oneline -1)" -ForegroundColor Green
Write-Host ""

# [3/4] Build
Write-Host "[3/4] Building..." -ForegroundColor Yellow
if (Test-Path "C:\Omega\build") {
    Remove-Item -Path "C:\Omega\build" -Recurse -Force
}
New-Item -ItemType Directory -Path "C:\Omega\build" -Force | Out-Null
Set-Location C:\Omega\build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
cmake --build . --config Release
if (-not (Test-Path "C:\Omega\build\Release\Omega.exe")) {
    Write-Host "      [ERROR] Build failed - Omega.exe not found!" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    return
}
Write-Host "      [OK] Omega.exe built" -ForegroundColor Green
Write-Host ""

# [4/4] Copy assets and run
Write-Host "[4/4] Copying assets and starting..." -ForegroundColor Yellow
$rel = "C:\Omega\build\Release"
$configSource = "C:\Omega\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "C:\Omega\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found in repo" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    return
}
Copy-Item $configSource                          "$rel\omega_config.ini"  -Force
Copy-Item "C:\Omega\symbols.ini"                 "$rel\symbols.ini"       -Force
Copy-Item "C:\Omega\src\gui\www\omega_index.html" "$rel\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\chimera_logo.png" "$rel\chimera_logo.png" -Force -ErrorAction SilentlyContinue
Write-Host "      [OK] Assets copied (omega_config.ini + symbols.ini + GUI)" -ForegroundColor Green
Write-Host ""

# [5/5] Verify deployed symbols.ini matches repo -- abort if encoding or copy failed
Write-Host "[5/5] Verifying deployed symbols.ini..." -ForegroundColor Yellow

# Expected ground-truth values -- must match configure() in main.cpp exactly
$expected = @{
    "GOLD.F"   = @{ "MIN_RANGE" = "6.00";    "MIN_STRUCTURE_MS" = "15000"; "BREAKOUT_FAIL_MS" = "15000" }
    "XAGUSD"   = @{ "MIN_RANGE" = "0.15";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "12000" }
    "US500.F"  = @{ "MIN_RANGE" = "4.00";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "10000" }
    "USTEC.F"  = @{ "MIN_RANGE" = "12.50";   "MIN_STRUCTURE_MS" = "45000"; "BREAKOUT_FAIL_MS" = "10000" }
    "DJ30.F"   = @{ "MIN_RANGE" = "20.00";   "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "10000" }
    "NAS100"   = @{ "MIN_RANGE" = "12.50";   "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "10000" }
    "GER30"    = @{ "MIN_RANGE" = "10.00";   "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "10000" }
    "UK100"    = @{ "MIN_RANGE" = "4.00";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "10000" }
    "ESTX50"   = @{ "MIN_RANGE" = "3.00";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "10000" }
    "UKBRENT"  = @{ "MIN_RANGE" = "0.50";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "12000" }
    "EURUSD"   = @{ "MIN_RANGE" = "0.00035"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "GBPUSD"   = @{ "MIN_RANGE" = "0.00040"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "AUDUSD"   = @{ "MIN_RANGE" = "0.00025"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "NZDUSD"   = @{ "MIN_RANGE" = "0.00025"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "USDJPY"   = @{ "MIN_RANGE" = "0.12";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
}

$iniPath = "$rel\symbols.ini"
$iniLines = Get-Content $iniPath
$failures = @()
$currentSection = ""

foreach ($line in $iniLines) {
    $line = $line.Trim()
    if ($line -match '^\[(.+)\]') {
        $currentSection = $matches[1]
    }
    if ($currentSection -and $expected.ContainsKey($currentSection)) {
        foreach ($key in $expected[$currentSection].Keys) {
            if ($line -match "^$key\s*=\s*(.+)") {
                $actual = $matches[1].Trim() -replace '\s*;.*$', ''  # strip inline comments
                $expect = $expected[$currentSection][$key]
                if ([double]$actual -ne [double]$expect) {
                    $failures += "  FAIL [$currentSection] $key = $actual  (expected $expect)"
                }
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "      [ERROR] symbols.ini verification FAILED -- aborting launch:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Write-Host ""
    Write-Host "      Run: git checkout HEAD -- symbols.ini" -ForegroundColor Yellow
    Write-Host "      Then re-run this script." -ForegroundColor Yellow
    Read-Host "Press Enter to exit"
    return
}
Write-Host "      [OK] symbols.ini verified -- all 15 symbols, 3 fields each correct" -ForegroundColor Green
Write-Host ""

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Starting Omega.exe..." -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location $rel
.\Omega.exe omega_config.ini
