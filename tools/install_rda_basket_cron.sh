#!/bin/bash
# Idempotent installer for the hourly RD-Agent basket executor cron rung.
# feedback-crontab-edit-via-script: NEVER inline sed/heredoc crontab edits (wiped the
# crontab once, 2026-07-04). Backs up first, adds the rung ONLY if absent. Safe to re-run.
#
# Why (S-2026-07-14h): /tmp/rda_basket.json previously refreshed ONLY via the 4h
# qlib_refresh retrain path (retrain_qlib.sh), so an intraday 3% re-export left the desk
# STOCK BASKET panel stale ("no BUY names") for up to 4h — S-2026-07-14f root-cause note.
# Hourly :05 run keeps the paper book turning over within the hour. run_basket_daily.sh
# takes ~60-90s (yfinance close freshen) and is idempotent (no-op when book == target).
#
#   bash tools/install_rda_basket_cron.sh          # install
#   bash tools/install_rda_basket_cron.sh remove   # remove the rung
set -uo pipefail
TAG="run_basket_daily.sh"
RUNG='5 * * * * /bin/bash /Users/jo/Omega/tools/rdagent/run_basket_daily.sh >> /tmp/rda_basket_cron.log 2>&1  # OMEGA-RDA-BASKET-HOURLY (paper/shadow basket refresh; S-2026-07-14h)'

BAK="/tmp/ct.bak.$(date +%Y%m%d_%H%M%S)"
crontab -l > "$BAK" 2>/dev/null || { echo "no existing crontab; starting fresh"; : > "$BAK"; }
echo "crontab backed up -> $BAK ($(wc -l < "$BAK") lines)"

if [ "${1:-install}" = "remove" ]; then
  grep -v "$TAG" "$BAK" | crontab -
  echo "removed rung(s) containing $TAG"
  crontab -l | grep -c "$TAG" | xargs echo "remaining matches:"
  exit 0
fi

if crontab -l 2>/dev/null | grep -q "$TAG"; then
  echo "rung already present — no change (idempotent)"; exit 0
fi

{ cat "$BAK"; echo "$RUNG"; } | crontab -
echo "installed hourly basket rung. crontab now $(crontab -l | wc -l | tr -d ' ') lines."
crontab -l | grep "$TAG"
