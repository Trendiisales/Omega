#!/bin/bash
# omega_health_poll.sh — Mac-side ALARM for VPS health (operator-mandated 2026-06-27).
# The missing alarm layer: VPS omega_health_alarm.ps1 computes status; THIS pulls it to
# the Mac and SCREAMS (macOS notification + RED marker) when not GREEN. Cron-driven
# (NOT launchd — launchd python/agent triggers the "App Background Activity" popup).
set -uo pipefail
MARKER=/tmp/omega_health_RED.txt
TS=$(date '+%Y-%m-%dT%H:%M:%S')

# pull status json from VPS (short timeout so cron never hangs)
JSON=$(timeout 25 ssh -o ConnectTimeout=12 omega-vps \
  "powershell -NoProfile -Command \"if(Test-Path C:\\Omega\\logs\\HEALTH_STATUS.json){Get-Content C:\\Omega\\logs\\HEALTH_STATUS.json -Raw}\"" 2>/dev/null \
  | tr -d '\r' | sed -n '/{/,/}/p')

if [ -z "$JSON" ]; then
  # VPS unreachable IS an alarm condition (the "Omega unreachable" failure mode)
  osascript -e 'display notification "VPS unreachable — cannot read health status" with title "🔴 OMEGA HEALTH" sound name "Basso"' 2>/dev/null
  echo "$TS VPS-UNREACHABLE" > "$MARKER"
  echo "$TS VPS unreachable" >> /tmp/omega_health_poll.log
  exit 0
fi

OVERALL=$(echo "$JSON" | python3 -c "import sys,json;print(json.load(sys.stdin).get('overall','?'))" 2>/dev/null)
REASONS=$(echo "$JSON" | python3 -c "import sys,json;print(' | '.join(json.load(sys.stdin).get('reasons',[])))" 2>/dev/null)

echo "$TS overall=$OVERALL $REASONS" >> /tmp/omega_health_poll.log

if [ "$OVERALL" != "GREEN" ]; then
  osascript -e "display notification \"${REASONS:0:200}\" with title \"🔴 OMEGA HEALTH $OVERALL\" sound name \"Basso\"" 2>/dev/null
  echo "$TS $OVERALL  $REASONS" > "$MARKER"
else
  [ -f "$MARKER" ] && rm -f "$MARKER"
fi
