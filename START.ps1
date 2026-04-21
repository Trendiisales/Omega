#Requires -Version 5.1
#Requires -RunAsAdministrator
# ==============================================================================
#                          OMEGA - START
#
#  CANONICAL entry point: stop service, rebuild from current checkout,
#  swap Omega.exe, start service via NSSM. This is the only supported way
#  to deploy a new build.
#
#  Uses the NSSM-managed "Omega" service for start/stop. Also briefly stops
#  "OmegaWatchdog" during rebuild so it doesn't fire QUICK_RESTART.ps1
#  mid-build and fight us for the exe.
#
#  Streams compile progress line-by-line so the terminal never looks hung.
#  Full logs still go to C:\Omega\configure_log.txt and C:\Omega\build_log.txt.
#
#  STALE BINARY PREVENTION:
#    - Deletes build\ entirely before rebuilding (no stale object files)
#    - Verifies local HEAD matches origin/main before building
#    - Copies exe to C:\Omega\Omega.exe (canonical watchdog path)
#    - Writes omega_build.stamp with git hash + exe SHA256
#    - Watermark/mode check blocks LIVE launch with testing config
# ==============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$OmegaDir      = "C:\Omega"
$OmegaExe      = "C:\Omega\Omega.exe"
$BuildExe      = "C:\Omega\build\Release\Omega.exe"
$StampFile     = "C:\Omega\omega_build.stamp"
$ServiceName   = "Omega"
$WatchdogName  = "OmegaWatchdog"
$NssmExe       = "C:\nssm\nssm-2.24\win64\nssm.exe"

$StopTimeoutSec  = 30
$StartTimeoutSec = 30

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA - START                                       " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

