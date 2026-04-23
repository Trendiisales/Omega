# =============================================================================
#  backtest\run_dpe_sweep.ps1
#
#  DomPersist walk-forward parameter sweep driver for Windows VPS.
#
#  Runs a 96-cell grid (6 thresholds x 4 persist_ticks x 4 session filters),
#  building the harness once per cell and replaying against
#  C:\Omega\logs\l2_ticks_2026-04-*.csv .
#
#  Output layout (under $OutDir, default: C:\Omega\backtest\dpe_sweep):
#      trades\i{imb}_p{persist}_s{session}.csv     # per-trade output
#      summary.csv                                 # one row per cell
#      build.log                                   # compile output
#      run.log                                     # replay output
#
#  Prerequisites:
#      - MSYS2 / mingw-w64 g++ on PATH  (or set $Env:GXX)
#      - C:\Omega\include\DomPersistEngine.hpp must be the Session-15 patched
#        copy with the #ifdef override guards. Driver verifies by grep.
#      - Shim headers at C:\Omega\backtest\shim\ (harness-local stubs for
#        OmegaTradeLedger.hpp and BracketTrendState.hpp).
#
#  Usage (Windows PowerShell 5.1, built-in):
#      cd C:\Omega\backtest
#      powershell -File run_dpe_sweep.ps1
#      powershell -File run_dpe_sweep.ps1 -OutDir D:\dpe_results -DryRun
#      powershell -File run_dpe_sweep.ps1 -ImbList 0.10,0.15 -PersistList 20
#
#  Usage (PowerShell 7+, if installed):
#      pwsh -File run_dpe_sweep.ps1 ...
#
#  Session 15 - no engine behaviour changes; override macros only.
#  Session 15 rev2 - PS 5.1 compatibility: -Encoding ascii on all CSV writes,
#                    renamed $matches (auto-var) to $hits in the log loop,
#                    invocation docs updated to powershell.exe.
# =============================================================================

[CmdletBinding()]
param(
    [string]   $RepoRoot    = "C:\Omega",
    [string]   $LogDir      = "C:\Omega\logs",
    [string]   $OutDir      = "C:\Omega\backtest\dpe_sweep",
    [string[]] $LogPattern  = @("l2_ticks_2026-04-*.csv"),
    [double[]] $ImbList     = @(0.05, 0.08, 0.10, 0.12, 0.15, 0.20),
    [int[]]    $PersistList = @(5, 10, 20, 40),
    [int[]]    $SessionList = @(1, 2, 3, 4),
    [switch]   $DryRun,
    [switch]   $KeepBinaries,
    [string]   $Gxx         = $Env:GXX
)

$ErrorActionPreference = "Stop"

# -------- Resolve g++ ---------------------------------------------------------
if (-not $Gxx) { $Gxx = "g++" }
try {
    $gxxVer = & $Gxx --version 2>&1 | Select-Object -First 1
    Write-Host "[BUILD] Using: $gxxVer"
}
catch {
    Write-Error "g++ not found. Install mingw-w64 via MSYS2 or set -Gxx <path>"
    exit 1
}

# -------- Paths ---------------------------------------------------------------
$HarnessSrc    = Join-Path $RepoRoot "backtest\dom_persist_walk_forward.cpp"
$IncludeDir    = Join-Path $RepoRoot "include"
$ShimDir       = Join-Path $RepoRoot "backtest\shim"
$EnginePatched = Join-Path $IncludeDir "DomPersistEngine.hpp"

foreach ($p in @($HarnessSrc, $IncludeDir, $ShimDir, $EnginePatched)) {
    if (-not (Test-Path $p)) {
        Write-Error "Missing required path: $p"
        exit 1
    }
}

# Verify engine has the Session-15 override guards (do not run against
# un-patched engine -- params would silently be ignored and all cells
# would produce identical results).
$engineText = Get-Content $EnginePatched -Raw
if ($engineText -notmatch "DPE_IMB_THRESHOLD_OVERRIDE") {
    Write-Error "Engine header missing S15 override guards. Re-deploy patched DomPersistEngine.hpp first."
    exit 1
}
if ($engineText -notmatch "DPE_PERSIST_TICKS_OVERRIDE") {
    Write-Error "Engine header missing DPE_PERSIST_TICKS_OVERRIDE guard. Re-deploy patched DomPersistEngine.hpp first."
    exit 1
}
Write-Host "[OK] Engine header has S15 override guards"

