# ==============================================================================
#                        OMEGA - DEPLOY AND START
#   Run from anywhere on the VPS - handles everything.
# ==============================================================================

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  Commodities and Indices  |  Breakout System" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# [1/5] Stop any running Omega process
Write-Host "[1/5] Stopping existing Omega process..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Host "      [OK] Stopped (or was not running)" -ForegroundColor Green
Write-Host ""

# [2/5] Pull latest from GitHub
Write-Host "[2/5] Pulling latest from GitHub..." -ForegroundColor Yellow
Set-Location C:\Omega
git fetch origin

# Check if the deploy script itself will change — if so, re-exec the new version.
# PowerShell loads the entire script into memory before running any of it, so
# git reset --hard below would overwrite the file but the OLD code keeps running.
# Fix: detect if the script differs from origin/main and re-launch if so.
$scriptChanged = git diff HEAD origin/main -- DEPLOY_OMEGA.ps1
git reset --hard origin/main

# Force-overwrite symbols.ini directly from git object store.
# git checkout HEAD silently fails when git considers the file clean despite wrong encoding.
# git show always outputs the correct committed bytes regardless of working tree state.
git show HEAD:symbols.ini | Out-File -FilePath "C:\Omega\symbols.ini" -Encoding utf8 -Force

if ($scriptChanged) {
    Write-Host "      [RESTART] Deploy script updated -- re-launching new version..." -ForegroundColor Cyan
    & "C:\Omega\DEPLOY_OMEGA.ps1"
    return
}

Write-Host "      [OK] Up to date: $(git log --oneline -1)" -ForegroundColor Green
Write-Host ""

# [3/5] Build
Write-Host "[3/5] Building..." -ForegroundColor Yellow
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

# [4/5] Copy assets
Write-Host "[4/5] Copying assets..." -ForegroundColor Yellow
$rel = "C:\Omega\build\Release"
$configSource = "C:\Omega\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "C:\Omega\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found in repo" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    return
}
Copy-Item $configSource "$rel\omega_config.ini" -Force
# Write symbols.ini to Release dir directly from git object store
git show HEAD:symbols.ini | Out-File -FilePath "$rel\symbols.ini" -Encoding utf8 -Force
Copy-Item "C:\Omega\src\gui\www\omega_index.html" "$rel\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Omega\src\gui\www\chimera_logo.png" "$rel\chimera_logo.png" -Force -ErrorAction SilentlyContinue
Write-Host "      [OK] Assets copied (omega_config.ini + symbols.ini + GUI)" -ForegroundColor Green
Write-Host ""

# [5/5] Verify deployed symbols.ini matches repo -- abort if values wrong
Write-Host "[5/5] Verifying deployed symbols.ini..." -ForegroundColor Yellow

$expected = @{
    "GOLD.F"  = @{ "MIN_RANGE" = "10.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "25000"; "MAX_SPREAD" = "2.50";  "BRACKET_RR" = "18.00"  }
    "XAGUSD"  = @{ "MIN_RANGE" = "0.40";     "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "12000"; "BRACKET_RR" = "0.30"   }
    "US500.F" = @{ "MIN_RANGE" = "12.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "BRACKET_RR" = "25.00"  }
    "USTEC.F" = @{ "MIN_RANGE" = "42.00";    "MIN_STRUCTURE_MS" = "45000"; "BREAKOUT_FAIL_MS" = "10000"; "BRACKET_RR" = "90.00"  }
    "DJ30.F"  = @{ "MIN_RANGE" = "86.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "BRACKET_RR" = "180.00" }
    "NAS100"  = @{ "MIN_RANGE" = "42.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "BRACKET_RR" = "90.00"  }
    "GER30"   = @{ "MIN_RANGE" = "44.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "BRACKET_RR" = "90.00"  }
    "UK100"   = @{ "MIN_RANGE" = "20.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "BRACKET_RR" = "40.00"  }
    "ESTX50"  = @{ "MIN_RANGE" = "11.00";    "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "BRACKET_RR" = "22.00"  }
    "USOIL.F" = @{ "MIN_RANGE" = "0.50";     "MIN_STRUCTURE_MS" = "25000"; "BREAKOUT_FAIL_MS" = "12000"; "BRACKET_RR" = "1.20"   }
    "UKBRENT" = @{ "MIN_RANGE" = "0.50";     "MIN_STRUCTURE_MS" = "25000"; "BREAKOUT_FAIL_MS" = "12000"; "BRACKET_RR" = "1.20"   }
    "EURUSD"  = @{ "MIN_RANGE" = "0.00035";  "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "GBPUSD"  = @{ "MIN_RANGE" = "0.00040";  "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "AUDUSD"  = @{ "MIN_RANGE" = "0.00025";  "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "NZDUSD"  = @{ "MIN_RANGE" = "0.00025";  "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "USDJPY"  = @{ "MIN_RANGE" = "0.12";     "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
}

$iniPath = "$rel\symbols.ini"
$iniLines = Get-Content $iniPath
$failures = @()
$currentSection = ""

foreach ($line in $iniLines) {
    $line = $line.Trim()
    if ($line -match '^\[(.+)\]') {
        $currentSection = $Matches[1]
    }
    if ($currentSection -and $expected.ContainsKey($currentSection)) {
        foreach ($key in $expected[$currentSection].Keys) {
            if ($line -match "^$key\s*=\s*(.+)") {
                $actual = $Matches[1].Trim() -replace '\s*;.*$', ''
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
    Write-Host "      Fix: git show HEAD:symbols.ini | Out-File C:\Omega\symbols.ini -Encoding utf8 -Force" -ForegroundColor Yellow
    Read-Host "Press Enter to exit"
    return
}
Write-Host "      [OK] symbols.ini verified -- all 16 symbols x 3 fields correct" -ForegroundColor Green
Write-Host ""

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Starting Omega.exe..." -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location $rel
.\Omega.exe omega_config.ini
