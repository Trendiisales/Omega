#!/bin/bash
# ibkr_login_watch.sh — macOS banner when IB Gateway needs OPERATOR action
# (weekly 2FA / login dialog, gateway process down, port flip, or dead exec path).
#
# WHY: the exec-socket reconnect watchdog (a1fb22f3) self-heals every drop the
# moment the gateway is back — but it cannot clear a login/2FA dialog (headless-
# unclearable, see memory feedback-ibkr-gateway-restart-caution). Operator asked
# (2026-07-18) for a notification when manual login is REQUIRED, nothing else;
# then tightened (S-2026-07-18z): detection must land WITHIN 3 MINUTES — a dead
# exec path is live-money exposure, 20 min is not acceptable.
#
# Three detections (all from ONE read-only ssh netstat pull):
#   1. LOGIN REQUIRED — API port 4001 not LISTENING on omega-new.
#   2. PORT FLIP (S-2026-07-18x class) — 4001 down but 4002 IS listening: a fresh
#      login came up on the paper/other port. The 07-18 debacle: every consumer
#      dialed a dead port while a generic "login required" pointed at the wrong
#      fix. Alert names the real fix (relogin on 4001 or repoint consumers).
#   3. NO CLIENT (market hours only) — gateway LISTENING but ZERO local
#      ESTABLISHED client pairs on 4001: Omega.exe/bridges not connected, exec +
#      data path dead while the listener looks healthy. Skipped when FX closed
#      (Fri>=21Z, Sat, Sun<22Z) — weekend disconnects are legitimate.
#
# Cadence/latency: cron EVERY 1 MIN; 3 consecutive fails => banner, so worst-case
# operator notification ~3 min after failure. Renotify every 6h while RED; single
# "recovered" banner. Overlap-guarded (lock dir) so a hung ssh cannot pile up
# processes (project-omega-vps-ram-red-reaper). NIGHTLY QUIET WINDOW 22:40-23:05Z
# (gateway auto-restart ~23:45 London): strikes neither count nor reset — a real
# outage entering the window resumes counting on exit, restart blips never fire.
# Read-only ssh (AUDIT_PROBE_SAFETY: no mutation, no gateway restart).
#
# Cron install: tools/install_ibkr_login_watch_cron.sh   (marker-line, idempotent)
# State: /tmp/ibkr_login_watch.state ("<mode> <count> <last_notify_epoch>",
# mode in ok|down|noclient)   Log: /tmp/ibkr_login_watch.log

STATE=/tmp/ibkr_login_watch.state
LOG=/tmp/ibkr_login_watch.log
LOCK=/tmp/ibkr_login_watch.lock
RENOTIFY_S=$((6*3600))
STRIKES=3
now_e=$(date +%s)
ts() { date -u '+%Y-%m-%d %H:%MZ'; }
notify() {  # $1 title  $2 body
  /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" 2>/dev/null
}

# Overlap guard: skip this tick if a previous run still holds the lock (hung ssh).
# Stale locks (>5 min, crashed run) are reclaimed.
if ! mkdir "$LOCK" 2>/dev/null; then
  lock_age=$(( now_e - $(stat -f %m "$LOCK" 2>/dev/null || echo "$now_e") ))
  if [ "$lock_age" -lt 300 ]; then
    exit 0
  fi
  rm -rf "$LOCK"; mkdir "$LOCK" 2>/dev/null || exit 0
fi
trap 'rm -rf "$LOCK"' EXIT

RAW=$(ssh -o ConnectTimeout=20 -o BatchMode=yes omega-new \
  "netstat -ano | findstr \":4001 :4002\"" 2>/dev/null)
SSH_RC=$?

# Column padding means a real port ends with whitespace — ':4001 ' cannot match :40012.
LISTEN=$(echo "$RAW" | grep ':4001 ' | grep -c LISTENING)
FLIP=$(echo "$RAW"   | grep ':4002 ' | grep -c LISTENING)
# Local client pair = loopback address WITH the API port, ESTABLISHED (both sides of a
# local pair show '127.0.0.1:4001'; the gateway's own uplink to IBKR shows 8.x:4001 and
# must NOT count as a client).
CLIENTS=$(echo "$RAW" | grep '127\.0\.0\.1:4001 ' | grep -c ESTABLISHED)

