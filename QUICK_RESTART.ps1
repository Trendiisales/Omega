#Requires -Version 5.1
# ==============================================================================
#  OMEGA - QUICK RESTART
#  BINARY LOCATION RULE (IMMUTABLE):
#    cmake builds to: C:\Omega\build\Release\Omega.exe
#    This script launches: C:\Omega\Omega.exe
#    Sync happens AFTER stop, BEFORE launch -- always runs latest build.
#
#  PRE_DELIVERY_CHECK.ps1 is called at three points and cannot be bypassed:
#    GATE 1 (pre-build)  : git reachable, reset clean, no stale objects
#    GATE 2 (post-build) : binary fresh, version_generated matches HEAD
#    GATE 3 (post-launch): log confirms running hash matches HEAD
#  If any gate fails, Omega does not start / is stopped immediately.
# ==============================================================================

param(
    [switch] $SkipVerify,
    [int]    $WaitSec    = 10,
    [string] $OmegaDir   = "C:\Omega",
    [string] $GitHubToken = ""   # Set in C:\Omega\omega_config.ini as github_token=
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Read GitHub token from C:\Omega\.github_token if not passed as parameter.
# This file is gitignored and lives only on the VPS.
if ($GitHubToken -eq "") {
    $ErrorActionPreference = "Continue"
    $tokenFile = "$OmegaDir\.github_token"
    if (Test-Path $tokenFile) { $GitHubToken = (Get-Content $tokenFile -Raw).Trim() }
    $ErrorActionPreference = "Stop"
}

$BuildExe   = "$OmegaDir\build\Release\Omega.exe"
$OmegaExe   = "$OmegaDir\Omega.exe"
$ConfigSrc  = "$OmegaDir\omega_config.ini"
$cmakeExe   = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
$buildDir   = "$OmegaDir\build"
$CheckScript = "$OmegaDir\PRE_DELIVERY_CHECK.ps1"
$PassFile   = "$OmegaDir\logs\PRE_DELIVERY_PASS.txt"

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  QUICK RESTART" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# --- Verify PRE_DELIVERY_CHECK.ps1 exists ------------------------------------
if (-not (Test-Path $CheckScript)) {
    Write-Host "  [FATAL] PRE_DELIVERY_CHECK.ps1 not found at $CheckScript" -ForegroundColor Red
    Write-Host "  Cannot proceed without the pre-delivery check script." -ForegroundColor Red
    exit 1
}

# --- Show config mode --------------------------------------------------------
$ErrorActionPreference = "Continue"
$modeMatch = Select-String -Path $ConfigSrc -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = if ($mode -eq "LIVE") { "Red" } elseif ($mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
Write-Host "  Mode    : $mode" -ForegroundColor $modeColor
Write-Host ""
$ErrorActionPreference = "Stop"

# --- [1] Stop Omega ----------------------------------------------------------
Write-Host "[1/5] Stopping Omega..." -ForegroundColor Yellow
$ErrorActionPreference = "Continue"
# Stop service first (NSSM)
$svcCheck = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svcCheck -and $svcCheck.Status -eq "Running") {
    Stop-Service "Omega" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
# Kill any remaining process
taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
Start-Sleep -Seconds 2
Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
# Poll until process is fully gone -- Windows holds file handles until kernel releases them
$waited = 0
do {
    Start-Sleep -Seconds 1
    $waited++
    $still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
} while ($still -and $waited -lt 15)
if ($still) {
    $still | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
# Extra wait for Windows to release file handles on log files
# Without this, git reset --hard fails with "unable to unlink" on locked logs
Start-Sleep -Seconds 3
$ErrorActionPreference = "Stop"
Write-Host "      [OK] Stopped" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# GATE 1 -- PRE-BUILD: git sync + stale object check
# Must pass before any build attempt. Omega does not build if this fails.
# ==============================================================================
Write-Host "[2/5] GATE 1 -- Pre-build checks..." -ForegroundColor Yellow
Write-Host ""

# Sync to origin/main:
# 1. fetch latest
# 2. git rm --cached logs/ -- clears ANY stale index entries for log/dat/csv files
#    (files stay on disk, gitignored -- this only removes them from git index)
# 3. reset --hard -- now has nothing to unlink under logs/
# 4. Force-checkout PRE_DELIVERY_CHECK.ps1
$ErrorActionPreference = "Continue"
& git -C $OmegaDir fetch origin 2>&1 | Out-Null
# Remove ALL index entries under logs/ -- eliminates "unable to unlink" and "not uptodate" errors
& git -C $OmegaDir rm -r --cached --force --ignore-unmatch logs/ 2>&1 | Out-Null
& git -C $OmegaDir reset --hard origin/main 2>&1 | Out-Null
& git -C $OmegaDir checkout origin/main -- PRE_DELIVERY_CHECK.ps1 2>&1 | Out-Null
Write-Host "  [GIT] Synced to origin/main" -ForegroundColor Cyan
$ErrorActionPreference = "Continue"

# FULL BUILD DIRECTORY WIPE -- the only guaranteed clean rebuild.
# Deleting .obj/.pch alone is not enough -- MSVC can still skip recompilation
# if it decides the PCH is valid. Wiping the entire output forces cmake to
# recompile every translation unit from scratch. No stale code possible.
$ErrorActionPreference = "Continue"
$dirsToWipe = @(
    "$buildDir\CMakeFiles",
    "$buildDir\Release",
    "$buildDir\Omega.dir",
    "$buildDir\x64"
)
foreach ($d in $dirsToWipe) {
    if (Test-Path $d) {
        Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
        Write-Host "  [CLEAN] Wiped $d" -ForegroundColor Cyan
    }
}
# Also wipe any loose .obj/.pch/.pdb at build root
Get-ChildItem -Path $buildDir -Include "*.obj","*.pch","*.pdb" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Write-Host "  [CLEAN] Full wipe complete -- guaranteed fresh compile" -ForegroundColor Green
$ErrorActionPreference = "Continue"

& $CheckScript -OmegaDir $OmegaDir -GitHubToken $GitHubToken
$gate1Exit = $LASTEXITCODE
if ($gate1Exit -ne 0) {
    Write-Host ""
    Write-Host "  [FATAL] GATE 1 FAILED -- build aborted. Fix failures above." -ForegroundColor Red
    Write-Host "  Omega was stopped and will NOT restart." -ForegroundColor Red
    Write-Host ""
    exit 1
}
Write-Host ""

# Read HEAD after gate 1 (we know local == origin/main at this point)
$ErrorActionPreference = "Continue"
$headHash = & git -C $OmegaDir rev-parse HEAD 2>$null
$headHash7 = if ($headHash -and $headHash.Length -ge 7) { $headHash.Substring(0,7) } else { "unknown" }
$ErrorActionPreference = "Continue"

# ==============================================================================
# [3/5] BUILD
# ==============================================================================
Write-Host "[3/5] Building..." -ForegroundColor Yellow
Write-Host ""

if (Test-Path $cmakeExe) {
    Write-Host "  [CMAKE] Configuring (writes version_generated.hpp with HEAD=$headHash7)..." -ForegroundColor Cyan
    $ErrorActionPreference = "Continue"
    & $cmakeExe -S $OmegaDir -B $buildDir -DCMAKE_BUILD_TYPE=Release 2>&1 `
        | Where-Object { $_ -match "\[Omega\]|error|warning" } `
        | ForEach-Object { Write-Host "    $_" }
    $ErrorActionPreference = "Continue"

    $preBuildTime = if (Test-Path $BuildExe) { (Get-Item $BuildExe).LastWriteTime } else { [DateTime]::MinValue }

    Write-Host "  [CMAKE] Building (full recompile -- 2-3 min)..." -ForegroundColor Cyan
    $buildOutput = & $cmakeExe --build $buildDir --config Release 2>&1
    $buildOutput | Where-Object { $_ -match "Omega.vcxproj|error C|warning C" } | ForEach-Object { Write-Host "    $_" }

    $buildFailed = $buildOutput | Where-Object { $_ -match "error C[0-9]+" }
    if ($buildFailed) {
        Write-Host ""
        Write-Host "  [BUILD FAILED] Compile errors -- aborting" -ForegroundColor Red
        exit 1
    }

    $postBuildTime = if (Test-Path $BuildExe) { (Get-Item $BuildExe).LastWriteTime } else { [DateTime]::MinValue }
    if ($postBuildTime -le $preBuildTime) {
        Write-Host ""
        Write-Host "  [BUILD FAILED] Binary timestamp unchanged -- cmake did not recompile" -ForegroundColor Red
        Write-Host "  pre=$preBuildTime  post=$postBuildTime" -ForegroundColor Red
        exit 1
    }
    Write-Host "  [BUILD OK] $postBuildTime" -ForegroundColor Green
    $ErrorActionPreference = "Continue"
} else {
    Write-Host "  [SKIP] cmake not found -- using existing binary" -ForegroundColor DarkGray
}

# Sync binary
if (Test-Path $BuildExe) {
    $buildTime  = (Get-Item $BuildExe).LastWriteTime
    $launchTime = if (Test-Path $OmegaExe) { (Get-Item $OmegaExe).LastWriteTime } else { [DateTime]::MinValue }
    if ($buildTime -gt $launchTime) {
        Copy-Item $BuildExe $OmegaExe -Force
        Write-Host "  [SYNC] Omega.exe updated from build\Release\Omega.exe" -ForegroundColor Green
    }
}

# ==============================================================================
# GATE 2 -- POST-BUILD: version_generated matches HEAD, binary is fresh
# ==============================================================================
Write-Host ""
Write-Host "[4/5] GATE 2 -- Post-build checks..." -ForegroundColor Yellow
Write-Host ""

& $CheckScript -OmegaDir $OmegaDir -GitHubToken $GitHubToken -ExpectedHash $headHash7 -PostBuild
$gate2Exit = $LASTEXITCODE
if ($gate2Exit -ne 0) {
    Write-Host ""
    Write-Host "  [FATAL] GATE 2 FAILED -- binary does not match HEAD. Omega will NOT start." -ForegroundColor Red
    Write-Host ""
    exit 1
}
Write-Host ""

# Read hash from version_generated.hpp -- this is what is baked into the binary
$ErrorActionPreference = "Continue"
$verHash = "unknown"
$verFile = "$OmegaDir\include\version_generated.hpp"
if (Test-Path $verFile) {
    $vl = Select-String -Path $verFile -Pattern "OMEGA_GIT_HASH" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vl -and $vl.Line -match '"([a-f0-9]+)"') { $verHash = $Matches[1] }
}
$buildTimeStr = if (Test-Path $OmegaExe) {
    (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss") + " UTC"
} else { "unknown" }

# --- Clean state files -------------------------------------------------------
$barFailed = "$OmegaDir\logs\ctrader_bar_failed.txt"
if (Test-Path $barFailed) {
    Remove-Item $barFailed -Force
    Write-Host "  [OK] Deleted ctrader_bar_failed.txt" -ForegroundColor Green
}

# --- Ensure log dirs ---------------------------------------------------------
New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# ==============================================================================
# [5/5] LAUNCH
# ==============================================================================
Write-Host "[5/5] Launching Omega..." -ForegroundColor Yellow
Set-Location $OmegaDir

Write-Host ""
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host "  RUNNING COMMIT : $verHash" -ForegroundColor Yellow
Write-Host "  BINARY TIME    : $buildTimeStr" -ForegroundColor Yellow
Write-Host "  MODE           : $mode" -ForegroundColor $modeColor
Write-Host "  GUI            : http://185.167.119.59:7779" -ForegroundColor Yellow
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host ""

$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "  [SERVICE] Starting Omega..." -ForegroundColor Cyan
    Start-Service "Omega"
    Start-Sleep -Seconds 3
    $svc = Get-Service -Name "Omega"
    $svcColor = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  [SERVICE] Status: $($svc.Status)" -ForegroundColor $svcColor
    Write-Host ""
} else {
    Write-Host "  [DIRECT] WARNING: Service not installed." -ForegroundColor Yellow
    $proc = Start-Process -FilePath $OmegaExe -ArgumentList "omega_config.ini" `
                          -WorkingDirectory $OmegaDir -PassThru -NoNewWindow
    Write-Host "  Omega PID: $($proc.Id)" -ForegroundColor DarkGray
    Write-Host ""
}

# ==============================================================================
# GATE 3 -- POST-LAUNCH: wait 60s then confirm log shows correct running hash
# ==============================================================================
Write-Host "  [GATE 3] Waiting 60s for Omega to write startup log..." -ForegroundColor Cyan
Start-Sleep -Seconds 60

Write-Host "  [GATE 3] Checking log for correct running hash..." -ForegroundColor Cyan
& $CheckScript -OmegaDir $OmegaDir -GitHubToken $GitHubToken -ExpectedHash $verHash -PostLaunch
$gate3Exit = $LASTEXITCODE

if ($gate3Exit -ne 0) {
    Write-Host ""
    Write-Host "  [FATAL] GATE 3 FAILED -- log does not confirm correct hash." -ForegroundColor Red
    Write-Host "  The wrong binary may be running. Stopping Omega now." -ForegroundColor Red
    $ErrorActionPreference = "Continue"
    $svcStop = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
    if ($svcStop) { Stop-Service "Omega" -Force -ErrorAction SilentlyContinue }
    taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
    Write-Host "  Omega stopped. Fix failures and run QUICK_RESTART.ps1 again." -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "  [ALL GATES PASSED] Hash verified end-to-end:" -ForegroundColor Green
Write-Host "  GitHub API -> local git -> version_generated.hpp -> running binary log" -ForegroundColor Green
Write-Host "  Hash: $verHash" -ForegroundColor Green
Write-Host ""

if (-not $SkipVerify) {
    Write-Host "  Running VERIFY_STARTUP..." -ForegroundColor Cyan
    Write-Host ""
    & "$OmegaDir\VERIFY_STARTUP.ps1" -WaitSec $WaitSec -OmegaDir $OmegaDir
}
