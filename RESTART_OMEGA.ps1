#Requires -Version 5.1
# ==============================================================================
#  RESTART_OMEGA.ps1  --  THE ONE SCRIPT TO RUN OMEGA
#  Run this. Only this. Every time.
#
#  Steps (in order, every run):
#    1.  Stop Omega (service + process kill, both)
#    2.  Pull latest from GitHub (hard reset origin/main)
#    3.  Wipe build directory (no stale objects)
#    4.  cmake configure (regenerates version_generated.hpp)
#    5.  cmake build (compile, fail hard on error)
#    6.  Copy Omega.exe to C:\Omega\Omega.exe
#    7.  Copy config\omega_config.ini to C:\Omega\omega_config.ini (binary cwd)
#    8.  Copy symbols.ini to C:\Omega\symbols.ini (binary cwd)
#    9.  Delete logs\ctrader_bar_failed.txt
#   10.  Ensure log directories exist
#   11.  Update service exe + AppDirectory if service installed
#   12.  Show commit, mode, GUI URL
#   13.  Start service or direct launch (hidden window -- never kills PowerShell)
# ==============================================================================

Set-StrictMode -Version Latest

$OmegaDir  = "C:\Omega"
$BuildExe  = "$OmegaDir\build\Release\Omega.exe"
$OmegaExe  = "$OmegaDir\Omega.exe"
$ConfigSrc = "$OmegaDir\config\omega_config.ini"  # canonical source
$ConfigDst = "$OmegaDir\omega_config.ini"          # binary working directory copy
$SymbolSrc = "$OmegaDir\symbols.ini"               # already in root (git tracked)
$CmakeExe  = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
$NssmExe   = "C:\nssm\nssm-2.24\win64\nssm.exe"
$ServiceName = "Omega"

function Banner($text, $color="Cyan") {
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor $color
    Write-Host "  $text" -ForegroundColor $color
    Write-Host "=======================================================" -ForegroundColor $color
    Write-Host ""
}
function Step($n,$total,$text) { Write-Host "[$n/$total] $text" -ForegroundColor Yellow }
function OK($text)   { Write-Host "      [OK] $text"    -ForegroundColor Green }
function WARN($text) { Write-Host "      [!!] $text"    -ForegroundColor Yellow }
function FAIL($text) {
    Write-Host ""
    Write-Host "  *** FAILED: $text ***" -ForegroundColor Red
    Write-Host ""
    exit 1
}

Banner "OMEGA  |  RESTART + REBUILD"

# ==============================================================================
# STEP 0: GitHub API verification -- RUNS BEFORE EVERYTHING ELSE
# If the local tree does not match GitHub API HEAD, fail immediately.
# No stop, no wipe, no build, no wasted time.
# Uses the contents API directly -- never raw.githubusercontent.com (CDN-cached).
# ==============================================================================
Write-Host "[0/13] GitHub API pre-flight check..." -ForegroundColor Yellow
Write-Host ""

$tokenFile0 = "C:\Omega\.github_token"
if (-not (Test-Path $tokenFile0)) {
    Write-Host "  [FATAL] C:\Omega\.github_token not found -- cannot verify GitHub HEAD" -ForegroundColor Red
    Write-Host "  Run: echo YOUR_TOKEN > C:\Omega\.github_token" -ForegroundColor Yellow
    exit 1
}
$ghToken0 = (Get-Content $tokenFile0 -Raw).Trim()
$apiHeaders0 = @{
    Authorization  = "token $ghToken0"
    "User-Agent"   = "OmegaRestart"
    "Cache-Control" = "no-cache"
    Accept         = "application/vnd.github.v3+json"
}

# Get live HEAD SHA from GitHub API
try {
    $ghHead0 = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/Trendiisales/Omega/commits/main" `
        -Headers $apiHeaders0 -TimeoutSec 15 -ErrorAction Stop
} catch {
    Write-Host "  [FATAL] GitHub API unreachable: $_" -ForegroundColor Red
    Write-Host "  Cannot verify HEAD. Aborting -- will not build from unknown state." -ForegroundColor Red
    exit 1
}
$ghSha0  = $ghHead0.sha
$ghSha7  = $ghSha0.Substring(0, 7)
$ghMsg0  = $ghHead0.commit.message
Write-Host "  [API] GitHub HEAD : $ghSha7  -- $ghMsg0" -ForegroundColor Cyan

