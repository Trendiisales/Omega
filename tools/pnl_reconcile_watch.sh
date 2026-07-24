#!/bin/bash
# pnl_reconcile_watch.sh — Mac-side driver for the PnL<->broker reconcile (audit gap 16).
# ssh's the box READ-ONLY, runs tools/pnl_broker_reconcile.py against the live gateway
# (127.0.0.1:4001) + the box ledger, and passes through the box's RESULT line + exit code.
# The reconcile is read-only (reqAccountSummary/reqPnL, no orders). Wrapped by
# monitor_crashsafe_wrap.sh (-q 'RESULT:' -R 'RESULT: (RED|BLIND)') so: no RESULT line =>
# box unreachable / py crash => MONITOR BROKEN; RED/BLIND => notify; GREEN => silent.
# ssh MUST start literally 'ssh omega-new' (feedback-vps-ssh-command-form).
set -u
OUT=$(ssh -o ConnectTimeout=25 -o BatchMode=yes omega-new "cd C:\\Omega && python tools\\pnl_broker_reconcile.py" 2>&1)
RC=$?
printf '%s\n' "$OUT"
exit $RC
