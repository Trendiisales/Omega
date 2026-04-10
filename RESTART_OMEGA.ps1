#Requires -Version 5.1
# ==============================================================================
#  RESTART_OMEGA.ps1  --  THE ONE SCRIPT TO RUN OMEGA
#  Run this. Only this. Every time.
#
#  Steps (in order, every run):
#    1.  Stop Omega (service + process kill, both)
#    2.  Pull latest from GitHub (hard reset origin/main)
#    3.  Wipe build directory (ALWAYS -- prevents locked obj file cascade)
#    4.  cmake configure (regenerates version_generated.hpp)
#    5.  MSBuild direct (vswhere discovery, fallback cmake --build)
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

# Verify key file integrity on GitHub via contents API
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

# Step 1d: CONFIRM dead
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

# Also kill any lingering MSBuild/cl.exe processes that may be holding obj locks
Write-Host "      Killing any lingering build tool processes..." -ForegroundColor DarkGray
Get-Process -Name "MSBuild"  -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name "cl"       -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name "link"     -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name "cmake"    -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Host "      Process gone -- waiting 3s for kernel handle release..." -ForegroundColor DarkGray
Start-Sleep -Seconds 3

$ErrorActionPreference = "Stop"
OK "Stopped and confirmed dead"

# ── POST-KILL: remove runtime files from git index ────────────────────────────
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
Step 2 13 "Pulling origin/main..."
Set-Location $OmegaDir
$ErrorActionPreference = "Continue"

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
git reset --hard origin/main 2>&1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

# Restore SSH remote after fetch
git remote set-url origin git@github.com:Trendiisales/Omega.git 2>&1 | Out-Null

$ErrorActionPreference = "Stop"

$gitHash    = (git log --format="%h" -1).Trim()
$gitMsg     = (git log --format="%s" -1).Trim()
$remoteHash = (git rev-parse --short origin/main).Trim()

if ($gitHash -ne $remoteHash) {
    Write-Host "  [!!] PULL FAILED: local=$gitHash remote=$remoteHash" -ForegroundColor Red
    FAIL "Local tree does not match origin/main after reset -- cannot build stale code"
}

OK "HEAD: $gitHash  -- $gitMsg"

