#!/bin/bash
# binance_api_probe_watch.sh — ACTIVE Binance API liveness (crypto twin of the IBKR
# probe, both-systems rule). ssh's chimera-direct and requires a real /api/v3/time
# reply from the box — proves Binance is actually answering the LIVE box (a dead
# connection / IP ban / network drop is invisible to file-freshness monitors). 3
# strikes -> macOS notify; renotify 15min; single RECOVERED. Read-only.
STATE=/tmp/binance_api_probe.state; LOG=/tmp/binance_api_probe.log
[ -f "$HOME/.chimera_watch_off" ] && exit 0
notify(){ /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" >/dev/null 2>&1; }
ts(){ date -u '+%Y-%m-%d %H:%MZ'; }
OUT=$(ssh -o ConnectTimeout=20 -o BatchMode=yes chimera-direct "curl -s -m 8 https://api.binance.com/api/v3/time" 2>/dev/null)
[ -z "$OUT" ] && OUT="(no response)"
read -r mode count last_n < "$STATE" 2>/dev/null || true; count=${count:-0}; last_n=${last_n:-0}; now_e=$(date +%s)
if printf '%s' "$OUT" | grep -q 'serverTime'; then
  [ "${count:-0}" -ge 3 ] && notify "BINANCE API RECOVERED" "/api/v3/time OK from the box — Binance answering again."
  echo "up 0 0" > "$STATE"; echo "[$(ts)] UP" >> "$LOG"; exit 0
fi
count=$((count+1))
if [ "$count" -ge 3 ]; then
  if [ $((now_e - last_n)) -ge $((15*60)) ]; then
    notify "BINANCE API DEAD" "Box cannot reach Binance /api/v3/time (${OUT}). Crypto book is BLIND — no data, no fills. Check network / IP ban / connection on chimera-direct."
    echo "down $count $now_e" > "$STATE"; echo "[$(ts)] DOWN x$count NOTIFIED: $OUT" >> "$LOG"
  else echo "down $count $last_n" > "$STATE"; echo "[$(ts)] DOWN x$count (renotify pending)" >> "$LOG"; fi
else echo "down $count $last_n" > "$STATE"; echo "[$(ts)] DOWN strike $count/3: $OUT" >> "$LOG"; fi
exit 2
