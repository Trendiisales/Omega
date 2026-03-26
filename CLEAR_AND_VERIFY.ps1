# CLEAR_AND_VERIFY.ps1

Write-Host ""
Write-Host "=== OMEGA CLEAR + VERIFY ===" -ForegroundColor Cyan

# Step 1: Stop Omega
Write-Host ""
Write-Host "[1] Stopping Omega..." -ForegroundColor Yellow
try { Stop-Process -Name Omega -Force -ErrorAction Stop; Start-Sleep 2; Write-Host "  Stopped." -ForegroundColor Green }
catch { Write-Host "  Not running." -ForegroundColor Gray }

# Step 2: Clear ALL trade CSVs - every location the code writes to
Write-Host ""
Write-Host "[2] Clearing ALL trade CSVs..." -ForegroundColor Yellow

$csvDirs = @(
    "C:\Omega\logs\trades",
    "C:\Omega\logs\shadow\trades",
    "C:\Omega\logs",
    "C:\Omega",
    "C:\Omega\build\Release\logs\trades",
    "C:\Omega\build\Release"
)

$patterns = @(
    "omega_trade_opens*.csv",
    "omega_trade_closes*.csv",
    "omega_gold_trade_closes*.csv",
    "omega_shadow_trades*.csv",
    "omega_trades*.csv"
)

$deleted = 0
foreach ($dir in $csvDirs) {
    if (Test-Path $dir) {
        foreach ($pat in $patterns) {
            $files = Get-ChildItem -Path $dir -Filter $pat -ErrorAction SilentlyContinue
            foreach ($f in $files) {
                Remove-Item $f.FullName -Force
                Write-Host "  DELETED: $($f.FullName)" -ForegroundColor Red
                $deleted++
            }
        }
    }
}

if ($deleted -eq 0) {
    Write-Host "  No CSV files found to delete." -ForegroundColor Gray
} else {
    Write-Host "  Deleted $deleted file(s) total." -ForegroundColor Green
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
    Get-Content $logFile -Tail 5 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  Not yet created (will appear on startup)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "  All files in C:\Omega\logs:"
Get-ChildItem "C:\Omega\logs" -Recurse | ForEach-Object {
    Write-Host ("    " + $_.FullName + "  " + [math]::Round($_.Length/1KB,1) + "KB") -ForegroundColor Gray
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
    Write-Host "  Log created OK: $logFile" -ForegroundColor Green
    Get-Content $logFile -Tail 8 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  Log not yet written - check manually in 5s" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== DONE ===" -ForegroundColor Cyan
Write-Host "GUI: http://185.167.119.59:7779" -ForegroundColor Green
