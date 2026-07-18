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
# 2026-06-24: WEEKDAY GATE. The task fired on Sat/Sun, writing rows with a frozen
# Friday-close spot + net_gex=0 (market shut -> delayed feed returns no greeks).
# Those dead rows poisoned the predicate study (fake low-vol neg-gamma). SPX index
# options trade Mon-Fri only -> skip weekends. (gex_chain.py also skips net_gex==0,
# catching Fri-evening/holiday closed-session rows the weekday gate can't.)
$dow=(Get-Date).ToUniversalTime().DayOfWeek
if($dow -eq "Saturday" -or $dow -eq "Sunday"){
  Add-Content "C:\Omega\logs\gex_snapshot.log" "[$ts] skip: weekend ($dow), market closed"
  exit 0
}
Add-Content "C:\Omega\logs\gex_snapshot.log" "[$ts] snapshot start"
foreach($ix in "SPX","NDX"){ & $py "C:\Omega\ibkr\gex_chain.py" --index $ix --port 4001 --expiries 1 --strikes-pct 2 --append $h --ts $ts *>> "C:\Omega\logs\gex_snapshot.log" }
