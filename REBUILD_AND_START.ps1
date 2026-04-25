#Requires -Version 5.1
# ==============================================================================
#                   OMEGA - CLEAN REBUILD AND START
#
#  Use when: you want a guaranteed clean build from scratch, pulling the exact
#  current origin/main. Preferred over DEPLOY_OMEGA.ps1 after a long gap.
#
#  STALE BINARY PREVENTION:
#    - Deletes build\ entirely before rebuilding (no stale object files)
#    - Verifies local HEAD matches origin/main before building
#    - Copies exe to C:\Omega\Omega.exe (canonical watchdog path)
#    - Writes omega_build.stamp with git hash + exe SHA256
#    - Verifies stamp hash matches the exe that is about to be launched
#    - Watermark/mode check blocks LIVE launch with testing config
#
#  RACE PREVENTION (post-5c31e334 hardening):
#    - NSSM service Start is set to SERVICE_DISABLED for entire script lifetime
#      (previous "nssm pause" is NOT a real NSSM command; it silently no-op'd
#      and NSSM's auto-restart respawned Omega.exe under services.exe before
#      step 7's direct launch, causing the singleton-mutex collision).
#    - Original Start value is captured and restored at the end.
#    - Pre-launch mutex/process re-poll: if anything (zombie watchdog, GUI
#      reload, NSSM recovery) has re-spawned Omega.exe between step 1 and
#      step 7, kill it and wait BEFORE attempting the direct launch.
# ==============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaDir  = "C:\Omega"
$OmegaExe  = "C:\Omega\Omega.exe"
$BuildExe  = "C:\Omega\build\Release\Omega.exe"
$StampFile = "C:\Omega\omega_build.stamp"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA - CLEAN REBUILD                               " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# [1/7] Stop -- must fully release C:\Omega\Omega.exe before copy step
Write-Host "[1/7] Stopping Omega..." -ForegroundColor Yellow

# Helper: try to open the target exe exclusive-write. Returns $true if writable.
function Test-FileWritable {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $true }
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $fs.Close()
        return $true
    } catch {
        return $false
    }
}

# Helper: Forced rename-aside of a locked exe so Copy-Item can write fresh.
# Always succeeds (or exits) -- never returns with the canonical path occupied.
function Rename-LockedExe {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return }
    $backup = "$Path.old_$(Get-Date -Format yyyyMMdd_HHmmss)"
    try {
        Rename-Item $Path $backup -Force
        Write-Host "      [OK] Renamed locked exe -> $(Split-Path -Leaf $backup)" -ForegroundColor Green
    } catch {
        Write-Host "      [ERROR] Cannot rename locked exe at $Path : $_" -ForegroundColor Red
        Write-Host "      Manual: tasklist | findstr Omega ; taskkill /F /IM Omega.exe" -ForegroundColor Red
        exit 1
    }
}

# Helper: kill every Omega.exe process and wait for them to be gone.
# Returns $true if all gone within $TimeoutSec, $false otherwise.
function Wait-OmegaGone {
    param([int]$TimeoutSec = 30)
    Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
    $waited = 0
    while (Get-Process -Name "Omega" -ErrorAction SilentlyContinue) {
        if ($waited -ge $TimeoutSec) { return $false }
        Start-Sleep -Seconds 1
        $waited++
        # Re-issue kill in case a new instance spawned in the gap
        Stop-Process -Name "Omega" -Force -ErrorAction SilentlyContinue
    }
    return $true
}

