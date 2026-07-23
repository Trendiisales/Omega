#!/usr/bin/env bash
# install_live_routing_cron.sh — idempotent Mac-cron installer for the live-routing
# selftest (S-2026-07-24). Per the crontab rule (feedback-crontab-edit-via-script):
# committed idempotent script + timestamped backup, never inline crontab paste.
#
# WHY: live_routing_selftest.py was SESSION-ONLY — it only ran when a human invoked
# it. On 2026-07-24 the IBKR execution socket dropped and EVERY order blocked
# "not connected" (0 fills) all day; the ORDER-EXECUTION OUTCOME check that catches
# it existed but nobody was running it. A check nobody runs is part of the problem.
# This puts it on a 15-min cadence with a macOS banner on RED, routed through
# monitor_crashsafe_wrap.sh so a MONITOR crash is reported honestly (state UNKNOWN)
# rather than mis-notified as a system RED.
#
# Exit codes: 0 GREEN, 1 RED (notify), 2 unreachable (known, silent), 3 AMBER (known,
# silent — live-but-cert-pending). -k lists the known non-crash codes; -r 1 = only
# a hard RED raises the banner.
set -euo pipefail
TS=$(date +%s)
crontab -l > /tmp/ct.bak.$TS 2>/dev/null || true
WRAP=/Users/jo/Omega/tools/monitor_crashsafe_wrap.sh
LINE="*/15 * * * * $WRAP -n live_routing_selftest -l /tmp/live_routing_selftest.log -k 1,2,3 -r 1 -t \"🚦 ORDERS NOT ROUTING\" -m \"LIVE-ROUTING RED — orders not landing (reject/block storm or 0-fill despite live); see /tmp/live_routing_selftest.log\" -- /usr/bin/python3 /Users/jo/Omega/tools/live_routing_selftest.py # live-routing outcome S-2026-07-24"
( crontab -l 2>/dev/null | grep -v live_routing_selftest ; echo "$LINE" ) | crontab -
echo "installed (backup /tmp/ct.bak.$TS):"
crontab -l | grep live_routing_selftest
