# =============================================================================
# VERIFY_ALL_ENGINES.ps1 -- 2026-05-05 audit-fixes-39 follow-up
# -----------------------------------------------------------------------------
# Comprehensive engine-by-engine verification. For every engine in the fleet,
# checks four things across the last 48h of logs:
#
#   1. INIT      -- Did the engine emit its startup banner / register itself?
#   2. TICKED    -- Has the engine ever produced a DIAG / heartbeat? (proves
#                   its on_tick handler is being called)
#   3. ARMED     -- Has the engine ever reached an ARMED / READY state?
#   4. FIRED     -- Has the engine ever ENTERED a position?
#
# Output: a matrix written to logs/ENGINE_VERIFICATION_<date>.txt.
#
# Run from PowerShell on the VPS:
#   cd C:\Omega
#   .\VERIFY_ALL_ENGINES.ps1
#
# No code changes. No state mutation. Pure read-only diagnostic.
# =============================================================================

$ErrorActionPreference = "Stop"
Set-Location C:\Omega\logs

# -- Find logs from the last 48h ----------------------------------------------
$logs = Get-ChildItem C:\Omega\logs\omega_*.log |
        Where-Object { $_.LastWriteTime -gt (Get-Date).AddHours(-48) } |
        Sort-Object LastWriteTime
if (-not $logs) {
    Write-Host "[ERR] no omega_*.log files modified in last 48h" -ForegroundColor Red
    exit 1
}
Write-Host ("[INFO] scanning {0} log file(s):" -f $logs.Count) -ForegroundColor Cyan
$logs | ForEach-Object {
    Write-Host ("       {0,-40}  {1,9} bytes  modified {2}" -f
                $_.Name, $_.Length, $_.LastWriteTime)
}
$logPaths = $logs.FullName

$reportPath = "C:\Omega\logs\ENGINE_VERIFICATION_$(Get-Date -Format yyyy-MM-dd_HH-mm-ss).txt"
"OMEGA ENGINE VERIFICATION REPORT" | Out-File $reportPath -Encoding utf8
"Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" | Out-File $reportPath -Encoding utf8 -Append
"Logs scanned:" | Out-File $reportPath -Encoding utf8 -Append
$logs | ForEach-Object { "  $($_.Name) ($($_.Length) bytes)" } | Out-File $reportPath -Encoding utf8 -Append
"" | Out-File $reportPath -Encoding utf8 -Append

