#Requires -Version 5.1
param([switch]$SkipVerify,[int]$WaitSec=10,[string]$OmegaDir="C:\Omega",[string]$GitHubToken="")

$ErrorActionPreference = "Continue"

if ($GitHubToken -eq "") {
    $tf = "$OmegaDir\.github_token"
    if (Test-Path $tf) { $GitHubToken = (Get-Content $tf -Raw).Trim() }
}

$OmegaExe    = "$OmegaDir\Omega.exe"
$BuildExe    = "$OmegaDir\build\Release\Omega.exe"
$buildDir    = "$OmegaDir\build"
$cmakeExe    = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
$ConfigSrc   = "$OmegaDir\omega_config.ini"
$restartStart = Get-Date

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   OMEGA  |  QUICK RESTART" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

$modeMatch = Select-String -Path $ConfigSrc -Pattern "^mode\s*=\s*(\S+)" -ErrorAction SilentlyContinue
$mode      = if ($modeMatch) { $modeMatch.Matches[0].Groups[1].Value } else { "UNKNOWN" }
$modeColor = if ($mode -eq "LIVE") { "Red" } elseif ($mode -eq "SHADOW") { "Yellow" } else { "Cyan" }
Write-Host "  Mode: $mode" -ForegroundColor $modeColor
Write-Host ""

# [1] STOP
Write-Host "[1/4] Stopping Omega..." -ForegroundColor Yellow
$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Stop-Service "Omega" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
for ($k = 0; $k -lt 5; $k++) {
    taskkill /F /IM Omega.exe /T 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    $still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
    if (-not $still) { break }
}
$still = Get-Process -Name "Omega" -ErrorAction SilentlyContinue
if ($still) {
    Write-Host "  [FATAL] Omega.exe still running -- run: taskkill /F /IM Omega.exe /T" -ForegroundColor Red
    exit 1
}
Start-Sleep -Seconds 3
Write-Host "  [OK] Stopped" -ForegroundColor Green
Write-Host ""

# [2] DOWNLOAD SOURCE FROM GITHUB API
Write-Host "[2/4] Downloading source from GitHub API..." -ForegroundColor Yellow
$apiHeaders = @{ Authorization="token $GitHubToken"; "User-Agent"="OmegaRestart"; Accept="application/vnd.github.v3+json" }
try {
    $commitResp = Invoke-RestMethod -Uri "https://api.github.com/repos/Trendiisales/Omega/commits/main" -Headers $apiHeaders -TimeoutSec 20 -ErrorAction Stop
} catch {
    Write-Host "  [FATAL] GitHub API unreachable: $_" -ForegroundColor Red
    exit 1
}
$ghSha  = $commitResp.sha
$ghSha7 = $ghSha.Substring(0,7)
Write-Host "  [API] HEAD: $ghSha7 -- $($commitResp.commit.message)" -ForegroundColor Cyan

$zipUrl  = "https://api.github.com/repos/Trendiisales/Omega/zipball/$ghSha"
$zipPath = "$env:TEMP\Omega_$ghSha7.zip"
try {
    Invoke-WebRequest -Uri $zipUrl -Headers @{ Authorization="token $GitHubToken"; "User-Agent"="OmegaRestart" } -OutFile $zipPath -TimeoutSec 120 -ErrorAction Stop
} catch {
    Write-Host "  [FATAL] Download failed: $_" -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] Downloaded $([math]::Round((Get-Item $zipPath).Length/1MB,1)) MB" -ForegroundColor Cyan

$extractPath = "$env:TEMP\Omega_extract_$ghSha7"
if (Test-Path $extractPath) { Remove-Item $extractPath -Recurse -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $extractPath)
$innerDir = Get-ChildItem $extractPath -Directory | Select-Object -First 1

# Copy source -- exclude .ps1 files (scripts are managed separately, not overwritten by zip)
$sourceExts = @("*.cpp","*.hpp","*.h","*.ini","*.cmake","*.txt","*.json","*.md")
foreach ($ext in $sourceExts) {
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

# Write SHA to git ref file
$refFile = "$OmegaDir\.git\refs\heads\main"
Set-Content -Path $refFile -Value $ghSha -Encoding ASCII -Force
& git -C $OmegaDir rm -r --cached --force --ignore-unmatch logs/ 2>&1 | Out-Null

Write-Host "  [OK] Source installed at $ghSha7" -ForegroundColor Green
Write-Host ""

# Wipe build dirs
foreach ($d in @("$buildDir\CMakeFiles","$buildDir\Release","$buildDir\Omega.dir","$buildDir\x64")) {
    if (Test-Path $d) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue; Write-Host "  [CLEAN] Wiped $d" -ForegroundColor Cyan }
}
Write-Host ""