# ── [3/13] Wipe build directory ──────────────────────────────────────────────
# ALWAYS wipe. No exceptions. No "preserve cache" optimization.
# Locked .obj/.pch files from crashed builds are fatal -- they silently produce
# stale binaries or hang the compiler. The extra 30s cmake configure is always
# worth paying versus the alternative of a 6-hour locked-obj debugging session.
Step 3 13 "Wiping build directory (ALWAYS -- prevents locked obj cascade)..."
$ErrorActionPreference = "Continue"
if (Test-Path "$OmegaDir\build") {
    # Remove-Item on locked files will fail. Use robocopy mirror-to-empty trick
    # which handles locked handles more gracefully than rm -rf on Windows.
    $emptyDir = "$env:TEMP\omega_empty_wipe"
    New-Item -ItemType Directory -Path $emptyDir -Force | Out-Null
    # robocopy /MIR mirrors empty dir over build dir, deleting all contents
    # Exit code 1 = files copied (expected on empty->populated), not an error
    $roboExit = (Start-Process robocopy -ArgumentList "`"$emptyDir`" `"$OmegaDir\build`" /MIR /NFL /NDL /NJH /NJS" -Wait -PassThru -NoNewWindow).ExitCode
    # robocopy exit codes: 0=no files, 1=files copied, 2=extra files, 3=both, 4+=errors
    if ($roboExit -ge 8) {
        Write-Host "      robocopy wipe failed (exit $roboExit) -- falling back to Remove-Item" -ForegroundColor Yellow
        Remove-Item "$OmegaDir\build" -Recurse -Force -ErrorAction SilentlyContinue
    }
    Remove-Item "$OmegaDir\build" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $emptyDir -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path "$OmegaDir\build" -Force | Out-Null
$ErrorActionPreference = "Stop"
OK "Build directory wiped and recreated"

# ── [4/13] cmake configure ───────────────────────────────────────────────────
# Always runs -- build dir is always fresh after step 3
Step 4 13 "cmake configure..."
if (-not (Test-Path $CmakeExe)) { FAIL "cmake not found at $CmakeExe" }
$ErrorActionPreference = "Continue"
$cmakeCfgLog = "$env:TEMP\omega_cmake_cfg.txt"
$cmakeCfgProc = Start-Process -FilePath $CmakeExe `
    -ArgumentList "-S `"$OmegaDir`" -B `"$OmegaDir\build`" -DCMAKE_BUILD_TYPE=Release" `
    -WorkingDirectory $OmegaDir `
    -RedirectStandardOutput $cmakeCfgLog `
    -RedirectStandardError "$env:TEMP\omega_cmake_cfg_err.txt" `
    -Wait -PassThru -NoNewWindow
$cmakeCfgExit = $cmakeCfgProc.ExitCode
if ($cmakeCfgExit -ne 0) {
    if (Test-Path "$env:TEMP\omega_cmake_cfg_err.txt") {
        Get-Content "$env:TEMP\omega_cmake_cfg_err.txt" | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    }
    FAIL "cmake configure failed (exit $cmakeCfgExit)"
}
$verFile = "$OmegaDir\include\version_generated.hpp"
if (-not (Test-Path $verFile)) { FAIL "version_generated.hpp not created" }
$verContent = Get-Content $verFile -Raw
$guiHash = "unknown"
if ($verContent -match 'OMEGA_GIT_HASH\s+"([a-f0-9]+)"') { $guiHash = $Matches[1] }
if ($guiHash -ne $gitHash) { FAIL "version hash mismatch: hpp=$guiHash HEAD=$gitHash" }
OK "Configure done (hash $guiHash confirmed)"

# ── [5/13] MSBuild direct ────────────────────────────────────────────────────
# Discover MSBuild via vswhere -- the correct way, handles all VS install layouts
# vswhere is shipped with VS 2017+ and Build Tools, always at this fixed path
Step 5 13 "Building with MSBuild..."
$ErrorActionPreference = "Continue"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild  = $null

if (Test-Path $vswhere) {
    $vsInstallPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
    if ($vsInstallPath -and (Test-Path $vsInstallPath)) {
        $msbuild = $vsInstallPath
        Write-Host "      MSBuild found via vswhere: $msbuild" -ForegroundColor DarkGray
    }
}

# Fallback: check common fixed paths if vswhere didn't find it
if (-not $msbuild) {
    $msbuildCandidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($candidate in $msbuildCandidates) {
        if (Test-Path $candidate) {
            $msbuild = $candidate
            Write-Host "      MSBuild found at fallback path: $msbuild" -ForegroundColor DarkGray
            break
        }
    }
}

$bldLog    = "$env:TEMP\omega_bld.txt"
$bldErrLog = "$env:TEMP\omega_bld_err.txt"

if ($msbuild) {
    Write-Host "      Using MSBuild direct..." -ForegroundColor DarkGray
    # /m = parallel build, /v:m = minimal verbosity (errors + warnings + -> lines only)
    # /p:Configuration=Release /p:Platform=x64 = explicit Release x64
    # Build the solution file if it exists, else the vcxproj
    $buildTarget = if (Test-Path "$OmegaDir\build\Omega.sln") {
        "`"$OmegaDir\build\Omega.sln`""
    } else {
        "`"$OmegaDir\build\Omega.vcxproj`""
    }
    Write-Host "      Build target: $buildTarget" -ForegroundColor DarkGray
    $bldProc = Start-Process -FilePath $msbuild `
        -ArgumentList "$buildTarget /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m" `
        -RedirectStandardOutput $bldLog `
        -RedirectStandardError  $bldErrLog `
        -Wait -PassThru -NoNewWindow
    $bldExit = $bldProc.ExitCode
} else {
    # Last resort: cmake --build (may hang if obj files are locked, but we wiped them so should be safe)
    Write-Host "      MSBuild not found -- falling back to cmake --build" -ForegroundColor Yellow
    Write-Host "      (Install VS Build Tools to get MSBuild for faster, more reliable builds)" -ForegroundColor Yellow
    $bldProc = Start-Process -FilePath $CmakeExe `
        -ArgumentList "--build `"$OmegaDir\build`" --config Release --target Omega" `
        -RedirectStandardOutput $bldLog `
        -RedirectStandardError  $bldErrLog `
        -Wait -PassThru -NoNewWindow
    $bldExit = $bldProc.ExitCode
}

# Always show relevant build output lines
if (Test-Path $bldLog) {
    Get-Content $bldLog | Where-Object { $_ -match "error C|warning C|Error|->|Building|Linking" } |
        ForEach-Object { Write-Host "      $_" -ForegroundColor DarkGray }
}
if ($bldExit -ne 0) {
    Write-Host "" 
    Write-Host "  [BUILD FAILED] Full error output:" -ForegroundColor Red
    if (Test-Path $bldLog)    { Get-Content $bldLog    | ForEach-Object { Write-Host "    $_" -ForegroundColor Red } }
    if (Test-Path $bldErrLog) { Get-Content $bldErrLog | ForEach-Object { Write-Host "    $_" -ForegroundColor Red } }
    FAIL "Build failed (exit $bldExit)"
}
if (-not (Test-Path $BuildExe)) { FAIL "$BuildExe not found after build" }
OK "Build succeeded"

# ── [6/13] Copy exe ──────────────────────────────────────────────────────────
Step 6 13 "Copying exe..."
Copy-Item $BuildExe $OmegaExe -Force -ErrorAction Stop
OK "Omega.exe  ->  $OmegaExe"

# ── [7/13] Copy config to binary working directory ───────────────────────────
Step 7 13 "Copying config to binary working directory..."
if (-not (Test-Path $ConfigSrc)) { FAIL "Config not found: $ConfigSrc" }
Copy-Item $ConfigSrc $ConfigDst -Force
OK "omega_config.ini  ->  $ConfigDst"

# ── [8/13] Copy symbols.ini to binary working directory ──────────────────────
Step 8 13 "Verifying symbols.ini in root..."
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
    Start-Service $ServiceName

    $startWait = 0
    while ($startWait -lt 30) {
        Start-Sleep -Seconds 1; $startWait++
        $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($svc.Status -eq "Running") { break }
        Write-Host "      Waiting for service... ($startWait s) state=$($svc.Status)" -ForegroundColor DarkGray
    }
    $col = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  Service status: $($svc.Status) (after ${startWait}s)" -ForegroundColor $col
    if ($svc.Status -ne "Running") { FAIL "$ServiceName failed to reach Running state after 30s" }
    OK "Omega running as service"
} else {
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
$logPath = "$OmegaDir\logs\latest.log"

Write-Host ""
Write-Host "  Waiting for engine startup and live log..." -ForegroundColor DarkGray

# Wait for latest.log to be written by the NEW binary
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
    if ($waited % 5 -eq 0) { Write-Host "  Waiting for fresh latest.log... (${waited}s)" -ForegroundColor DarkGray }
}

if (-not $logFresh) {
    Write-Host "  [!!] latest.log was not updated after launch within 30s" -ForegroundColor Red
    Write-Host "       Service state: $((Get-Service $ServiceName -EA SilentlyContinue).Status)" -ForegroundColor Red
    if (Test-Path $logPath) {
        $lwt = (Get-Item $logPath).LastWriteTime
        Write-Host "       latest.log LastWriteTime: $lwt (launch was: $launchTime)" -ForegroundColor Red
        Write-Host "       This file is from a prior run -- refusing to read it" -ForegroundColor Red
    }
    FAIL "latest.log not updated by new binary within 30s -- engine did not start"
}

# Wait up to 90s for [OMEGA] RUNNING COMMIT: <hash>
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
    if ($waitB % 10 -eq 0) { Write-Host "  Still waiting for RUNNING COMMIT... (${waitB}s)" -ForegroundColor DarkGray }
}

if ($runningHash -eq "NOT_FOUND") {
    Write-Host ""
    Write-Host "  [!!] RUNNING COMMIT never appeared after 90s" -ForegroundColor Red
    Write-Host "  Last 15 lines of latest.log:" -ForegroundColor Red
    Get-Content $logPath -Tail 15 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    FAIL "Engine startup failed -- RUNNING COMMIT not found in latest.log"
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
