#!/bin/bash
# omega_health_poll.sh — Mac-side ALARM + SELF-HEAL for VPS health (operator-mandated 2026-06-27;
# self-heal + staleness-detection added 2026-06-29 after recurring HEALTH_STATUS.json staleness).
#
# WHY THE REWRITE: the JSON was written by a one-shot (omega_health_alarm.ps1) driven by a Windows
# Scheduled Task on the 3GB/67MB-free box. That task kept getting killed/stalled by paging-thrash, so
# the JSON FROZE and this poll — which only read the *contents* — reported the stale value forever
# (the staleness blind spot the operator kept hitting). FIX: drive the writer FROM this reliable Mac
# 15-min cron (the writer is hang-safe: no git fetch, no process kill) AND alarm on TIMESTAMP AGE so a
# dead/stale writer is itself a loud, distinct alarm — not a silent frozen value.
set -uo pipefail
MARKER=/tmp/omega_health_RED.txt
TS=$(date '+%Y-%m-%dT%H:%M:%S')
STALE_MIN=25   # JSON older than this = writer dead/thrashed -> distinct STALE alarm

notify(){ osascript -e "display notification \"${1:0:200}\" with title \"$2\" sound name \"Basso\"" 2>/dev/null; }

# ONE ssh round-trip: (1) RUN the writer to regenerate the JSON fresh (removes the dead-scheduler
# dependency), then (2) emit the JSON. Short timeout so cron never hangs on the slow box.
JSON=$(timeout 40 ssh -o ConnectTimeout=12 omega-vps \
  "powershell -NoProfile -Command \"& { try { & C:\\Omega\\tools\\omega_health_alarm.ps1 } catch {}; if (Test-Path C:\\Omega\\logs\\HEALTH_STATUS.json) { Get-Content C:\\Omega\\logs\\HEALTH_STATUS.json -Raw } }\"" \
  2>/dev/null | tr -d '\r' | sed -n '/{/,/}/p')

if [ -z "$JSON" ]; then
  notify "VPS unreachable OR health writer failed to run — cannot read health status" "🔴 OMEGA HEALTH"
  echo "$TS VPS-UNREACHABLE-OR-WRITER-FAILED" > "$MARKER"
  echo "$TS VPS unreachable / writer failed" >> /tmp/omega_health_poll.log
  exit 0
fi

# --- STALENESS GUARD (the fix): trust the JSON only if its own ts is fresh ---
JTS=$(echo "$JSON" | python3 -c "import sys,json;print(json.load(sys.stdin).get('ts',''))" 2>/dev/null)
AGE_MIN=-1
if [ -n "$JTS" ]; then
  JEPOCH=$(date -j -u -f "%Y-%m-%dT%H:%M:%SZ" "$JTS" +%s 2>/dev/null)
  [ -n "$JEPOCH" ] && AGE_MIN=$(( ( $(date -u +%s) - JEPOCH ) / 60 ))
fi
if [ "$AGE_MIN" -ge "$STALE_MIN" ] 2>/dev/null || [ "$AGE_MIN" -lt 0 ] 2>/dev/null; then
  notify "HEALTH WRITER STALE/DEAD — JSON is ${AGE_MIN}min old (writer ran but did not refresh). VPS thrashing? Check RAM." "🔴 OMEGA HEALTH STALE"
  echo "$TS WRITER-STALE age=${AGE_MIN}min" > "$MARKER"
  echo "$TS WRITER-STALE age=${AGE_MIN}min" >> /tmp/omega_health_poll.log
  exit 0
fi

OVERALL=$(echo "$JSON" | python3 -c "import sys,json;print(json.load(sys.stdin).get('overall','?'))" 2>/dev/null)
REASONS=$(echo "$JSON" | python3 -c "import sys,json;print(' | '.join(json.load(sys.stdin).get('reasons',[])))" 2>/dev/null)
echo "$TS overall=$OVERALL age=${AGE_MIN}min $REASONS" >> /tmp/omega_health_poll.log

if [ "$OVERALL" != "GREEN" ]; then
  notify "${REASONS:0:200}" "🔴 OMEGA HEALTH $OVERALL"
  echo "$TS $OVERALL  $REASONS" > "$MARKER"
else
  [ -f "$MARKER" ] && rm -f "$MARKER"
fi
