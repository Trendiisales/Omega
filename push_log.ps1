# ==============================================================================
#  OMEGA - Push current log to git as logs/latest.log
#  Called automatically as a POST_BUILD step from CMakeLists.txt
#  Can also be run manually: powershell -File C:\Omega\push_log.ps1 -RepoRoot C:\Omega
# ==============================================================================

param(
    [string]$RepoRoot = "C:\Omega"
)

$ErrorActionPreference = "Stop"

# Locate today's log
$today    = Get-Date -Format "yyyy-MM-dd"
$logPath  = "$RepoRoot\logs\omega_$today.log"
$destPath = "$RepoRoot\logs\latest.log"

if (-not (Test-Path $logPath)) {
    $logPath = "$RepoRoot\logs\omega.log"
}

if (-not (Test-Path $logPath)) {
    Write-Host "[push_log] No log file found - skipping push" -ForegroundColor Yellow
    exit 0
}

# Copy to latest.log
Copy-Item $logPath $destPath -Force
Write-Host "[push_log] Copied $logPath -> logs/latest.log" -ForegroundColor Cyan

# Git push
Set-Location $RepoRoot

git add -f logs/latest.log

$staged = git diff --cached --name-only
if (-not $staged) {
    Write-Host "[push_log] No changes to log - nothing to push" -ForegroundColor Yellow
    exit 0
}

$hash    = (git rev-parse --short HEAD 2>$null).Trim()
$ts      = Get-Date -Format "yyyy-MM-dd HH:mm"
$message = "log: latest.log post-build $hash $ts"

git commit -m $message
git push origin main

Write-Host "[push_log] Log pushed to origin/main" -ForegroundColor Green