# -----------------------------------------------------------------------------
# Engine fleet -- mapping of (display name, log-prefix patterns)
# -----------------------------------------------------------------------------
# patterns is a hashtable with keys: init, ticked, armed, fired
# Each value is a regex matched against the log files.
# -----------------------------------------------------------------------------
$engines = @(
    # ── Gold engines ────────────────────────────────────────────────────────
    @{ name = "GoldHybridBracket (HBG)";          init = "HYBRID-GOLD.*ARMED|g_bracket_gold|HybridGold register";        ticked = "HYBRID-GOLD-DIAG";           armed = "HYBRID-GOLD\] ARMED";              fired = "HYBRID-GOLD\] FIRE|HYBRID-GOLD\] FILL|EXIT.*HybridBracketGold" },
    @{ name = "GoldMidScalper";                   init = "MID-SCALPER-GOLD ARMED|g_gold_midscalper|MidScalperGold register|MIDSCALPER-GOLD-DIAG";    ticked = "MID-SCALPER-GOLD-DIAG|MIDSCALPER-GOLD-DIAG";     armed = "MID-SCALPER-GOLD\] ARMED";        fired = "MID-SCALPER-GOLD\] FIRE|MID-SCALPER-GOLD\] FILL|EXIT.*MidScalperGold" },
    @{ name = "MacroCrashEngine (MCE)";           init = "MacroCrash.*ARMED|g_macro_crash|MCE.*regis";                 ticked = "MCE-DIAG|MacroCrash.*tick";  armed = "MCE-BLOCK|MacroCrash.*ARMED|MCE.*ARMED";                fired = "MCE.*ENTRY|EXIT.*MacroCrash" },
    @{ name = "RSIReversalEngine";                init = "RSIRev.*ARMED|g_rsi_reversal";                                ticked = "RSIRev-DIAG|RSI-REV-DIAG";   armed = "RSIRev.*ARMED";                    fired = "RSIRev.*ENTRY|EXIT.*RSIReversal" },
    @{ name = "NBM gold London";                  init = "NBM-GOLD.*ARMED|g_nbm_gold_london";                           ticked = "NBM-GOLD-DIAG";              armed = "NBM-GOLD\] ARMED";                 fired = "NBM-GOLD\] FIRE|EXIT.*NBM" },
    @{ name = "GoldEngineStack";                  init = "GoldStack.*ARMED|g_gold_stack";                               ticked = "GoldStack.*DIAG|gold_stack.*tick"; armed = "GoldStack.*ARMED";          fired = "GoldStack.*ENTRY|EXIT.*GoldStack" },
    @{ name = "HTFSwing h1_gold";                 init = "h1_swing_gold.*ARMED|HTF-H1.*ARMED";                          ticked = "HTF-H1-DIAG|h1_swing_gold.*tick"; armed = "HTF-H1.*ARMED";              fired = "HTF-H1.*ENTRY|EXIT.*h1_swing_gold" },
    @{ name = "HTFSwing h4_gold";                 init = "h4_regime_gold.*ARMED|HTF-H4.*ARMED";                         ticked = "HTF-H4-DIAG|h4_regime_gold.*tick"; armed = "HTF-H4.*ARMED";             fired = "HTF-H4.*ENTRY|EXIT.*h4_regime_gold" },
    @{ name = "CandleFlow (CFE)";                 init = "g_candle_flow|CFE.*ARMED";                                    ticked = "CFE-DIAG|candle_flow.*tick"; armed = "CFE.*ARMED";                       fired = "CFE.*ENTRY|EXIT.*CandleFlow" },
    @{ name = "EMACross (ECE)";                   init = "g_ema_cross|ECE.*ARMED";                                      ticked = "ECE-DIAG|ema_cross.*tick";   armed = "ECE.*ARMED";                       fired = "ECE.*ENTRY|EXIT.*EMACross" },
    @{ name = "XauusdFvg";                        init = "XauusdFvg.*ARMED|g_xauusd_fvg|FVG.*ARMED";                    ticked = "FVG-DIAG|XauusdFvg.*tick";   armed = "FVG.*ARMED";                       fired = "FVG.*ENTRY|EXIT.*XauusdFvg|EXIT.*Fvg" },

    # ── Tsmom portfolio (5 cells) ───────────────────────────────────────────
    @{ name = "Tsmom_H1_long";                    init = "Tsmom_H1_long\] ARMED|TsmomPortfolio.*ARMED";                 ticked = "Tsmom_H1_long";              armed = "Tsmom_H1_long\] ARMED";            fired = "Tsmom_H1_long\] ENTRY|Tsmom_H1_long.*EXIT" },
    @{ name = "Tsmom_H2_long";                    init = "Tsmom_H2_long\] ARMED";                                       ticked = "Tsmom_H2_long";              armed = "Tsmom_H2_long\] ARMED";            fired = "Tsmom_H2_long\] ENTRY|Tsmom_H2_long.*EXIT" },
    @{ name = "Tsmom_H4_long";                    init = "Tsmom_H4_long\] ARMED";                                       ticked = "Tsmom_H4_long";              armed = "Tsmom_H4_long\] ARMED";            fired = "Tsmom_H4_long\] ENTRY|Tsmom_H4_long.*EXIT" },
    @{ name = "Tsmom_H6_long";                    init = "Tsmom_H6_long\] ARMED";                                       ticked = "Tsmom_H6_long";              armed = "Tsmom_H6_long\] ARMED";            fired = "Tsmom_H6_long\] ENTRY|Tsmom_H6_long.*EXIT" },
    @{ name = "Tsmom_D1_long";                    init = "Tsmom_D1_long\] ARMED";                                       ticked = "Tsmom_D1_long";              armed = "Tsmom_D1_long\] ARMED";            fired = "Tsmom_D1_long\] ENTRY|Tsmom_D1_long.*EXIT" },

    # ── FX London-open / Asian-open engines (audit-fixes-37 cohort) ─────────
    @{ name = "EurusdLondonOpen";                 init = "EUR-LDN-OPEN.*ARMED|EurusdLondonOpen|g_eurusd_london_open";   ticked = "EUR-LDN-OPEN-DIAG";          armed = "EUR-LDN-OPEN\] ARMED";             fired = "EUR-LDN-OPEN\] FIRE|EUR-LDN-OPEN\] FILL|EXIT.*EurusdLondonOpen" },
    @{ name = "GbpusdLondonOpen";                 init = "GBP-LDN-OPEN.*ARMED|GbpusdLondonOpen|g_gbpusd_london_open";   ticked = "GBP-LDN-OPEN-DIAG";          armed = "GBP-LDN-OPEN\] ARMED";             fired = "GBP-LDN-OPEN\] FIRE|GBP-LDN-OPEN\] FILL|EXIT.*GbpusdLondonOpen" },
    @{ name = "UsdjpyAsianOpen";                  init = "USDJPY-ASIAN.*ARMED|UsdjpyAsianOpen|g_usdjpy_asian_open";     ticked = "USDJPY-ASIAN-DIAG";          armed = "USDJPY-ASIAN\] ARMED";             fired = "USDJPY-ASIAN\] FIRE|USDJPY-ASIAN\] FILL|EXIT.*UsdjpyAsianOpen" },
    @{ name = "AudusdSydneyOpen";                 init = "AUD-SYD-OPEN.*ARMED|AudusdSydneyOpen|g_audusd_sydney_open";   ticked = "AUD-SYD-OPEN-DIAG";          armed = "AUD-SYD-OPEN\] ARMED";             fired = "AUD-SYD-OPEN\] FIRE|AUD-SYD-OPEN\] FILL|EXIT.*AudusdSydneyOpen" },
    @{ name = "NzdusdAsianOpen";                  init = "NZD-ASN-OPEN.*ARMED|NZD-ASIAN-OPEN.*ARMED|NzdusdAsianOpen|g_nzdusd_asian_open"; ticked = "NZD-ASN-OPEN-DIAG|NZD-ASIAN-OPEN-DIAG"; armed = "NZD-ASN-OPEN\] ARMED|NZD-ASIAN-OPEN\] ARMED"; fired = "NZD-ASN-OPEN\] FIRE|NZD-ASIAN-OPEN\] FIRE|EXIT.*NzdusdAsianOpen" },

    # ── Index hybrid bracket engines ────────────────────────────────────────
    @{ name = "HybridBracket SP (US500)";         init = "HYBRID-US500.*ARMED|g_hybrid_sp";                             ticked = "HYBRID-US500-DIAG";          armed = "HYBRID-US500\] ARMED";             fired = "HYBRID-US500\] FIRE|HYBRID-US500\] FILL" },
    @{ name = "HybridBracket NQ (USTEC)";         init = "HYBRID-USTEC.*ARMED|g_hybrid_nq";                             ticked = "HYBRID-USTEC-DIAG";          armed = "HYBRID-USTEC\] ARMED";             fired = "HYBRID-USTEC\] FIRE|HYBRID-USTEC\] FILL" },
    @{ name = "HybridBracket US30 (DJ30)";        init = "HYBRID-DJ30.*ARMED|HYBRID-US30.*ARMED|g_hybrid_us30";         ticked = "HYBRID-DJ30-DIAG|HYBRID-US30-DIAG"; armed = "HYBRID-DJ30\] ARMED|HYBRID-US30\] ARMED"; fired = "HYBRID-DJ30\] FIRE|HYBRID-US30\] FIRE" },
    @{ name = "HybridBracket NAS100";             init = "HYBRID-NAS100.*ARMED|g_hybrid_nas100";                        ticked = "HYBRID-NAS100-DIAG";         armed = "HYBRID-NAS100\] ARMED";            fired = "HYBRID-NAS100\] FIRE|HYBRID-NAS100\] FILL" },

    # ── Index flow engines ──────────────────────────────────────────────────
    @{ name = "IndexFlow SP (US500)";             init = "IFLOW-US500.*ARMED|g_iflow_sp";                               ticked = "IFLOW-US500-DIAG";           armed = "IFLOW-US500\] ARMED";              fired = "IFLOW-US500\] FIRE|IFLOW-US500\] FILL" },
    @{ name = "IndexFlow NQ (USTEC)";             init = "IFLOW-USTEC.*ARMED|g_iflow_nq";                               ticked = "IFLOW-USTEC-DIAG";           armed = "IFLOW-USTEC\] ARMED";              fired = "IFLOW-USTEC\] FIRE|IFLOW-USTEC\] FILL" },
    @{ name = "IndexFlow NAS100";                 init = "IFLOW-NAS.*ARMED|g_iflow_nas";                                ticked = "IFLOW-NAS100-DIAG|IFLOW-NAS-DIAG"; armed = "IFLOW-NAS100\] ARMED|IFLOW-NAS\] ARMED"; fired = "IFLOW-NAS100\] FIRE|IFLOW-NAS\] FIRE" },
    @{ name = "IndexFlow US30 (DJ30)";            init = "IFLOW-US30.*ARMED|g_iflow_us30|IFLOW-DJ30";                   ticked = "IFLOW-US30-DIAG|IFLOW-DJ30-DIAG"; armed = "IFLOW-US30\] ARMED|IFLOW-DJ30\] ARMED"; fired = "IFLOW-US30\] FIRE|IFLOW-DJ30\] FIRE" },

    # ── Index macro crash (4-symbol shadow cohort) ──────────────────────────
    @{ name = "IndexMacroCrash SP";               init = "IMACRO-US500|IMACRO-SP|g_imacro_sp";                          ticked = "IMACRO-US500-DIAG|IMACRO-SP-DIAG"; armed = "IMACRO-US500\] ARMED|IMACRO-SP\] ARMED"; fired = "IMACRO-US500\] FIRE|IMACRO-SP\] FIRE" },
    @{ name = "IndexMacroCrash NQ";               init = "IMACRO-USTEC|IMACRO-NQ|g_imacro_nq";                          ticked = "IMACRO-USTEC-DIAG|IMACRO-NQ-DIAG"; armed = "IMACRO-USTEC\] ARMED|IMACRO-NQ\] ARMED"; fired = "IMACRO-USTEC\] FIRE|IMACRO-NQ\] FIRE" },
    @{ name = "IndexMacroCrash NAS";              init = "IMACRO-NAS|g_imacro_nas";                                     ticked = "IMACRO-NAS-DIAG|IMACRO-NAS100-DIAG"; armed = "IMACRO-NAS\] ARMED|IMACRO-NAS100\] ARMED"; fired = "IMACRO-NAS\] FIRE|IMACRO-NAS100\] FIRE" },
    @{ name = "IndexMacroCrash US30";             init = "IMACRO-US30|IMACRO-DJ30|g_imacro_us30";                       ticked = "IMACRO-US30-DIAG|IMACRO-DJ30-DIAG"; armed = "IMACRO-US30\] ARMED|IMACRO-DJ30\] ARMED"; fired = "IMACRO-US30\] FIRE|IMACRO-DJ30\] FIRE" },

    # ── TrendPullback indices (LIVE) ────────────────────────────────────────
    @{ name = "TrendPullback SP";                 init = "TPB-SP|trend_pb_sp|g_trend_pb_sp";                            ticked = "TPB-SP-DIAG|trend_pb_sp.*tick"; armed = "TPB-SP\] ARMED";                fired = "TPB-SP\] ENTRY|EXIT.*trend_pb_sp" },
    @{ name = "TrendPullback NQ";                 init = "TPB-NQ|trend_pb_nq|g_trend_pb_nq";                            ticked = "TPB-NQ-DIAG|trend_pb_nq.*tick"; armed = "TPB-NQ\] ARMED";                fired = "TPB-NQ\] ENTRY|EXIT.*trend_pb_nq" },

    # ── Minimal H4 US30 ─────────────────────────────────────────────────────
    @{ name = "MinimalH4 US30";                   init = "MINIMAL-H4-US30|g_minimal_h4_us30";                           ticked = "MINIMAL-H4-US30-DIAG|H4-US30-DIAG"; armed = "MINIMAL-H4-US30\] ARMED";    fired = "MINIMAL-H4-US30\] ENTRY|EXIT.*MinimalH4US30" },

    # ── Cross-asset bracket engines (FX + commodities) ──────────────────────
    @{ name = "BracketEngine EURUSD";             init = "BRACKET-EURUSD|g_bracket_eurusd";                             ticked = "BRACKET-EURUSD";             armed = "BRACKET-EURUSD\] ARMED";           fired = "BRACKET-EURUSD\] FIRE|BRACKET-EURUSD\] FILL|BRACKET-EURUSD.*SHADOW FILL" },
    @{ name = "BracketEngine GBPUSD";             init = "BRACKET-GBPUSD|g_bracket_gbpusd";                             ticked = "BRACKET-GBPUSD";             armed = "BRACKET-GBPUSD\] ARMED";           fired = "BRACKET-GBPUSD\] FIRE|BRACKET-GBPUSD\] FILL" },
    @{ name = "BracketEngine AUDUSD";             init = "BRACKET-AUDUSD|g_bracket_audusd";                             ticked = "BRACKET-AUDUSD";             armed = "BRACKET-AUDUSD\] ARMED";           fired = "BRACKET-AUDUSD\] FIRE|BRACKET-AUDUSD\] FILL" },
    @{ name = "BracketEngine NZDUSD";             init = "BRACKET-NZDUSD|g_bracket_nzdusd";                             ticked = "BRACKET-NZDUSD";             armed = "BRACKET-NZDUSD\] ARMED";           fired = "BRACKET-NZDUSD\] FIRE|BRACKET-NZDUSD\] FILL" },
    @{ name = "BracketEngine USDJPY";             init = "BRACKET-USDJPY|g_bracket_usdjpy";                             ticked = "BRACKET-USDJPY";             armed = "BRACKET-USDJPY\] ARMED";           fired = "BRACKET-USDJPY\] FIRE|BRACKET-USDJPY\] FILL" },
    @{ name = "BracketEngine BRENT";              init = "BRACKET-BRENT|g_bracket_brent";                               ticked = "BRACKET-BRENT";              armed = "BRACKET-BRENT\] ARMED";            fired = "BRACKET-BRENT\] FIRE|BRACKET-BRENT\] FILL" },
    @{ name = "BracketEngine GER40";              init = "BRACKET-GER40|g_bracket_ger30";                               ticked = "BRACKET-GER40";              armed = "BRACKET-GER40\] ARMED";            fired = "BRACKET-GER40\] FIRE|BRACKET-GER40\] FILL" },
    @{ name = "BracketEngine UK100";              init = "BRACKET-UK100|g_bracket_uk100";                               ticked = "BRACKET-UK100";              armed = "BRACKET-UK100\] ARMED";            fired = "BRACKET-UK100\] FIRE|BRACKET-UK100\] FILL" }
)