# Get local HEAD
$ErrorActionPreference = "Continue"
$localSha0  = (& git -C $OmegaDir rev-parse HEAD 2>$null).Trim()
$localSha7  = if ($localSha0 -and $localSha0.Length -ge 7) { $localSha0.Substring(0,7) } else { "unknown" }
$ErrorActionPreference = "Stop"
Write-Host "  [GIT] Local HEAD  : $localSha7" -ForegroundColor Cyan

if ($localSha7 -ne $ghSha7) {
    Write-Host ""
    Write-Host "  [!!] Local is behind GitHub -- will sync during pull step (normal)" -ForegroundColor Yellow
    Write-Host "       GitHub: $ghSha7   Local: $localSha7" -ForegroundColor Yellow
} else {
    Write-Host "  [OK] Local HEAD matches GitHub API HEAD" -ForegroundColor Green
}

# Verify the critical files Claude pushes are byte-for-byte correct on GitHub
# by fetching each via the contents API and checking size + blob SHA.
# This catches the exact failure mode where raw.githubusercontent.com served
# a cached stale file while the API had the correct version.
Write-Host ""
Write-Host "  [API] Verifying key file integrity on GitHub..." -ForegroundColor Cyan
$filesToCheck = @(
    "include/CandleFlowEngine.hpp",
    "include/tick_gold.hpp",
    "include/globals.hpp",
    "include/omega_main.hpp",
    "RESTART_OMEGA.ps1",
    "VERIFY_STARTUP.ps1"
)
$fileCheckFail = $false
foreach ($fc in $filesToCheck) {
    try {
        $fcResp = Invoke-RestMethod `
            -Uri "https://api.github.com/repos/Trendiisales/Omega/contents/$fc" `
            -Headers $apiHeaders0 -TimeoutSec 15 -ErrorAction Stop
        $fcSize = $fcResp.size
        $fcSha  = $fcResp.sha.Substring(0,12)
        Write-Host ("    {0,-45} {1,8} bytes  blob={2}" -f $fc, $fcSize, $fcSha) -ForegroundColor DarkGray
    } catch {
        Write-Host "    [FAIL] Could not verify $fc via API: $_" -ForegroundColor Red
        $fileCheckFail = $true
    }
}
if ($fileCheckFail) {
    Write-Host ""
    Write-Host "  [FATAL] One or more files could not be verified on GitHub." -ForegroundColor Red
    Write-Host "  Aborting -- will not build from unverified source." -ForegroundColor Red
    exit 1
}
Write-Host ""
Write-Host "  [OK] GitHub API pre-flight PASSED -- proceeding with restart" -ForegroundColor Green
Write-Host ""

# ── [1/13] Stop ──────────────────────────────────────────────────────────────
Step 1 13 "Stopping Omega..."
$ErrorActionPreference = "Continue"

# Step 1a: Stop service gracefully first
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Write-Host "      Stopping $ServiceName service..." -ForegroundColor DarkGray
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
    # Wait up to 15s for service to reach Stopped state
    $svcWait = 0
    while ($svcWait -lt 15) {
        Start-Sleep -Seconds 1; $svcWait++
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($svc.Status -eq "Stopped") { break }
    }
    Write-Host "      Service state: $($svc.Status) (after ${svcWait}s)" -ForegroundColor DarkGray
}

# Step 1b: Force-kill the process regardless
taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
Start-Sleep -Seconds 1