# 1a. NSSM: stop service AND disable auto-restart for script lifetime.
#     "nssm pause" / "nssm continue" do NOT exist as NSSM verbs -- the only
#     reliable way to block service-manager respawn is to set Start to
#     SERVICE_DISABLED. We capture the original value and restore it at exit.
$nssm = Get-Command nssm -ErrorAction SilentlyContinue
$nssmServiceExists = $false
$origStartType = $null
if ($nssm) {
    $savedPrefNssm = $ErrorActionPreference
    $ErrorActionPreference = "Continue"

    # Probe whether the Omega service is registered
    & nssm status Omega 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $nssmServiceExists = $true

        # Capture original Start type so we can restore it later. Output is
        # one of: SERVICE_AUTO_START | SERVICE_DEMAND_START | SERVICE_DISABLED
        # | SERVICE_DELAYED_AUTO_START
        $origStartType = (& nssm get Omega Start 2>&1 | Out-String).Trim()
        if ([string]::IsNullOrWhiteSpace($origStartType)) {
            $origStartType = "SERVICE_AUTO_START"   # safe default
        }

        # Stop the service (synchronous in nssm)
        & nssm stop Omega 2>&1 | Out-Null

        # Block service-manager auto-restart for the rest of the script.
        # SERVICE_DISABLED prevents BOTH manual SCM starts AND NSSM's own
        # AppExit/Throttle recovery from re-spawning Omega.exe.
        & nssm set Omega Start SERVICE_DISABLED 2>&1 | Out-Null

        Write-Host "      [OK] NSSM Omega stopped, Start=SERVICE_DISABLED (was $origStartType)" -ForegroundColor Green
    } else {
        Write-Host "      [INFO] NSSM Omega service not registered -- skipping service stop" -ForegroundColor Gray
    }

    $ErrorActionPreference = $savedPrefNssm
}

# 1b. Watchdog may respawn Omega -- stop every form it can run as
Stop-Process -Name "OMEGA_WATCHDOG" -Force -ErrorAction SilentlyContinue
try {
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe' OR Name='pwsh.exe' OR Name='cmd.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and ($_.CommandLine -match 'OMEGA_WATCHDOG|Omega\.exe') } |
        ForEach-Object {
            # Don't kill our own process
            if ($_.ProcessId -ne $PID) {
                Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
            }
        }
} catch {
    # CIM query failed -- not fatal
}

# 1c. Kill any remaining Omega.exe processes (crashed, detached, NSSM children, etc.)
if (-not (Wait-OmegaGone -TimeoutSec 30)) {
    Write-Host "      [ERROR] Omega.exe still running after 30s -- manual intervention needed" -ForegroundColor Red
    Write-Host "      Try: taskkill /F /IM Omega.exe" -ForegroundColor Red
    exit 1
}

# 1d. Verify writability up to 30s. Some handles outlive
# the process briefly (AV scan, file indexer). Was 5s -- empirically too short.
$canWrite = $false
for ($i = 0; $i -lt 30; $i++) {
    if (Test-FileWritable -Path $OmegaExe) { $canWrite = $true; break }
    Start-Sleep -Seconds 1
}

# 1e. If still locked, rename it. Don't try to copy-overwrite a locked file.
if (-not $canWrite) {
    Write-Host "      [WARN] $OmegaExe still locked after 30s -- forcing rename" -ForegroundColor Yellow
    Rename-LockedExe -Path $OmegaExe
}

Write-Host "      [OK] Omega fully stopped (write-probe ${i}s)" -ForegroundColor Green
Write-Host ""

# [2/7] Sync to origin/main
Write-Host "[2/7] Syncing to origin/main..." -ForegroundColor Yellow
Set-Location $OmegaDir
# git commands write informational messages to stderr ("Already on 'main'", fetch progress, etc.).
# PowerShell with $ErrorActionPreference="Stop" treats ANY native stderr as a terminating error.
# Workaround: drop to Continue for git housekeeping, then restore Stop for the rest of the script.
$savedPref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
git fetch origin 2>&1 | Out-Null
git checkout main 2>&1 | Out-Null        # "Already on 'main'" is normal stderr, not an error
git reset --hard origin/main 2>&1 | Out-Null
$ErrorActionPreference = $savedPref

