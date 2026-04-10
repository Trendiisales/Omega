#Requires -Version 5.1
param(
    [string] $OmegaDir     = "C:\Omega",
    [string] $GitHubToken  = "",
    [string] $ExpectedHash = "",
    [switch] $PostBuild,
    [switch] $PostLaunch
)
if ($GitHubToken -eq "") {
    $tokenFile = "$OmegaDir\.github_token"
    if (Test-Path $tokenFile) { $GitHubToken = (Get-Content $tokenFile -Raw).Trim() }
}
$ErrorActionPreference = "Continue"
$buildDir = "$OmegaDir\build"
$OmegaExe = "$OmegaDir\Omega.exe"
$VerFile  = "$OmegaDir\include\version_generated.hpp"
$PassFile = "$OmegaDir\logs\PRE_DELIVERY_PASS.txt"
$FailFile = "$OmegaDir\logs\PRE_DELIVERY_FAIL.txt"
$LogFile  = "$OmegaDir\logs\latest.log"
$global:pdc_failures = @()
$global:pdc_results  = @()
function Pass($check, $detail) { Write-Host "  [PASS] $check -- $detail" -ForegroundColor Green; $global:pdc_results += "PASS|$check|$detail" }
function Fail($check, $detail) { Write-Host "  [FAIL] $check -- $detail" -ForegroundColor Red; $global:pdc_results += "FAIL|$check|$detail"; $global:pdc_failures += "${check}: $detail" }
function Info($check, $detail) { Write-Host "  [INFO] $check -- $detail" -ForegroundColor Cyan; $global:pdc_results += "INFO|$check|$detail" }
Write-Host ""; Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "   PRE-DELIVERY CHECK -- ALL MUST PASS" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow; Write-Host ""
$headCheck = & git -C $OmegaDir rev-parse HEAD 2>&1
if ($LASTEXITCODE -eq 0) { Pass "Git reachable" "rev-parse HEAD succeeded" } else { Fail "Git reachable" "FAILED: $headCheck" }
& git -C $OmegaDir rm -r --cached --force --ignore-unmatch logs/ 2>&1 | Out-Null
$resetOut = & git -C $OmegaDir reset --hard HEAD 2>&1
if ($LASTEXITCODE -eq 0) { Pass "Git reset" "reset --hard HEAD: $($resetOut | Select-Object -Last 1)" } else { Fail "Git reset" "FAILED: $resetOut" }
$apiHead = "unknown"
if ($GitHubToken -ne "") {
    try {
        $apiHeaders = @{ Authorization="token $GitHubToken"; "User-Agent"="Omega-PreCheck"; "Cache-Control"="no-cache" }
        $apiResp = Invoke-RestMethod -Uri "https://api.github.com/repos/Trendiisales/Omega/git/refs/heads/main" -Headers $apiHeaders -Method Get -ErrorAction Stop
        $apiHead = $apiResp.object.sha.Substring(0,7)
        Pass "GitHub API HEAD" "HEAD=$apiHead (live from API, no CDN cache)"
    } catch { Fail "GitHub API HEAD" "API call failed: $_" }
} else { Info "GitHub API HEAD" "No token -- skipping" }
$localHead = & git -C $OmegaDir rev-parse HEAD 2>$null
$localHead7 = if ($localHead -and $localHead.Length -ge 7) { $localHead.Substring(0,7) } else { "unknown" }
Info "Local HEAD" "local=$localHead7 api=$apiHead (informational only -- not a failure gate)"
$verHash = "unknown"
if (Test-Path $VerFile) {
    $vl = Select-String -Path $VerFile -Pattern 'OMEGA_GIT_HASH' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vl -and $vl.Line -match '"([a-f0-9]{7,})"') { $verHash = $Matches[1] }
}
if ($PostBuild -or $PostLaunch) {
    if ($verHash -eq "unknown") { Fail "version_generated.hpp" "File missing -- cmake did not run" }
    elseif ($localHead7 -ne "unknown" -and $verHash -ne $localHead7) { Fail "version_generated.hpp" "verHash=$verHash != localHead=$localHead7" }
    else { Pass "version_generated.hpp" "hash=$verHash matches HEAD=$localHead7" }
} else { Info "version_generated.hpp" "pre-build: not checked" }
if ($PostBuild -and -not $PostLaunch) {
    if (Test-Path "$buildDir\CMakeFiles") { Pass "Build dirs wiped" "CMakeFiles present post-build -- wipe+rebuild completed successfully" }
    else { Fail "Build dirs wiped" "CMakeFiles absent -- cmake build may have failed" }
    if (Test-Path $OmegaExe) {
        $binaryAge = [int]((Get-Date) - (Get-Item $OmegaExe).LastWriteTime).TotalMinutes
        $binaryTime = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
        if ($binaryAge -le 10) { Pass "Binary fresh" "Omega.exe built $binaryAge min ago at $binaryTime" }
        else { Fail "Binary fresh" "Omega.exe is ${binaryAge}min old" }
    } else { Fail "Binary fresh" "Omega.exe not found" }
}
if ($PostBuild -and $ExpectedHash -ne "" -and $verHash -ne "unknown") {
    if ($verHash -eq $ExpectedHash) { Pass "Binary hash" "version_generated=$verHash == expected=$ExpectedHash" }
    else { Fail "Binary hash" "version_generated=$verHash != expected=$ExpectedHash" }
} elseif ($PostBuild -and $verHash -ne "unknown" -and $localHead7 -ne "unknown") {
    if ($verHash -eq $localHead7) { Pass "Binary hash" "version_generated=$verHash == localHEAD=$localHead7" }
    else { Fail "Binary hash" "version_generated=$verHash != localHEAD=$localHead7" }
}
if ($PostLaunch) {
    if (Test-Path $LogFile) {
        $commitLine = Get-Content $LogFile | Where-Object { $_ -match "RUNNING COMMIT" } | Select-Object -Last 1
        if ($commitLine -and $commitLine -match "RUNNING COMMIT\s*:\s*([a-f0-9]+)") {
            $logHash = $Matches[1].Substring(0,[Math]::Min(7,$Matches[1].Length))
            if ($localHead7 -ne "unknown" -and $logHash -eq $localHead7) { Pass "Log hash matches" "log=$logHash == HEAD=$localHead7" }
            else { Fail "Log hash matches" "log=$logHash != HEAD=$localHead7 -- WRONG BINARY RUNNING" }
        } else { Fail "Log hash matches" "No RUNNING COMMIT in log" }
    } else { Fail "Log hash matches" "latest.log not found" }
}
Write-Host ""; Write-Host "=======================================================" -ForegroundColor Yellow
$timestamp = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")
if ($global:pdc_failures.Count -eq 0) {
    Write-Host "  ALL CHECKS PASSED -- Omega may proceed" -ForegroundColor Green
    Write-Host "  HEAD: $localHead7  TIME: $timestamp" -ForegroundColor Green
    Write-Host "=======================================================" -ForegroundColor Yellow
    $passContent = "PASS`ntimestamp=$timestamp`nhash=$localHead7`n"
    $global:pdc_results | ForEach-Object { $passContent += "$_`n" }
    Set-Content -Path $PassFile -Value $passContent -Force
    if (Test-Path $FailFile) { Remove-Item $FailFile -Force -ErrorAction SilentlyContinue }
    Write-Host ""; exit 0
} else {
    Write-Host "  !! $($global:pdc_failures.Count) CHECK(S) FAILED -- OMEGA WILL NOT START !!" -ForegroundColor Red
    Write-Host ""
    foreach ($f in $global:pdc_failures) { Write-Host "  FAILED: $f" -ForegroundColor Red }
    Write-Host ""; Write-Host "  Fix all failures above then run QUICK_RESTART.ps1 again." -ForegroundColor Yellow
    Write-Host "=======================================================" -ForegroundColor Yellow
    $failContent = "FAIL`ntimestamp=$timestamp`nhash=$localHead7`n"
    $global:pdc_failures | ForEach-Object { $failContent += "FAILED: $_`n" }
    Set-Content -Path $FailFile -Value $failContent -Force
    if (Test-Path $PassFile) { Remove-Item $PassFile -Force -ErrorAction SilentlyContinue }
    Write-Host ""; exit 1
}
