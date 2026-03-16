$ErrorActionPreference = "Stop"

param(
    [string]$CsvPath = "C:\Omega\build\Release\logs\trades\omega_trade_closes.csv",
    [int]$MinTrades = 30
)

function New-Metrics {
    return [ordered]@{
        trades = 0
        wins = 0
        losses = 0
        net = 0.0
        gross_win = 0.0
        gross_loss = 0.0
    }
}

function Add-Trade([hashtable]$m, $row) {
    $pnl = [double]$row.pnl
    $m.trades++
    $m.net += $pnl
    if ($pnl -gt 0) {
        $m.wins++
        $m.gross_win += $pnl
    } else {
        $m.losses++
        $m.gross_loss += [math]::Abs($pnl)
    }
}

function Finish-Metrics($name, [hashtable]$m, [int]$minTrades) {
    if ($m.trades -eq 0) { return $null }
    $wr = if ($m.trades -gt 0) { 100.0 * $m.wins / $m.trades } else { 0.0 }
    $avgWin = if ($m.wins -gt 0) { $m.gross_win / $m.wins } else { 0.0 }
    $avgLoss = if ($m.losses -gt 0) { $m.gross_loss / $m.losses } else { 0.0 }
    $pf = if ($m.gross_loss -gt 0) { $m.gross_win / $m.gross_loss } else { 999.0 }
    $exp = if ($m.trades -gt 0) { $m.net / $m.trades } else { 0.0 }
    $status = if ($m.trades -lt $minTrades) {
        "INSUFFICIENT_SAMPLE"
    } elseif ($exp -gt 0 -and $pf -ge 1.20) {
        "KEEP"
    } else {
        "DISABLE_OR_RETUNE"
    }

    [pscustomobject]@{
        bucket = $name
        trades = $m.trades
        win_rate_pct = [math]::Round($wr, 2)
        net_pnl = [math]::Round($m.net, 2)
        avg_win = [math]::Round($avgWin, 2)
        avg_loss = [math]::Round($avgLoss, 2)
        profit_factor = [math]::Round($pf, 2)
        expectancy = [math]::Round($exp, 4)
        status = $status
    }
}

if (-not (Test-Path $CsvPath)) {
    $legacyCsv = "C:\Omega\build\Release\omega_shadow.csv"
    if (Test-Path $legacyCsv) {
        Write-Host "[INFO] New full trade CSV not found, falling back to legacy shadow CSV: $legacyCsv" -ForegroundColor Yellow
        $CsvPath = $legacyCsv
    } else {
        Write-Host "[ERROR] CSV not found: $CsvPath" -ForegroundColor Red
        exit 1
    }
}

$rows = Import-Csv -Path $CsvPath
if (-not $rows -or $rows.Count -eq 0) {
    Write-Host "[INFO] No trades yet in $CsvPath" -ForegroundColor Yellow
    exit 0
}

$overall = New-Metrics
$bySymbol = @{}
$byEngine = @{}
$bySymbolEngine = @{}

foreach ($r in $rows) {
    Add-Trade -m $overall -row $r

    $sym = "$($r.symbol)"
    if (-not $bySymbol.ContainsKey($sym)) { $bySymbol[$sym] = New-Metrics }
    Add-Trade -m $bySymbol[$sym] -row $r

    $eng = if ([string]::IsNullOrWhiteSpace($r.engine)) { "UNKNOWN_ENGINE" } else { "$($r.engine)" }
    if (-not $byEngine.ContainsKey($eng)) { $byEngine[$eng] = New-Metrics }
    Add-Trade -m $byEngine[$eng] -row $r

    $k = "$sym | $eng"
    if (-not $bySymbolEngine.ContainsKey($k)) { $bySymbolEngine[$k] = New-Metrics }
    Add-Trade -m $bySymbolEngine[$k] -row $r
}

Write-Host "`n=== OMEGA BASELINE REPORT ===" -ForegroundColor Cyan
Write-Host "CSV: $CsvPath"
Write-Host "Trades: $($rows.Count)"
Write-Host "Rule: KEEP only if trades >= $MinTrades, expectancy > 0, PF >= 1.20`n"

Write-Host "--- OVERALL ---" -ForegroundColor Cyan
Finish-Metrics -name "ALL" -m $overall -minTrades $MinTrades | Format-Table -AutoSize

Write-Host "`n--- BY SYMBOL ---" -ForegroundColor Cyan
$symReport = foreach ($k in ($bySymbol.Keys | Sort-Object)) {
    Finish-Metrics -name $k -m $bySymbol[$k] -minTrades $MinTrades
}
$symReport | Sort-Object -Property net_pnl -Descending | Format-Table -AutoSize

Write-Host "`n--- BY ENGINE ---" -ForegroundColor Cyan
$engReport = foreach ($k in ($byEngine.Keys | Sort-Object)) {
    Finish-Metrics -name $k -m $byEngine[$k] -minTrades $MinTrades
}
$engReport | Sort-Object -Property net_pnl -Descending | Format-Table -AutoSize

Write-Host "`n--- BY SYMBOL+ENGINE ---" -ForegroundColor Cyan
$seReport = foreach ($k in ($bySymbolEngine.Keys | Sort-Object)) {
    Finish-Metrics -name $k -m $bySymbolEngine[$k] -minTrades $MinTrades
}
$seReport | Sort-Object -Property net_pnl -Descending | Format-Table -AutoSize
