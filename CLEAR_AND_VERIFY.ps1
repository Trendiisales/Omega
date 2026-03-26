# ─────────────────────────────────────────────────────────────────────────────
# CLEAR_AND_VERIFY.ps1
# Run from C:\Omega on the VPS after git pull + rebuild
# 1. Stops Omega
# 2. Clears old trade CSV history
# 3. Verifies log directory and today's log
# 4. Rebuilds and restarts
# ─────────────────────────────────────────────────────────────────────────────

Write-Host "`n=== OMEGA CLEAR + VERIFY ===" -ForegroundColor Cyan

# Step 1: Stop Omega
Write-Host "`n[1] Stopping Omega..." -ForegroundColor Yellow
try { Stop-Process -Name Omega -Force -ErrorAction Stop; Start-Sleep 2; Write-Host "  Stopped." -ForegroundColor Green }
catch { Write-Host "  Not running." -ForegroundColor Gray }

# Step 2: Pull latest
Write-Host "`n[2] Pulling latest code..." -ForegroundColor Yellow
Set-Location C:\Omega
$pull = git pull 2>&1
Write-Host "  $pull" -ForegroundColor Gray

# Step 3: Clear trade CSVs
Write-Host "`n[3] Clearing trade history CSVs..." -ForegroundColor Yellow
$csvPaths = @(
    "C:\Omega\logs\omega_trade_opens.csv",
    "C:\Omega\logs\omega_trade_closes.csv",
    "C:\Omega\logs\omega_trades.csv",
    "C:\Omega\omega_trade_opens.csv",
    "C:\Omega\omega_trade_closes.csv",
    "C:\Omega\omega_trades.csv",
    "C:\Omega\build\Release\omega_trade_opens.csv",
    "C:\Omega\build\Release\omega_trade_closes.csv",
    "C:\Omega\build\Release\omega_trades.csv"
)
foreach ($csv in $csvPaths) {
    if (Test-Path $csv) {
        Remove-Item $csv -Force
        Write-Host "  DELETED: $csv" -ForegroundColor Red
    } else {
        Write-Host "  not found: $csv" -ForegroundColor DarkGray
    }
}

# Step 4: Verify log directory
Write-Host "`n[4] Verifying C:\Omega\logs..." -ForegroundColor Yellow
if (-not (Test-Path "C:\Omega\logs")) {
    New-Item -ItemType Directory -Path "C:\Omega\logs" | Out-Null
    Write-Host "  CREATED C:\Omega\logs" -ForegroundColor Green
} else {
    Write-Host "  EXISTS C:\Omega\logs" -ForegroundColor Green
}

$today = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd")
$logFile = "C:\Omega\logs\omega_$today.log"
Write-Host "  Expected log: $logFile"
if (Test-Path $logFile) {
    $size = (Get-Item $logFile).Length
    Write-Host "  EXISTS — $size bytes" -ForegroundColor Green
    Write-Host "  Last 5 lines:" -ForegroundColor Gray
    Get-Content $logFile -Tail 5 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  NOT YET CREATED (will appear on first Omega startup)" -ForegroundColor Yellow
}

# List all log files
Write-Host "`n  All files in C:\Omega\logs:"
Get-ChildItem "C:\Omega\logs" | ForEach-Object {
    Write-Host "    $($_.Name)  $([math]::Round($_.Length/1KB,1))KB  $($_.LastWriteTime)" -ForegroundColor Gray
}

# Step 5: Rebuild
Write-Host "`n[5] Rebuilding..." -ForegroundColor Yellow
$buildResult = cmake --build C:\Omega\build --config Release 2>&1
$lastLines = $buildResult | Select-Object -Last 5
$lastLines | ForEach-Object { Write-Host "  $_" -ForegroundColor Gray }
if ($buildResult -match "Error|error" -and $buildResult -notmatch "0 Error") {
    Write-Host "`n  BUILD FAILED — do not start" -ForegroundColor Red
    exit 1
}
Write-Host "  Build OK" -ForegroundColor Green

# Step 6: Start Omega
Write-Host "`n[6] Starting Omega..." -ForegroundColor Yellow
Start-Process "C:\Omega\build\Release\Omega.exe" -WorkingDirectory "C:\Omega"
Start-Sleep 3

# Step 7: Verify startup
Write-Host "`n[7] Post-start log check..." -ForegroundColor Yellow
Start-Sleep 2
if (Test-Path $logFile) {
    Write-Host "  Log created: $logFile" -ForegroundColor Green
    Get-Content $logFile -Tail 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  Log not yet written — wait 5s and check manually" -ForegroundColor Yellow
}

Write-Host "`n=== DONE ===" -ForegroundColor Cyan
Write-Host "GUI:  http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "Check P&L card — should show Closed / Floating / NZD rows with live pulse dot" -ForegroundColor Green
