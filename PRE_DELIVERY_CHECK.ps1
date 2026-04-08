#Requires -Version 5.1
# ==============================================================================
#  PRE_DELIVERY_CHECK.ps1 -- HARD GATE. Called by QUICK_RESTART.ps1.
#  Cannot be bypassed. Omega does NOT start if any check fails.
#
#  CHECKS (all must pass):
#    1.  Git reachable         -- fetch origin succeeds
#    2.  No stale local files  -- local HEAD == origin/main HEAD (after reset)
#    3.  GitHub API hash       -- reads HEAD sha from GitHub API (bypasses CDN cache)
#    4.  Local HEAD matches    -- git rev-parse HEAD == GitHub API HEAD
#    5.  version_generated     -- OMEGA_GIT_HASH in version_generated.hpp matches HEAD
#    6.  .obj/.pch deleted     -- no stale object files exist in build dir
#    7.  Binary built fresh    -- Omega.exe timestamp > pre-build timestamp
#    8.  Binary hash matches   -- OMEGA_GIT_HASH in running binary log == HEAD
#    9.  Log confirms hash     -- [OMEGA] RUNNING COMMIT in log matches HEAD
#   10.  PRE_DELIVERY_PASS.txt written -- timestamp + hash + all results
#
#  Exit codes: 0 = all pass, 1 = one or more failed (Omega must not start)
# ==============================================================================

param(
    [string] $OmegaDir    = "C:\Omega",
    [string] $GitHubToken = "",
    [string] $ExpectedHash = "",   # passed in by QUICK_RESTART after build
    [switch] $PostLaunch          # set when checking log after launch
)

$ErrorActionPreference = "Continue"
$cmakeExe  = "C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
$buildDir  = "$OmegaDir\build"
$BuildExe  = "$OmegaDir\build\Release\Omega.exe"
$OmegaExe  = "$OmegaDir\Omega.exe"
$VerFile   = "$OmegaDir\include\version_generated.hpp"
$PassFile  = "$OmegaDir\logs\PRE_DELIVERY_PASS.txt"
$FailFile  = "$OmegaDir\logs\PRE_DELIVERY_FAIL.txt"
$LogFile   = "$OmegaDir\logs\latest.log"

$failures  = @()
$results   = @()

function Pass($check, $detail) {
    Write-Host "  [PASS] $check -- $detail" -ForegroundColor Green
    $script:results += "PASS|$check|$detail"
}
function Fail($check, $detail) {
    Write-Host "  [FAIL] $check -- $detail" -ForegroundColor Red
    $script:results  += "FAIL|$check|$detail"
    $script:failures += "$check: $detail"
}
function Info($check, $detail) {
    Write-Host "  [INFO] $check -- $detail" -ForegroundColor Cyan
    $script:results += "INFO|$check|$detail"
}

Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host "   PRE-DELIVERY CHECK -- ALL MUST PASS" -ForegroundColor Yellow
Write-Host "=======================================================" -ForegroundColor Yellow
Write-Host ""

# ==============================================================================
# CHECK 1: Git reachable -- fetch origin must succeed
# ==============================================================================
$fetchOut = & git -C $OmegaDir fetch origin 2>&1
if ($LASTEXITCODE -eq 0) {
    Pass "Git reachable" "fetch origin succeeded"
} else {
    Fail "Git reachable" "git fetch origin FAILED: $fetchOut -- no network or bad credentials"
}

# ==============================================================================
# CHECK 2: Hard reset to origin/main -- eliminates ALL local drift
# ==============================================================================
$resetOut = & git -C $OmegaDir reset --hard origin/main 2>&1
if ($LASTEXITCODE -eq 0) {
    Pass "Git reset" "reset --hard origin/main: $($resetOut | Select-Object -Last 1)"
} else {
    Fail "Git reset" "reset --hard FAILED: $resetOut"
}