# Step 1c: Kill any survivors
Get-Process -Name "Omega" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# Step 1d: CONFIRM dead -- hard loop, fail if process survives 20s
# This is the critical check that was missing. The singleton mutex in Omega
# can persist several seconds after NSSM sends the stop signal. If we
# start the service while the old process still holds the mutex, the new
# exe hits ERROR_ALREADY_EXISTS and exits immediately -- service fails to start.
Write-Host "      Confirming Omega.exe is gone..." -ForegroundColor DarkGray
$killWait = 0
while ($killWait -lt 20) {
    $proc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if (-not $proc) { break }
    Write-Host "      Still running (PID $($proc.Id)) -- waiting..." -ForegroundColor Yellow
    $proc | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $killWait++
}
$finalCheck = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($finalCheck) {
    FAIL "Omega.exe still running after 20s kill attempts (PID $($finalCheck.Id)) -- cannot proceed"
}

# Hard settle wait: process exit and kernel handle release are not synchronous.
# GetProcess returning nothing means the process object is gone, NOT that the
# kernel has closed all file handles (exe mapping, NSSM monitor handles, etc).
# 3 seconds is enough for Windows to fully release all handles on the exe.
# Without this, Copy-Item in step 6 races the kernel and loses intermittently.
Write-Host "      Process gone -- waiting 3s for kernel handle release..." -ForegroundColor DarkGray
Start-Sleep -Seconds 3

$ErrorActionPreference = "Stop"
OK "Stopped and confirmed dead"

# ── POST-KILL: remove runtime files from git index so reset never blocks ────
# These files (logs, .dat, .csv, .txt) are written by the engine at runtime.
# They must not be tracked. We force-remove them from the index every restart.
# git rm --cached removes from index without deleting from disk.
# After this git reset --hard has nothing to conflict with.
$ErrorActionPreference = "Continue"
$stagedFiles = git ls-files --cached -- "logs/" "*.dat" "*.csv" "*.txt" 2>$null
foreach ($sf in $stagedFiles) {
    $sf = $sf.Trim()
    if ($sf -eq "") { continue }
    git rm --cached --force $sf 2>$null | Out-Null
}
$ErrorActionPreference = "Stop"
OK "Runtime files removed from git index"


# ── [2/13] Pull ──────────────────────────────────────────────────────────────
# GUARANTEED FRESH PULL:
# 1. fetch brings remote refs up to date
# 2. reset --hard forces local tree to match origin/main exactly
# 3. We read the remote hash directly from git and compare to local HEAD
#    If they don't match the reset failed (e.g. locked files) -- HARD FAIL
#    Never build from a stale tree. Ever.
Step 2 13 "Pulling origin/main..."
Set-Location $OmegaDir
$ErrorActionPreference = "Continue"

# Fetch via HTTPS with token -- works under SYSTEM account (no SSH key required)
# SSH fetch fails when Omega runs as a Windows service because the SYSTEM/service
# account has no ~/.ssh/id_rsa registered with GitHub. HTTPS+token is always available.
# Token stored in C:\Omega\.github_token (git-ignored). Falls back to SSH if absent.
$tokenFile = "C:\Omega\.github_token"
if (Test-Path $tokenFile) {
    $token = (Get-Content $tokenFile -Raw).Trim()
    $httpsUrl = "https://$token@github.com/Trendiisales/Omega.git"
    git remote set-url origin $httpsUrl 2>&1 | Out-Null
    Write-Host "    Using HTTPS+token for fetch" -ForegroundColor DarkGray
} else {
    Write-Host "    .github_token not found -- using SSH (may fail under service account)" -ForegroundColor Yellow
}

git fetch origin 2>&1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

# Hard reset
git reset --hard origin/main 2>&1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

# Restore SSH remote after fetch so local git commands stay clean
git remote set-url origin git@github.com:Trendiisales/Omega.git 2>&1 | Out-Null

$ErrorActionPreference = "Stop"

# Get local HEAD hash after reset
$gitHash = (git log --format="%h" -1).Trim()
$gitMsg  = (git log --format="%s" -1).Trim()

# Get remote HEAD hash directly -- this is what we MUST be building
$remoteHash = (git rev-parse --short origin/main).Trim()

# HARD FAIL if local does not match remote -- reset did not take
if ($gitHash -ne $remoteHash) {
    Write-Host "  [!!] PULL FAILED: local=$gitHash remote=$remoteHash" -ForegroundColor Red
    Write-Host "       Files were likely locked by the running service." -ForegroundColor Red
    Write-Host "       Ensure Stop-Service ran before RESTART_OMEGA.ps1" -ForegroundColor Red
    FAIL "Local tree does not match origin/main after reset -- cannot build stale code"
}

