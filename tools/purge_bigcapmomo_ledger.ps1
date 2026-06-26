# purge_bigcapmomo_ledger.ps1  (S-2026-06-26)
# Removes the BAD bare-tag `BigCapMomo` trades from the trade-close ledger(s).
# Keeps `BigCapMomoCons` (the selective, kept config) and every other engine.
# CSV-aware (Import-Csv respects quoted fields) BUT format-preserving: it writes
# back the ORIGINAL raw lines for kept rows, not an Export-Csv rewrite.
# In-memory engine_pnl (top-line) still holds these until the next Omega.exe restart.

$ErrorActionPreference = 'Stop'
$tradesDir = 'C:\Omega\logs\trades'
$today     = (Get-Date).ToString('yyyy-MM-dd')
$stamp     = (Get-Date).ToString('yyyyMMdd_HHmmss')
$badTag    = 'BigCapMomo'   # EXACT match -- 'BigCapMomoCons' is preserved

# GUI (buildHistoryJson) reads today's daily file if present, else the cumulative.
$targets = @(
    (Join-Path $tradesDir 'omega_trade_closes.csv'),
    (Join-Path $tradesDir ("omega_trade_closes_$today.csv"))
)

foreach ($f in $targets) {
    if (-not (Test-Path $f)) { Write-Output "SKIP (absent): $f"; continue }

    $raw = Get-Content $f
    if ($raw.Count -lt 2) { Write-Output "SKIP (empty): $f"; continue }
    $hdr     = $raw[0]
    $dataRaw = $raw[1..($raw.Count-1)]

    $objs = @(Import-Csv $f)   # quote-correct objects, same row order as $dataRaw

    # Guard: a mismatch means an embedded newline desynced raw<->parsed. Abort.
    if ($objs.Count -ne $dataRaw.Count) {
        Write-Output ("ABORT (row count mismatch raw={0} parsed={1}): {2}" -f $dataRaw.Count,$objs.Count,$f)
        continue
    }

    $keepRaw = New-Object System.Collections.Generic.List[string]
    $badN = 0; $badNet = 0.0; $keepNet = 0.0
    for ($i=0; $i -lt $objs.Count; $i++) {
        if ($objs[$i].engine -eq $badTag) {
            $badN++; $badNet += [double]$objs[$i].net_pnl
        } else {
            $keepRaw.Add($dataRaw[$i]) | Out-Null
            $keepNet += [double]$objs[$i].net_pnl
        }
    }

    if ($badN -eq 0) { Write-Output "NOCHANGE (no '$badTag' rows): $f"; continue }

    # Archive original, then write header + kept original lines (format preserved)
    $bak = "$f.pre_bigcappurge_$stamp"
    Copy-Item $f $bak -Force
    $out = ,$hdr + $keepRaw.ToArray()
    Set-Content -Path $f -Value $out -Encoding ASCII

    Write-Output ("FIXED  : {0}" -f $f)
    Write-Output ("  archived -> {0}" -f $bak)
    Write-Output ("  removed  : {0} rows  net {1}" -f $badN, [math]::Round($badNet,2))
    Write-Output ("  kept     : {0} rows  net {1}" -f $keepRaw.Count, [math]::Round($keepNet,2))
}

Write-Output ''
Write-Output 'NOTE: top-line engine_pnl (in-memory g_omegaLedger) clears on the next Omega.exe restart (pending 29028fe5 deploy).'
