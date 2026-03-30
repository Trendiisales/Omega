# ==============================================================================
#  OMEGA — Push current log to git as logs/latest.log
#  Called automatically as a POST_BUILD step from CMakeLists.txt
#  Can also be run manually: powershell -File C:\Omega\push_log.ps1
# ==============================================================================

$ErrorActionPreference = "Stop"
$RepoRoot = "C:\Omega"

# ── Locate today's log ────────────────────────────────────────────────────────
$today    = Get-Date -Format "yyyy-MM-dd"
$logPath  = "$RepoRoot\logs\omega_$today.log"
$destPath = "$RepoRoot\logs\latest.log"

if (-not (Test-Path $logPath)) {
    # Fall back to plain omega.log if dated file doesn't exist yet
    $logPath = "$RepoRoot\logs\omega.log"
}

if (-not (Test-Path $logPath)) {
    Write-Host "[push_log] No log file found — skipping push" -ForegroundColor Yellow
    exit 0
}

# ── Copy to latest.log ────────────────────────────────────────────────────────
Copy-Item $logPath $destPath -Force
Write-Host "[push_log] Copied $logPath → logs/latest.log" -ForegroundColor Cyan

# ── Git push ──────────────────────────────────────────────────────────────────
Set-Location $RepoRoot

# Force-add: bypasses the logs/ and *.log entries in .gitignore
git add -f logs/latest.log

# Only commit if there is actually something staged
$staged = git diff --cached --name-only
if (-not $staged) {
    Write-Host "[push_log] No changes to log — nothing to push" -ForegroundColor Yellow
    exit 0
}

$hash    = (git rev-parse --short HEAD 2>$null).Trim()
$message = "log: latest.log updated post-build $hash $(Get-Date -Format 'yyyy-MM-dd HH:mm')"

git commit -m $message
git push origin main

Write-Host "[push_log] Log pushed to origin/main" -ForegroundColor Green