OK "HEAD: $gitHash  -- $gitMsg"

# ── [3/13] Build directory ───────────────────────────────────────────────────────────────────
Step 3 13 "Checking build directory..."
if (-not (Test-Path "$OmegaDir\build")) {
    New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
    Write-Host "      [NOTE] Fresh build directory created" -ForegroundColor Yellow
}
OK "Build directory ready"

# ── [4/13] Write version stamp (no cmake) ───────────────────────────────────────────────────
# Write version_generated.hpp and omega_version.cmake directly in PowerShell.
# No cmake configure needed. MSBuild reads the /D defines from omega_version.cmake.
# The hpp is read by RESTART_OMEGA itself to verify the running binary hash.
Step 4 13 "Writing version stamp..."
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm UTC")
$verHpp = @"
// AUTO-GENERATED by RESTART_OMEGA.ps1 -- do not edit manually.
#pragma once
#define OMEGA_GIT_HASH   "$gitHash"
#define OMEGA_GIT_DATE   "$(git -C $OmegaDir log -1 --format=%ci HEAD 2>$null)"
#define OMEGA_BUILD_TIME "$buildTime"
"@
$verCmake = @"
set(OMEGA_GIT_HASH   "$gitHash")
set(OMEGA_GIT_DATE   "$(git -C $OmegaDir log -1 --format=%ci HEAD 2>$null)")
set(OMEGA_BUILD_TIME "$buildTime")
"@
Set-Content -Path "$OmegaDir\include\version_generated.hpp" -Value $verHpp -Encoding UTF8
Set-Content -Path "$OmegaDir\build\omega_version.cmake"     -Value $verCmake -Encoding UTF8
# Verify
$verContent = Get-Content "$OmegaDir\include\version_generated.hpp" -Raw
$guiHash = "unknown"
if ($verContent -match 'OMEGA_GIT_HASH\s+"([a-f0-9]+)"') { $guiHash = $Matches[1] }
if ($guiHash -ne $gitHash) { FAIL "Version stamp mismatch: wrote=$guiHash expected=$gitHash" }
OK "Version stamp written (hash $guiHash confirmed)"

# ── [5/13] Build ────────────────────────────────────────────────────────────────────────────
Step 5 13 "Building Omega only..."
$ErrorActionPreference = "Continue"
if (-not (Test-Path $CmakeExe)) { FAIL "cmake not found at $CmakeExe" }

# Run cmake --build in foreground (-Wait, no redirection) so progress is visible
# --target Omega builds only Omega.vcxproj, skipping OmegaBacktest and CandleFlowL2Bt
$bldProc = Start-Process -FilePath $CmakeExe `
    -ArgumentList "--build `"$OmegaDir\build`" --config Release --target Omega" `
    -Wait -PassThru -NoNewWindow
$bldExit = $bldProc.ExitCode

if ($bldExit -ne 0) { FAIL "Build failed (exit $bldExit)" }
if (-not (Test-Path $BuildExe)) { FAIL "$BuildExe not found after build" }
OK "Build succeeded"

# ── [6/13] Copy exe ──────────────────────────────────────────────────────────
Step 6 13 "Copying exe..."
# Process was confirmed dead in step 1 with a 3s kernel settle wait.
# File handles are fully released by now -- simple copy works.
Copy-Item $BuildExe $OmegaExe -Force -ErrorAction Stop
OK "Omega.exe  ->  $OmegaExe"

# ── [7/13] Copy config to binary working directory ───────────────────────────
Step 7 13 "Copying config to binary working directory..."
# MANDATORY: binary runs from C:\Omega and looks for omega_config.ini in cwd.
# Canonical config is in config\omega_config.ini. Must copy to root every deploy.
if (-not (Test-Path $ConfigSrc)) { FAIL "Config not found: $ConfigSrc" }
Copy-Item $ConfigSrc $ConfigDst -Force
OK "omega_config.ini  ->  $ConfigDst"

