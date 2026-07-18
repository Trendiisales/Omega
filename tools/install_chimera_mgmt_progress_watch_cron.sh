#!/bin/bash
# install_chimera_mgmt_progress_watch_cron.sh — idempotent marker-line crontab
# install for tools/chimera_mgmt_progress_watch.sh (every 10 min). Pattern per
# feedback-crontab-edit-via-script: backup first, marker-managed line, rerun-safe.
# Remove: `$0 remove`.
set -euo pipefail
MARKER="# OMEGA-CHIMERA-MGMT-PROGRESS-WATCH"
LINE="*/10 * * * * /bin/bash /Users/jo/Omega/tools/chimera_mgmt_progress_watch.sh >> /tmp/chimera_mgmt_watch_cron.log 2>&1 $MARKER"
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
