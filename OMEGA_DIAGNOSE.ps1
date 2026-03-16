$ErrorActionPreference = "Stop"

param(
    [string]$LogRoot = "C:\Omega\build\Release\logs",
    [int]$Days = 5
)

function Get-RecentFiles([string]$Pattern, [int]$Take) {
    @(Get-ChildItem -Path $Pattern -ErrorAction SilentlyContinue |
        Sort-Object Name |
        Select-Object -Last $Take)
}

function Import-CsvSet([System.IO.FileInfo[]]$Files) {
    $rows = @()
    foreach ($f in $Files) {
        $rows += Import-Csv -Path $f.FullName
    }
    $rows
}

function To-Double($Value) {
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace("$Value")) { return 0.0 }
    try { return [double]$Value } catch { return 0.0 }
}

function New-Stats {
    [ordered]@{
        trades = 0
        wins = 0
        losses = 0
        net = 0.0
        gross_win = 0.0
        gross_loss = 0.0
        hold_sum = 0.0
    }
}

function Add-Stats([hashtable]$Stats, $Row) {
    $pnl = To-Double $Row.pnl
    $hold = To-Double $Row.hold_sec
    $Stats.trades++
    $Stats.net += $pnl
    $Stats.hold_sum += $hold
    if ($pnl -gt 0) {
        $Stats.wins++
        $Stats.gross_win += $pnl
    } else {
        $Stats.losses++
        $Stats.gross_loss += [math]::Abs($pnl)
    }
}

function Finish-Stats([string]$Name, [hashtable]$Stats) {
    if ($Stats.trades -eq 0) { return $null }
    $wr = 100.0 * $Stats.wins / $Stats.trades
    $avgWin = if ($Stats.wins -gt 0) { $Stats.gross_win / $Stats.wins } else { 0.0 }
    $avgLoss = if ($Stats.losses -gt 0) { $Stats.gross_loss / $Stats.losses } else { 0.0 }
    $pf = if ($Stats.gross_loss -gt 0) { $Stats.gross_win / $Stats.gross_loss } else { 999.0 }
    $exp = $Stats.net / $Stats.trades
    $avgHold = $Stats.hold_sum / $Stats.trades

    [pscustomobject]@{
        bucket = $Name
        trades = $Stats.trades
        win_rate_pct = [math]::Round($wr, 2)
        net_pnl = [math]::Round($Stats.net, 2)
        avg_win = [math]::Round($avgWin, 2)
        avg_loss = [math]::Round($avgLoss, 2)
        profit_factor = [math]::Round($pf, 2)
        expectancy = [math]::Round($exp, 4)
        avg_hold_sec = [math]::Round($avgHold, 1)
    }
}

function Get-StatsReport($Rows, [scriptblock]$KeySelector) {
    $buckets = @{}
    foreach ($r in $Rows) {
        $k = & $KeySelector $r
        if ([string]::IsNullOrWhiteSpace("$k")) { $k = "UNKNOWN" }
        if (-not $buckets.ContainsKey($k)) { $buckets[$k] = New-Stats }
        Add-Stats -Stats $buckets[$k] -Row $r
    }
    foreach ($k in ($buckets.Keys | Sort-Object)) {
        Finish-Stats -Name $k -Stats $buckets[$k]
    }
}

function Get-CountReport($Rows, [scriptblock]$KeySelector, [string]$CountName = "count") {
    $counts = @{}
    foreach ($r in $Rows) {
        $k = & $KeySelector $r
        if ([string]::IsNullOrWhiteSpace("$k")) { $k = "UNKNOWN" }
        if (-not $counts.ContainsKey($k)) { $counts[$k] = 0 }
        $counts[$k]++
    }
    foreach ($k in ($counts.Keys | Sort-Object)) {
        $obj = [ordered]@{ bucket = $k }
        $obj[$CountName] = $counts[$k]
        [pscustomobject]$obj
    }
}

function Get-LogLines([System.IO.FileInfo[]]$Files) {
    foreach ($f in $Files) {
        Get-Content -Path $f.FullName
    }
}

$tradeFiles = Get-RecentFiles "$LogRoot\trades\omega_trade_closes_*.csv" $Days
if (-not $tradeFiles) {
    $fallback = "$LogRoot\trades\omega_trade_closes.csv"
    if (Test-Path $fallback) { $tradeFiles = @(Get-Item $fallback) }
}