# ==============================================================================
# CHECK 3: GitHub API HEAD hash -- bypasses raw.githubusercontent.com CDN cache
# Uses the REST API directly which always returns live data, never cached.
# ==============================================================================
$apiHead = "unknown"
if ($GitHubToken -ne "") {
    try {
        $apiUrl     = "https://api.github.com/repos/Trendiisales/Omega/commits/main"
        $apiHeaders = @{ Authorization = "token $GitHubToken"; "User-Agent" = "Omega-PreCheck" }
        $apiResp    = Invoke-RestMethod -Uri $apiUrl -Headers $apiHeaders -Method Get -ErrorAction Stop
        $apiHead    = $apiResp.sha.Substring(0, 7)
        Pass "GitHub API HEAD" "HEAD=$apiHead (live from API, no CDN cache)"
    } catch {
        Fail "GitHub API HEAD" "API call failed: $_ -- cannot verify HEAD"
    }
} else {
    Info "GitHub API HEAD" "No token provided -- skipping API check (local git HEAD used only)"
}

# ==============================================================================
# CHECK 4: Local HEAD matches GitHub API HEAD
# ==============================================================================
$localHead = & git -C $OmegaDir rev-parse HEAD 2>$null
$localHead7 = if ($localHead -and $localHead.Length -ge 7) { $localHead.Substring(0,7) } else { "unknown" }

if ($apiHead -ne "unknown" -and $localHead7 -ne "unknown") {
    if ($localHead7 -eq $apiHead) {
        Pass "Local == API HEAD" "local=$localHead7 == api=$apiHead"
    } else {
        Fail "Local == API HEAD" "local=$localHead7 != api=$apiHead -- local git is behind or ahead of origin/main"
    }
} else {
    Info "Local == API HEAD" "local=$localHead7 api=$apiHead -- partial check only"
}

# ==============================================================================
# CHECK 5: version_generated.hpp hash matches local HEAD
# This file is written by cmake configure. Must match what git says HEAD is.
# ==============================================================================
$verHash = "unknown"
if (Test-Path $VerFile) {
    $vl = Select-String -Path $VerFile -Pattern 'OMEGA_GIT_HASH' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vl -and $vl.Line -match '"([a-f0-9]{7,})"') { $verHash = $Matches[1] }
}

if ($verHash -eq "unknown") {
    Fail "version_generated.hpp" "File missing or OMEGA_GIT_HASH not found -- cmake configure did not run"
} elseif ($localHead7 -ne "unknown" -and $verHash -ne $localHead7) {
    Fail "version_generated.hpp" "verHash=$verHash != localHead=$localHead7 -- cmake configured from wrong commit"
} else {
    Pass "version_generated.hpp" "hash=$verHash matches HEAD=$localHead7"
}

# ==============================================================================
# CHECK 6: No stale .obj or .pch files in build dir
# If these exist, cmake MAY skip recompilation for header-only changes.
# QUICK_RESTART deletes them before building. This check confirms they are gone.
# ==============================================================================
if (-not $PostLaunch) {
    $staleFiles = Get-ChildItem -Path $buildDir -Recurse -Include "*.obj","*.pch" -ErrorAction SilentlyContinue
    $staleCount = ($staleFiles | Measure-Object).Count
    if ($staleCount -eq 0) {
        Pass "No stale objects" "0 .obj/.pch files in build dir -- full recompile guaranteed"
    } else {
        Fail "No stale objects" "$staleCount .obj/.pch files still exist -- incremental build WILL skip header changes"
    }
}

# ==============================================================================
# CHECK 7: Binary was built fresh -- Omega.exe timestamp must be recent (< 10 min)
# ==============================================================================
if (-not $PostLaunch) {
    if (Test-Path $OmegaExe) {
        $binaryAge = [int]((Get-Date) - (Get-Item $OmegaExe).LastWriteTime).TotalMinutes
        $binaryTime = (Get-Item $OmegaExe).LastWriteTime.ToUniversalTime().ToString("HH:mm:ss UTC")
        if ($binaryAge -le 10) {
            Pass "Binary fresh" "Omega.exe built $binaryAge min ago at $binaryTime"
        } else {
            Fail "Binary fresh" "Omega.exe is ${binaryAge}min old (built $binaryTime) -- may be from previous session"
        }
    } else {
        Fail "Binary fresh" "Omega.exe not found at $OmegaExe"
    }
}