# ------------------------------------------------------------------------------
# Helper: wait for a service to reach a target status, return $true on success.
# ------------------------------------------------------------------------------
function Wait-ForServiceStatus {
    param(
        [string]$Name,
        [string]$TargetStatus,
        [int]   $TimeoutSec
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if ($null -eq $svc) { return $false }
        if ($svc.Status -eq $TargetStatus) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# ------------------------------------------------------------------------------
# [1/8] Pause watchdog so it does not restart Omega mid-rebuild
# ------------------------------------------------------------------------------
Write-Host "[1/8] Pausing OmegaWatchdog..." -ForegroundColor Yellow
$watchdogSvc      = Get-Service -Name $WatchdogName -ErrorAction SilentlyContinue
$watchdogWasUp    = $false
if ($null -eq $watchdogSvc) {
    Write-Host "      [SKIP] $WatchdogName not installed" -ForegroundColor DarkGray
} elseif ($watchdogSvc.Status -eq 'Running') {
    $watchdogWasUp = $true
    Stop-Service -Name $WatchdogName -Force -ErrorAction SilentlyContinue
    if (Wait-ForServiceStatus -Name $WatchdogName -TargetStatus 'Stopped' -TimeoutSec 15) {
        Write-Host "      [OK] $WatchdogName stopped (will restart at end)" -ForegroundColor Green
    } else {
        Write-Host "      [WARN] $WatchdogName did not stop cleanly -- continuing anyway" -ForegroundColor Yellow
    }
} else {
    Write-Host "      [SKIP] $WatchdogName already $($watchdogSvc.Status)" -ForegroundColor DarkGray
}
Write-Host ""

# ------------------------------------------------------------------------------
# [2/8] Stop Omega service
#
#   Sequence:
#     a) Stop-Service Omega               -- normal path
#     b) nssm stop Omega (if NSSM exists) -- handles NSSM-specific states
#     c) taskkill /F /IM Omega.exe        -- last resort
#     d) verify no Omega.exe running      -- abort if still alive
# ------------------------------------------------------------------------------
Write-Host "[2/8] Stopping Omega service..." -ForegroundColor Yellow

$omegaSvc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($null -eq $omegaSvc) {
    Write-Host "      [WARN] Service '$ServiceName' not installed -- is this a first run?" -ForegroundColor Yellow
    Write-Host "             Run INSTALL_SERVICE.ps1 -InstallNssm before START.ps1" -ForegroundColor Yellow
    # Don't exit -- maybe user wants to rebuild then install. But flag it.
} else {
    if ($omegaSvc.Status -eq 'Stopped') {
        Write-Host "      [OK] Service already Stopped" -ForegroundColor Green
    } else {
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        if (Wait-ForServiceStatus -Name $ServiceName -TargetStatus 'Stopped' -TimeoutSec $StopTimeoutSec) {
            Write-Host "      [OK] Service Stopped via Stop-Service" -ForegroundColor Green
        } else {
            Write-Host "      [WARN] Stop-Service did not reach Stopped in ${StopTimeoutSec}s -- trying NSSM stop" -ForegroundColor Yellow
            if (Test-Path $NssmExe) {
                & $NssmExe stop $ServiceName 2>&1 | Out-Null
                if (Wait-ForServiceStatus -Name $ServiceName -TargetStatus 'Stopped' -TimeoutSec 15) {
                    Write-Host "      [OK] Service Stopped via nssm stop" -ForegroundColor Green
                } else {
                    Write-Host "      [WARN] nssm stop did not reach Stopped either -- falling back to taskkill" -ForegroundColor Yellow
                }
            } else {
                Write-Host "      [WARN] NSSM not found at $NssmExe -- falling back to taskkill" -ForegroundColor Yellow
            }
        }
    }
}

# Final belt-and-braces: no Omega.exe process may remain.
$remaining = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($remaining) {
    Write-Host "      [WARN] Omega.exe still alive after service stop -- taskkill /F" -ForegroundColor Yellow
    taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    $stillThere = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if ($stillThere) {
        Write-Host "      [FATAL] Cannot kill Omega.exe -- aborting rebuild" -ForegroundColor Red
        exit 1
    }
    Write-Host "      [OK] Omega.exe killed via taskkill /F" -ForegroundColor Green
} else {
    Write-Host "      [OK] No Omega.exe process remains" -ForegroundColor Green
}
Write-Host ""

# ------------------------------------------------------------------------------
# [3/8] Sync to origin/main
# ------------------------------------------------------------------------------
Write-Host "[3/8] Syncing to origin/main..." -ForegroundColor Yellow
Set-Location $OmegaDir
# git commands write informational messages to stderr ("Already on 'main'",
# fetch progress, etc.). PowerShell with $ErrorActionPreference="Stop" treats
# ANY native stderr as a terminating error. Workaround: drop to Continue for
# git housekeeping, then restore Stop for the rest of the script.
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

# ------------------------------------------------------------------------------
# [4/8] Clean build -- with streaming progress
# ------------------------------------------------------------------------------
Write-Host "[4/8] Clean build..." -ForegroundColor Yellow
Remove-Item -Path "$OmegaDir\build" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
Set-Location "$OmegaDir\build"

# --- configure (quiet, only surface meaningful lines) ---
Write-Host "      cmake configure..." -ForegroundColor DarkGray
$savedPrefCmake = $ErrorActionPreference
$ErrorActionPreference = "Continue"

# WORKAROUND MSB8071: VS 17.14's MSBuild has a regex-engine OOM bug when
# "structured output" is enabled. Symptom during cmake's compiler probe:
#   error MSB8071: Cannot parse tool output '...Compiler Version 19.44.35222 for x64'
#   ...System.OutOfMemoryException at System.Text.RegularExpressions.RegexRunner.Scan
# Disabling structured output dodges the bug entirely without affecting build output.
$env:UseStructuredOutput = "false"

# Also pass /p:UseStructuredOutput=false explicitly to any MSBuild invocation
# cmake forks, because cmake's generator doesn't always inherit the env var.
$env:_CL_ = "/Zm256"   # bump cl.exe precompiled-header heap for safety on large CRTP headers

# -A x64 is load-bearing. Default VS generator platform is Win32 (x86),
# whose 32-bit cl.exe has a ~2 GB heap ceiling and C1060s on large CRTP
# templates like BreakoutEngineBase<>. x64 cl.exe has no such ceiling.
cmake .. -A x64 -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_VS_GLOBALS=UseStructuredOutput=false" 2>&1 |
    Tee-Object -FilePath "$OmegaDir\configure_log.txt" |
    ForEach-Object {
        $line = $_
        if ($line -match '^-- (Configuring|Generating|Build files|Found)') {
            Write-Host "        $line" -ForegroundColor DarkGray
        }
        elseif ($line -match 'error|FAIL' -and $line -notmatch '^-- ') {
            Write-Host "        $line" -ForegroundColor Red
        }
    }
$ErrorActionPreference = $savedPrefCmake

# --- build (stream file-by-file so terminal never looks hung) ---
Write-Host ""
Write-Host "      cmake build (streaming; full log -> build_log.txt)" -ForegroundColor DarkGray
Write-Host ""

$script:buildStart = Get-Date
$script:fileCount  = 0

$savedPrefCmake = $ErrorActionPreference
$ErrorActionPreference = "Continue"
cmake --build . --config Release -- /p:UseStructuredOutput=false 2>&1 |
    Tee-Object -FilePath "$OmegaDir\build_log.txt" |
    ForEach-Object {
        $line = $_

        # MSBuild emits the currently-compiling source file as a bare filename
        # line ("  tick_gold.cpp"). Surface those with a counter and timer.
        if ($line -match '^\s*([A-Za-z0-9_\-\.]+\.(cpp|cc|cxx|c))\s*$') {
            $script:fileCount++
            $elapsed = ((Get-Date) - $script:buildStart).ToString("mm\:ss")
            Write-Host ("        [{0,3}] {1}  [{2}]" -f $script:fileCount, $matches[1], $elapsed) -ForegroundColor Cyan
        }
        elseif ($line -match 'Omega\.vcxproj -> ') {
            Write-Host "        [LINK] $line" -ForegroundColor Green
        }
        elseif ($line -match ': error ' -or $line -match ' error C') {
            Write-Host "        $line" -ForegroundColor Red
        }
        elseif ($line -match ': warning ' -or $line -match ' warning C') {
            Write-Host "        $line" -ForegroundColor Yellow
        }
        elseif ($line -match 'Build succeeded|Build FAILED|Warning\(s\)|Error\(s\)|Time Elapsed') {
            Write-Host "        $line" -ForegroundColor Gray
        }
    }
$ErrorActionPreference = $savedPrefCmake

if (-not (Test-Path $BuildExe)) {
    Write-Host "      [ERROR] Build failed -- $BuildExe not found" -ForegroundColor Red
    Write-Host "      Last 50 lines of build_log.txt:" -ForegroundColor Red
    Get-Content "$OmegaDir\build_log.txt" -Tail 50
    exit 1
}
Write-Host "      [OK] Build succeeded ($script:fileCount files compiled)" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [5/8] Copy assets -- exe to canonical path
#
#   Copy-Item can fail transiently if the filesystem has not fully released
#   the old exe handle after stop. Retry up to 5 times with 1s spacing.
# ------------------------------------------------------------------------------
Write-Host "[5/8] Copying assets to $OmegaDir..." -ForegroundColor Yellow

$configSource = "$OmegaDir\config\omega_config.ini"
if (-not (Test-Path $configSource)) { $configSource = "$OmegaDir\omega_config.ini" }
if (-not (Test-Path $configSource)) {
    Write-Host "      [ERROR] omega_config.ini not found" -ForegroundColor Red
    exit 1
}

$copied = $false
for ($attempt = 1; $attempt -le 5; $attempt++) {
    try {
        Copy-Item $BuildExe $OmegaExe -Force -ErrorAction Stop
        $copied = $true
        break
    } catch {
        Write-Host "      [RETRY $attempt/5] exe copy failed: $($_.Exception.Message)" -ForegroundColor Yellow
        Start-Sleep -Seconds 1
    }
}
if (-not $copied) {
    Write-Host "      [FATAL] Could not copy new Omega.exe after 5 attempts -- old binary still on disk" -ForegroundColor Red
    exit 1
}

Copy-Item $configSource "$OmegaDir\omega_config.ini"    -Force
git show HEAD:symbols.ini | Out-File -FilePath "$OmegaDir\symbols.ini" -Encoding utf8 -Force
Copy-Item "$OmegaDir\src\gui\www\omega_index.html" "$OmegaDir\omega_index.html" -Force -ErrorAction SilentlyContinue
Copy-Item "$OmegaDir\src\gui\www\chimera_logo.png" "$OmegaDir\chimera_logo.png" -Force -ErrorAction SilentlyContinue

Write-Host "      [OK] Omega.exe -> $OmegaExe" -ForegroundColor Green
Write-Host "      [OK] config + symbols + GUI assets copied" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# [6/8] Write build stamp
# ------------------------------------------------------------------------------
Write-Host "[6/8] Writing build stamp..." -ForegroundColor Yellow
$exeHash   = (Get-FileHash -Path $OmegaExe -Algorithm SHA256).Hash
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")

"GIT_HASH=$localHead"   | Out-File -FilePath $StampFile -Encoding utf8
"EXE_SHA256=$exeHash"   | Out-File -FilePath $StampFile -Encoding utf8 -Append
"BUILD_TIME=$buildTime" | Out-File -FilePath $StampFile -Encoding utf8 -Append
"EXE_PATH=$OmegaExe"    | Out-File -FilePath $StampFile -Encoding utf8 -Append

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

# ------------------------------------------------------------------------------
# [7/8] Pre-live config check
# ------------------------------------------------------------------------------
Write-Host "[7/8] Config check..." -ForegroundColor Yellow
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
    # Restart watchdog on blocked exit so system state is restored.
    if ($watchdogWasUp) {
        Start-Service -Name $WatchdogName -ErrorAction SilentlyContinue
    }
    exit 1
}
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# ------------------------------------------------------------------------------
# [8/8] Start Omega service via NSSM
#
#   Start-Service Omega is the correct path -- NSSM registers as a real
#   Windows service so SCM commands work. If the service isn't installed
#   yet, fall through with a clear instruction.
# ------------------------------------------------------------------------------
Write-Host "[8/8] Starting Omega service..." -ForegroundColor Yellow

$omegaSvc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($null -eq $omegaSvc) {
    Write-Host "      [FATAL] Service '$ServiceName' not installed" -ForegroundColor Red
    Write-Host "              Run: .\INSTALL_SERVICE.ps1 -InstallNssm" -ForegroundColor Red
    Write-Host "              Then re-run START.ps1" -ForegroundColor Red
    if ($watchdogWasUp) {
        Start-Service -Name $WatchdogName -ErrorAction SilentlyContinue
    }
    exit 1
}

Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not (Wait-ForServiceStatus -Name $ServiceName -TargetStatus 'Running' -TimeoutSec $StartTimeoutSec)) {
    Write-Host "      [FATAL] Service did not reach Running in ${StartTimeoutSec}s" -ForegroundColor Red
    Write-Host "              Check: $OmegaDir\logs\omega_service_stdout.log" -ForegroundColor Red
    Write-Host "              Check: $OmegaDir\logs\omega_service_stderr.log" -ForegroundColor Red
    if ($watchdogWasUp) {
        Start-Service -Name $WatchdogName -ErrorAction SilentlyContinue
    }
    exit 1
}

