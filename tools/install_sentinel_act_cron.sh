#!/bin/bash
# Idempotent installer — schedules the real-time anomaly control loop (sentinel_act.py)
# every 5 min in DRY-RUN (decide + log + alert; emits NO halt flag until --arm). A
# high-severity catch (exit 2) fires a macOS notification via the crashsafe wrapper,
# so a live failure (LOT_SIZE storm / fill-drought / phantom / reject-storm) is
# SURFACED + LOGGED to outputs/sentinel_decisions.jsonl within 5 min — the ACT layer
# actually running, not just built. Arm (--arm) once the binary halt-flag poll deploys.
# Follows the operator crontab rule: committed idempotent script + backup, never an
# inline sed/heredoc (feedback-crontab-edit-via-script).
set -euo pipefail
TS=$(date +%s)
crontab -l > /tmp/ct.bak.$TS 2>/dev/null || true
REPO="/Users/jo/Omega"
# MUST use the miniforge python that has numpy — sentinel_act imports trade_sentinel
# which imports numpy. /usr/bin/python3 has NO numpy → import dies → the crypto branch
# is swallowed → every run falsely reports "healthy" (audit 2026-07-24b caught this).
# Same interpreter the sibling trade_sentinel cron uses.
PY="/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python"
LINE="*/5 * * * * ${REPO}/tools/monitor_crashsafe_wrap.sh -n sentinel_act -l /tmp/sentinel_act.log -k 2 -r 2 -t \"🛑 SENTINEL ACT — anomaly caught\" -m \"sentinel_act caught a live anomaly (LOT_SIZE/fill-drought/phantom/reject-storm) and decided a HALT. See outputs/sentinel_decisions.jsonl + /tmp/sentinel_act.log\" -- ${PY} ${REPO}/tools/ml_loss_miner/sentinel_act.py # OMEGA-SENTINEL-ACT-LOOP"
( crontab -l 2>/dev/null | grep -v "OMEGA-SENTINEL-ACT-LOOP" ; echo "$LINE" ) | crontab -
echo "installed (backup /tmp/ct.bak.$TS):"
crontab -l | grep "OMEGA-SENTINEL-ACT-LOOP"
