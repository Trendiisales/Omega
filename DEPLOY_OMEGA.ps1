#Requires -Version 5.1
# ==============================================================================
#                        OMEGA - DEPLOY AND START
#
#  CANONICAL FLOW:
#    1. Stop any running Omega process
#    2. Pull latest from GitHub (git reset --hard origin/main)
#    3. Build in build\Release\
#    4. Copy Omega.exe  -> C:\Omega\Omega.exe        (THE ONLY EXE THE WATCHDOG USES)
#    5. Copy config/assets -> C:\Omega\
#    6. Write stamp     -> C:\Omega\omega_build.stamp (git hash + exe SHA256 + build time)
#    7. Verify symbols.ini values
#    8. Pre-live config check (watermark, mode)
#    9. Run from C:\Omega (canonical working directory for all relative paths)
#
#  STALE BINARY PREVENTION:
#    - Omega.exe is ALWAYS copied to C:\Omega\Omega.exe before launch.
#      The watchdog and START_OMEGA.ps1 ONLY ever start C:\Omega\Omega.exe.
#      build\Release\Omega.exe is a build artefact, never run directly.
#    - omega_build.stamp is written after every successful build+copy.
#      It contains the git hash, exe SHA256, and build time.
#    - The watchdog reads the stamp on every restart to verify the exe
#      it is about to launch matches what was last deployed. If stamp is
#      missing or hash mismatches, watchdog aborts and alerts instead of
#      silently restarting stale code.
#    - START_OMEGA.ps1 performs the same stamp check before starting.
# ==============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaDir  = "C:\Omega"
$OmegaExe  = "C:\Omega\Omega.exe"           # CANONICAL exe -- watchdog always uses this
$BuildExe  = "C:\Omega\build\Release\Omega.exe"
$StampFile = "C:\Omega\omega_build.stamp"   # verification file read by watchdog + START_OMEGA

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  Commodities and Indices  |  Breakout System" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# ------------------------------------------------------------------------------
# [1/8] Stop any running Omega process
# ------------------------------------------------------------------------------
Write-Host "[1/8] Stopping existing Omega process..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Write-Host "      [OK] Stopped (or was not running)" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [2/8] Pull latest from GitHub -- hard reset to origin/main
# ------------------------------------------------------------------------------
Write-Host "[2/8] Pulling latest from GitHub..." -ForegroundColor Yellow
Set-Location $OmegaDir
$savedPref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
git fetch origin 2>&1 | Out-Null

# Detect if this deploy script itself changed -- if so, re-exec the new version.
$scriptChanged = (git diff HEAD origin/main -- DEPLOY_OMEGA.ps1 2>&1)
git reset --hard origin/main 2>&1 | Out-Null
$ErrorActionPreference = $savedPref

# Force-write symbols.ini directly from git object store (bypasses encoding drift)
git show HEAD:symbols.ini | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding utf8 -Force

if ($scriptChanged) {
    Write-Host "      [RESTART] Deploy script updated -- re-launching new version..." -ForegroundColor Cyan
    & "$OmegaDir\DEPLOY_OMEGA.ps1"
    return
}

$gitHead = (git rev-parse HEAD).Trim()
Write-Host "      [OK] HEAD $gitHead  ($(git log --oneline -1))" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [3/8] Build
# ------------------------------------------------------------------------------
Write-Host "[3/8] Building..." -ForegroundColor Yellow
if (Test-Path "$OmegaDir\build") {
    Remove-Item -Path "$OmegaDir\build" -Recurse -Force
}
New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
Set-Location "$OmegaDir\build"
$savedPrefCmake = $ErrorActionPreference
$ErrorActionPreference = "Continue"
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null   # configure output not useful -- suppress
$ErrorActionPreference = $savedPrefCmake
cmake --build . --config Release 2>&1                  # build output stays visible for error diagnosis

if (-not (Test-Path $BuildExe)) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  *** BUILD FAILED -- $BuildExe not found ***" -ForegroundColor Red
    Write-Host "  *** Aborting. Previous binary at $OmegaExe is unchanged. ***" -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "      [OK] Build succeeded" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [4/8] Copy assets -- exe MUST go to C:\Omega\Omega.exe (canonical path)