# [3] BUILD
Write-Host "[3/4] Building..." -ForegroundColor Yellow
& $cmakeExe -S $OmegaDir -B $buildDir -DCMAKE_BUILD_TYPE=Release "-DOMEGA_FORCE_GIT_HASH=$ghSha7" 2>&1 | Where-Object { $_ -match "\[Omega\]|error|warning" } | ForEach-Object { Write-Host "    $_" }
$buildOutput = & $cmakeExe --build $buildDir --config Release 2>&1
$buildOutput | Where-Object { $_ -match "Omega.vcxproj|error C" } | ForEach-Object { Write-Host "    $_" }
$buildFailed = $buildOutput | Where-Object { $_ -match "error C[0-9]+" }
if ($buildFailed) { Write-Host "  [FATAL] Build failed" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $BuildExe)) { Write-Host "  [FATAL] Binary not found after build" -ForegroundColor Red; exit 1 }
Copy-Item $BuildExe $OmegaExe -Force
$buildTime = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
Write-Host "  [OK] Built at $buildTime" -ForegroundColor Green
Write-Host ""

# Read baked hash
$verHash = "unknown"
$verFile = "$OmegaDir\include\version_generated.hpp"
if (Test-Path $verFile) {
    $vl = Select-String -Path $verFile -Pattern 'OMEGA_GIT_HASH' | Select-Object -First 1
    if ($vl -and $vl.Line -match '"([a-f0-9]+)"') { $verHash = $Matches[1] }
}

New-Item -ItemType Directory -Path "$OmegaDir\logs"        -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\shadow" -Force | Out-Null
New-Item -ItemType Directory -Path "$OmegaDir\logs\trades" -Force | Out-Null

# [4] LAUNCH
Write-Host "[4/4] Launching Omega..." -ForegroundColor Yellow
Write-Host ""
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host "  COMMIT : $verHash" -ForegroundColor Yellow
Write-Host "  BUILT  : $buildTime" -ForegroundColor Yellow
Write-Host "  MODE   : $mode" -ForegroundColor $modeColor
Write-Host "  GUI    : http://185.167.119.59:7779" -ForegroundColor Yellow
Write-Host "########################################################" -ForegroundColor Yellow
Write-Host ""

$svc = Get-Service -Name "Omega" -ErrorAction SilentlyContinue
if ($svc) {
    Start-Service "Omega"
    Start-Sleep -Seconds 3
    $svc = Get-Service -Name "Omega"
    $svcColor = if ($svc.Status -eq "Running") { "Green" } else { "Red" }
    Write-Host "  [SERVICE] Status: $($svc.Status)" -ForegroundColor $svcColor
} else {
    $proc = Start-Process -FilePath $OmegaExe -ArgumentList "omega_config.ini" -WorkingDirectory $OmegaDir -PassThru -NoNewWindow
    Write-Host "  [DIRECT] PID: $($proc.Id)" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "  Waiting 60s for startup log..." -ForegroundColor Cyan
Start-Sleep -Seconds 60

$logToday = "$OmegaDir\logs\omega_$((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')).log"
$gfeLine = Get-Content $logToday -ErrorAction SilentlyContinue | Select-String "GFE-CONFIG" | Select-Object -Last 1
if ($gfeLine -and $gfeLine -notmatch "DISABLED") {
    Write-Host "  [PASS] GoldFlow: ACTIVE" -ForegroundColor Green
} else {
    Write-Host "  [WARN] GoldFlow: not confirmed in log yet" -ForegroundColor Yellow
}

if (-not $SkipVerify) {
    Write-Host ""
    Write-Host "  Running VERIFY_STARTUP..." -ForegroundColor Cyan
    & "$OmegaDir\VERIFY_STARTUP.ps1" -WaitSec $WaitSec -OmegaDir $OmegaDir
}

$elapsed = (Get-Date) - $restartStart
Write-Host ""
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ("  TOTAL RESTART TIME: {0:mm}m {0:ss}s" -f $elapsed) -ForegroundColor Cyan
Write-Host ("  Started : " + $restartStart.ToUniversalTime().ToString("HH:mm:ss UTC")) -ForegroundColor DarkGray
Write-Host ("  Finished: " + (Get-Date).ToUniversalTime().ToString("HH:mm:ss UTC")) -ForegroundColor DarkGray
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""