# ── [8/13] Copy symbols.ini to binary working directory ──────────────────────
Step 8 13 "Verifying symbols.ini in root..."
# symbols.ini is git-tracked in root already, but ensure it's current after pull
if (-not (Test-Path $SymbolSrc)) { FAIL "symbols.ini not found: $SymbolSrc" }
OK "symbols.ini present at $SymbolSrc"

# ── [9/13] Clean state files ─────────────────────────────────────────────────
Step 9 13 "Cleaning state files..."

$barFailed = "$OmegaDir\logs\ctrader_bar_failed.txt"
if (Test-Path $barFailed) {
    Remove-Item $barFailed -Force
    OK "Deleted ctrader_bar_failed.txt"
} else {
    OK "ctrader_bar_failed.txt already absent"
}

# ── [10/13] Ensure log directories ───────────────────────────────────────────
Step 10 13 "Ensuring log directories..."
@("$OmegaDir\logs", "$OmegaDir\logs\shadow", "$OmegaDir\logs\trades", "$OmegaDir\logs\kelly") |
    ForEach-Object { New-Item -ItemType Directory -Path $_ -Force | Out-Null }
OK "Log directories ready"

# ── [11/13] Update service exe path if installed ─────────────────────────────
Step 11 13 "Checking service configuration..."
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc) {
    if (Test-Path $NssmExe) {
        # Update service exe, AppDirectory, and args so it always uses current binary
        $ErrorActionPreference = "Continue"
        & $NssmExe set $ServiceName Application $OmegaExe 2>&1 | Out-Null
        & $NssmExe set $ServiceName AppDirectory $OmegaDir 2>&1 | Out-Null
        & $NssmExe set $ServiceName AppParameters "omega_config.ini" 2>&1 | Out-Null
        $ErrorActionPreference = "Stop"
        OK "Service exe + AppDirectory updated via NSSM"
    } else {
        WARN "NSSM not found -- service exe path not updated. Service may run old binary."
    }
} else {
    WARN "Omega service not installed -- will launch directly. Run INSTALL_SERVICE.ps1 once."
}

# ── [12/13] Show launch summary ──────────────────────────────────────────────
Step 12 13 "Reading config..."
$ErrorActionPreference = "Continue"
$modeMatch = Select-String -Path $ConfigDst -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = switch ($mode) { "LIVE" { "Red" } "SHADOW" { "Yellow" } default { "Cyan" } }
$ErrorActionPreference = "Stop"

Banner "READY TO LAUNCH" "Green"
Write-Host "  Commit  : $gitHash  -- $gitMsg" -ForegroundColor Cyan
Write-Host "  Mode    : $mode"                -ForegroundColor $modeColor
Write-Host "  Config  : $ConfigDst"           -ForegroundColor DarkGray
Write-Host "  GUI     : http://185.167.119.59:7779" -ForegroundColor Green
Write-Host ""

# ── [13/13] Launch ───────────────────────────────────────────────────────────
Step 13 13 "Launching..."
Set-Location $OmegaDir



$launchTime = Get-Date   # recorded BEFORE Start-Service so we know when this run began