$localHead  = (git rev-parse HEAD).Trim()
$remoteHead = (git rev-parse origin/main).Trim()
if ($localHead -ne $remoteHead) {
    Write-Host "      [ERROR] Repo not aligned to origin/main after reset" -ForegroundColor Red
    Write-Host "      local=$localHead  remote=$remoteHead" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] HEAD $localHead  ($(git log --oneline -1))" -ForegroundColor Green
Write-Host ""

# [3/7] Clean build
Write-Host "[3/7] Clean build..." -ForegroundColor Yellow
Remove-Item -Path "$OmegaDir\build" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
Set-Location "$OmegaDir\build"
$savedPrefCmake = $ErrorActionPreference
$ErrorActionPreference = "Continue"
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null   # configure output not useful -- suppress
$ErrorActionPreference = $savedPrefCmake
cmake --build . --config Release 2>&1                  # build output stays visible for error diagnosis

if (-not (Test-Path $BuildExe)) {
    Write-Host "      [ERROR] Build failed -- $BuildExe not found" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] Build succeeded" -ForegroundColor Green
Write-Host ""

# [4/7] Copy assets -- exe to canonical path
Write-Host "[4/7] Copying assets to $OmegaDir..." -ForegroundColor Yellow

$configSource = "$OmegaDir\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "$OmegaDir\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found" -ForegroundColor Red
    exit 1
}

# Pre-copy guard: even though step 1 cleared the lock, the build phase ran
# afterwards. AV scanner / file indexer / a respawned watchdog could have
# re-acquired a handle on $OmegaExe in the meantime. Re-probe and rename if
# still locked. Never try to Copy-Item over a locked file.
if (Test-Path $OmegaExe) {
    $copyOk = $false
    for ($j = 0; $j -lt 10; $j++) {
        if (Test-FileWritable -Path $OmegaExe) { $copyOk = $true; break }
        Start-Sleep -Seconds 1
    }
    if (-not $copyOk) {
        Write-Host "      [WARN] $OmegaExe re-locked after build -- forcing rename" -ForegroundColor Yellow
        Rename-LockedExe -Path $OmegaExe
    }
}

Copy-Item $BuildExe     $OmegaExe                       -Force
Copy-Item $configSource "$OmegaDir\omega_config.ini"    -Force
git show HEAD:symbols.ini | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding utf8 -Force
Copy-Item "$OmegaDir\src\gui\www\omega_index.html" "$OmegaDir\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "$OmegaDir\src\gui\www\chimera_logo.png" "$OmegaDir\chimera_logo.png" -Force -ErrorAction SilentlyContinue

Write-Host "      [OK] Omega.exe -> $OmegaExe" -ForegroundColor Green
Write-Host "      [OK] config + symbols + GUI assets copied" -ForegroundColor Green
Write-Host ""

# [5/7] Write build stamp
Write-Host "[5/7] Writing build stamp..." -ForegroundColor Yellow
$exeHash   = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")

"GIT_HASH=$localHead"   | Out-File -FilePath $StampFile -Encoding utf8
"EXE_SHA256=$exeHash"  | Out-File -FilePath $StampFile -Encoding utf8 -Append
"BUILD_TIME=$buildTime" | Out-File -FilePath $StampFile -Encoding utf8 -Append
"EXE_PATH=$OmegaExe"   | Out-File -FilePath $StampFile -Encoding utf8 -Append

# Verify: re-read stamp and confirm hash matches the exe we just copied
$stampLines  = Get-Content $StampFile
$stampHash   = ($stampLines | Where-Object { $_ -match '^EXE_SHA256=' }) -replace '^EXE_SHA256=', ''
$currentHash = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
if ($stampHash.Trim() -ne $currentHash.Trim()) {
    Write-Host "      [ERROR] Stamp hash does not match exe -- file system error?" -ForegroundColor Red
    Write-Host "      stamp=$stampHash" -ForegroundColor Red
    Write-Host "      exe  =$currentHash" -ForegroundColor Red
    exit 1
}
Write-Host "      [OK] Stamp verified: git=$localHead  sha256=$($exeHash.Substring(0,16))..." -ForegroundColor Green
Write-Host ""