# -----------------------------------------------------------------------------
# Run the four checks per engine
# -----------------------------------------------------------------------------
function CheckPattern([string[]]$paths, [string]$pattern) {
    if ([string]::IsNullOrEmpty($pattern)) { return @{ count = 0; first = ""; last = "" } }
    $matches = Select-String -Path $paths -Pattern $pattern -ErrorAction SilentlyContinue
    if (-not $matches) { return @{ count = 0; first = ""; last = "" } }
    $arr = @($matches)
    return @{
        count = $arr.Count
        first = ($arr | Select-Object -First 1).Line
        last  = ($arr | Select-Object -Last  1).Line
    }
}

# Header
$hdr = "{0,-32}  {1,6}  {2,6}  {3,6}  {4,6}  STATUS" -f "Engine","Init","Tick","Armed","Fired"
Write-Host ""
Write-Host $hdr -ForegroundColor Yellow
Write-Host ("-" * 90) -ForegroundColor Yellow
$hdr | Out-File $reportPath -Encoding utf8 -Append
("-" * 90) | Out-File $reportPath -Encoding utf8 -Append

$results = @()
foreach ($eng in $engines) {
    $init   = CheckPattern $logPaths $eng.init
    $ticked = CheckPattern $logPaths $eng.ticked
    $armed  = CheckPattern $logPaths $eng.armed
    $fired  = CheckPattern $logPaths $eng.fired

    # Status verdict
    $status = if ($init.count   -eq 0) { "MISSING -- never initialized in binary" }
              elseif ($ticked.count -eq 0) { "STARVED -- inited but no ticks" }
              elseif ($armed.count  -eq 0) { "GATED   -- ticking but never armed" }
              elseif ($fired.count  -eq 0) { "ARMED-ONLY -- never entered" }
              else { "HEALTHY -- entered N=$($fired.count)" }

    $color = switch -Wildcard ($status) {
        "MISSING*"     { "Red" }
        "STARVED*"     { "Red" }
        "GATED*"       { "DarkYellow" }
        "ARMED-ONLY*"  { "Yellow" }
        "HEALTHY*"     { "Green" }
        default        { "White" }
    }

    $line = "{0,-32}  {1,6}  {2,6}  {3,6}  {4,6}  {5}" -f
            $eng.name, $init.count, $ticked.count, $armed.count, $fired.count, $status
    Write-Host $line -ForegroundColor $color
    $line | Out-File $reportPath -Encoding utf8 -Append

    $results += [pscustomobject]@{
        Engine    = $eng.name
        InitCount = $init.count
        InitFirst = $init.first
        Ticked    = $ticked.count
        TickFirst = $ticked.first
        TickLast  = $ticked.last
        Armed     = $armed.count
        Fired     = $fired.count
        Status    = $status
    }
}

