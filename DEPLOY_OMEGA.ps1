#Requires -Version 5.1
# ==============================================================================
#                        OMEGA - DEPLOY AND START
#
#  CANONICAL FLOW:
#    1. Stop any running Omega process
#    2. Pull latest from GitHub (git reset --hard origin/main)
#    3. Capture SOURCE_HASH = last commit touching actual source files
#       (This is immune to log-push commits which only touch logs/)
#    4. Build in build\Release\
#    5. Copy Omega.exe  -> C:\Omega\Omega.exe
#    6. Copy config/assets
#    7. Write stamp with SOURCE_HASH (not HEAD, never a log-push hash)
#    8. VALIDATE: re-read stamp, verify exe hash, verify source hash visible
#    9. Config check (watermark, mode)
#   10. Launch
#
#  ANTI-STALE-BINARY GUARANTEES:
#    - SOURCE_HASH is computed BEFORE push_log.ps1 runs (POST_BUILD step).
#      It finds the last commit that touched src/ include/ CMakeLists.txt etc.
#      Log-push commits (touching only logs/) are invisible to this query.
#    - The stamp GIT_HASH always shows what code was compiled, never a log commit.
#    - A VALIDATION step after stamping re-reads and verifies every field.
#      If validation fails, Omega does NOT start.
#    - START_OMEGA.ps1 and OmegaWatchdog.ps1 verify exe SHA256 against stamp
#      on every start/restart. Mismatch = hard block.
#    - The GUI version badge (OMEGA_GIT_HASH in version_generated.hpp) also
#      uses the source hash via cmake/UpdateGitHash.cmake.
#
#  ROOT CAUSE OF PREVIOUS 7-WEEK STALE BINARY (now fixed):
#    - push_log.ps1 ran as a CMake POST_BUILD step, pushing a log commit AFTER
#      the build. This moved HEAD forward to a commit with no code changes.
#    - DEPLOY_OMEGA.ps1 then read HEAD and stamped that log-push hash.
#    - GUI and stamp showed e.g. "2774e1e" (log push) not "c86acef" (fix commit).
#    - No one could tell what code was actually running.
#    - Fixed by: (a) computing source hash before push_log runs, (b) cmake
#      using git log -- src/ not rev-parse HEAD, (c) validation step below.
# ==============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaDir  = "C:\Omega"
$OmegaExe  = "C:\Omega\Omega.exe"
$BuildExe  = "C:\Omega\build\Release\Omega.exe"
$StampFile = "C:\Omega\omega_build.stamp"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  Commodities and Indices  |  Breakout System" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# ------------------------------------------------------------------------------
# [1/9] Stop any running Omega process
# ------------------------------------------------------------------------------
Write-Host "[1/9] Stopping existing Omega process..." -ForegroundColor Yellow
Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Write-Host "      [OK] Stopped (or was not running)" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [2/9] Pull latest from GitHub
# ------------------------------------------------------------------------------
Write-Host "[2/9] Pulling latest from GitHub..." -ForegroundColor Yellow
Set-Location $OmegaDir
$savedPref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
git fetch origin 2>&1 | Out-Null

$scriptChanged = (git diff HEAD origin/main -- DEPLOY_OMEGA.ps1 2>&1)
git reset --hard origin/main 2>&1 | Out-Null
$ErrorActionPreference = $savedPref

git show HEAD:symbols.ini | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding utf8 -Force

# Only restart if script changed AND this is not already a restarted instance
if ($scriptChanged -and -not $env:OMEGA_DEPLOY_RESTARTED) {
    Write-Host "      [RESTART] Deploy script updated -- re-launching new version..." -ForegroundColor Cyan
    $env:OMEGA_DEPLOY_RESTARTED = "1"
    & "$OmegaDir\DEPLOY_OMEGA.ps1"
    return
}
$gitHeadFull = (git rev-parse HEAD).Trim()
Write-Host "      [OK] HEAD = $gitHeadFull  ($(git log --oneline -1))" -ForegroundColor DarkGray
Write-Host ""

# ------------------------------------------------------------------------------
# [3/9] Compute SOURCE_HASH -- immune to log-push commits
#
# This is the critical fix. We find the last commit that touched REAL source
# files BEFORE the build runs (and before push_log.ps1 runs as POST_BUILD).
# Log-push commits only touch logs/latest.log -- they will never appear here.
# SOURCE_HASH is what gets stamped, shown in GUI, and verified by watchdog.
# ------------------------------------------------------------------------------
Write-Host "[3/9] Computing source hash..." -ForegroundColor Yellow

