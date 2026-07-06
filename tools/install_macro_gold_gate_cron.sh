#!/usr/bin/env bash
# install_macro_gold_gate_cron.sh — idempotent installer for the macro_gold_gate.py refresher.
#
# WHY: macro_gold_gate.py produces logs/macro/macro_gold_gate.tsv (real_yield + dollar + hostile
# score, last field = stamp_ms), the LIVE input the C++ MacroGoldGate reads. It was NOT in crontab
# and went 8.6d stale (DATA_HEALTH macrogoldgate.tsv HIGH). This adds it back at a 4h cadence,
# matching refresh_daily_feeds. Safe crontab discipline (feedback-crontab-edit-via-script): back up
# the current crontab to /tmp/ct.bak.<ts> BEFORE any change, append ONLY if the line is absent,
# never rewrite an existing line. Re-runnable (no-op if already installed).
set -euo pipefail

PY="${MACRO_GATE_PY:-/usr/bin/python3}"
SCRIPT="$HOME/Omega/tools/macro_gold_gate.py"
LOG="/tmp/macro_gold_gate.log"
MARK="# [macro-gold-gate refresher — install_macro_gold_gate_cron.sh]"
LINE="0 */4 * * * $PY $SCRIPT >> $LOG 2>&1  $MARK"

[ -f "$SCRIPT" ] || { echo "FATAL: $SCRIPT not found"; exit 1; }

TS="$(TZ='Pacific/Auckland' date '+%Y%m%d_%H%M%S')"
BAK="/tmp/ct.bak.$TS"
crontab -l > "$BAK" 2>/dev/null || : > "$BAK"
echo "[install] crontab backed up -> $BAK ($(wc -l < "$BAK") lines)"

if grep -qF "$SCRIPT" "$BAK"; then
  echo "[install] macro_gold_gate already scheduled — no change."
  exit 0
fi

# Append the new line to the EXACT backed-up content (never regenerate from scratch).
{ cat "$BAK"; echo "$LINE"; } | crontab -
echo "[install] added: $LINE"
echo "[install] crontab now $(crontab -l | wc -l) lines. Restore if wrong: crontab $BAK"
