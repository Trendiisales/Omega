#Requires -Version 5.1
param([switch]$SkipVerify,[string]$OmegaDir="C:\Omega",[string]$GitHubToken="")

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
$mode = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = if ($mode -eq "LIVE") { "Red" } elseif ($mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
Write-Host "  Mode: $mode" -ForegroundColor $modeColor
Write-Host ""

# ==============================================================================
# STEP 1: STOP
# ==============================================================================
Write-Host "[1/4] Stopping Omega..." -ForegroundColor Yellow
$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Stop-Service "Omega" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
# Kill ALL Omega.exe processes -- loop until confirmed dead
for ($i = 0; $i -lt 15; $i++) {
    taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    $still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if (-not $still) { break }
    if ($i -eq 14) {
        Write-Host "  [FATAL] Cannot kill Omega.exe after 30s -- PIDs still running:" -ForegroundColor Red
        $still | ForEach-Object { Write-Host "    PID $($_.Id)" -ForegroundColor Red }
        exit 1
    }
}
# Hard confirmation: process must be gone
$confirm = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($confirm) {
    Write-Host "  [FATAL] Omega.exe still running after kill attempts. Aborting." -ForegroundColor Red
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
        $rel  = $_.FullName.Substring($innerDir.FullName.Length).TrimStart('\','/')
        $dest = Join-Path $OmegaDir $rel
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

# Wipe build
foreach ($d in @("$buildDir\CMakeFiles","$buildDir\Release","$buildDir\Omega.dir","$buildDir\x64")) {
    if (Test-Path $d) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }
}
Get-ChildItem -Path $buildDir -Include "*.obj","*.pch","*.pdb" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Write-Host "  [OK] Build wiped" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# STEP 3: BUILD
# ==============================================================================
Write-Host "[3/4] Building..." -ForegroundColor Yellow

& $cmakeExe -S $OmegaDir -B $buildDir -DCMAKE_BUILD_TYPE=Release "-DOMEGA_FORCE_GIT_HASH=$ghSha7" 2>&1 | Where-Object { $_ -match "\[Omega\]|error" } | ForEach-Object { Write-Host "    $_" }
$buildOut = & $cmakeExe --build $buildDir --config Release 2>&1
$buildOut | Where-Object { $_ -match "Omega.vcxproj|error C" } | ForEach-Object { Write-Host "    $_" }

if (-not (Test-Path $BuildExe)) {
    Write-Host "  [FATAL] Build failed -- Omega.exe not produced" -ForegroundColor Red
    exit 1
}

# Copy binary -- retry if locked
for ($i = 0; $i -lt 5; $i++) {
    try { Copy-Item $BuildExe $OmegaExe -Force -ErrorAction Stop; break }
    catch { Start-Sleep -Seconds 2 }
}

$builtAt = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
Write-Host "  [OK] Built $ghSha7 at $builtAt" -ForegroundColor Green
Write-Host ""

# ==============================================================================
# STEP 4: LAUNCH
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

$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc) {
    Start-Service "Omega" -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    $svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
    $col = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  [SERVICE] $($svc.Status)" -ForegroundColor $col
} else {
    $proc = Start-Process -FilePath $OmegaExe -ArgumentList "omega_config.ini" -WorkingDirectory $OmegaDir -PassThru -NoNewWindow
    Write-Host "  [DIRECT] PID $($proc.Id)" -ForegroundColor Green
}

# Hard verify: running Omega.exe must have same timestamp as newly built EXE
Start-Sleep -Seconds 5
$runningProc = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if (-not $runningProc) {
    Write-Host "  [FATAL] Omega.exe not running after launch!" -ForegroundColor Red
    exit 1
}
# Get the path of the running exe and check its timestamp
try {
    $runningExePath = $runningProc | Select-Object -First 1 | ForEach-Object { $_.Path }
    $runningExeTime = (Get-Item $runningExePath -ErrorAction Stop).LastWriteTimeUtc
    $builtExeTime   = (Get-Item $OmegaExe -ErrorAction Stop).LastWriteTimeUtc
    $diffSec = [math]::Abs(($runningExeTime - $builtExeTime).TotalSeconds)
    if ($diffSec -gt 10) {
        Write-Host "" -ForegroundColor Red
        Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Red
        Write-Host "  ║  WRONG BINARY RUNNING -- ABORTING                ║" -ForegroundColor Red
        Write-Host "  ║  Running EXE: $runningExePath" -ForegroundColor Red
        Write-Host "  ║  Running built: $($runningExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
        Write-Host "  ║  Expected:      $($builtExeTime.ToString('HH:mm:ss')) UTC" -ForegroundColor Red
        Write-Host "  ║  Diff: ${diffSec}s -- old binary still running!" -ForegroundColor Red
        Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Red
        Write-Host "  Kill PID $($runningProc.Id) manually and re-run QUICK_RESTART.ps1" -ForegroundColor Red
        exit 1
    }
    Write-Host "  [OK] Running EXE timestamp matches built EXE (+${diffSec}s)" -ForegroundColor Green
} catch {
    Write-Host "  [WARN] Could not verify running EXE path: $_" -ForegroundColor Yellow
}

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
Write-Host ""
