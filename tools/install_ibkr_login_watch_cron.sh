#!/bin/bash
# install_ibkr_login_watch_cron.sh — idempotent marker-line crontab install for
# tools/ibkr_login_watch.sh (EVERY 1 MIN since S-2026-07-18z: operator requires <=3-min
# detection; 3-strike in the watch keeps the false-alarm behavior of the old 10-min/2-strike).
# Pattern per feedback-crontab-edit-via-script:
# backup first, marker-managed line, rerun-safe. Remove: `$0 remove`.
set -euo pipefail
MARKER="# OMEGA-IBKR-LOGIN-WATCH"
LINE="* * * * * /bin/bash /Users/jo/Omega/tools/ibkr_login_watch.sh >> /tmp/ibkr_login_watch_cron.log 2>&1 $MARKER"
BAK="/tmp/ct.bak.$(date +%s)"
crontab -l 2>/dev/null > "$BAK" || true
echo "[install] crontab backed up to $BAK"
if [ "${1:-}" = "remove" ]; then
  grep -v "$MARKER" "$BAK" | crontab -
  echo "[install] removed."
  exit 0
fi
{ grep -v "$MARKER" "$BAK" || true; echo "$LINE"; } | crontab -
echo "[install] installed:"
crontab -l | grep "$MARKER"
