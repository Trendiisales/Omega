# =============================================================================
# run_cbe_sweep.ps1 -- CompressionBreakoutEngine REENTER_COMP parameter sweep
#
# Grid:
#   reenter_tol        = 0.10, 0.25, 0.50, 0.75, 1.00
#   reenter_needs_be   = 0, 1
#   min_hold_ms        = 5000, 15000, 30000, 60000
#   Total: 5 * 2 * 4 = 40 cells
#   Plus 1 baseline: gate fully disabled (CBE_SWEEP_REENTER_DISABLED)
#   Grand total: 41 cells
#
# Train split: 2026-04-13, 14, 15, 16, 17 (5 days)
# Test  split: 2026-04-20, 21, 22, 23      (4 days)
#
# Per cell: builds binary with /D flags, runs once on train files, once on test
# files. Emits two trade CSVs per cell under results\.
#
# Output structure:
#   backtest\build\cbe_sweep\<cell_id>\cbe_sweep.exe
#   backtest\results\cbe_sweep\<cell_id>\train.csv
#   backtest\results\cbe_sweep\<cell_id>\test.csv
#   backtest\results\cbe_sweep\sweep_index.csv   <- cell_id -> params mapping
#
# Called from: C:\Omega
# Usage:
#   .\backtest\run_cbe_sweep.ps1
#   .\backtest\run_cbe_sweep.ps1 -SkipBuild        # reuse existing binaries
#   .\backtest\run_cbe_sweep.ps1 -CellFilter "tol050"   # run subset
# =============================================================================

param(
    [switch]$SkipBuild,
    [string]$CellFilter = ""
)

$ErrorActionPreference = "Stop"
$OmegaRoot = "C:\Omega"
$HarnessSrc = Join-Path $OmegaRoot "backtest\cbe_walk_forward.cpp"

if (-not (Test-Path $HarnessSrc)) {
    Write-Host "[FATAL] Harness source not found: $HarnessSrc" -ForegroundColor Red
    exit 1
}

# --- Paths ---
$BuildRoot   = Join-Path $OmegaRoot "backtest\build\cbe_sweep"
$ResultsRoot = Join-Path $OmegaRoot "backtest\results\cbe_sweep"
$IndexCsv    = Join-Path $ResultsRoot "sweep_index.csv"

New-Item -ItemType Directory -Force -Path $BuildRoot    | Out-Null
New-Item -ItemType Directory -Force -Path $ResultsRoot  | Out-Null

# --- Data files ---
$LogsDir = Join-Path $OmegaRoot "logs"

# Train: 13-17 April (unsuffixed format)
$TrainFiles = @(
    "l2_ticks_2026-04-13.csv",
    "l2_ticks_2026-04-14.csv",
    "l2_ticks_2026-04-15.csv",
    "l2_ticks_2026-04-16.csv",
    "l2_ticks_2026-04-17.csv"
) | ForEach-Object { Join-Path $LogsDir $_ }

# Test: 20-23 April (21 unsuffixed, 22-23 XAUUSD-suffixed)
$TestFiles = @(
    "l2_ticks_2026-04-20.csv",
    "l2_ticks_2026-04-21.csv",
    "l2_ticks_XAUUSD_2026-04-22.csv",
    "l2_ticks_XAUUSD_2026-04-23.csv"
) | ForEach-Object { Join-Path $LogsDir $_ }

# Verify all input files exist
$AllFiles = $TrainFiles + $TestFiles
foreach ($f in $AllFiles) {
    if (-not (Test-Path $f)) {
        Write-Host "[FATAL] Missing input: $f" -ForegroundColor Red
        exit 2
    }
}
Write-Host "[OK] All 9 input files present"

# --- Grid ---
$Tols     = @("0.10", "0.25", "0.50", "0.75", "1.00")
$NeedsBe  = @(0, 1)
$Holds    = @(5000, 15000, 30000, 60000)

# Build cell list: each cell has ID + define flags
$Cells = New-Object System.Collections.ArrayList

# 40 param cells
foreach ($tol in $Tols) {
    foreach ($nb in $NeedsBe) {
        foreach ($h in $Holds) {
            $tolId = $tol.Replace(".", "") + "0"   # 0.10 -> "0100", 0.25 -> "0250"
            $id = "tol${tolId}_be${nb}_h${h}"
            $defs = @(
                "/DCBE_SWEEP_REENTER_TOL=$tol",
                "/DCBE_SWEEP_REENTER_NEEDS_BE=$nb",
                "/DCBE_SWEEP_MIN_HOLD_MS=$h"
            )
            [void]$Cells.Add([PSCustomObject]@{
                id    = $id
                defs  = $defs
                tol   = $tol
                be    = $nb
                hold  = $h
                mode  = "active"
            })
        }
    }
}