# Walk recent commits, skip any that only touch logs/
# This is more reliable than path-based git log which has PowerShell
# backtick line-continuation issues on some Windows PowerShell versions.
$sourceHash = ""
$sourceHashShort = ""
$recentCommits = (git log --oneline -20 2>$null) -split "`n" | Where-Object { $_.Trim() -ne "" }
foreach ($commitLine in $recentCommits) {
    if ($commitLine -notmatch '^([a-f0-9]+)\s+') { continue }
    $shortHash = $Matches[1]
    $fullHash  = (git rev-parse $shortHash 2>$null).Trim()
    # Get all files this commit touches
    $filesChanged = (git show --name-only --format="" $fullHash 2>$null) -split "`n" | Where-Object { $_.Trim() -ne "" }
    $nonLogFiles  = $filesChanged | Where-Object { -not $_.StartsWith("logs/") }
    if ($nonLogFiles) {
        # Found a commit that touches real files -- this is our source hash
        $sourceHash      = $fullHash
        $sourceHashShort = $shortHash
        break
    }
}
if (-not $sourceHash) {
    # Fallback: HEAD if nothing found in last 20 commits
    $sourceHash = $gitHeadFull
    $sourceHashShort = $gitHeadFull.Substring(0, 7)
    Write-Host "      [WARN] Could not find source commit in last 20 -- using HEAD" -ForegroundColor Yellow
}

if ($sourceHash -ne $gitHeadFull) {
    Write-Host "      [OK] SOURCE_HASH = $sourceHashShort ($sourceHash)" -ForegroundColor Green
    Write-Host "      [NOTE] HEAD is a log-push commit -- source hash differs (expected)" -ForegroundColor Cyan
    Write-Host "      [NOTE] Stamp will record SOURCE_HASH, not HEAD" -ForegroundColor Cyan
} else {
    Write-Host "      [OK] SOURCE_HASH = $sourceHashShort (HEAD = source, no log-push)" -ForegroundColor Green
}
Write-Host ""

# ------------------------------------------------------------------------------
# [4/9] Build
# ------------------------------------------------------------------------------
Write-Host "[4/9] Building from $sourceHashShort ..." -ForegroundColor Yellow
# INCREMENTAL BUILD: only wipe and reconfigure if CMakeCache.txt is missing
# or if CMakeLists.txt changed since last build. A full wipe adds 3-5 minutes
# every deploy -- incremental builds take ~30s when only a few files changed.
$needsReconfigure = $false
if (-not (Test-Path "$OmegaDir\build\CMakeCache.txt")) {
    $needsReconfigure = $true
    Write-Host "      [INFO] No CMake cache -- full configure required" -ForegroundColor Cyan
} else {
    # Check if CMakeLists.txt is newer than cache
    $cacheTime = (Get-Item "$OmegaDir\build\CMakeCache.txt").LastWriteTimeUtc
    $cmakeTime = (Get-Item "$OmegaDir\CMakeLists.txt").LastWriteTimeUtc
    if ($cmakeTime -gt $cacheTime) {
        $needsReconfigure = $true
        Write-Host "      [INFO] CMakeLists.txt changed -- reconfiguring" -ForegroundColor Cyan
    }
}

New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
Set-Location "$OmegaDir\build"
$savedPrefCmake = $ErrorActionPreference
$ErrorActionPreference = "Continue"
if ($needsReconfigure) {
    cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
} else {
    Write-Host "      [INFO] Using existing CMake cache -- incremental build" -ForegroundColor Cyan
}
$ErrorActionPreference = $savedPrefCmake
cmake --build . --config Release 2>&1
$buildExitCode = $LASTEXITCODE

if ($buildExitCode -ne 0) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  *** BUILD FAILED (exit code $buildExitCode) ***" -ForegroundColor Red
    Write-Host "  *** Compile errors above. Aborting deploy. ***" -ForegroundColor Red
    Write-Host "  *** Previous binary at $OmegaExe is unchanged. ***" -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Set-Location $OmegaDir
    exit 1
}

