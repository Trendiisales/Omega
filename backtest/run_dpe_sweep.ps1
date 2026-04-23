# =============================================================================
#  backtest\run_dpe_sweep.ps1
#
#  DomPersist walk-forward parameter sweep driver for Windows VPS.
#  Uses Microsoft Visual C++ (cl.exe) from Visual Studio 2022 Build Tools.
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
#      - Visual Studio 2022 Build Tools installed (cl.exe). Auto-detected from
#        common install paths; override with -VsDevCmd <path-to-VsDevCmd.bat>
#        or -Cl <path-to-cl.exe>.
#      - C:\Omega\include\DomPersistEngine.hpp must be the Session-15 patched
#        copy with the #ifdef override guards. Driver verifies by grep.
#      - No shim headers needed in rev3 -- the harness uses the real
#        OmegaTradeLedger.hpp and BracketTrendState.hpp from include/.
#
#  Usage (Windows PowerShell 5.1, built-in):
#      cd C:\Omega\backtest
#      powershell -File run_dpe_sweep.ps1
#      powershell -File run_dpe_sweep.ps1 -DryRun
#      powershell -File run_dpe_sweep.ps1 -ImbList "0.10,0.15" -PersistList "20"
#
#  Note: list params are STRINGS in rev3 -- PS 5.1 does not split comma-
#  delimited strings into typed [double[]] when invoked via -File. The script
#  parses these internally. Default values reproduce the full 96-cell grid.
#
#  Session 15 rev3 - MSVC / cl.exe port. Key changes from rev2:
#      - replaced g++ detection with VS 2022 Build Tools / cl.exe detection
#        via vswhere + VsDevCmd.bat environment bootstrap
#      - replaced [double[]]/[int[]] params with [string] + internal split,
#        working around PS 5.1 parameter binder behaviour with -File
#      - replaced Write-Error + exit with throw + direct exit for hard halts
#      - flag translation to MSVC: /std:c++17 /O2 /I /D /Fe /Fo
# =============================================================================

[CmdletBinding()]
param(
    [string] $RepoRoot    = "C:\Omega",
    [string] $LogDir      = "C:\Omega\logs",
    [string] $OutDir      = "C:\Omega\backtest\dpe_sweep",
    [string] $LogPattern  = "l2_ticks_2026-04-*.csv",
    [string] $ImbList     = "0.05,0.08,0.10,0.12,0.15,0.20",
    [string] $PersistList = "5,10,20,40",
    [string] $SessionList = "1,2,3,4",
    [switch] $DryRun,
    [switch] $KeepBinaries,
    [string] $Cl          = "",
    [string] $VsDevCmd    = ""
)

$ErrorActionPreference = "Stop"

# -------- Helper: halt with message ------------------------------------------
function Die([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# -------- Parse list params (PS 5.1-safe comma splitting) --------------------
function Parse-DoubleList([string]$s, [string]$name) {
    $parts = $s -split ','
    $out = @()
    foreach ($p in $parts) {
        $p = $p.Trim()
        if ($p -eq '') { continue }
        $d = 0.0
        if (-not [double]::TryParse($p, [ref]$d)) {
            Die "Bad numeric in -${name}: '$p'"
        }
        $out += $d
    }
    if ($out.Count -eq 0) { Die "-$name must contain at least one value" }
    return $out
}

function Parse-IntList([string]$s, [string]$name) {
    $parts = $s -split ','
    $out = @()
    foreach ($p in $parts) {
        $p = $p.Trim()
        if ($p -eq '') { continue }
        $i = 0
        if (-not [int]::TryParse($p, [ref]$i)) {
            Die "Bad integer in -${name}: '$p'"
        }
        $out += $i
    }
    if ($out.Count -eq 0) { Die "-$name must contain at least one value" }
    return $out
}

$ImbArr     = Parse-DoubleList $ImbList     "ImbList"
$PersistArr = Parse-IntList    $PersistList "PersistList"
$SessionArr = Parse-IntList    $SessionList "SessionList"

Write-Host "[OK] Param grid: $($ImbArr.Count) imb x $($PersistArr.Count) persist x $($SessionArr.Count) session = $($ImbArr.Count * $PersistArr.Count * $SessionArr.Count) cells"

# -------- Resolve MSVC toolchain ---------------------------------------------
# If user supplied -Cl, use it directly (assume env already set up).
# Otherwise, locate vswhere.exe and use it to find VsDevCmd.bat, then spawn
# a cmd session to import the VC environment into this PowerShell session.

function Import-VsEnv([string]$vsDevCmdPath) {
    Write-Host "[BUILD] Importing VS env from: $vsDevCmdPath"
    # Run VsDevCmd in cmd and dump resulting env as KEY=VALUE
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        $cmd = "`"$vsDevCmdPath`" -arch=x64 -host_arch=x64 > NUL && set"
        & cmd.exe /c $cmd | Out-File -FilePath $tmp -Encoding ascii
        $imported = 0
        Get-Content $tmp | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                $key = $Matches[1]
                $val = $Matches[2]
                # Skip variables that PowerShell shouldn't override
                if ($key -in @('PROMPT','PWD','HOME','USER')) { return }
                Set-Item -Path "Env:$key" -Value $val
                $imported++
            }
        }
        Write-Host "[BUILD] Imported $imported env vars"
    }
    finally {
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }
}

if (-not $Cl) {
    if (-not $VsDevCmd) {
        # vswhere.exe ships with VS installer at a fixed location since VS 2017
        $vswhere = "${Env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path $vswhere)) {
            Die "vswhere.exe not found at $vswhere. Specify -VsDevCmd <path> or -Cl <path>."
        }
        # Ask vswhere for latest VS install with VC compilers
        $vsPath = & $vswhere -latest -products '*' `
            -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' `
            -property 'installationPath'
        if (-not $vsPath) {
            Die "vswhere found no VS install with VC tools. Install 'Desktop development with C++' workload."
        }
        Write-Host "[BUILD] VS install: $vsPath"
        $VsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
        if (-not (Test-Path $VsDevCmd)) {
            Die "VsDevCmd.bat not found at: $VsDevCmd"
        }
    }
    Import-VsEnv $VsDevCmd

    # After import, cl.exe should be on PATH
    $clCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $clCmd) {
        Die "cl.exe not on PATH after importing VS env. Check VsDevCmd output."
    }
    $Cl = $clCmd.Source
}
else {
    if (-not (Test-Path $Cl)) { Die "-Cl path does not exist: $Cl" }
    Write-Host "[BUILD] Using -Cl override: $Cl"
}

