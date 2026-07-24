#!/bin/bash
# position_protection_watch.sh — DE RIGUEUR broker stops on EVERY position.
# Self-healing: ssh's the box, runs place_stops.py which places a disaster STP on any
# held LONG that lacks a protective SELL order (idempotent — skips ones already stopped,
# skips BMY's resting close). If it HAD to place any (something was naked), notify — that
# means the exec's on-fill stop missed one. This is the guarantee that no position ever
# sits unprotected, and the alert if it ever does. Read-only except placing protective stops.
STATE=/tmp/position_protection.state; LOG=/tmp/position_protection.log
notify(){ /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" >/dev/null 2>&1; }
ts(){ date -u '+%Y-%m-%d %H:%MZ'; }
OUT=$(ssh -o ConnectTimeout=20 -o BatchMode=yes omega-new "cd C:\Omega && python tools\place_stops.py" 2>/dev/null)
[ -z "$OUT" ] && { echo "[$(ts)] probe unreachable" >> "$LOG"; exit 0; }
PLACED=$(printf '%s' "$OUT" | grep -cE "SELL STP.*PreSubmitted|SELL STP.*Submitted")
if [ "$PLACED" -gt 0 ]; then
  SYMS=$(printf '%s' "$OUT" | grep -E "SELL STP.*(Pre)?Submitted" | awk -F: '{print $1}' | tr '\n' ' ')
  notify "🛑 NAKED POSITION HEALED" "Placed missing broker stop(s) on: ${SYMS}-- the exec on-fill stop MISSED these. Investigate."
  echo "[$(ts)] HEALED naked: $SYMS" >> "$LOG"
else
  echo "[$(ts)] OK all stopped" >> "$LOG"
fi