if (-not (Test-Path $BuildExe)) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  *** BUILD FAILED -- $BuildExe not found ***" -ForegroundColor Red
    Write-Host "  *** Aborting. Previous binary at $OmegaExe is unchanged. ***" -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Set-Location $OmegaDir
    exit 1
}
Write-Host "      [OK] Build succeeded" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [5/9] Copy assets
# ------------------------------------------------------------------------------
Write-Host "[5/9] Copying assets to $OmegaDir..." -ForegroundColor Yellow

$configSource = "$OmegaDir\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "$OmegaDir\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found" -ForegroundColor Red
    exit 1
}

Copy-Item "$BuildExe"    "$OmegaExe"                              -Force
Copy-Item $configSource  "$OmegaDir\omega_config.ini"             -Force
git show HEAD:symbols.ini | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding utf8 -Force
Copy-Item "$OmegaDir\src\gui\www\omega_index.html" "$OmegaDir\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "$OmegaDir\src\gui\www\chimera_logo.png" "$OmegaDir\chimera_logo.png" -Force -ErrorAction SilentlyContinue

Write-Host "      [OK] Omega.exe, config, symbols, GUI assets copied" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [6/9] Write build stamp using SOURCE_HASH (not HEAD)
# ------------------------------------------------------------------------------
Write-Host "[6/9] Writing build stamp (source=$sourceHashShort)..." -ForegroundColor Yellow
$exeHash   = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")

"GIT_HASH=$sourceHash"  | Out-File -FilePath $StampFile -Encoding utf8
"GIT_HASH_SHORT=$sourceHashShort" | Out-File -FilePath $StampFile -Encoding utf8 -Append
"HEAD_HASH=$gitHeadFull"| Out-File -FilePath $StampFile -Encoding utf8 -Append
"EXE_SHA256=$exeHash"   | Out-File -FilePath $StampFile -Encoding utf8 -Append
"BUILD_TIME=$buildTime" | Out-File -FilePath $StampFile -Encoding utf8 -Append
"EXE_PATH=$OmegaExe"   | Out-File -FilePath $StampFile -Encoding utf8 -Append

Write-Host "      [OK] Stamp written: source=$sourceHashShort  exe_sha=$($exeHash.Substring(0,16))..." -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [7/9] VALIDATION -- re-read and verify every field
# This step ensures the stamp is self-consistent before Omega is allowed to start.
# If anything is wrong here, it is caught NOW, not discovered 7 weeks later.
# ------------------------------------------------------------------------------
Write-Host "[7/9] Validating stamp..." -ForegroundColor Yellow
$errors = @()