# Sanity-print cl.exe version. Note: cl.exe with no args prints its banner;
# other compilers (g++ via -Cl shim during smoke tests) error here. Catch so
# the driver continues either way -- we already verified the exe is runnable.
try {
    $banner = & $Cl 2>&1 | Select-Object -First 1
    if ($banner) { Write-Host "[BUILD] $banner" }
} catch {
    Write-Host "[BUILD] (banner probe failed, continuing)"
}

# -------- Paths ---------------------------------------------------------------
$HarnessSrc    = Join-Path $RepoRoot "backtest\dom_persist_walk_forward.cpp"
$IncludeDir    = Join-Path $RepoRoot "include"
$EnginePatched = Join-Path $IncludeDir "DomPersistEngine.hpp"

foreach ($p in @($HarnessSrc, $IncludeDir, $EnginePatched)) {
    if (-not (Test-Path $p)) { Die "Missing required path: $p" }
}

# Verify engine has the Session-15 override guards.
$engineText = Get-Content $EnginePatched -Raw
if ($engineText -notmatch "DPE_IMB_THRESHOLD_OVERRIDE") {
    Die "Engine header missing S15 override guards. Re-deploy patched DomPersistEngine.hpp first."
}
if ($engineText -notmatch "DPE_PERSIST_TICKS_OVERRIDE") {
    Die "Engine header missing DPE_PERSIST_TICKS_OVERRIDE guard. Re-deploy patched DomPersistEngine.hpp first."
}
Write-Host "[OK] Engine header has S15 override guards"

# -------- Collect input CSVs --------------------------------------------------
$hits = Get-ChildItem -Path $LogDir -Filter $LogPattern -ErrorAction SilentlyContinue | Sort-Object Name
$InputCsvs = @($hits)

if ($InputCsvs.Count -eq 0) {
    Die "No input CSVs found in $LogDir matching $LogPattern"
}

Write-Host "[OK] Found $($InputCsvs.Count) input CSV(s):"
foreach ($f in $InputCsvs) {
    $sz = "{0:N1} MB" -f ($f.Length / 1MB)
    Write-Host "     $($f.Name)  $sz"
}

# -------- Prepare output dirs ------------------------------------------------
$TradesDir = Join-Path $OutDir "trades"
$BinDir    = Join-Path $OutDir "bin"
$ObjDir    = Join-Path $OutDir "obj"
New-Item -ItemType Directory -Path $OutDir    -Force | Out-Null
New-Item -ItemType Directory -Path $TradesDir -Force | Out-Null
New-Item -ItemType Directory -Path $BinDir    -Force | Out-Null
New-Item -ItemType Directory -Path $ObjDir    -Force | Out-Null

$BuildLog   = Join-Path $OutDir "build.log"
$RunLog     = Join-Path $OutDir "run.log"
$SummaryCsv = Join-Path $OutDir "summary.csv"

# PS 5.1 default encoding for Set-Content/Add-Content is system code page;
# force ASCII so downstream Python csv.DictReader gets clean rows.
"build log started $(Get-Date -Format o)" | Set-Content $BuildLog -Encoding ascii
"run log started $(Get-Date -Format o)"   | Set-Content $RunLog   -Encoding ascii

"imb_thresh,persist_ticks,session_filter,trades,wins,wr_pct,pnl_usd," +
"maxdd_usd,expectancy_usd,build_s,run_s,status" |
    Set-Content $SummaryCsv -Encoding ascii

# -------- Sweep ---------------------------------------------------------------
$totalCells = $ImbArr.Count * $PersistArr.Count * $SessionArr.Count
$cellIdx    = 0
$startTime  = Get-Date