# -------- Collect input CSVs --------------------------------------------------
# Note: renamed from $matches (PowerShell automatic variable) to $hits.
$InputCsvs = @()
foreach ($pat in $LogPattern) {
    $hits = Get-ChildItem -Path $LogDir -Filter $pat -ErrorAction SilentlyContinue |
            Sort-Object Name
    $InputCsvs += $hits
}
$InputCsvs = $InputCsvs | Sort-Object Name -Unique

if ($InputCsvs.Count -eq 0) {
    Write-Error "No input CSVs found in $LogDir matching $($LogPattern -join ', ')"
    exit 1
}

Write-Host "[OK] Found $($InputCsvs.Count) input CSV(s):"
foreach ($f in $InputCsvs) {
    $sz = "{0:N1} MB" -f ($f.Length / 1MB)
    Write-Host "     $($f.Name)  $sz"
}

# -------- Prepare output dirs ------------------------------------------------
$TradesDir = Join-Path $OutDir "trades"
$BinDir    = Join-Path $OutDir "bin"
New-Item -ItemType Directory -Path $OutDir    -Force | Out-Null
New-Item -ItemType Directory -Path $TradesDir -Force | Out-Null
New-Item -ItemType Directory -Path $BinDir    -Force | Out-Null

$BuildLog   = Join-Path $OutDir "build.log"
$RunLog     = Join-Path $OutDir "run.log"
$SummaryCsv = Join-Path $OutDir "summary.csv"

# PS 5.1 default encoding for Set-Content/Add-Content is system code page
# (CP1252 on Western Windows) and Out-File defaults to UTF-16 LE. Force ASCII
# for all text outputs so the Python analyzer and any downstream csv reader
# gets byte-clean content with no BOM. All content here is pure ASCII.
"build log started $(Get-Date -Format o)" | Set-Content $BuildLog -Encoding ascii
"run log started $(Get-Date -Format o)"   | Set-Content $RunLog   -Encoding ascii

# CSV header
"imb_thresh,persist_ticks,session_filter,trades,wins,wr_pct,pnl_usd," +
"maxdd_usd,expectancy_usd,build_s,run_s,status" |
    Set-Content $SummaryCsv -Encoding ascii

# -------- Sweep ---------------------------------------------------------------
$totalCells = $ImbList.Count * $PersistList.Count * $SessionList.Count
$cellIdx    = 0
$startTime  = Get-Date