$shadowSignalFiles = Get-RecentFiles "$LogRoot\shadow\signals\omega_shadow_signals_*.csv" $Days
if (-not $shadowSignalFiles) {
    $fallback = "$LogRoot\shadow\omega_shadow_signals.csv"
    if (Test-Path $fallback) { $shadowSignalFiles = @(Get-Item $fallback) }
}

$shadowSignalEventFiles = Get-RecentFiles "$LogRoot\shadow\events\omega_shadow_signal_events_*.csv" $Days
if (-not $shadowSignalEventFiles) {
    $fallback = "$LogRoot\shadow\omega_shadow_signal_events.csv"
    if (Test-Path $fallback) { $shadowSignalEventFiles = @(Get-Item $fallback) }
}

$logFiles = Get-RecentFiles "$LogRoot\omega_*.log" $Days

if (-not $tradeFiles -and -not $shadowSignalFiles -and -not $shadowSignalEventFiles -and -not $logFiles) {
    Write-Host "[ERROR] No Omega logs found under $LogRoot" -ForegroundColor Red
    exit 1
}

$tradeRows = if ($tradeFiles) { Import-CsvSet $tradeFiles } else { @() }
$shadowSignalRows = if ($shadowSignalFiles) { Import-CsvSet $shadowSignalFiles } else { @() }
$shadowSignalEventRows = if ($shadowSignalEventFiles) { Import-CsvSet $shadowSignalEventFiles } else { @() }
$logLines = if ($logFiles) { @(Get-LogLines $logFiles) } else { @() }

$expectedSymbols = @(
    "US500.F", "USTEC.F", "USOIL.F", "GOLD.F",
    "DJ30.F", "GER30", "UK100", "ESTX50", "XAGUSD", "EURUSD", "UKBRENT"
)

Write-Host ""
Write-Host "=== OMEGA DIAGNOSIS ===" -ForegroundColor Cyan
Write-Host "Log root: $LogRoot"
Write-Host "Trade files: $($tradeFiles.Count)"
Write-Host "Shadow signal files: $($shadowSignalFiles.Count)"
Write-Host "Shadow signal event files: $($shadowSignalEventFiles.Count)"
Write-Host "Rolling logs: $($logFiles.Count)"
Write-Host ""

if ($tradeRows.Count -gt 0) {
    $overall = New-Stats
    foreach ($r in $tradeRows) { Add-Stats -Stats $overall -Row $r }

    Write-Host "--- OVERALL TRADE PERFORMANCE ---" -ForegroundColor Cyan
    Finish-Stats -Name "ALL" -Stats $overall | Format-Table -AutoSize

    Write-Host "`n--- BY SYMBOL ---" -ForegroundColor Cyan
    Get-StatsReport $tradeRows { param($r) "$($r.symbol)" } |
        Sort-Object net_pnl -Descending |
        Format-Table -AutoSize

    Write-Host "`n--- BY ENGINE ---" -ForegroundColor Cyan
    Get-StatsReport $tradeRows { param($r) if ([string]::IsNullOrWhiteSpace("$($r.engine)")) { "UNKNOWN_ENGINE" } else { "$($r.engine)" } } |
        Sort-Object net_pnl -Descending |
        Format-Table -AutoSize

    $goldTrades = @($tradeRows | Where-Object { $_.symbol -eq "GOLD.F" })
    if ($goldTrades.Count -gt 0) {
        Write-Host "`n--- GOLD SUMMARY ---" -ForegroundColor Yellow
        $goldOverall = New-Stats
        foreach ($r in $goldTrades) { Add-Stats -Stats $goldOverall -Row $r }
        Finish-Stats -Name "GOLD.F" -Stats $goldOverall | Format-Table -AutoSize

        Write-Host "`n--- GOLD BY ENGINE ---" -ForegroundColor Yellow
        Get-StatsReport $goldTrades { param($r) if ([string]::IsNullOrWhiteSpace("$($r.engine)")) { "UNKNOWN_ENGINE" } else { "$($r.engine)" } } |
            Sort-Object net_pnl -Descending |
            Format-Table -AutoSize

        Write-Host "`n--- GOLD BY EXIT REASON ---" -ForegroundColor Yellow
        Get-StatsReport $goldTrades { param($r) if ([string]::IsNullOrWhiteSpace("$($r.exit_reason)")) { "UNKNOWN_EXIT" } else { "$($r.exit_reason)" } } |
            Sort-Object net_pnl -Descending |
            Format-Table -AutoSize

        Write-Host "`n--- GOLD BY REGIME ---" -ForegroundColor Yellow
        Get-StatsReport $goldTrades { param($r) if ([string]::IsNullOrWhiteSpace("$($r.regime)")) { "UNKNOWN_REGIME" } else { "$($r.regime)" } } |
            Sort-Object net_pnl -Descending |
            Format-Table -AutoSize

        Write-Host "`n--- GOLD BY SIDE ---" -ForegroundColor Yellow
        Get-StatsReport $goldTrades { param($r) "$($r.side)" } |
            Sort-Object net_pnl -Descending |
            Format-Table -AutoSize
    }
} else {
    Write-Host "--- OVERALL TRADE PERFORMANCE ---" -ForegroundColor Cyan
    Write-Host "No trade close CSV rows found."
}