foreach ($imb in $ImbArr) {
    foreach ($persist in $PersistArr) {
        foreach ($session in $SessionArr) {
            $cellIdx += 1
            $cellTag = "i{0:F2}_p{1}_s{2}" -f $imb, $persist, $session
            $binPath = Join-Path $BinDir "dpe_wf_$cellTag.exe"
            $objPath = Join-Path $ObjDir "$cellTag.obj"
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

            # --- Build via cl.exe ----
            # MSVC flag mapping:
            #   /std:c++17  -- language standard
            #   /EHsc       -- standard C++ exception handling (required; engine uses noexcept)
            #   /O2         -- full optimisation
            #   /nologo     -- suppress banner
            #   /W3         -- warning level (not /W4 -- engine has some benign warnings at W4)
            #   /I <dir>    -- include dir
            #   /D<K>=<V>   -- preprocessor define
            #   /Fe<exe>    -- output exe path
            #   /Fo<obj>    -- output obj path
            $buildT0 = Get-Date
            $clArgs = @(
                "/nologo", "/std:c++17", "/EHsc", "/O2", "/W3",
                "/I", $IncludeDir,
                "/DDPE_IMB_THRESHOLD_OVERRIDE=$imb",
                "/DDPE_PERSIST_TICKS_OVERRIDE=$persist",
                "/DDPE_SESSION_FILTER=$session",
                $HarnessSrc,
                "/Fe:$binPath",
                "/Fo:$objPath"
            )
            "`n=== BUILD $cellTag ===" | Add-Content $BuildLog -Encoding ascii
            "CMD: $Cl $($clArgs -join ' ')" | Add-Content $BuildLog -Encoding ascii
            $buildOut = & $Cl @clArgs 2>&1
            $buildExit = $LASTEXITCODE
            $buildOut | Out-String | Add-Content $BuildLog -Encoding ascii
            $buildS = [int]((Get-Date) - $buildT0).TotalSeconds

            if ($buildExit -ne 0 -or -not (Test-Path $binPath)) {
                Write-Host "  BUILD FAILED (exit $buildExit) -- see build.log" -ForegroundColor Red
                "$imb,$persist,$session,0,0,0.00,0.00,0.00,0.00,$buildS,0,BUILD_FAIL" |
                    Add-Content $SummaryCsv -Encoding ascii
                continue
            }

            # --- Run ----
            $runT0 = Get-Date
            $runArgs = @($InputCsvs | ForEach-Object { $_.FullName })

            # Engine's own [DPE] log lines go to stdout; strip them so the
            # trades.csv stays parseable. stderr gets SUMMARY + progress.
            $rawStdout = & $binPath @runArgs 2> "$tradesPath.stderr"
            $cleanStdout = $rawStdout | Where-Object { $_ -notmatch '^\[DPE' }
            $cleanStdout | Set-Content $tradesPath -Encoding ascii
            $runS = [int]((Get-Date) - $runT0).TotalSeconds

            "`n=== RUN $cellTag ===" | Add-Content $RunLog -Encoding ascii
            Get-Content "$tradesPath.stderr" | Add-Content $RunLog -Encoding ascii

            $summaryLine = Get-Content "$tradesPath.stderr" |
                           Where-Object { $_ -match '^SUMMARY,' } |
                           Select-Object -First 1

            if (-not $summaryLine) {
                Write-Host "  NO SUMMARY (check run.log)" -ForegroundColor Yellow
                "$imb,$persist,$session,0,0,0.00,0.00,0.00,0.00,$buildS,$runS,NO_SUMMARY" |
                    Add-Content $SummaryCsv -Encoding ascii
                continue
            }

            $fields = @{}
            foreach ($kv in ($summaryLine -replace '^SUMMARY,','' -split ',')) {
                $parts = $kv -split '=', 2
                if ($parts.Count -eq 2) { $fields[$parts[0]] = $parts[1] }
            }

            $trades = $fields.trades;         if (-not $trades) { $trades = 0 }
            $wins   = $fields.wins;           if (-not $wins)   { $wins   = 0 }
            $wr     = $fields.wr;             if (-not $wr)     { $wr     = "0.00" }
            $pnl    = $fields.pnl_usd;        if (-not $pnl)    { $pnl    = "0.00" }
            $dd     = $fields.maxdd_usd;      if (-not $dd)     { $dd     = "0.00" }
            $exp    = $fields.expectancy_usd; if (-not $exp)    { $exp    = "0.00" }

            "$imb,$persist,$session,$trades,$wins,$wr,$pnl,$dd,$exp,$buildS,$runS,OK" |
                Add-Content $SummaryCsv -Encoding ascii

            Write-Host "  trades=$trades wr=$wr pnl=$pnl maxdd=$dd  (build ${buildS}s, run ${runS}s)"

            if (-not $KeepBinaries) {
                Remove-Item $binPath -ErrorAction SilentlyContinue
                Remove-Item $objPath -ErrorAction SilentlyContinue
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

$okCells = (Get-Content $SummaryCsv | Select-Object -Skip 1 |
            Where-Object { $_ -match ',OK$' }).Count
Write-Host "$okCells/$totalCells cells completed successfully"
