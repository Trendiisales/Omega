#!/usr/bin/env bash
# run_all.sh — one scheduled invocation of the Omega research loops.
#
# This is what launchd/cron calls. It runs ONE governed tick of each loop and
# then the dead-man monitor. There is NO while/sleep here — the schedule is the
# loop; each call does one bounded unit of work and exits. That is the
# structural guarantee against the runaway/hung-loop class that took the box
# down before.
#
# Tier 1 (discovery) is deliberately NOT here — it is a Workflow run on explicit
# operator opt-in with a finite candidate batch, not an autonomous schedule.
set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PYTHON:-python3}"

# Halt switch short-circuits everything.
if [ -f "${OMEGA_LOOP_DIR:-$HOME/.omega_loops}/HALT" ]; then
  echo "[run_all] HALT set — skipping all loops"; exit 0
fi

# Each tick is independently governed; a failure in one does not block the next.
"$PY" "$DIR/tier2_improvement.py"   || echo "[run_all] tier2 nonzero (continuing)"
"$PY" "$DIR/tier3_bt_integrity.py"  || echo "[run_all] tier3 nonzero (continuing)"

# Dead-man monitor last; --reclaim clears any tick that hung mid-body.
"$PY" "$DIR/monitor.py" --reclaim
exit 0