# ------------------------------------------------------------------------------
Write-Host "[4/8] Copying assets to $OmegaDir..." -ForegroundColor Yellow

$configSource = "$OmegaDir\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "$OmegaDir\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found" -ForegroundColor Red
    exit 1
}

# THE CRITICAL COPY: build artifact -> canonical exe path used by watchdog
Copy-Item "$BuildExe"                "$OmegaExe"                                          -Force
Copy-Item $configSource              "$OmegaDir\omega_config.ini"                          -Force
git show HEAD:symbols.ini | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding utf8 -Force
Copy-Item "$OmegaDir\src\gui\www\omega_index.html" "$OmegaDir\omega_index.html"            -Force -ErrorAction SilentlyContinue
Copy-Item "$OmegaDir\src\gui\www\chimera_logo.png" "$OmegaDir\chimera_logo.png"            -Force -ErrorAction SilentlyContinue

Write-Host "      [OK] Omega.exe copied -> $OmegaExe" -ForegroundColor Green
Write-Host "      [OK] omega_config.ini + symbols.ini + GUI assets copied" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [5/8] Write build stamp -- the anti-stale-binary record
#
#  Format (plain text, one key=value per line):
#    GIT_HASH   = git commit hash that was built
#    EXE_SHA256 = SHA256 of C:\Omega\Omega.exe just copied
#    BUILD_TIME = UTC timestamp of this deploy
#    EXE_PATH   = canonical exe path (sanity reference)
#
#  Consumed by:
#    OmegaWatchdog.ps1  -- verifies exe hash before every restart
#    START_OMEGA.ps1    -- verifies exe hash + git HEAD match before starting
# ------------------------------------------------------------------------------
Write-Host "[5/8] Writing build stamp..." -ForegroundColor Yellow
$exeHash   = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")

"GIT_HASH=$gitHead"      | Out-File -FilePath $StampFile -Encoding utf8
"EXE_SHA256=$exeHash"   | Out-File -FilePath $StampFile -Encoding utf8 -Append
"BUILD_TIME=$buildTime" | Out-File -FilePath $StampFile -Encoding utf8 -Append
"EXE_PATH=$OmegaExe"   | Out-File -FilePath $StampFile -Encoding utf8 -Append

Write-Host "      [OK] Stamp: git=$gitHead  sha256=$($exeHash.Substring(0,16))..." -ForegroundColor Green
Write-Host "      [OK] Stamp file: $StampFile" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [6/8] Verify deployed symbols.ini matches expected values
# ------------------------------------------------------------------------------
Write-Host "[6/8] Verifying symbols.ini..." -ForegroundColor Yellow

