# CLEAR_AND_VERIFY.ps1
# Audited against main.cpp + OmegaTelemetryServer.cpp source
# Covers every write path and every read/fallback path the GUI checks

Write-Host ""
Write-Host "=== OMEGA CLEAR + VERIFY ===" -ForegroundColor Cyan

# -- Step 1: Stop Omega --------------------------------------------------------
Write-Host ""
Write-Host "[1] Stopping Omega..." -ForegroundColor Yellow
try {
    Stop-Process -Name Omega -Force -ErrorAction Stop
    Start-Sleep 2
    Write-Host "  Stopped." -ForegroundColor Green
} catch {
    Write-Host "  Not running." -ForegroundColor Gray
}

# -- Step 2: PRESERVE trade CSVs  -  only wipe open-position state files ---------
Write-Host ""
Write-Host "[2] Preserving ALL trade history CSVs..." -ForegroundColor Green
Write-Host "  NOTE: Historical trade data is NEVER deleted." -ForegroundColor Cyan
Write-Host "  Only open-position state files are cleared on restart." -ForegroundColor Cyan

# Only delete the opens log (tracks currently open positions  -  stale after restart)
# NEVER delete closes, shadow, gold, or tod files  -  these are the historical record
$stateOnlyPatterns = @(
    "omega_trade_opens.csv",
    "omega_trade_opens_*.csv"
)

$deleted = 0
$stateDirs = @(
    "C:\Omega\logs\trades",
    "C:\Omega\build\Release\logs\trades"
)
foreach ($dir in $stateDirs) {
    if (Test-Path $dir) {
        foreach ($pat in $stateOnlyPatterns) {
            $files = Get-ChildItem -Path $dir -Filter $pat -ErrorAction SilentlyContinue
            foreach ($f in $files) {
                Remove-Item $f.FullName -Force
                Write-Host "  CLEARED (state only): $($f.FullName)" -ForegroundColor Yellow
                $deleted++
            }
        }
    }
}

if ($deleted -eq 0) {
    Write-Host "  No state files to clear." -ForegroundColor DarkGray
} else {
    Write-Host "  Cleared $deleted state file(s). Trade history preserved." -ForegroundColor Green
}

# -- Step 3: Show what historical data exists ----------------------------------
Write-Host ""
Write-Host "[3] Historical trade data on disk:" -ForegroundColor Yellow
$histPatterns = @("omega_trade_closes*.csv","omega_shadow*.csv","omega_gold*.csv","omega_shadow_trades*.csv")
$histDirs = @("C:\Omega\logs\trades","C:\Omega\logs\shadow","C:\Omega\logs\shadow\trades","C:\Omega\logs\gold")
$totalRows = 0
foreach ($dir in $histDirs) {
    if (Test-Path $dir) {
        foreach ($pat in $histPatterns) {
            $files = Get-ChildItem -Path $dir -Filter $pat -ErrorAction SilentlyContinue
            foreach ($f in $files) {
                $rows = (Get-Content $f.FullName -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
                $totalRows += $rows
                Write-Host "  PRESERVED: $($f.FullName)  $([math]::Round($f.Length/1KB,1))KB  ($rows rows)" -ForegroundColor Green
            }
        }
    }
}
if ($totalRows -eq 0) {
    Write-Host "  No historical trade data yet  -  will accumulate from first trade." -ForegroundColor Yellow
}

# -- Step 4: Verify log directory ---------------------------------------------
Write-Host ""
Write-Host "[4] Verifying C:\Omega\logs..." -ForegroundColor Yellow
foreach ($d in @("C:\Omega\logs","C:\Omega\logs\trades","C:\Omega\logs\gold","C:\Omega\logs\shadow\trades")) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d | Out-Null
        Write-Host "  CREATED: $d" -ForegroundColor Green
    } else {
        Write-Host "  EXISTS:  $d" -ForegroundColor Gray
    }
}

$today = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd")
$logFile = "C:\Omega\logs\omega_$today.log"
Write-Host ""
Write-Host "  Expected log file: $logFile"
if (Test-Path $logFile) {
    $sz = (Get-Item $logFile).Length
    Write-Host "  EXISTS - $sz bytes" -ForegroundColor Green
    Write-Host "  Last 5 lines:"
    Get-Content $logFile -Tail 5 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  Not yet created (created on first Omega startup)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "  Full C:\Omega\logs tree:"
Get-ChildItem "C:\Omega\logs" -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    $size = if ($_.PSIsContainer) { "[DIR]" } else { "$([math]::Round($_.Length/1KB,1))KB" }
    Write-Host ("    " + $_.FullName + "  " + $size) -ForegroundColor Gray
}

# -- Step 5: Rebuild -----------------------------------------------------------
Write-Host ""
Write-Host "[5] Rebuilding..." -ForegroundColor Yellow
cmake --build C:\Omega\build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "  BUILD FAILED - not starting Omega" -ForegroundColor Red
    exit 1
}
Write-Host "  Build OK" -ForegroundColor Green

# -- Step 6: Start Omega -------------------------------------------------------
Write-Host ""
Write-Host "[6] Starting Omega..." -ForegroundColor Yellow
& "C:\Omega\build\Release\Omega.exe"
Start-Sleep 6

# -- Step 7: Post-start verification ------------------------------------------
Write-Host ""
Write-Host "[7] Post-start checks..." -ForegroundColor Yellow

if (Test-Path $logFile) {
    $sz = (Get-Item $logFile).Length
    Write-Host "  Log OK: $logFile ($sz bytes)" -ForegroundColor Green
    Get-Content $logFile -Tail 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} else {
    Write-Host "  Log not yet written - wait 5s and recheck" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "  Checking for new trade CSVs created on startup:"
foreach ($dir in @("C:\Omega\logs\trades","C:\Omega\logs\gold")) {
    if (Test-Path $dir) {
        Get-ChildItem $dir -Filter "*.csv" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "  CREATED: $($_.FullName)  $([math]::Round($_.Length/1KB,1))KB" -ForegroundColor Green
        }
    }
}

Write-Host ""
Write-Host "=== DONE ===" -ForegroundColor Cyan
Write-Host "GUI: http://185.167.119.59:7779" -ForegroundColor Green
Write-Host "Trade history is PRESERVED across restarts." -ForegroundColor Green
