#Requires -Version 5.1
param([switch]$SkipVerify,[string]$OmegaDir="C:\Omega",[string]$GitHubToken="",[int]$GracefulTimeoutSec=20,[switch]$ForceKill)

Set-StrictMode -Off
$ErrorActionPreference = "Continue"

if ($GitHubToken -eq "") {
    $tf = "$OmegaDir\.github_token"
    if (Test-Path $tf) { $GitHubToken = (Get-Content $tf -Raw).Trim() }
}

$OmegaExe  = "$OmegaDir\Omega.exe"
$BuildExe  = "$OmegaDir\build\Release\Omega.exe"
$buildDir  = "$OmegaDir\build"
$cmakeExe  = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
$ConfigSrc = "$OmegaDir\omega_config.ini"
$startTime = Get-Date

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  QUICK RESTART" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

$modeMatch = Select-String -Path $ConfigSrc -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue

# ==============================================================================
# PRE-CHECK: Warn if CandleFlow position is open
# A restart force-closes any open CFE position at current market price.
# Three FC losses today ($59 + $64 + $57 = $180) were all caused by restarting
# while a CFE position was open. This check prevents that.
# ==============================================================================
$logFile = "$OmegaDir\logs\omega_service_stdout.log"
if (Test-Path $logFile) {
    $tail = Get-Content $logFile -Tail 1000
    $lastEntry = ($tail | Select-String "\[CFE\] ENTRY") | Select-Object -Last 1
    $lastExit  = ($tail | Select-String "\[CFE\] EXIT")  | Select-Object -Last 1
    $cfeOpen = $false
    if ($lastEntry) {
        if (-not $lastExit) {
            $cfeOpen = $true
        } elseif ($lastEntry.LineNumber -gt $lastExit.LineNumber) {
            $cfeOpen = $true
        }
    }
    if ($cfeOpen) {
        Write-Host ""
        Write-Host "╔══════════════════════════════════════════════════════════╗" -ForegroundColor Red
        Write-Host "║  WARNING: CandleFlow position is OPEN                    ║" -ForegroundColor Red
        Write-Host "║  Restarting now will FORCE CLOSE at current market price ║" -ForegroundColor Red
        Write-Host "╚══════════════════════════════════════════════════════════╝" -ForegroundColor Red
        Write-Host ""
        Write-Host "  Last entry:" -ForegroundColor Yellow
        Write-Host "  $($lastEntry.Line)" -ForegroundColor Yellow
        Write-Host ""
        Write-Host "  Type YES to force-restart anyway (will take the loss): " -ForegroundColor Red -NoNewline
        $confirm = Read-Host
        if ($confirm -ne "YES") {
            Write-Host ""
            Write-Host "  Restart cancelled. Wait for position to close then retry." -ForegroundColor Green
            Write-Host ""
            exit 0
        }
        Write-Host "  Proceeding with restart..." -ForegroundColor Red
        Write-Host ""
    } else {
        Write-Host "  [OK] No open CandleFlow position detected" -ForegroundColor Green
    }
}
$mode = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = if ($mode -eq "LIVE") { "Red" } elseif ($mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
Write-Host "  Mode: $mode" -ForegroundColor $modeColor
Write-Host ""

# ==============================================================================
# STEP 1: STOP  --  GRACEFUL SHUTDOWN, THEN FORCE KILL ONLY IF NEEDED
# ==============================================================================
# Omega's own shutdown path (include/quote_loop.hpp:802-892 + trade_lifecycle.hpp:422)
# is wired to flatten every open position, save all state, and send a clean FIX
# Logout when either:
#   * SIGINT/SIGTERM is delivered (std::signal handlers in omega_main.hpp:33-34)
#   * The process receives CTRL_CLOSE_EVENT / CTRL_BREAK_EVENT / CTRL_C_EVENT
#     (SetConsoleCtrlHandler in omega_main.hpp:35)
#
# On a Windows console app, `taskkill` WITHOUT /F sends CTRL_BREAK_EVENT which
# triggers console_ctrl_handler() -> g_running=false -> main loop exits ->
# shutdown_cb runs forceClose() across every engine. Typical completion time
# is 2-4 seconds. The handler itself waits up to 4s for g_shutdown_done so
# we give it a 20s ceiling ($GracefulTimeoutSec) before escalating.
#
# `taskkill /F` bypasses ALL of this: positions stay open at the broker until
# the new binary reconnects and each engine's recovery logic catches up. Use
# /F ONLY when graceful shutdown has already failed, or the operator passed
# -ForceKill explicitly (e.g. process is truly hung).
# ==============================================================================
Write-Host "[1/4] Stopping Omega..." -ForegroundColor Yellow

$initialProcs = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if (-not $initialProcs) {
    Write-Host "  [OK] No Omega.exe running -- nothing to stop" -ForegroundColor Green
} else {
    $initialPids = ($initialProcs | ForEach-Object { $_.Id }) -join ","
    Write-Host "  Found Omega.exe PID(s): $initialPids" -ForegroundColor Cyan

    if ($ForceKill) {
        Write-Host "  -ForceKill specified -- skipping graceful shutdown" -ForegroundColor Yellow
        taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
    } else {
        # Send CTRL_BREAK to every Omega.exe PID. Without /F taskkill delivers
        # WM_CLOSE to GUI apps; for console apps like Omega it delivers
        # CTRL_BREAK_EVENT which hits console_ctrl_handler().
        Write-Host "  Sending graceful shutdown signal (taskkill without /F)..." -ForegroundColor Cyan
        foreach ($p in $initialProcs) {
            taskkill /PID $p.Id /T 2>&1 | Out-Null
        }

        # Poll for clean exit. Show a heartbeat every 3s so the operator can
        # see Omega's own shutdown logs scrolling past if they want.
        $gracefulStart  = Get-Date
        $exitedCleanly  = $false
        while (((Get-Date) - $gracefulStart).TotalSeconds -lt $GracefulTimeoutSec) {
            $still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
            if (-not $still) { $exitedCleanly = $true; break }
            $elapsedSec = [math]::Floor(((Get-Date) - $gracefulStart).TotalSeconds)
            Write-Host ("  Waiting for graceful exit... {0}s / {1}s (PIDs still alive: {2})" -f $elapsedSec, $GracefulTimeoutSec, (($still | ForEach-Object { $_.Id }) -join ",")) -ForegroundColor DarkGray
            Start-Sleep -Seconds 3
        }

        if ($exitedCleanly) {
            $elapsedGraceful = [math]::Round(((Get-Date) - $gracefulStart).TotalSeconds, 1)
            Write-Host "  [OK] Omega shut down cleanly in ${elapsedGraceful}s -- positions flattened by shutdown_cb" -ForegroundColor Green
        } else {
            Write-Host "  [WARN] Graceful shutdown timed out after ${GracefulTimeoutSec}s. Falling back to taskkill /F." -ForegroundColor Yellow
            Write-Host "  Check logs/omega_service_stdout.log for shutdown progress BEFORE next restart." -ForegroundColor Yellow
        }
    }

    # Hard-kill loop -- runs only if graceful was skipped or timed out, or to
    # mop up stragglers (e.g. child processes with /T).
    for ($i = 0; $i -lt 10; $i++) {
        $still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
        if (-not $still) { break }
        taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
        Start-Sleep -Seconds 2
        if ($i -eq 9) {
            $stillAgain = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
            if ($stillAgain) {
                Write-Host "  [FATAL] Cannot kill Omega.exe after 20s of /F attempts -- PIDs still running:" -ForegroundColor Red
                $stillAgain | ForEach-Object { Write-Host "    PID $($_.Id)" -ForegroundColor Red }
                exit 1
            }
        }
    }
}

# Hard confirmation: process must be gone before we proceed to build+launch
$confirm = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($confirm) {
    Write-Host "  [FATAL] Omega.exe still running after all kill attempts. Aborting." -ForegroundColor Red
    exit 1
}
Start-Sleep -Seconds 2
Write-Host "  [OK] Stopped -- confirmed no Omega.exe process" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# STEP 2: DOWNLOAD SOURCE FROM GITHUB API
# ==============================================================================
Write-Host "[2/4] Downloading source from GitHub..." -ForegroundColor Yellow

$apiHdr = @{ Authorization="token $GitHubToken"; "User-Agent"="OmegaRestart" }

$commitResp = Invoke-RestMethod -Uri "https://api.github.com/repos/Trendiisales/Omega/commits/main" -Headers $apiHdr -TimeoutSec 20
$ghSha  = $commitResp.sha
$ghSha7 = $ghSha.Substring(0,7)
Write-Host "  HEAD: $ghSha7" -ForegroundColor Cyan

$zipPath = "$env:TEMP\Omega_$ghSha7.zip"
Invoke-WebRequest -Uri "https://api.github.com/repos/Trendiisales/Omega/zipball/$ghSha" -Headers $apiHdr -OutFile $zipPath -TimeoutSec 120
Write-Host "  Downloaded: $([math]::Round((Get-Item $zipPath).Length/1MB,1)) MB" -ForegroundColor Cyan

$extractPath = "$env:TEMP\Omega_ext_$ghSha7"
if (Test-Path $extractPath) { Remove-Item $extractPath -Recurse -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $extractPath)
$innerDir = Get-ChildItem $extractPath -Directory | Select-Object -First 1

foreach ($ext in @("*.cpp","*.hpp","*.h","*.ini","*.cmake","*.txt","*.json","*.md")) {
    Get-ChildItem -Path $innerDir.FullName -Filter $ext -Recurse | ForEach-Object {
        $rel = $_.FullName.Substring($innerDir.FullName.Length).TrimStart('\','/')
        # Guard: skip paths that look absolute after stripping the zip prefix.
        # GitHub zip entries can produce rel paths starting with a drive letter,
        # causing Join-Path C:\Omega + C:\... = C:\Omega\C: which crashes.
        if ($rel -eq "" -or $rel -match "^[A-Za-z]:" -or $rel -match "\.\.") {
            Write-Host "  [SKIP] Bad relative path: $rel" -ForegroundColor DarkYellow
            return
        }
        $dest    = Join-Path $OmegaDir $rel
        $destDir = Split-Path $dest -Parent
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        Copy-Item $_.FullName $dest -Force
    }
}

Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
Remove-Item $extractPath -Recurse -Force -ErrorAction SilentlyContinue

# Update git HEAD to match downloaded SHA
$packedRefs = "$OmegaDir\.git\packed-refs"
if (Test-Path $packedRefs) {
    $pr = Get-Content $packedRefs
    $pr = $pr | Where-Object { $_ -notmatch "refs/heads/main" }
    Set-Content -Path $packedRefs -Value $pr -Force
}
$refDir = "$OmegaDir\.git\refs\heads"
if (-not (Test-Path $refDir)) { New-Item -ItemType Directory -Path $refDir -Force | Out-Null }
Set-Content -Path "$refDir\main" -Value $ghSha -Encoding ASCII -Force

Write-Host "  [OK] Source at $ghSha7" -ForegroundColor Green
Write-Host ""

# INCREMENTAL build -- keep CMakeCache.txt and .vcxproj files, delete only .obj/.pch.
# Force-touch all source files so MSVC sees them as newer than surviving .obj files
# and recompiles everything. This avoids the 4-5 minute full reconfigure+recompile.
#
# Safe because:
# (1) Source files are force-touched to NOW -- MSVC dependency tracking always recompiles them.
# (2) Binary is verified via CimInstance after launch -- stale binary detection is reliable.
# (3) CMakeCache.txt and .vcxproj are stable -- no need to regenerate on every restart.
#
# Fall back to full wipe if build dir doesn't exist yet (first run).
if (Test-Path $buildDir) {
    # Delete only compiled artifacts -- keep cmake config
    Get-ChildItem -Path $buildDir -Include "*.obj","*.pch","*.pdb","*.iobj","*.ipdb" -Recurse -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Host "  [OK] Build artifacts wiped (.obj/.pch deleted, CMakeCache preserved)" -ForegroundColor Green
} else {
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    Write-Host "  [OK] Build directory created (first run -- full configure will run)" -ForegroundColor Green
}

# Force-touch ALL source files to NOW so MSVC sees every file as newer than any .obj.
# This guarantees full recompile of changed files without needing to track which changed.
$touchTime = Get-Date
Get-ChildItem -Path $OmegaDir -Include "*.cpp","*.hpp","*.h" -Recurse -ErrorAction SilentlyContinue |
    ForEach-Object { $_.LastWriteTime = $touchTime }

# Verify touch succeeded -- at least main.cpp must have a fresh timestamp.
# If touch failed silently (permissions/locked), old .obj files survive and stale code runs.
$mainCpp = "$OmegaDir\src\main.cpp"
if (Test-Path $mainCpp) {
    $mainAge = ((Get-Date) - (Get-Item $mainCpp).LastWriteTime).TotalSeconds
    if ($mainAge -gt 10) {
        Write-Host "  [FATAL] Source file touch failed -- main.cpp still has old timestamp (age=${mainAge}s)" -ForegroundColor Red
        Write-Host "  Cannot guarantee incremental build is safe. Check file permissions." -ForegroundColor Red
        exit 1
    }
    Write-Host "  [OK] Source timestamps updated (main.cpp age=${mainAge}s)" -ForegroundColor Green
} else {
    Write-Host "  [WARN] Cannot verify touch -- main.cpp not found at expected path" -ForegroundColor Yellow
}
Write-Host ""

# ==============================================================================
# STEP 3: BUILD
# ==============================================================================
Write-Host "[3/4] Building..." -ForegroundColor Yellow

# ALWAYS run cmake configure to re-inject OMEGA_FORCE_GIT_HASH into the binary.
# The hash is injected at configure time (not build time) via UpdateGitHash.cmake.
# Skipping configure means the binary embeds the hash from the PREVIOUS restart.
# Configure is fast (~5s) -- only the compile step is slow.
# Using existing build dir (no --fresh) so .vcxproj files are reused -- fast configure.
& $cmakeExe -S $OmegaDir -B $buildDir -DCMAKE_BUILD_TYPE=Release "-DOMEGA_FORCE_GIT_HASH=$ghSha7" 2>&1 | Where-Object { $_ -match "\[Omega\]|error" } | ForEach-Object { Write-Host "    $_" }
$buildOut = & $cmakeExe --build $buildDir --config Release 2>&1
$buildOut | Where-Object { $_ -match "Omega.vcxproj|error C" } | ForEach-Object { Write-Host "    $_" }

if (-not (Test-Path $BuildExe)) {
    Write-Host "  [FATAL] Build failed -- Omega.exe not produced" -ForegroundColor Red
    exit 1
}

# Copy binary -- retry if locked, FATAL if all retries fail
$copyOk = $false
for ($i = 0; $i -lt 5; $i++) {
    try { Copy-Item $BuildExe $OmegaExe -Force -ErrorAction Stop; $copyOk = $true; break }
    catch {
        Write-Host "  [WARN] Copy attempt $($i+1) failed (locked?): $_" -ForegroundColor Yellow
        Start-Sleep -Seconds 2
    }
}
if (-not $copyOk) {
    Write-Host "  [FATAL] Could not copy new Omega.exe after 5 attempts -- old binary still on disk. Aborting." -ForegroundColor Red
    exit 1
}

$builtAt = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
Write-Host "  [OK] Built $ghSha7 at $builtAt" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# STEP 4: LAUNCH (direct process -- no service)
# ==============================================================================
Write-Host "[4/4] Launching..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

Write-Host ""
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host "  COMMIT : $ghSha7" -ForegroundColor Yellow
Write-Host "  BUILT  : $builtAt" -ForegroundColor Yellow
Write-Host "  MODE   : $mode" -ForegroundColor $modeColor
Write-Host "  GUI    : http://185.167.119.59:7779" -ForegroundColor Yellow
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host ""

$proc = Start-Process -FilePath $OmegaExe -ArgumentList "omega_config.ini" -WorkingDirectory $OmegaDir -PassThru -NoNewWindow
Write-Host "  [DIRECT] PID $($proc.Id)" -ForegroundColor Green

# ── STALE BINARY CHECK 1: CimInstance timestamp ──────────────────────────────
# Wait 15s for process to stabilise (was 5s -- too short if startup is slow).
Start-Sleep -Seconds 15
$runningProc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if (-not $runningProc) {
    Write-Host "  [FATAL] Omega.exe not running 15s after launch -- crashed on startup!" -ForegroundColor Red
    Write-Host "  Check: $OmegaDir\logs\omega_service_stdout.log" -ForegroundColor Red
    exit 1
}
$runningPid  = ($runningProc | Select-Object -First 1).Id
$cimProc     = Get-CimInstance Win32_Process -Filter "ProcessId = $runningPid" -ErrorAction SilentlyContinue
if (-not $cimProc -or [string]::IsNullOrEmpty($cimProc.ExecutablePath)) {
    Write-Host "  [FATAL] Cannot determine path of running Omega.exe (PID $runningPid) via CimInstance." -ForegroundColor Red
    Write-Host "  Kill PID $runningPid manually and re-run QUICK_RESTART.ps1" -ForegroundColor Red
    exit 1
}
$runningExePath = $cimProc.ExecutablePath
$runningExeTime = (Get-Item $runningExePath -ErrorAction Stop).LastWriteTimeUtc
$builtExeTime   = (Get-Item $OmegaExe -ErrorAction Stop).LastWriteTimeUtc
$diffSec = [math]::Abs(($runningExeTime - $builtExeTime).TotalSeconds)
if ($diffSec -gt 10) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
    Write-Host "  ║  WRONG BINARY RUNNING -- ABORTING                ║" -ForegroundColor Red
    Write-Host "  ║  Running EXE : $runningExePath" -ForegroundColor Red
    Write-Host "  ║  Running built: $($runningExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
    Write-Host "  ║  Expected     : $($builtExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
    Write-Host "  ║  Diff: ${diffSec}s -- old binary still running!" -ForegroundColor Red
    Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
    Write-Host "  Kill PID $runningPid manually and re-run QUICK_RESTART.ps1" -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] EXE timestamp matches built binary (+${diffSec}s)" -ForegroundColor Green

# ── STALE BINARY CHECK 2: Git hash in startup log ────────────────────────────
# The binary prints "[Omega] Git hash: XXXXXXX | Built: ..." unconditionally on
# startup. Scrape the log and confirm the running binary reports $ghSha7.
# This catches the case where cmake configure embedded a stale hash, or the wrong
# Omega.exe was launched from a different directory.
# Poll for up to 30s -- log may not be flushed instantly.
$logStdout = "$OmegaDir\logs\omega_service_stdout.log"
$hashFound = $false
$hashInLog = ""
for ($hi = 0; $hi -lt 15; $hi++) {
    if (Test-Path $logStdout) {
        $tail = Get-Content $logStdout -Tail 200 -ErrorAction SilentlyContinue
        $hashLine = $tail | Where-Object { $_ -match "\[Omega\] Git hash:" } | Select-Object -Last 1
        if ($hashLine) {
            # Extract 7-char hash from line like: [Omega] Git hash: e1c70e5 | Built: ...
            if ($hashLine -match "Git hash:\s*([0-9a-f]{7})") {
                $hashInLog = $Matches[1]
                $hashFound = $true
                break
            }
        }
    }
    Start-Sleep -Seconds 2
}

if (-not $hashFound) {
    Write-Host "  [FATAL] Git hash line not found in log after 30s." -ForegroundColor Red
    Write-Host "  Binary may have crashed at startup or is not logging." -ForegroundColor Red
    Write-Host "  Check: $logStdout" -ForegroundColor Red
    exit 1
}

if ($hashInLog -ne $ghSha7) {
    Write-Host "" -ForegroundColor Red
    Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
    Write-Host "  ║  STALE BINARY DETECTED -- WRONG HASH IN LOG      ║" -ForegroundColor Red
    Write-Host "  ║  Expected hash : $ghSha7                          ║" -ForegroundColor Red
    Write-Host "  ║  Log reports   : $hashInLog                       ║" -ForegroundColor Red
    Write-Host "  ║  Binary built with wrong source. Kill and retry.  ║" -ForegroundColor Red
    Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
    taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
    exit 1
}
Write-Host "  [OK] Git hash confirmed in log: $hashInLog == $ghSha7 -- not stale" -ForegroundColor Green

if (-not $SkipVerify) {
    Write-Host ""
    Write-Host "  Waiting 60s then running VERIFY_STARTUP..." -ForegroundColor Cyan
    Start-Sleep -Seconds 60
    & "$OmegaDir\VERIFY_STARTUP.ps1" -OmegaDir $OmegaDir
}

$elapsed = (Get-Date) - $startTime
Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ("  DONE: {0:mm}m {0:ss}s" -f $elapsed) -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan



