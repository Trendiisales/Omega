#!/bin/bash
# Idempotent installer for the AUPOS/AUNEG gold BE-floor companion cron rung.
# feedback-crontab-edit-via-script: NEVER inline sed/heredoc crontab edits (wiped the
# crontab once). This backs up first, then adds the rung ONLY if absent. Safe to re-run.
#
#   bash tools/install_gold_befloor_cron.sh          # install
#   bash tools/install_gold_befloor_cron.sh remove   # remove the rung
set -uo pipefail
TAG="gold_befloor_companion.py"
RUNG='* * * * * cd /Users/jo/Omega && GOLD_LOT=1.0 GOLD_PUSH_STATE=1 /usr/bin/python3 /Users/jo/Omega/tools/gold_befloor_companion.py >> /tmp/gold_befloor_companion.log 2>&1  # AUPOS/AUNEG gold BE-floor companion (PAPER, deploy-forward; S-2026-07-05)'

BAK="/tmp/ct.bak.$(date +%Y%m%d_%H%M%S)"
crontab -l > "$BAK" 2>/dev/null || { echo "no existing crontab; starting fresh"; : > "$BAK"; }
echo "crontab backed up -> $BAK ($(wc -l < "$BAK") lines)"

if [ "${1:-install}" = "remove" ]; then
  grep -v "$TAG" "$BAK" | crontab -
  echo "removed rung(s) containing $TAG"; crontab -l | grep -c "$TAG" | xargs echo "remaining matches:"
  exit 0
fi

if crontab -l 2>/dev/null | grep -q "$TAG"; then
  echo "rung already present — no change (idempotent)"; exit 0
fi

# append the single rung, preserving everything else verbatim
{ cat "$BAK"; echo "$RUNG"; } | crontab -
echo "installed AUPOS/AUNEG rung. crontab now $(crontab -l | wc -l | tr -d ' ') lines."
crontab -l | grep "$TAG"
