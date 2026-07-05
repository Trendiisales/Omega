#!/usr/bin/env bash
# retire_stall_python_cron.sh — idempotent retirement of the Mac-cron stall_accountant.py
# gold/index PROTECTION zoo, now that it is a native C++ engine in Omega.exe (StallCompanion,
# commit 95565d49). Run ONLY AFTER the VPS binary is verified live + writing companion_state.json.
#
# Removes the ACTIVE (non-comment) cron lines that:
#   * run stall_accountant.py with SKIP_CRYPTO=1   (the 25 Omega gold/index books)
#   * run companion_aggregate.py                    (the desk roll-up pusher)
# KEEPS: the 3 SKIP_OMEGA crypto-intraday stall lines (separate crypto binary, cohort 4);
#        every comment line (history/context preserved); everything unrelated.
#
# SAFETY (feedback-crontab-edit-via-script — a prior inline sed/heredoc WIPED the crontab):
#   * backs the live crontab up to /tmp/ct.bak.<epoch> BEFORE any change
#   * builds the new crontab with awk into a temp file, prints a diff summary
#   * installs via `crontab <file>` (atomic); idempotent (2nd run = no-op)
#   * restore with:  crontab /tmp/ct.bak.<epoch>
set -euo pipefail

TS=$(date +%s)
BAK="/tmp/ct.bak.${TS}"
NEW="/tmp/ct.new.${TS}"

crontab -l > "$BAK" 2>/dev/null || { echo "no crontab to edit"; exit 0; }
echo "[retire] backed up live crontab -> $BAK ($(wc -l <"$BAK" | tr -d ' ') lines)"

# Drop ONLY active lines matching the two retire patterns; keep comments + everything else.
awk '
  /^[[:space:]]*#/ { print; next }                                   # keep comments verbatim
  /companion_aggregate\.py/ { next }                                 # drop active aggregate pusher
  /stall_accountant\.py/ && /SKIP_CRYPTO=1/ { next }                 # drop active Omega gold/index books
  { print }                                                          # keep all else (incl. SKIP_OMEGA crypto)
' "$BAK" > "$NEW"

REMOVED=$(( $(wc -l <"$BAK") - $(wc -l <"$NEW") ))
echo "[retire] lines removed: $REMOVED"
if [ "$REMOVED" -eq 0 ]; then
  echo "[retire] nothing to remove — already retired (idempotent no-op). Leaving crontab unchanged."
  rm -f "$NEW"; exit 0
fi

echo "[retire] --- lines being removed: ---"
# NOTE `|| true`: under `set -o pipefail`, `diff` returns 1 when the files differ
# (which they always do here) and aborts the script BEFORE `crontab "$NEW"` installs —
# leaving the crontab UNCHANGED while the removed-lines summary is already printed
# (looks like it worked; didn't). The `|| true` neutralises diff's expected non-zero.
{ diff <(grep -v '^[[:space:]]*#' "$BAK") <(grep -v '^[[:space:]]*#' "$NEW") || true; } | grep '^<' | sed 's/^< /  - /' | sed 's/\(.\{140\}\).*/\1 .../' || true

# sanity: the 3 crypto SKIP_OMEGA lines MUST survive
KEEP_CRYPTO=$(grep -c 'stall_accountant\.py' "$NEW" || true)
echo "[retire] stall_accountant.py lines still active after retire (expect 3 crypto-intraday): $KEEP_CRYPTO"

crontab "$NEW"
echo "[retire] installed new crontab. Restore if needed:  crontab $BAK"
rm -f "$NEW"