# Baseline: gate disabled entirely
[void]$Cells.Add([PSCustomObject]@{
    id    = "disabled"
    defs  = @("/DCBE_SWEEP_REENTER_DISABLED")
    tol   = "n/a"
    be    = -1
    hold  = -1
    mode  = "disabled"
})

Write-Host "[INFO] Grid = $($Cells.Count) cells"

# Write index CSV up front
"cell_id,mode,reenter_tol,needs_be,min_hold_ms" | Out-File -FilePath $IndexCsv -Encoding ascii
foreach ($c in $Cells) {
    "$($c.id),$($c.mode),$($c.tol),$($c.be),$($c.hold)" | Out-File -FilePath $IndexCsv -Encoding ascii -Append
}
Write-Host "[OK] Wrote $IndexCsv"

# Apply filter if requested
if ($CellFilter -ne "") {
    $Cells = $Cells | Where-Object { $_.id -like "*$CellFilter*" }
    Write-Host "[INFO] Filtered to $($Cells.Count) cells matching '$CellFilter'"
}

# --- Build + run each cell ---
$Idx = 0
$Total = $Cells.Count
$Failed = @()

foreach ($c in $Cells) {
    $Idx++
    $CellBuildDir = Join-Path $BuildRoot $c.id
    $CellResDir   = Join-Path $ResultsRoot $c.id
    $Exe          = Join-Path $CellBuildDir "cbe_sweep.exe"
    $TrainOut     = Join-Path $CellResDir "train.csv"
    $TestOut      = Join-Path $CellResDir "test.csv"

    New-Item -ItemType Directory -Force -Path $CellBuildDir | Out-Null
    New-Item -ItemType Directory -Force -Path $CellResDir   | Out-Null

    Write-Host ""
    Write-Host "[$Idx/$Total] $($c.id)" -ForegroundColor Cyan

    # Build
    if (-not $SkipBuild -or -not (Test-Path $Exe)) {
        $CompileArgs = @(
            "/std:c++17", "/O2", "/EHsc", "/W4", "/WX",
            "/nologo"
        ) + $c.defs + @(
            $HarnessSrc,
            "/Fe:$Exe",
            "/Fo:$CellBuildDir\"
        )
        $ClOut = & cl @CompileArgs 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [FAIL] compile error:" -ForegroundColor Red
            $ClOut | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
            $Failed += $c.id
            continue
        }
        Write-Host "  [build] ok"
    } else {
        Write-Host "  [build] skipped (existing)"
    }

    # Run train split
    Write-Host "  [train] running on $($TrainFiles.Count) files..."
    $TrainProc = Start-Process -FilePath $Exe `
        -ArgumentList (@($TrainOut) + $TrainFiles) `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardError (Join-Path $CellResDir "train.stderr.log")
    if ($TrainProc.ExitCode -ne 0) {
        Write-Host "  [FAIL] train exit $($TrainProc.ExitCode)" -ForegroundColor Red
        $Failed += "$($c.id)/train"
        continue
    }
    $TrainSummary = Get-Content (Join-Path $CellResDir "train.stderr.log") | Where-Object { $_ -match "^\[SUMMARY\]" } | Select-Object -First 1
    Write-Host "  [train] $TrainSummary"

    # Run test split
    Write-Host "  [test]  running on $($TestFiles.Count) files..."
    $TestProc = Start-Process -FilePath $Exe `
        -ArgumentList (@($TestOut) + $TestFiles) `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardError (Join-Path $CellResDir "test.stderr.log")
    if ($TestProc.ExitCode -ne 0) {
        Write-Host "  [FAIL] test exit $($TestProc.ExitCode)" -ForegroundColor Red
        $Failed += "$($c.id)/test"
        continue
    }
    $TestSummary = Get-Content (Join-Path $CellResDir "test.stderr.log") | Where-Object { $_ -match "^\[SUMMARY\]" } | Select-Object -First 1
    Write-Host "  [test]  $TestSummary"
}

Write-Host ""
Write-Host "=" * 60
Write-Host "[DONE] $($Cells.Count) cells processed"
if ($Failed.Count -gt 0) {
    Write-Host "[WARN] $($Failed.Count) failures:" -ForegroundColor Yellow
    $Failed | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    exit 1
}
Write-Host "[OK] all cells complete"
Write-Host ""
Write-Host "Next step:"
Write-Host "  python backtest\analyze_cbe_sweep.py"