# [6/7] Pre-live config check
Write-Host "[6/7] Config check..." -ForegroundColor Yellow
$configFile = "$OmegaDir\omega_config.ini"
$wmMatch    = Select-String -Path $configFile -Pattern "session_watermark_pct\s*=\s*([0-9.]+)" -ErrorAction SilentlyContinue
$modeMatch  = Select-String -Path $configFile -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$watermark  = if ($wmMatch)   { $wmMatch.Matches[0].Groups[1].Value   } else { "NOT_FOUND" }
$mode       = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "NOT_FOUND" }

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  CONFIG CHECK" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "  git HEAD              = $localHead" -ForegroundColor Cyan
Write-Host "  exe SHA256            = $($exeHash.Substring(0,16))..." -ForegroundColor Cyan
Write-Host "  mode                  = $mode" -ForegroundColor Cyan
Write-Host "  session_watermark_pct = $watermark" -ForegroundColor Cyan

$testingActive = $false

if ($watermark -eq "NOT_FOUND" -or [double]$watermark -eq 0.0) {
    Write-Host ""
    Write-Host "  *** WARNING: session_watermark_pct=0.0 (TESTING VALUE) ***" -ForegroundColor Red
    Write-Host "  *** No drawdown protection. Set to 0.27 before LIVE.   ***" -ForegroundColor Red
    Write-Host "  *** See PRE_LIVE_CHECKLIST.md                          ***" -ForegroundColor Red
    $testingActive = $true
}

if ($mode -eq "LIVE" -and $testingActive) {
    Write-Host ""
    Write-Host "  *** FATAL: mode=LIVE with testing values -- BLOCKED ***" -ForegroundColor Red
    Remove-Item $StampFile -Force -ErrorAction SilentlyContinue
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# [7/7] Launch
Write-Host "[7/7] Starting Omega.exe from $OmegaDir..." -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  git=$localHead  |  mode=$mode" -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# 7a. PRE-LAUNCH MUTEX DEFENSE
#     Between step 1 and now, the build phase ran for ~30-90s. In that window
#     a stray Omega.exe COULD have spawned -- e.g. a watchdog we missed,
#     a leftover SCM-triggered start before we set SERVICE_DISABLED, a manual
#     double-click. If anything is holding the singleton mutex when we launch,
#     our direct .\Omega.exe call exits immediately with "ALREADY RUNNING".
#     Detect and kill any pre-existing Omega.exe RIGHT BEFORE the launch.
$stray = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($stray) {
    Write-Host "      [WARN] Stray Omega.exe detected pre-launch (PID=$($stray.Id -join ',')) -- killing" -ForegroundColor Yellow
    if (-not (Wait-OmegaGone -TimeoutSec 15)) {
        Write-Host "      [ERROR] Could not clear stray Omega.exe -- manual: taskkill /F /IM Omega.exe" -ForegroundColor Red
        exit 1
    }
    # Mutex is named-kernel object; Windows releases it within ~1s of last handle close.
    Start-Sleep -Seconds 2
    Write-Host "      [OK] Stray cleared, mutex released" -ForegroundColor Green
}

# 7b. Restore NSSM service Start type so future crashes get auto-recovered.
#     NOTE: this does NOT start the service -- our direct .\Omega.exe launch
#     below is what runs Omega in this session. NSSM's auto-restart kicks in
#     only if Omega.exe exits unexpectedly later.
if ($nssmServiceExists -and $origStartType) {
    $savedPrefNssmEnd = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & nssm set Omega Start $origStartType 2>&1 | Out-Null
    $ErrorActionPreference = $savedPrefNssmEnd
    Write-Host "      [OK] NSSM Start restored to $origStartType" -ForegroundColor Green
}

Set-Location $OmegaDir
.\Omega.exe omega_config.ini
