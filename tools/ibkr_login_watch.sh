#!/bin/bash
# ibkr_login_watch.sh — macOS banner when IB Gateway needs OPERATOR action
# (weekly 2FA / login dialog, gateway process down, port flip, or dead exec path).
#
# WHY: the exec-socket reconnect watchdog (a1fb22f3) self-heals every drop the
# moment the gateway is back — but it cannot clear a login/2FA dialog (headless-
# unclearable, see memory feedback-ibkr-gateway-restart-caution). Operator asked
# (2026-07-18) for a notification when manual login is REQUIRED, nothing else.
#
# Three detections (all from ONE read-only ssh netstat pull):
#   1. LOGIN REQUIRED — API port 4001 not LISTENING on omega-new.
#   2. PORT FLIP (S-2026-07-18x class) — 4001 down but 4002 IS listening: a fresh
#      login came up on the paper/other port. Today's debacle: every consumer
#      dialed a dead port while a generic "login required" pointed at the wrong
#      fix. Alert names the real fix (relogin on 4001 or repoint consumers).
#   3. NO CLIENT (market hours only) — gateway LISTENING but ZERO local
#      ESTABLISHED client pairs on 4001: Omega.exe/bridges not connected, exec +
#      data path dead while the listener looks healthy. Skipped when FX closed
#      (Fri>=21Z, Sat, Sun<22Z) — weekend disconnects are legitimate.
#
# One failed check is ignored (nightly 23:45 London auto-restart window,
# transient ssh hiccup); 2 consecutive fails (checks 10 min apart via cron)
# => RED + banner. Re-notifies every 6h while RED; single "recovered" banner.
# Read-only ssh (AUDIT_PROBE_SAFETY: no mutation, no gateway restart).
#
# Cron install: tools/install_ibkr_login_watch_cron.sh   (marker-line, idempotent)
# State: /tmp/ibkr_login_watch.state   Log: /tmp/ibkr_login_watch.log

STATE=/tmp/ibkr_login_watch.state
LOG=/tmp/ibkr_login_watch.log
RENOTIFY_S=$((6*3600))
now_e=$(date +%s)
ts() { date -u '+%Y-%m-%d %H:%MZ'; }
notify() {  # $1 title  $2 body
  /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" 2>/dev/null
}

RAW=$(ssh -o ConnectTimeout=25 -o BatchMode=yes omega-new \
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
dow=$(date -u +%u); hr=$(date -u +%H); hr=${hr#0}
market_open=1
if [ "$dow" = "6" ] || { [ "$dow" = "5" ] && [ "$hr" -ge 21 ]; } || { [ "$dow" = "7" ] && [ "$hr" -lt 22 ]; }; then
  market_open=0
fi

# read state: "<phase> <last_notify_epoch>"  phase in ok|fail1|red|nc1|ncred
read -r phase last_n 2>/dev/null < "$STATE" || true
phase=${phase:-ok}; last_n=${last_n:-0}

# ssh itself failed (box unreachable) — don't alarm as login-required; log only.
if [ $SSH_RC -ne 0 ] && [ -z "$RAW" ] && ! ssh -o ConnectTimeout=25 -o BatchMode=yes omega-new "echo up" >/dev/null 2>&1; then
  echo "[$(ts)] SSH-UNREACHABLE (no verdict)" >> "$LOG"
  exit 0
fi

if [ "$LISTEN" -gt 0 ]; then
  # -------- gateway up. Exec-path check (market hours only). --------
  if [ "$market_open" = "1" ] && [ "$CLIENTS" -eq 0 ]; then
    case "$phase" in
      nc1|ncred)
        if [ "$phase" = "nc1" ] || [ $((now_e - last_n)) -ge $RENOTIFY_S ]; then
          notify "IBKR EXEC PATH DEAD" "Gateway 4001 up but ZERO connected clients 10+ min (market open) — Omega.exe/bridges not on the API. Orders + IBKR data dead. Check Omega service + OmegaIbkrBridge task on omega-new."
          echo "ncred $now_e" > "$STATE"
          echo "[$(ts)] NCRED — gateway up, 0 clients, notified" >> "$LOG"
        else
          echo "ncred $last_n" > "$STATE"
          echo "[$(ts)] NCRED — still 0 clients (renotify pending)" >> "$LOG"
        fi
        ;;
      *)
        echo "nc1 0" > "$STATE"
        echo "[$(ts)] nc1 — gateway up but 0 established clients (waiting for 2nd confirm)" >> "$LOG"
        ;;
    esac
    exit 0
  fi
  if [ "$phase" = "red" ] || [ "$phase" = "ncred" ]; then
    notify "IBKR RECOVERED" "4001 listening, clients connected — exec watchdog self-heals within 15s."
    echo "[$(ts)] RECOVERED (was $phase)" >> "$LOG"
  fi
  echo "ok 0" > "$STATE"
  exit 0
fi

# -------- 4001 down. Distinguish port flip from login-required. --------
if [ "$FLIP" -gt 0 ]; then
  TITLE="IBKR PORT FLIP"
  BODY="Gateway is listening on 4002, NOT 4001 — a login came up on the wrong port (S-2026-07-18x class). Every consumer dials 4001 and is now blind. Fix: re-login selecting live 4001, or repoint consumers (bbf2a4b1 list)."
else
  TITLE="IBKR LOGIN REQUIRED"
  BODY="Gateway 4001 down 10+ min on omega-new — login/2FA dialog or process down. Complete login on IBKR app / VPS screen. Orders blocked until then."
fi

case "$phase" in
  fail1|red)
    if [ "$phase" = "fail1" ] || [ $((now_e - last_n)) -ge $RENOTIFY_S ]; then
      notify "$TITLE" "$BODY"
      echo "red $now_e" > "$STATE"
      echo "[$(ts)] RED ($TITLE) — notified operator" >> "$LOG"
    else
      echo "red $last_n" > "$STATE"
      echo "[$(ts)] RED — still down (renotify pending)" >> "$LOG"
    fi
    ;;
  *)
    echo "fail1 0" > "$STATE"
    echo "[$(ts)] fail1 — 4001 not listening (flip4002=$FLIP, waiting for 2nd confirm)" >> "$LOG"
    ;;
esac
