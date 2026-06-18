# OmegaGexSnapshot wrapper — appends an hourly net-GEX regime snapshot per index
# to data/gex_history.csv (the dealer-gamma predicate-study dataset). Scheduled
# task OmegaGexSnapshot (daily 14:00 UTC + hourly repetition during the session).
#
# 2026-06-18: DROPPED DAX — EUREX index options return contracts with no conId,
# which gex_chain.py cannot hash -> ValueError crash + 1.5MB log spam. DAX is not
# a validated GEX index anyway (only SPX is, per omega-gex-dealer-gamma). NDX kept:
# it exits gracefully ("no greeks/OI") when unentitled and would start appending
# if NDX/OPRA entitlement is added (NDX GEX would directly gate NqMomentum/NAS).
$ts=(Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
$py="C:\Omega\bracket-bot\.venv\Scripts\python.exe"
$h="C:\Omega\data\gex_history.csv"
Add-Content "C:\Omega\logs\gex_snapshot.log" "[$ts] snapshot start"
foreach($ix in "SPX","NDX"){ & $py "C:\Omega\ibkr\gex_chain.py" --index $ix --port 4001 --expiries 1 --strikes-pct 2 --append $h --ts $ts *>> "C:\Omega\logs\gex_snapshot.log" }
