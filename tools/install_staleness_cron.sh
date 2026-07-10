#!/usr/bin/env bash
# install_staleness_cron.sh — idempotently register tools/staleness_scan.sh on cron so the
# whole staleness surface (STALENESS_REGISTRY.md) is swept regularly, and a macOS
# notification fires when a LIVE feed goes stale. Committed + idempotent per the crontab
# safety rule (never inline-edit the crontab -> backup first, dedupe by marker).
#
# Usage:  bash tools/install_staleness_cron.sh          # install/update
#         bash tools/install_staleness_cron.sh --remove # remove
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MARKER="# OMEGA-STALENESS-SCAN"
SCAN="$ROOT/tools/staleness_scan.sh"
LOG="/tmp/omega_staleness_scan.log"
# every 30 min at an off-minute (fleet-friendly); notify on RED (live stale)
LINE="13,43 * * * * out=\$(/bin/bash $SCAN 2>&1); echo \"\$out\" > $LOG; echo \"\$out\" | grep -q 'RESULT: RED' && /usr/bin/osascript -e 'display notification \"LIVE feed stale — see $LOG\" with title \"Omega Staleness RED\"' $MARKER"

BAK="/tmp/crontab.bak.$(date +%s)"
crontab -l 2>/dev/null > "$BAK" || true
echo "[install] crontab backed up -> $BAK"

# strip any prior marker line, then optionally re-add
NEW="$(crontab -l 2>/dev/null | grep -v "$MARKER" || true)"
if [ "${1:-}" = "--remove" ]; then
  printf '%s\n' "$NEW" | crontab -
  echo "[install] removed OMEGA-STALENESS-SCAN cron."
  exit 0
fi
printf '%s\n%s\n' "$NEW" "$LINE" | crontab -
echo "[install] installed staleness scan cron (13,43 * * * *). Log: $LOG"
crontab -l | grep "$MARKER" >/dev/null && echo "[install] verified present."