foreach ($imb in $ImbList) {
    foreach ($persist in $PersistList) {
        foreach ($session in $SessionList) {
            $cellIdx += 1
            $cellTag = "i{0:F2}_p{1}_s{2}" -f $imb, $persist, $session
            $binPath = Join-Path $BinDir "dpe_wf_$cellTag.exe"
            $tradesPath = Join-Path $TradesDir "$cellTag.csv"

            $elapsed = (Get-Date) - $startTime
            $eta = if ($cellIdx -gt 1) {
                $per = $elapsed.TotalSeconds / ($cellIdx - 1)
                $remain = $per * ($totalCells - $cellIdx + 1)
                "{0:N0}s remaining" -f $remain
            } else { "unknown" }

            Write-Host "[$cellIdx/$totalCells] $cellTag  (ETA $eta)"

            if ($DryRun) {
                Write-Host "  [DRY] would build and run"
                "$imb,$persist,$session,0,0,0.00,0.00,0.00,0.00,0,0,DRY" |
                    Add-Content $SummaryCsv -Encoding ascii
                continue
            }

            # --- Build ----
            $buildT0 = Get-Date
            $gxxArgs = @(
                "-std=c++17", "-O2",
                "-I$IncludeDir", "-I$ShimDir",
                "-DDPE_IMB_THRESHOLD_OVERRIDE=$imb",
                "-DDPE_PERSIST_TICKS_OVERRIDE=$persist",
                "-DDPE_SESSION_FILTER=$session",
                $HarnessSrc,
                "-o", $binPath
            )
            "`n=== BUILD $cellTag ===" | Add-Content $BuildLog -Encoding ascii
            $buildOut = & $Gxx @gxxArgs 2>&1
            $buildExit = $LASTEXITCODE
            $buildOut  | Out-String | Add-Content $BuildLog -Encoding ascii
            $buildS = [int]((Get-Date) - $buildT0).TotalSeconds

            if ($buildExit -ne 0 -or -not (Test-Path $binPath)) {
                Write-Host "  BUILD FAILED (exit $buildExit)" -ForegroundColor Red
                "$imb,$persist,$session,0,0,0.00,0.00,0.00,0.00,$buildS,0,BUILD_FAIL" |
                    Add-Content $SummaryCsv -Encoding ascii
                continue
            }

            # --- Run ----
            $runT0 = Get-Date
            $runArgs = @($InputCsvs | ForEach-Object { $_.FullName })

            # Capture stdout (trade CSV) and stderr (logs + summary) separately.
            # The engine's own [DPE] log lines go to stdout too; strip them with
            # a Where-Object so trades.csv stays parseable.
            $rawStdout = & $binPath @runArgs 2> "$tradesPath.stderr"
            $cleanStdout = $rawStdout | Where-Object { $_ -notmatch '^\[DPE' }
            $cleanStdout | Set-Content $tradesPath -Encoding ascii
            $runS = [int]((Get-Date) - $runT0).TotalSeconds

            # Append stderr to run.log
            "`n=== RUN $cellTag ===" | Add-Content $RunLog -Encoding ascii
            Get-Content "$tradesPath.stderr" | Add-Content $RunLog -Encoding ascii

            # Parse SUMMARY line from stderr
            $summaryLine = Get-Content "$tradesPath.stderr" |
                           Where-Object { $_ -match '^SUMMARY,' } |
                           Select-Object -First 1

            if (-not $summaryLine) {
                Write-Host "  NO SUMMARY (check $tradesPath.stderr)" -ForegroundColor Yellow
                "$imb,$persist,$session,0,0,0.00,0.00,0.00,0.00,$buildS,$runS,NO_SUMMARY" |
                    Add-Content $SummaryCsv -Encoding ascii
                continue
            }

            # SUMMARY,trades=X,wins=Y,wr=Z,pnl_usd=A,maxdd_usd=B,expectancy_usd=C,...
            $fields = @{}
            foreach ($kv in ($summaryLine -replace '^SUMMARY,','' -split ',')) {
                $parts = $kv -split '=', 2
                if ($parts.Count -eq 2) { $fields[$parts[0]] = $parts[1] }
            }

            $trades = $fields.trades;  if (-not $trades) { $trades = 0 }
            $wins   = $fields.wins;    if (-not $wins)   { $wins   = 0 }
            $wr     = $fields.wr;      if (-not $wr)     { $wr     = "0.00" }
            $pnl    = $fields.pnl_usd; if (-not $pnl)    { $pnl    = "0.00" }
            $dd     = $fields.maxdd_usd;     if (-not $dd)  { $dd  = "0.00" }
            $exp    = $fields.expectancy_usd;if (-not $exp) { $exp = "0.00" }

            "$imb,$persist,$session,$trades,$wins,$wr,$pnl,$dd,$exp,$buildS,$runS,OK" |
                Add-Content $SummaryCsv -Encoding ascii

            Write-Host "  trades=$trades wr=$wr pnl=$pnl maxdd=$dd  (build ${buildS}s, run ${runS}s)"

            if (-not $KeepBinaries) {
                Remove-Item $binPath -ErrorAction SilentlyContinue
            }
            Remove-Item "$tradesPath.stderr" -ErrorAction SilentlyContinue
        }
    }
}

$totalElapsed = (Get-Date) - $startTime
Write-Host ""
Write-Host "DONE. $totalCells cells in $([int]$totalElapsed.TotalSeconds)s"
Write-Host "Summary: $SummaryCsv"
Write-Host "Trades:  $TradesDir"

# -------- Final sanity ------------------------------------------------------
$okCells = (Get-Content $SummaryCsv | Select-Object -Skip 1 |
            Where-Object { $_ -match ',OK$' }).Count
Write-Host "$okCells/$totalCells cells completed successfully"
