#!/usr/bin/env bash
# archive_close_csv_gdrive.sh — daily Google-Drive archive of the FULL 565-name research
# CSV (operator: "daily removal of the csv to google drive to keep space free"). Runs on the
# MAC, where rclone + the gdrive: remote are already configured (the VPS has neither, and the
# VPS never holds the 15MB blob — it only gets the slim 39-name file, so VPS space is a non-
# issue by design). Keeps a dated copy in gdrive + a single rolling local file (no pile-up).
#
# Invoked from the already-scheduled Mac refresh (tools/rdagent/refresh_close_yf.py appends a
# call after it writes) — NO crontab edit. Idempotent + best-effort (never blocks the refresh).
set -uo pipefail
CSV="${1:-$HOME/Omega/data/rdagent/sp500_long_close.csv}"
REMOTE="${GDRIVE_REMOTE:-gdrive:omega/rdagent/close_archive}"
STAMP="$(TZ='UTC' date '+%Y-%m-%d')"

[ -f "$CSV" ] || { echo "[gdrive-archive] no CSV at $CSV — skip"; exit 0; }
command -v rclone >/dev/null 2>&1 || { echo "[gdrive-archive] rclone not found — skip"; exit 0; }

# dated archive copy (durable off-box backup); --no-traverse keeps it cheap
if rclone copyto "$CSV" "$REMOTE/sp500_long_close_${STAMP}.csv" --no-traverse 2>/tmp/gdrive_arch.err; then
  echo "[gdrive-archive] archived -> $REMOTE/sp500_long_close_${STAMP}.csv"
  # retention: keep 30 days of dated archives in gdrive, prune older to keep drive tidy
  rclone delete "$REMOTE" --min-age 30d --include "sp500_long_close_*.csv" 2>/dev/null \
    && echo "[gdrive-archive] pruned archives older than 30d"
else
  echo "[gdrive-archive] FAIL: $(head -1 /tmp/gdrive_arch.err 2>/dev/null)"
fi
exit 0