$expected = @{
    "XAUUSD"  = @{ "MIN_RANGE" = "10.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "25000"; "MAX_SPREAD" = "2.50";  "MAX_RANGE" = "18.00"  }
    "XAGUSD"  = @{ "MIN_RANGE" = "0.40";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "12000"; "MAX_RANGE" = "0.30"   }
    "US500.F" = @{ "MIN_RANGE" = "12.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "25.00"  }
    "USTEC.F" = @{ "MIN_RANGE" = "42.00";   "MIN_STRUCTURE_MS" = "45000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "90.00"  }
    "DJ30.F"  = @{ "MIN_RANGE" = "86.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "180.00" }
    "NAS100"  = @{ "MIN_RANGE" = "42.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "90.00"  }
    "GER40"   = @{ "MIN_RANGE" = "44.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "90.00"  }
    "UK100"   = @{ "MIN_RANGE" = "20.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "40.00"  }
    "ESTX50"  = @{ "MIN_RANGE" = "11.00";   "MIN_STRUCTURE_MS" = "30000"; "BREAKOUT_FAIL_MS" = "10000"; "MAX_RANGE" = "22.00"  }
    "USOIL.F" = @{ "MIN_RANGE" = "0.50";    "MIN_STRUCTURE_MS" = "25000"; "BREAKOUT_FAIL_MS" = "12000"; "MAX_RANGE" = "1.20"   }
    "UKBRENT" = @{ "MIN_RANGE" = "0.50";    "MIN_STRUCTURE_MS" = "25000"; "BREAKOUT_FAIL_MS" = "12000"; "MAX_RANGE" = "1.20"   }
    "EURUSD"  = @{ "MIN_RANGE" = "0.00035"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "GBPUSD"  = @{ "MIN_RANGE" = "0.00040"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "AUDUSD"  = @{ "MIN_RANGE" = "0.00025"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "NZDUSD"  = @{ "MIN_RANGE" = "0.00025"; "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
    "USDJPY"  = @{ "MIN_RANGE" = "0.12";    "MIN_STRUCTURE_MS" = "20000"; "BREAKOUT_FAIL_MS" = "8000"  }
}

$iniLines = Get-Content "$OmegaDir\symbols.ini"
$failures = @()
$currentSection = ""

foreach ($line in $iniLines) {
    $line = $line.Trim()
    if ($line -match '^\[(.+)\]') { $currentSection = $Matches[1] }
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
    Write-Host "      [ERROR] symbols.ini verification FAILED -- aborting:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Write-Host ""
    Write-Host "      Fix: git show HEAD:symbols.ini | Out-File C:\Omega\symbols.ini -Encoding utf8 -Force" -ForegroundColor Yellow
    Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "      [OK] symbols.ini verified (16 symbols x key fields)" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [7/8] Pre-live config check -- BLOCKS launch if mode=LIVE + testing values
# ------------------------------------------------------------------------------
Write-Host "[7/8] Config check..." -ForegroundColor Yellow
$configFile = "$OmegaDir\omega_config.ini"

$wmMatch   = Select-String -Path $configFile -Pattern "session_watermark_pct\s*=\s*([0-9.]+)" -ErrorAction SilentlyContinue
$modeMatch = Select-String -Path $configFile -Pattern "^mode\s*=\s*(\S+)"                      -ErrorAction SilentlyContinue
$watermark = if ($wmMatch)   { $wmMatch.Matches[0].Groups[1].Value   } else { "NOT_FOUND" }
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "NOT_FOUND" }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  CONFIG CHECK" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  git HEAD              = $gitHead" -ForegroundColor Cyan
Write-Host "  exe SHA256            = $($exeHash.Substring(0,16))..." -ForegroundColor Cyan
Write-Host "  mode                  = $mode" -ForegroundColor Cyan
Write-Host "  session_watermark_pct = $watermark" -ForegroundColor Cyan

$testingActive = $false

if ($watermark -eq "NOT_FOUND") {
    Write-Host ""
    Write-Host "  *** WARNING: session_watermark_pct not found in config ***" -ForegroundColor Red
    $testingActive = $true
} elseif ([double]$watermark -eq 0.0) {
    Write-Host ""
    Write-Host "  *** WARNING: session_watermark_pct=0.0 (TESTING VALUE)   ***" -ForegroundColor Red
    Write-Host "  *** No drawdown protection active                         ***" -ForegroundColor Red
    Write-Host "  *** Restore to 0.27 in config/omega_config.ini for LIVE   ***" -ForegroundColor Red
    Write-Host "  *** See PRE_LIVE_CHECKLIST.md                             ***" -ForegroundColor Red
    $testingActive = $true
}

if ($mode -eq "LIVE" -and $testingActive) {
    Write-Host ""
    Write-Host "  *** FATAL: mode=LIVE with testing values active -- BLOCKED ***" -ForegroundColor Red
    Write-Host "  *** Deploy aborted. Stamp file removed.                     ***" -ForegroundColor Red
    Write-Host "  *** Fix config, re-run DEPLOY_OMEGA.ps1.                    ***" -ForegroundColor Red
    Write-Host "=======================================================" -ForegroundColor Yellow
    # Remove stamp so watchdog cannot restart into a bad config
    Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

# Ensure log directories exist
New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# ------------------------------------------------------------------------------
# [8/8] Launch from C:\Omega -- canonical working directory
# ------------------------------------------------------------------------------
Write-Host "[8/8] Starting Omega.exe from $OmegaDir..." -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  git=$gitHead  |  mode=$mode" -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "  On crash, watchdog restarts: $OmegaExe" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location $OmegaDir
.\Omega.exe omega_config.ini