# -----------------------------------------------------------------------------
# Summary section
# -----------------------------------------------------------------------------
$missing  = ($results | Where-Object { $_.Status -like "MISSING*" }).Count
$starved  = ($results | Where-Object { $_.Status -like "STARVED*" }).Count
$gated    = ($results | Where-Object { $_.Status -like "GATED*" }).Count
$armOnly  = ($results | Where-Object { $_.Status -like "ARMED-ONLY*" }).Count
$healthy  = ($results | Where-Object { $_.Status -like "HEALTHY*" }).Count
$total    = $results.Count

"" | Out-File $reportPath -Encoding utf8 -Append
"SUMMARY" | Out-File $reportPath -Encoding utf8 -Append
("Total engines checked   : {0}" -f $total)   | Out-File $reportPath -Encoding utf8 -Append
("HEALTHY (entered)        : {0}" -f $healthy) | Out-File $reportPath -Encoding utf8 -Append
("ARMED-ONLY (no entries)  : {0}" -f $armOnly) | Out-File $reportPath -Encoding utf8 -Append
("GATED (no arms)          : {0}" -f $gated)   | Out-File $reportPath -Encoding utf8 -Append
("STARVED (no ticks)       : {0}" -f $starved) | Out-File $reportPath -Encoding utf8 -Append
("MISSING (not init)       : {0}" -f $missing) | Out-File $reportPath -Encoding utf8 -Append

