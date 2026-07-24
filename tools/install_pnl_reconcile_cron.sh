#!/bin/bash
# install_pnl_reconcile_cron.sh — idempotent marker-line crontab install for the
# PnL<->broker-cash reconcile monitor (audit gap 16, S-2026-07-24). Every 2h it ssh's
# the box (read-only) and reconciles IBKR realized PnL vs the ledger; RED on divergence
# beyond threshold, BLIND if it can't read (fail loud, never false-green). Wrapped by
# monitor_crashsafe_wrap so a box-unreachable / py-crash reads as MONITOR BROKEN, not RED.
# Follows the operator crontab rule: committed idempotent script + backup, never an inline
# sed/heredoc (feedback-crontab-edit-via-script). Remove: `$0 remove`.
set -euo pipefail
MARKER="# OMEGA-PNL-RECONCILE"
REPO="/Users/jo/Omega"
LINE="0 */2 * * * ${REPO}/tools/monitor_crashsafe_wrap.sh -n pnl_reconcile -l /tmp/pnl_reconcile.log -k 1,2 -R \"RESULT: (RED|BLIND)\" -q \"RESULT:\" -t \"💰 PNL RECONCILE RED\" -m \"IBKR realized PnL diverges from the ledger (or reconcile went blind). See /tmp/pnl_reconcile.log\" -- /bin/bash ${REPO}/tools/pnl_reconcile_watch.sh $MARKER"
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
