#!/bin/bash
# install_chimera_executor_watch_cron.sh — idempotent marker-line crontab install
# for tools/chimera_executor_watch.sh (every 1 min, S-2026-07-18af: was 10 min;
# 3-strike grace in the watch => banner ~3min after true failure, and the
# /tmp/chimera_health.json feed behind the desk CC truth chip stays <=1min
# fresh; overlap lock in the watch stops ssh pile-up). Pattern per
# feedback-crontab-edit-via-script: backup first, marker-managed line, rerun-safe.
# Remove: `$0 remove`.
set -euo pipefail
MARKER="# OMEGA-CHIMERA-EXECUTOR-WATCH"
LINE="* * * * * /bin/bash /Users/jo/Omega/tools/chimera_executor_watch.sh >> /tmp/chimera_executor_watch_cron.log 2>&1 $MARKER"
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