# FX market window (UTC): closed Fri>=21Z, all Sat, Sun<22Z.
dow=$(date -u +%u); hr=$(date -u +%H); hr=${hr#0}; mi=$(date -u +%M); mi=${mi#0}
market_open=1
if [ "$dow" = "6" ] || { [ "$dow" = "5" ] && [ "$hr" -ge 21 ]; } || { [ "$dow" = "7" ] && [ "$hr" -lt 22 ]; }; then
  market_open=0
fi
# Nightly gateway auto-restart quiet window 22:40-23:05Z (23:45 London BST is 22:45Z).
quiet=0
hm=$((hr*60 + mi))
if [ "$hm" -ge $((22*60+40)) ] && [ "$hm" -le $((23*60+5)) ]; then quiet=1; fi

# read state
read -r mode count last_n 2>/dev/null < "$STATE" || true
mode=${mode:-ok}; count=${count:-0}; last_n=${last_n:-0}

# ssh itself failed (box unreachable) — don't alarm as login-required; log only.
if [ $SSH_RC -ne 0 ] && [ -z "$RAW" ] && ! ssh -o ConnectTimeout=20 -o BatchMode=yes omega-new "echo up" >/dev/null 2>&1; then
  echo "[$(ts)] SSH-UNREACHABLE (no verdict)" >> "$LOG"
  exit 0
fi

fire() {  # $1 newmode  $2 title  $3 body
  if [ "$count" -ge "$STRIKES" ] && [ $((now_e - last_n)) -lt $RENOTIFY_S ]; then
    echo "$1 $count $last_n" > "$STATE"
    echo "[$(ts)] $1 x$count — RED holding (renotify pending)" >> "$LOG"
    return
  fi
  notify "$2" "$3"
  echo "$1 $STRIKES $now_e" > "$STATE"
  echo "[$(ts)] $1 x$count — RED, notified operator ($2)" >> "$LOG"
}

strike() {  # $1 newmode  $2 title  $3 body
  if [ "$quiet" = "1" ]; then
    echo "[$(ts)] $1 — in nightly quiet window, strike held at $count" >> "$LOG"
    return
  fi
  if [ "$mode" != "$1" ]; then count=0; fi
  count=$((count+1))
  if [ "$count" -ge "$STRIKES" ]; then
    fire "$1" "$2" "$3"
  else
    echo "$1 $count $last_n" > "$STATE"
    echo "[$(ts)] $1 strike $count/$STRIKES" >> "$LOG"
  fi
}

if [ "$LISTEN" -gt 0 ]; then
  # -------- gateway up. Exec-path check (market hours only). --------
  if [ "$market_open" = "1" ] && [ "$CLIENTS" -eq 0 ]; then
    strike noclient "IBKR EXEC PATH DEAD" "Gateway 4001 up but ZERO connected clients ${STRIKES}+ min (market open) — Omega.exe/bridges not on the API. Orders + IBKR data dead. Check Omega service + OmegaIbkrBridge task on omega-new."
    exit 0
  fi
  if [ "$count" -ge "$STRIKES" ]; then
    notify "IBKR RECOVERED" "4001 listening, clients connected — exec watchdog self-heals within 15s."
    echo "[$(ts)] RECOVERED (was $mode)" >> "$LOG"
  fi
  echo "ok 0 0" > "$STATE"
  exit 0
fi

# -------- 4001 down. Distinguish port flip from login-required. --------
if [ "$FLIP" -gt 0 ]; then
  strike down "IBKR PORT FLIP" "Gateway is listening on 4002, NOT 4001 — a login came up on the wrong port (S-2026-07-18x class). Every consumer dials 4001 and is now blind. Fix: re-login selecting live 4001, or repoint consumers (bbf2a4b1 list)."
else
  strike down "IBKR LOGIN REQUIRED" "Gateway 4001 down ${STRIKES}+ min on omega-new — login/2FA dialog or process down. Complete login on IBKR app / VPS screen. Orders blocked until then."
fi