if ($shadowSignalEventRows.Count -gt 0) {
    Write-Host "`n--- SHADOW SIGNAL EVENTS ---" -ForegroundColor Cyan
    Get-CountReport $shadowSignalEventRows {
        param($r)
        "$($r.symbol) | $($r.verdict)"
    } "signals" | Sort-Object signals -Descending | Format-Table -AutoSize

    Write-Host "`n--- BLOCKED REASONS BY SYMBOL ---" -ForegroundColor Cyan
    $blocked = @($shadowSignalEventRows | Where-Object { $_.verdict -eq "BLOCKED" })
    if ($blocked.Count -gt 0) {
        Get-CountReport $blocked {
            param($r)
            "$($r.symbol) | $($r.reason)"
        } "blocked" | Sort-Object blocked -Descending | Format-Table -AutoSize
    } else {
        Write-Host "No BLOCKED shadow signals recorded."
    }

    Write-Host "`n--- ELIGIBLE SIGNALS BY SYMBOL ---" -ForegroundColor Cyan
    $eligible = @($shadowSignalEventRows | Where-Object { $_.verdict -eq "ELIGIBLE" })
    if ($eligible.Count -gt 0) {
        Get-CountReport $eligible {
            param($r)
            "$($r.symbol)"
        } "eligible" | Sort-Object eligible -Descending | Format-Table -AutoSize
    } else {
        Write-Host "No ELIGIBLE shadow signals recorded."
    }
} elseif ($shadowSignalRows.Count -gt 0) {
    Write-Host "`n--- SHADOW SIGNAL VERDICTS ---" -ForegroundColor Cyan
    Get-CountReport $shadowSignalRows {
        param($r)
        "$($r.symbol) | $($r.verdict)"
    } "signals" | Sort-Object signals -Descending | Format-Table -AutoSize
} else {
    Write-Host "`n--- SHADOW SIGNAL VERDICTS ---" -ForegroundColor Cyan
    Write-Host "No shadow signal event rows found."
}

$tickCounts = @{}
$learnedExt = @{}
$unknownSymbols = @()
if ($logLines.Count -gt 0) {
    foreach ($line in $logLines) {
        if ($line -match '^\[TICK\]\s+(\S+)\s') {
            $sym = $matches[1]
            if (-not $tickCounts.ContainsKey($sym)) { $tickCounts[$sym] = 0 }
            $tickCounts[$sym]++
        }
        if ($line -match '\[OMEGA-SECURITY\]\s+learned ext id\s+(\S+)\s+->\s+(\d+)') {
            $learnedExt[$matches[1]] = $matches[2]
        }
        if ($line -match '\[OMEGA-MD\]\s+Unknown string symbol') {
            $unknownSymbols += $line
        }
        if ($line -match '\[OMEGA-MD\]\s+Unknown numeric ID') {
            $unknownSymbols += $line
        }
    }

    Write-Host "`n--- FEED / SUBSCRIPTION CHECK ---" -ForegroundColor Cyan
    $feedRows = foreach ($sym in $expectedSymbols) {
        [pscustomobject]@{
            symbol = $sym
            ticks = if ($tickCounts.ContainsKey($sym)) { $tickCounts[$sym] } else { 0 }
            learned_ext_id = if ($learnedExt.ContainsKey($sym)) { $learnedExt[$sym] } else { "" }
        }
    }
    $feedRows | Format-Table -AutoSize

    if ($unknownSymbols.Count -gt 0) {
        Write-Host "`n--- UNKNOWN MARKET-DATA SYMBOLS / IDS ---" -ForegroundColor Yellow
        $unknownSymbols | Select-Object -First 20
    }
}

