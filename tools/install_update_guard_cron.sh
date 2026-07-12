#!/usr/bin/env bash
# IDEMPOTENT install of the software-update protection guard cron (S-2026-07-12).
# Notifies if ANY box's auto-reboot protection is weakened (would let an update
# restart a box + kill live trading). Backs up crontab, adds only if absent.
set -euo pipefail
MARK="update_guard S-2026-07-12"
WRAP="/Users/jo/Omega/tools/monitor_crashsafe_wrap.sh"
G="/Users/jo/Omega/tools/update_guard_selftest.py"
LINE="23 */2 * * * ${WRAP} -n update_guard -l /tmp/update_guard.log -k 2 -r 2 -q \"^RESULT:\" -t \"🔄 UPDATE PROTECTION BROKEN\" -m \"A box can auto-reboot for a software update — it will kill live trading. Re-lock it; see /tmp/update_guard.log\" -- /usr/bin/python3 ${G} # ${MARK}"
[ -x "$WRAP" ] && [ -f "$G" ] || { echo "FATAL: wrapper/guard missing"; exit 1; }
crontab -l > "/tmp/crontab.bak.$(date +%s)" 2>/dev/null || true
if crontab -l 2>/dev/null | grep -q "$MARK"; then echo "already installed"; exit 0; fi
{ crontab -l 2>/dev/null || true; echo "$LINE"; } | crontab -
echo "installed update_guard cron (23 */2 * * *):"; crontab -l | grep "$MARK" | sed 's/ -m ".*"//'