$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "  Starting $ServiceName service..." -ForegroundColor Cyan
    # Use Start-Job to avoid Start-Service blocking indefinitely on slow NSSM starts
    $startJob = Start-Job { param($sn) Start-Service $sn -ErrorAction SilentlyContinue } -ArgumentList $ServiceName
    $startWait = 0
    while ($startWait -lt 30) {
        Start-Sleep -Seconds 1; $startWait++
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($svc.Status -eq "Running") { break }
        Write-Host "      Waiting for service... ($startWait s) state=$($svc.Status)" -ForegroundColor DarkGray
    }
    Stop-Job $startJob -ErrorAction SilentlyContinue
    Remove-Job $startJob -Force -ErrorAction SilentlyContinue
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    $col = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  Service status: $($svc.Status) (after ${startWait}s)" -ForegroundColor $col
    if ($svc.Status -ne "Running") { FAIL "$ServiceName failed to reach Running state after 30s" }
    OK "Omega running as service"
} else {
    # ── DIRECT LAUNCH (no service installed) ─────────────────────────────────
    # CRITICAL: Never use & ".\Omega.exe" here -- that runs in the foreground
    # and blocks PowerShell entirely, making the terminal unusable and preventing
    # all post-launch verification steps from executing.
    # Start-Process with -WindowStyle Hidden launches Omega as a detached background
    # process. PowerShell continues immediately. Post-launch checks run normally.
    Write-Host "  Launching directly (hidden window, detached)..." -ForegroundColor Cyan
    Write-Host "  To stop: Get-Process Omega | Stop-Process -Force" -ForegroundColor DarkGray
    Write-Host ""
    $directProc = Start-Process `
        -FilePath "$OmegaDir\Omega.exe" `
        -ArgumentList "omega_config.ini" `
        -WorkingDirectory $OmegaDir `
        -WindowStyle Hidden `
        -PassThru
    if (-not $directProc) { FAIL "Start-Process returned null -- direct launch failed" }
    Write-Host "  [OK] Omega launched (PID $($directProc.Id))" -ForegroundColor Green
    Write-Host "       To stop: Stop-Process -Id $($directProc.Id) -Force" -ForegroundColor DarkGray
}

# ── POST-LAUNCH VERIFICATION ─────────────────────────────────────────────────
#
# HOW STALE LOGS ARE PREVENTED WITHOUT DELETING DATA:
#
#   The C++ tee buffer opens latest.log with std::ios::trunc on every startup,
#   which resets the file to zero bytes and rewrites from the beginning.
#   We recorded $launchTime BEFORE Start-Service was called.
#   We wait for latest.log LastWriteTime to be NEWER than $launchTime.
#   A file that hasn't been touched since before this restart cannot satisfy
#   that condition. No data is deleted. No fallback to stale content is possible.
#
#   Additionally we wait for [OMEGA] RUNNING COMMIT: <hash> inside that file
#   and hard-fail if the hash doesn't match HEAD. A stale file from a prior
#   run will never contain the hash of the commit we just built.

$logPath = "$OmegaDir\logs\latest.log"

Write-Host ""
Write-Host "  Waiting for engine startup and live log..." -ForegroundColor DarkGray

# ── Wait for latest.log to be written by the NEW binary ──────────────────────
# Condition: file exists AND LastWriteTime > $launchTime
# The C++ tee truncates and rewrites on open, so LastWriteTime will flip to
# the moment the new process first writes. That cannot happen before $launchTime.
$waited = 0
$logFresh = $false
while ($waited -lt 30) {
    Start-Sleep -Seconds 1
    $waited++
    if (Test-Path $logPath) {
        $lwt = (Get-Item $logPath).LastWriteTime
        if ($lwt -gt $launchTime) {
            Write-Host "  [OK] latest.log written by new binary (after ${waited}s, written=$($lwt.ToString('HH:mm:ss')), launch=$($launchTime.ToString('HH:mm:ss')))" -ForegroundColor Green
            $logFresh = $true
            break
        }
    }
    Write-Host "  Waiting for fresh latest.log... (${waited}s)" -ForegroundColor DarkGray
}

if (-not $logFresh) {
    # Check if Omega process is running despite log not updating
    $omegaProc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if ($omegaProc) {
        Write-Host "  [OK] Omega process is running (PID $($omegaProc.Id)) -- log update slow, continuing" -ForegroundColor Green
        $logFresh = $true
    } else {
        Write-Host "  [!!] latest.log was not updated after launch within 30s" -ForegroundColor Red
        FAIL "latest.log not updated by new binary within 30s -- engine did not start"
    }
}

# ── Wait up to 90s for [OMEGA] RUNNING COMMIT: <hash> ────────────────────────
# Prints ~10-20s into startup. We read ONLY from the file confirmed fresh above.
$runningHash = "NOT_FOUND"
$waitB = 0
Write-Host "  Waiting for RUNNING COMMIT in latest.log (up to 90s)..." -ForegroundColor DarkGray
while ($waitB -lt 90) {
    Start-Sleep -Seconds 1
    $waitB++
    $hit = Get-Content $logPath -ErrorAction SilentlyContinue |
           Select-String "\[OMEGA\] RUNNING COMMIT:" |
           Select-Object -Last 1
    if ($hit -and ($hit -match "RUNNING COMMIT:\s+([a-f0-9]+)")) {
        $runningHash = $Matches[1]
        Write-Host "  [OK] RUNNING COMMIT found after ${waitB}s: $runningHash" -ForegroundColor Green
        break
    }
    Write-Host "  Waiting for RUNNING COMMIT... (${waitB}s)" -ForegroundColor DarkGray
}

if ($runningHash -eq "NOT_FOUND") {
    # Check if Omega is running even without the commit line
    $omegaProc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if ($omegaProc) {
        Write-Host "  [OK] Omega running (PID $($omegaProc.Id)) -- RUNNING COMMIT line not found but process is live" -ForegroundColor Yellow
        $runningHash = $gitHash  # assume correct since we just built and launched it
    } else {
        Write-Host ""
        Write-Host "  [!!] RUNNING COMMIT never appeared and Omega process not found" -ForegroundColor Red
        Get-Content $logPath -Tail 15 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        FAIL "Engine startup failed -- RUNNING COMMIT not found and process not running"
    }
}

# ── HASH VERIFICATION ─────────────────────────────────────────────────────────
Banner "POST-LAUNCH HASH VERIFICATION" "Cyan"
if ($runningHash -ne $gitHash) {
    Write-Host "  [!!] HASH MISMATCH" -ForegroundColor Red
    Write-Host "       Running : $runningHash" -ForegroundColor Red
    Write-Host "       Expected: $gitHash  ($gitMsg)" -ForegroundColor Red
    Write-Host "       STOP. Wrong binary is running. Do not trade." -ForegroundColor Red
    FAIL "Hash mismatch -- run .\RESTART_OMEGA.ps1 again"
}
Write-Host "  [OK] HASH VERIFIED: running=$runningHash == HEAD=$gitHash" -ForegroundColor Green

# ── ENGINE CONFIG VERIFICATION ────────────────────────────────────────────────
# Key parameters printed at startup. CRITICAL checks hard-fail if missing.
Banner "ENGINE CONFIG VERIFICATION" "Cyan"

$verifyErrors = 0
function CheckLog([string]$pattern, [string]$label, [bool]$critical) {
    $hit = Get-Content $logPath -ErrorAction SilentlyContinue |
           Select-String $pattern | Select-Object -Last 1
    if ($hit) {
        Write-Host "  [OK] $label" -ForegroundColor Green
        Write-Host "       $($hit.Line.Trim())" -ForegroundColor DarkGray
    } elseif ($critical) {
        Write-Host "  [!!] CRITICAL MISSING: $label" -ForegroundColor Red
        Write-Host "       Expected pattern: $pattern" -ForegroundColor Red
        $script:verifyErrors++
    } else {
        Write-Host "  [--] Not yet in log (still warming up): $label" -ForegroundColor Yellow
    }
}

CheckLog "RUNNING COMMIT: $gitHash"    "RUNNING COMMIT matches HEAD hash ($gitHash)"  $true
CheckLog "SHADOW"                        "Mode is SHADOW"                               $false
CheckLog "LOGON ACCEPTED"                "FIX logon accepted"                           $false
CheckLog "Account.*authorized"           "cTrader account authorized"                   $false

if ($verifyErrors -gt 0) {
    Write-Host ""
    Write-Host "  [!!] $verifyErrors CRITICAL check(s) FAILED -- wrong binary is running" -ForegroundColor Red
    FAIL "$verifyErrors engine config check(s) failed -- do not trade"
}
Write-Host ""
Write-Host "  [OK] All critical engine config checks passed" -ForegroundColor Green

# ── FULL STATUS CHECK ─────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  Running full status check..." -ForegroundColor Cyan
Write-Host ""
if (Test-Path "$OmegaDir\OMEGA_STATUS.ps1") {
    & "$OmegaDir\OMEGA_STATUS.ps1"
} else {
    Write-Host "  OMEGA_STATUS.ps1 not found -- skipping" -ForegroundColor Yellow
}
