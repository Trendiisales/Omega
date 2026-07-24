#!/bin/bash
# Idempotent installer — schedules the real-time anomaly control loop (sentinel_act.py)
# every 5 min, ARMED (--arm --push). S-2026-07-24 (audit gap 5): the loop was cronned
# in DRY-RUN so it detected reject-storm/fill-drought/phantom, logged... and NEVER wrote
# the halt flag the binary polls — "the monitor that acts" didn't act. Now ARMED: on a
# HALT/DISABLE/CANCEL decision it writes outputs/control/halt_<system>.flag AND scp's it
# to the box control dir (--push). A high-severity catch (exit 2) also fires a macOS
# notification via the crashsafe wrapper, so a live failure is SURFACED + LOGGED to
# outputs/sentinel_decisions.jsonl within 5 min.
#   NOTE (owed, do NOT force): the BINARY POLL that honours the flag is deploy-gated
#   (BINARY_POLL_SPEC in sentinel_act.py) and the box dir C:/Omega/control/ did NOT exist
#   as of 2026-07-24 — until both land, --push is a no-op scp and the halt is one deploy
#   away. Arming now makes the flag write + escalate real (proof the loop acts); it does
#   not yet stop the live binary. Verify a flag lands end-to-end before trusting the halt.
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
LINE="*/5 * * * * ${REPO}/tools/monitor_crashsafe_wrap.sh -n sentinel_act -l /tmp/sentinel_act.log -k 2 -r 2 -t \"🛑 SENTINEL ACT — anomaly caught\" -m \"sentinel_act caught a live anomaly (LOT_SIZE/fill-drought/phantom/reject-storm) and decided a HALT. See outputs/sentinel_decisions.jsonl + /tmp/sentinel_act.log\" -- ${PY} ${REPO}/tools/ml_loss_miner/sentinel_act.py --arm --push # OMEGA-SENTINEL-ACT-LOOP"
( crontab -l 2>/dev/null | grep -v "OMEGA-SENTINEL-ACT-LOOP" ; echo "$LINE" ) | crontab -
echo "installed (backup /tmp/ct.bak.$TS):"
crontab -l | grep "OMEGA-SENTINEL-ACT-LOOP"
