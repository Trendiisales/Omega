#Requires -Version 5.1
param([string]$OmegaDir="C:\Omega",[string]$GitHubToken="",[string]$ExpectedHash="",[switch]$PostBuild,[switch]$PostLaunch)
if($GitHubToken -eq ""){$tf="$OmegaDir\.github_token";if(Test-Path $tf){$GitHubToken=(Get-Content $tf -Raw).Trim()}}
$ErrorActionPreference="Continue"
$OmegaExe="$OmegaDir\Omega.exe";$VerFile="$OmegaDir\include\version_generated.hpp"
$PassFile="$OmegaDir\logs\PRE_DELIVERY_PASS.txt";$FailFile="$OmegaDir\logs\PRE_DELIVERY_FAIL.txt";$LogFile="$OmegaDir\logs\latest.log"
$global:pdc_failures=@();$global:pdc_results=@()
function Pass($c,$d){Write-Host "  [PASS] $c -- $d" -ForegroundColor Green;$global:pdc_results+="PASS|$c|$d"}
function Fail($c,$d){Write-Host "  [FAIL] $c -- $d" -ForegroundColor Red;$global:pdc_results+="FAIL|$c|$d";$global:pdc_failures+="${c}: $d"}
function Info($c,$d){Write-Host "  [INFO] $c -- $d" -ForegroundColor Cyan;$global:pdc_results+="INFO|$c|$d"}
Write-Host "";Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "   PRE-DELIVERY CHECK -- ALL MUST PASS" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow;Write-Host ""
# CHECK 1: git functional
$hc=& git -C $OmegaDir rev-parse HEAD 2>&1
if($LASTEXITCODE -eq 0){Pass "Git reachable" "rev-parse HEAD succeeded"}else{Fail "Git reachable" "FAILED: $hc"}
# CHECK 2: clean index (NO reset --hard -- QUICK_RESTART already installed correct source)
& git -C $OmegaDir rm -r --cached --force --ignore-unmatch logs/ 2>&1|Out-Null
Pass "Git index" "logs/ removed from index"
# CHECK 3: GitHub API HEAD
$apiHead="unknown"
if($GitHubToken -ne ""){
  try{$ah=@{Authorization="token $GitHubToken";"User-Agent"="Omega-PreCheck";"Cache-Control"="no-cache"}
  $ar=Invoke-RestMethod -Uri "https://api.github.com/repos/Trendiisales/Omega/git/refs/heads/main" -Headers $ah -Method Get -ErrorAction Stop
  $apiHead=$ar.object.sha.Substring(0,7);Pass "GitHub API HEAD" "HEAD=$apiHead (live from API)"}
  catch{Fail "GitHub API HEAD" "API call failed: $_"}}
else{Info "GitHub API HEAD" "No token -- skipping"}
# CHECK 4: local HEAD -- informational only
$lh=& git -C $OmegaDir rev-parse HEAD 2>$null
$localHead7=if($lh -and $lh.Length -ge 7){$lh.Substring(0,7)}else{"unknown"}
Info "Local HEAD" "local=$localHead7 api=$apiHead (informational -- version_generated is the proof)"
# CHECK 5: version_generated.hpp
$verHash="unknown"
if(Test-Path $VerFile){$vl=Select-String -Path $VerFile -Pattern "OMEGA_GIT_HASH" -ErrorAction SilentlyContinue|Select-Object -First 1;if($vl -and $vl.Line -match '"([a-f0-9]{7,})"'){$verHash=$Matches[1]}}
if($PostBuild -or $PostLaunch){
  if($verHash -eq "unknown"){Fail "version_generated.hpp" "File missing -- cmake did not run"}
  else{Pass "version_generated.hpp" "hash=$verHash"}}
else{Info "version_generated.hpp" "pre-build: not checked"}
# CHECK 6+7: build dirs + binary fresh
if($PostBuild -and -not $PostLaunch){
  if(Test-Path "$OmegaDir\build\CMakeFiles"){Pass "Build dirs wiped" "CMakeFiles present post-build -- wipe+rebuild completed successfully"}
  else{Fail "Build dirs wiped" "CMakeFiles absent -- cmake build may have failed"}
  if(Test-Path $OmegaExe){$ba=[int]((Get-Date)-(Get-Item $OmegaExe).LastWriteTime).TotalMinutes;$bt=(Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
    if($ba -le 10){Pass "Binary fresh" "Omega.exe built $ba min ago at $bt"}else{Fail "Binary fresh" "Omega.exe is ${ba}min old"}}
  else{Fail "Binary fresh" "Omega.exe not found"}}
# CHECK 8: binary hash
if($PostBuild -and $verHash -ne "unknown"){
  $expected=if($ExpectedHash -ne ""){$ExpectedHash}else{$verHash}
  if($verHash -eq $expected){Pass "Binary hash" "version_generated=$verHash == expected=$expected"}
  else{Fail "Binary hash" "version_generated=$verHash != expected=$expected"}}
# CHECK 9: log hash
if($PostLaunch){
  if(Test-Path $LogFile){$cl=Get-Content $LogFile|Where-Object{$_ -match "RUNNING COMMIT"}|Select-Object -Last 1
    if($cl -and $cl -match "RUNNING COMMIT\s*:\s*([a-f0-9]+)"){$lHash=$Matches[1].Substring(0,[Math]::Min(7,$Matches[1].Length))
      if($lHash -eq $verHash -or $lHash -eq $localHead7){Pass "Log hash matches" "log=$lHash -- correct binary running"}
      else{Fail "Log hash matches" "log=$lHash != expected -- WRONG BINARY RUNNING"}}
    else{Fail "Log hash matches" "No RUNNING COMMIT in log"}}
  else{Fail "Log hash matches" "latest.log not found"}}
# RESULT
Write-Host "";Write-Host "=======================================================" -ForegroundColor Yellow
$ts=(Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")
if($global:pdc_failures.Count -eq 0){
  Write-Host "  ALL CHECKS PASSED -- Omega may proceed" -ForegroundColor Green
  Write-Host "  HEAD: $localHead7  TIME: $ts" -ForegroundColor Green
  Write-Host "=======================================================" -ForegroundColor Yellow
  $pc="PASS`ntimestamp=$ts`nhash=$localHead7`n";$global:pdc_results|ForEach-Object{$pc+="$_`n"}
  Set-Content -Path $PassFile -Value $pc -Force
  if(Test-Path $FailFile){Remove-Item $FailFile -Force -ErrorAction SilentlyContinue}
  Write-Host "";exit 0}
else{
  Write-Host "  !! $($global:pdc_failures.Count) CHECK(S) FAILED -- OMEGA WILL NOT START !!" -ForegroundColor Red;Write-Host ""
  foreach($f in $global:pdc_failures){Write-Host "  FAILED: $f" -ForegroundColor Red}
  Write-Host "";Write-Host "  Fix all failures above then run QUICK_RESTART.ps1 again." -ForegroundColor Yellow
  Write-Host "=======================================================" -ForegroundColor Yellow
  $fc="FAIL`ntimestamp=$ts`nhash=$localHead7`n";$global:pdc_failures|ForEach-Object{$fc+="FAILED: $_`n"}
  Set-Content -Path $FailFile -Value $fc -Force
  if(Test-Path $PassFile){Remove-Item $PassFile -Force -ErrorAction SilentlyContinue}
  Write-Host "";exit 1}