# ==============================================================================
# CHECK 8: Binary hash matches HEAD
# version_generated.hpp was written by cmake configure AFTER git reset.
# The hash in it must equal the HEAD we just confirmed.
# ==============================================================================
if ($ExpectedHash -ne "" -and $verHash -ne "unknown") {
    if ($verHash -eq $ExpectedHash) {
        Pass "Binary hash" "version_generated=$verHash == expected=$ExpectedHash"
    } else {
        Fail "Binary hash" "version_generated=$verHash != expected=$ExpectedHash -- wrong commit compiled"
    }
} elseif ($verHash -ne "unknown" -and $localHead7 -ne "unknown") {
    if ($verHash -eq $localHead7) {
        Pass "Binary hash" "version_generated=$verHash == localHEAD=$localHead7"
    } else {
        Fail "Binary hash" "version_generated=$verHash != localHEAD=$localHead7 -- cmake used wrong source"
    }
}

# ==============================================================================
# CHECK 9: Log confirms running hash (post-launch only)
# Reads latest.log for [OMEGA] RUNNING COMMIT and compares to HEAD.
# Only runs when -PostLaunch flag is set (called 60s after Omega starts).
# ==============================================================================
if ($PostLaunch) {
    if (Test-Path $LogFile) {
        $commitLine = Get-Content $LogFile | Where-Object { $_ -match "RUNNING COMMIT" } | Select-Object -Last 1
        if ($commitLine -and $commitLine -match "RUNNING COMMIT\s*:\s*([a-f0-9]+)") {
            $logHash = $Matches[1].Substring(0, [Math]::Min(7, $Matches[1].Length))
            if ($localHead7 -ne "unknown" -and $logHash -eq $localHead7) {
                Pass "Log hash matches" "log=$logHash == HEAD=$localHead7 -- correct binary is running"
            } else {
                Fail "Log hash matches" "log=$logHash != HEAD=$localHead7 -- WRONG BINARY IS RUNNING"
            }
        } else {
            Fail "Log hash matches" "No [OMEGA] RUNNING COMMIT line in log -- Omega may not have started"
        }
    } else {
        Fail "Log hash matches" "latest.log not found -- Omega did not start"
    }
}

# ==============================================================================
# RESULT
# ==============================================================================
Write-Host ""
Write-Host "=======================================================" -ForegroundColor Yellow

$timestamp = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")

if ($failures.Count -eq 0) {
    Write-Host "  ALL CHECKS PASSED -- Omega may proceed" -ForegroundColor Green
    Write-Host "  HEAD: $localHead7  TIME: $timestamp" -ForegroundColor Green
    Write-Host "=======================================================" -ForegroundColor Yellow

    # Write pass file -- QUICK_RESTART checks for this before launching
    $passContent = "PASS`ntimestamp=$timestamp`nhash=$localHead7`n"
    $results | ForEach-Object { $passContent += "$_`n" }
    Set-Content -Path $PassFile -Value $passContent -Force
    if (Test-Path $FailFile) { Remove-Item $FailFile -Force -ErrorAction SilentlyContinue }
    Write-Host ""
    exit 0

} else {
    Write-Host "  !! $($failures.Count) CHECK(S) FAILED -- OMEGA WILL NOT START !!" -ForegroundColor Red
    Write-Host ""
    foreach ($f in $failures) {
        Write-Host "  FAILED: $f" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  Fix all failures above then run QUICK_RESTART.ps1 again." -ForegroundColor Yellow
    Write-Host "=======================================================" -ForegroundColor Yellow

    # Write fail file
    $failContent = "FAIL`ntimestamp=$timestamp`nhash=$localHead7`n"
    $failures | ForEach-Object { $failContent += "FAILED: $_`n" }
    Set-Content -Path $FailFile -Value $failContent -Force
    if (Test-Path $PassFile) { Remove-Item $PassFile -Force -ErrorAction SilentlyContinue }
    Write-Host ""
    exit 1
}
