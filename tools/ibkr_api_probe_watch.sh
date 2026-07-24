#!/bin/bash
# ibkr_api_probe_watch.sh — ACTIVE IBKR API liveness watch (un-fakeable outcome check).
# ssh's omega-new, runs the TWS-handshake probe on the box; a DOWN verdict => macOS
# notification. Closes the 2FA-stuck blind spot the port/socket/exec-log monitors all
# miss (they stay green while the API Server is disconnected). 3 strikes -> notify;
# renotify 15min while RED; single RECOVERED. Read-only (no gateway mutation).
STATE=/tmp/ibkr_api_probe.state; LOG=/tmp/ibkr_api_probe.log
[ -f "$HOME/.omega_ibkr_watch_off" ] && exit 0
notify(){ /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" >/dev/null 2>&1; }
ts(){ date -u '+%Y-%m-%d %H:%MZ'; }
OUT=$(ssh -o ConnectTimeout=20 -o BatchMode=yes omega-new "cd C:\Omega && python tools\ibkr_api_probe.py" 2>/dev/null)
[ -z "$OUT" ] && { echo "[$(ts)] ssh/probe unreachable — no verdict" >> "$LOG"; exit 0; }
read -r mode count last_n < "$STATE" 2>/dev/null || true; count=${count:-0}; last_n=${last_n:-0}; now_e=$(date +%s)
if printf '%s' "$OUT" | grep -q '^UP'; then
  [ "${count:-0}" -ge 3 ] && notify "IBKR API RECOVERED" "TWS handshake OK on 4001 — IBKR is answering again."
  echo "up 0 0" > "$STATE"; echo "[$(ts)] UP" >> "$LOG"; exit 0
fi
count=$((count+1))
if [ "$count" -ge 3 ]; then
  if [ $((now_e - last_n)) -ge $((15*60)) ]; then
    notify "IBKR API DEAD" "TWS handshake FAILED on 4001 (${OUT}). API Server disconnected — 2FA/login on omega-new. No fills, no data."
    echo "down $count $now_e" > "$STATE"; echo "[$(ts)] DOWN x$count NOTIFIED: $OUT" >> "$LOG"
  else echo "down $count $last_n" > "$STATE"; echo "[$(ts)] DOWN x$count (renotify pending)" >> "$LOG"; fi
else echo "down $count $last_n" > "$STATE"; echo "[$(ts)] DOWN strike $count/3: $OUT" >> "$LOG"; fi
exit 2