Write-Host "`n--- SYMBOL DIAGNOSIS ---" -ForegroundColor Cyan
$tradeCountBySymbol = @{}
$tradePnlBySymbol = @{}
foreach ($r in $tradeRows) {
    $sym = "$($r.symbol)"
    if (-not $tradeCountBySymbol.ContainsKey($sym)) {
        $tradeCountBySymbol[$sym] = 0
        $tradePnlBySymbol[$sym] = 0.0
    }
    $tradeCountBySymbol[$sym]++
    $tradePnlBySymbol[$sym] += To-Double $r.pnl
}

$blockedBySymbol = @{}
$eligibleBySymbol = @{}
$topBlockedReason = @{}
if ($shadowSignalEventRows.Count -gt 0 -or $shadowSignalRows.Count -gt 0) {
    foreach ($sym in $expectedSymbols) {
        $rows = if ($shadowSignalEventRows.Count -gt 0) {
            @($shadowSignalEventRows | Where-Object { $_.symbol -eq $sym })
        } else {
            @($shadowSignalRows | Where-Object { $_.symbol -eq $sym })
        }
        $blockedRows = @($rows | Where-Object { $_.verdict -eq "BLOCKED" })
        $eligibleRows = @($rows | Where-Object { $_.verdict -eq "ELIGIBLE" })
        $blockedBySymbol[$sym] = $blockedRows.Count
        $eligibleBySymbol[$sym] = $eligibleRows.Count
        if ($blockedRows.Count -gt 0) {
            $topBlockedReason[$sym] = ($blockedRows | Group-Object reason | Sort-Object Count -Descending | Select-Object -First 1).Name
        } else {
            $topBlockedReason[$sym] = ""
        }
    }
}

$diagnosisRows = foreach ($sym in $expectedSymbols) {
    $ticks = if ($tickCounts.ContainsKey($sym)) { $tickCounts[$sym] } else { 0 }
    $trades = if ($tradeCountBySymbol.ContainsKey($sym)) { $tradeCountBySymbol[$sym] } else { 0 }
    $net = if ($tradePnlBySymbol.ContainsKey($sym)) { [math]::Round($tradePnlBySymbol[$sym], 2) } else { 0.0 }
    $eligible = if ($eligibleBySymbol.ContainsKey($sym)) { $eligibleBySymbol[$sym] } else { 0 }
    $blocked = if ($blockedBySymbol.ContainsKey($sym)) { $blockedBySymbol[$sym] } else { 0 }
    $reason = if ($topBlockedReason.ContainsKey($sym)) { $topBlockedReason[$sym] } else { "" }

    $diagnosis =
        if ($ticks -eq 0) { "NO_FEED_OR_BAD_SYMBOL_MAPPING" }
        elseif ($trades -eq 0 -and $eligible -eq 0 -and $blocked -gt 0) { "ENTRY_GATES_TOO_TIGHT" }
        elseif ($trades -eq 0 -and $eligible -gt 0) { "SIGNALS_SEEN_BUT_NOT_EXECUTED" }
        elseif ($trades -gt 0 -and $net -lt 0) { "TRADING_BUT_LOSING" }
        elseif ($trades -gt 0 -and $net -ge 0) { "TRADING_OK_OR_FLAT" }
        else { "NO_EVIDENCE_YET" }

    [pscustomobject]@{
        symbol = $sym
        ticks = $ticks
        trades = $trades
        net_pnl = $net
        eligible = $eligible
        blocked = $blocked
        top_block_reason = $reason
        diagnosis = $diagnosis
    }
}
$diagnosisRows | Format-Table -AutoSize

Write-Host ""
Write-Host "Notes:" -ForegroundColor Yellow
Write-Host "1. Gold shadow-signal rows describe the CRTP gold engine, not the active GoldEngineStack executor."
Write-Host "2. For gold, trust the trade-close CSV sections above more than the shadow-signal section."
Write-Host "3. If an extended symbol has zero ticks, inspect SecurityList learn lines and broker symbol aliases first."