if (-not (Test-Path $StampFile)) {
    $errors += "Stamp file not found at $StampFile"
} else {
    $stampLines    = Get-Content $StampFile
    $vGitHash      = (($stampLines | Where-Object { $_ -match '^GIT_HASH=' })       -replace '^GIT_HASH=',       '').Trim()
    $vGitShort     = (($stampLines | Where-Object { $_ -match '^GIT_HASH_SHORT=' })  -replace '^GIT_HASH_SHORT=', '').Trim()
    $vHeadHash     = (($stampLines | Where-Object { $_ -match '^HEAD_HASH=' })       -replace '^HEAD_HASH=',      '').Trim()
    $vExeHash      = (($stampLines | Where-Object { $_ -match '^EXE_SHA256=' })      -replace '^EXE_SHA256=',     '').Trim()
    $vBuildTime    = (($stampLines | Where-Object { $_ -match '^BUILD_TIME=' })      -replace '^BUILD_TIME=',     '').Trim()
    $vExePath      = (($stampLines | Where-Object { $_ -match '^EXE_PATH=' })        -replace '^EXE_PATH=',       '').Trim()

    # Check 1: exe SHA256 matches what we just wrote
    $reHashExe = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
    if ($vExeHash -ne $reHashExe) {
        $errors += "EXE_SHA256 MISMATCH: stamp=$($vExeHash.Substring(0,16)) actual=$($reHashExe.Substring(0,16))"
    }

    # Check 2: source hash in stamp matches what we computed
    if ($vGitHash -ne $sourceHash) {
        $errors += "GIT_HASH MISMATCH: stamp=$($vGitHash.Substring(0,7)) expected=$sourceHashShort"
    }

    # Check 3: source hash must NOT be a log-only commit.
    # A log-push commit touches ONLY logs/latest.log -- nothing else.
    # Detection: get ALL files changed in the commit, check if any are outside logs/.
    # This is simpler and more reliable than checking specific paths with backtick
    # line continuation (which is fragile in PowerShell).
    $allFilesInCommit = (git show --name-only --format="" $vGitHash 2>$null) -split "`n" | Where-Object { $_.Trim() -ne "" }
    $nonLogFiles = $allFilesInCommit | Where-Object { -not $_.StartsWith("logs/") }
    if (-not $nonLogFiles) {
        $errors += "GIT_HASH $($vGitHash.Substring(0,7)) only touches logs/ -- this is a log-push commit, not a code commit. Deploy pipeline error."
    }

    # Check 4: all required fields present
    if (-not $vGitHash)   { $errors += "GIT_HASH field missing from stamp" }
    if (-not $vExeHash)   { $errors += "EXE_SHA256 field missing from stamp" }
    if (-not $vBuildTime) { $errors += "BUILD_TIME field missing from stamp" }
    if (-not $vExePath)   { $errors += "EXE_PATH field missing from stamp" }

    # Check 5: GUI version_generated.hpp contains a real source hash
    $versionFile = "$OmegaDir\include\version_generated.hpp"
    if (Test-Path $versionFile) {
        $versionContent = Get-Content $versionFile -Raw
        if ($versionContent -match 'OMEGA_GIT_HASH\s+"([a-f0-9]+)"') {
            $guiHash = $Matches[1]
            # GUI hash should match source hash short form (7 chars)
            if ($guiHash -ne $vGitShort -and $guiHash.Length -le 8) {
                Write-Host "      [WARN] GUI hash ($guiHash) differs from stamp source ($vGitShort)" -ForegroundColor Yellow
                Write-Host "             This may be stale if cmake cached the old version header." -ForegroundColor Yellow
            }
        }
    }

    if ($errors.Count -eq 0) {
        Write-Host "      [OK] Stamp validated:" -ForegroundColor Green
        Write-Host "           source_hash = $vGitHash" -ForegroundColor Green
        Write-Host "           head_hash   = $vHeadHash" -ForegroundColor Green
        Write-Host "           exe_sha256  = $($vExeHash.Substring(0,16))..." -ForegroundColor Green
        Write-Host "           built       = $vBuildTime" -ForegroundColor Green
        Write-Host "           exe_path    = $vExePath" -ForegroundColor Green
    }
}

if ($errors.Count -gt 0) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  *** VALIDATION FAILED -- Omega will NOT start ***" -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    $errors | ForEach-Object { Write-Host "  ERROR: $_" -ForegroundColor Red }
    Write-Host "" -ForegroundColor Red
    Write-Host "  Stamp file removed. Run DEPLOY_OMEGA.ps1 again after fixing the issue." -ForegroundColor Yellow
    Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host ""

# ------------------------------------------------------------------------------
# [8/9] Verify symbols.ini
# ------------------------------------------------------------------------------
Write-Host "[8/9] Verifying symbols.ini..." -ForegroundColor Yellow

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
    Write-Host "      [ERROR] symbols.ini verification FAILED:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "      [OK] symbols.ini verified" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [9/9] Pre-live config check then launch
# ------------------------------------------------------------------------------
Write-Host "[9/9] Config check and launch..." -ForegroundColor Yellow
$configFile = "$OmegaDir\omega_config.ini"

$wmMatch   = Select-String -Path $configFile -Pattern "session_watermark_pct\s*=\s*([0-9.]+)" -ErrorAction SilentlyContinue
$modeMatch = Select-String -Path $configFile -Pattern "^mode\s*=\s*(\S+)"                      -ErrorAction SilentlyContinue
$watermark = if ($wmMatch)   { $wmMatch.Matches[0].Groups[1].Value   } else { "NOT_FOUND" }
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "NOT_FOUND" }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  DEPLOY COMPLETE -- FINAL STATUS" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  source commit  = $sourceHash" -ForegroundColor Green
Write-Host "  head commit    = $gitHeadFull" -ForegroundColor DarkGray
Write-Host "  exe SHA256     = $($exeHash.Substring(0,16))...  [VALIDATED]" -ForegroundColor Green
Write-Host "  mode           = $mode" -ForegroundColor Cyan
Write-Host "  watermark_pct  = $watermark" -ForegroundColor Cyan
Write-Host "  built          = $buildTime" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