# Confirm the service actually has a process behind it (NSSM can show Running
# briefly even if the child exe crashes immediately; give it 5s then verify).
Start-Sleep -Seconds 5
$omegaProc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($null -eq $omegaProc) {
    Write-Host "      [FATAL] Service reports Running but no Omega.exe process found" -ForegroundColor Red
    Write-Host "              Binary likely crashed on startup. Check logs." -ForegroundColor Red
    if ($watchdogWasUp) {
        Start-Service -Name $WatchdogName -ErrorAction SilentlyContinue
    }
    exit 1
}
$pidList = ($omegaProc | ForEach-Object { $_.Id }) -join ", "
Write-Host "      [OK] Service Running  PID(s): $pidList" -ForegroundColor Green
Write-Host ""

# ------------------------------------------------------------------------------
# Restart watchdog if we stopped it
# ------------------------------------------------------------------------------
if ($watchdogWasUp) {
    Write-Host "      Resuming $WatchdogName..." -ForegroundColor Yellow
    Start-Service -Name $WatchdogName -ErrorAction SilentlyContinue
    if (Wait-ForServiceStatus -Name $WatchdogName -TargetStatus 'Running' -TimeoutSec 10) {
        Write-Host "      [OK] $WatchdogName resumed" -ForegroundColor Green
    } else {
        Write-Host "      [WARN] $WatchdogName did not reach Running -- start manually if needed" -ForegroundColor Yellow
    }
    Write-Host ""
}

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  git=$localHead  |  mode=$mode" -ForegroundColor Cyan
Write-Host "  GUI -> http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""