Write-Host ""
Write-Host "SUMMARY" -ForegroundColor Cyan
Write-Host ("Total engines checked   : {0}" -f $total)
Write-Host ("HEALTHY (entered)        : {0}" -f $healthy) -ForegroundColor Green
Write-Host ("ARMED-ONLY (no entries)  : {0}" -f $armOnly) -ForegroundColor Yellow
Write-Host ("GATED (no arms)          : {0}" -f $gated)   -ForegroundColor DarkYellow
Write-Host ("STARVED (no ticks)       : {0}" -f $starved) -ForegroundColor Red
Write-Host ("MISSING (not init)       : {0}" -f $missing) -ForegroundColor Red

# -----------------------------------------------------------------------------
# Bonus diagnostics: build hash + tick-flow per symbol
# -----------------------------------------------------------------------------
"" | Out-File $reportPath -Encoding utf8 -Append
"BUILD HASH / RUNNING COMMIT" | Out-File $reportPath -Encoding utf8 -Append
"-" * 60 | Out-File $reportPath -Encoding utf8 -Append
$build = Select-String -Path $logPaths -Pattern "RUNNING COMMIT|GIT_HASH|git_hash|build_stamp|version_generated" -ErrorAction SilentlyContinue
if ($build) {
    ($build | Select-Object -First 5 | ForEach-Object { $_.Line }) | Out-File $reportPath -Encoding utf8 -Append
} else {
    "  [WARN] no build/commit identifier found in logs" | Out-File $reportPath -Encoding utf8 -Append
}

"" | Out-File $reportPath -Encoding utf8 -Append
"TICK COUNTS PER SYMBOL (last 24h)" | Out-File $reportPath -Encoding utf8 -Append
"-" * 60 | Out-File $reportPath -Encoding utf8 -Append
$symbols = @("XAUUSD","US500.F","USTEC.F","DJ30.F","NAS100","GER40","UK100","ESTX50",
             "EURUSD","GBPUSD","AUDUSD","NZDUSD","USDJPY","BRENT","USOIL.F",
             "VIX.F","DX.F")
foreach ($s in $symbols) {
    $tk = Select-String -Path $logPaths -Pattern ("\[TICK\] {0} " -f [regex]::Escape($s)) -ErrorAction SilentlyContinue
    $cnt = if ($tk) { @($tk).Count } else { 0 }
    ("  {0,-12} {1,8} TICK lines" -f $s, $cnt) | Out-File $reportPath -Encoding utf8 -Append
}

"" | Out-File $reportPath -Encoding utf8 -Append
"REPORT END" | Out-File $reportPath -Encoding utf8 -Append

Write-Host ""
Write-Host ("Full report saved to: {0}" -f $reportPath) -ForegroundColor Cyan
Write-Host "Paste the file contents back to Claude for diagnosis."
