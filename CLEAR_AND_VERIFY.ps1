# CLEAR_AND_VERIFY.ps1
# Run from C:\Omega on the VPS after git pull + rebuild

Write-Host ""
Write-Host "=== OMEGA CLEAR + VERIFY ===" -ForegroundColor Cyan

# Step 1: Stop Omega
Write-Host ""
Write-Host "[1] Stopping Omega..." -ForegroundColor Yellow
try { Stop-Process -Name Omega -Force -ErrorAction Stop; Start-Sleep 2; Write-Host "  Stopped." -ForegroundColor Green }
catch { Write-Host "  Not running." -ForegroundColor Gray }

# Step 2: Clear trade CSVs
Write-Host ""
Write-Host "[2] Clearing trade history CSVs..." -ForegroundColor Yellow
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

# Step 3: Verify log directory
Write-Host ""
Write-Host "[3] Verifying C:\Omega\logs..." -ForegroundColor Yellow
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
    Write-Host "  EXISTS - $size bytes" -ForegroundColor Green
    Write-Host "  Last 5 lines:" -ForegroundColor Gray
    Get-Content $logFile -Tail 5 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  NOT YET CREATED (will appear on first Omega startup)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "  All files in C:\Omega\logs:"
Get-ChildItem "C:\Omega\logs" | ForEach-Object {
    Write-Host ("    " + $_.Name + "  " + [math]::Round($_.Length/1KB,1) + "KB  " + $_.LastWriteTime) -ForegroundColor Gray
}

# Step 4: Rebuild
Write-Host ""
Write-Host "[4] Rebuilding..." -ForegroundColor Yellow
cmake --build C:\Omega\build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "  BUILD FAILED - do not start" -ForegroundColor Red
    exit 1
}
Write-Host "  Build OK" -ForegroundColor Green

# Step 5: Start Omega
Write-Host ""
Write-Host "[5] Starting Omega..." -ForegroundColor Yellow
Start-Process "C:\Omega\build\Release\Omega.exe" -WorkingDirectory "C:\Omega"
Start-Sleep 5

# Step 6: Verify log created
Write-Host ""
Write-Host "[6] Post-start log check..." -ForegroundColor Yellow
if (Test-Path $logFile) {
    Write-Host "  Log created: $logFile" -ForegroundColor Green
    Get-Content $logFile -Tail 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  Log not yet written - wait 5s and check manually" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== DONE ===" -ForegroundColor Cyan
Write-Host "GUI: http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "P&L card now shows Closed / Floating / NZD rows with live pulse dot" -ForegroundColor Green