$testingActive = $false

if ($watermark -eq "NOT_FOUND" -or [double]$watermark -eq 0.0) {
    Write-Host "  *** WARNING: session_watermark_pct=0.0 (TESTING VALUE) ***" -ForegroundColor Red
    Write-Host "  *** No drawdown protection active                       ***" -ForegroundColor Red
    $testingActive = $true
}

if ($mode -eq "LIVE" -and $testingActive) {
    Write-Host ""
    Write-Host "  *** FATAL: mode=LIVE with testing values -- BLOCKED ***" -ForegroundColor Red
    Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
    Read-Host "Press Enter to exit"
    exit 1
}

New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# Push log to git AFTER stamp is validated -- correct order ensures HEAD moves
# AFTER the source hash is captured, so the next deploy sees the right commit.
# The log push commit (logs/latest.log only) will be HEAD, but our source hash
# walk skips it and finds the real source commit. This is the correct sequence:
#   1. Build (source hash captured at step [3/9])
#   2. Stamp written + validated (step [6/9] + [7/9])
#   3. Log pushed here (HEAD moves to log-push commit -- harmless now)
#   4. Omega starts
Write-Host "Pushing log to git..." -ForegroundColor DarkGray
$savedPrefLog = $ErrorActionPreference
$ErrorActionPreference = "Continue"
powershell -NonInteractive -NoProfile -ExecutionPolicy Bypass `
    -File "$OmegaDir\push_log.ps1" -RepoRoot "$OmegaDir" 2>&1 | Out-Null
$ErrorActionPreference = $savedPrefLog
Write-Host "      [OK] Log pushed (stamp already written -- hash is safe)" -ForegroundColor DarkGray
Write-Host ""

Write-Host "Starting Omega.exe  [source=$sourceHashShort  mode=$mode]..." -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host ""

Set-Location $OmegaDir

# Kill any existing Omega process -- use taskkill /T to kill entire process tree
Write-Host "  Killing any existing Omega processes..." -ForegroundColor Yellow
taskkill /F /IM Omega.exe /T 2>$null | Out-Null
Start-Sleep -Seconds 1
# Double-check with Stop-Process as fallback
Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
# Confirm dead
$still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($still) {
    Write-Host "  WARNING: Omega still running (PID=$($still.Id)) -- trying again..." -ForegroundColor Red
    $still | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
Write-Host "  [OK] Omega not running." -ForegroundColor Green

$statusFile = "$OmegaDir\logs\startup_status.txt"

# Clear any previous status
if (Test-Path $statusFile) { Remove-Item $statusFile -Force }

# Launch Omega in background so we can monitor startup
$proc = Start-Process -FilePath ".\Omega.exe" -ArgumentList "omega_config.ini" -PassThru -NoNewWindow

# Wait up to 150s for startup_status.txt to appear and contain OK or FAIL
Write-Host "Waiting for startup verification (up to 150s)..." -ForegroundColor Yellow
$deadline = (Get-Date).AddSeconds(150)
$startupOk = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if (Test-Path $statusFile) {
        $status = Get-Content $statusFile -Raw
        if ($status -match "^OK:") {
            Write-Host "" 
            Write-Host "  *** STARTUP OK ***" -ForegroundColor Green
            Write-Host "  $status" -ForegroundColor Green
            $startupOk = $true
            break
        } elseif ($status -match "^FAIL:") {
            Write-Host ""
            Write-Host "  *** STARTUP FAILED ***" -ForegroundColor Red
            Write-Host "  $status" -ForegroundColor Red
            Write-Host "  Killing Omega process..." -ForegroundColor Red
            if (!$proc.HasExited) { $proc.Kill() }
            Write-Host ""
            Write-Host "  Fix the issue above and redeploy." -ForegroundColor Yellow
            exit 1
        }
        # STARTING = still initialising, keep waiting
    }
}

if (-not $startupOk) {
    Write-Host ""
    Write-Host "  *** STARTUP TIMEOUT -- no status after 150s ***" -ForegroundColor Red
    Write-Host "  Check logs/latest.log for errors" -ForegroundColor Red
    if (!$proc.HasExited) { $proc.Kill() }
    exit 1
}

Write-Host "  Omega is running. PID=$($proc.Id)" -ForegroundColor Green
