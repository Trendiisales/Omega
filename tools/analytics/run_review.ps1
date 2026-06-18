# run_review.ps1 — Omega continuous-improvement review. Runs the omnibus ledger
# analytics on the live shadow ledger + writes a dated, flagged report.
# Scheduled WEEKLY (task OmegaWeeklyReview). Operator/AI reviews the RANKED FLAGS
# and acts on the worst (loose-exit -> tighten trail; neg-expectancy -> cull;
# cost-fragile -> drop; regime-dependent -> add gate; broker-mismatch -> fill audit).
# See OMEGA.md "Continuous Improvement Loop".
$ErrorActionPreference = "SilentlyContinue"
$ledger = "C:\Omega\logs\trades\omega_trade_closes.csv"
$outdir = "C:\Omega\logs\analytics"
New-Item -ItemType Directory -Force -Path $outdir | Out-Null
$stamp  = Get-Date -Format "yyyy-MM-dd"
$report = Join-Path $outdir "review_$stamp.txt"
# combine cumulative + daily + shadow closes so thin live data is pooled
$tmp = Join-Path $env:TEMP "omega_all_closes.csv"
Get-ChildItem C:\Omega\logs\trades\*closes*.csv, C:\Omega\logs\trades\shadow\*.csv -ErrorAction SilentlyContinue |
    ForEach-Object { Get-Content $_.FullName } | Set-Content $tmp
# --since 2026-06-01 excludes the pre-fix 100x-contaminated archive (S-2026-06-18);
# verdicts are PF-based (contamination-immune) but this keeps the $ report clean too.
$since = "2026-06-01"
python C:\Omega\tools\analytics\ledger_analytics.py $tmp --min-n 5 --since $since *> $report
python C:\Omega\tools\analytics\capture_ratio.py $tmp --since $since *>> $report
Write-Output "[REVIEW] $stamp -> $report"
# surface the ranked flags to the service log so they're visible in monitoring
$flags = Select-String -Path $report -Pattern "RANKED FLAGS" -Context 0,12
if ($flags) { $flags.Context.PostContext | ForEach-Object { Write-Output "[WEEKLY-REVIEW] $_" } }
