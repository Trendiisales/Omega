#!/bin/bash
# ibkr_login_watch.sh — macOS banner when IB Gateway needs OPERATOR action
# (weekly 2FA / login dialog, or gateway process down).
#
# WHY: the exec-socket reconnect watchdog (a1fb22f3) self-heals every drop the
# moment the gateway is back — but it cannot clear a login/2FA dialog (headless-
# unclearable, see memory feedback-ibkr-gateway-restart-caution). Operator asked
# (2026-07-18) for a notification when manual login is REQUIRED, nothing else.
#
# Detection: API port 4002 not LISTENING on omega-new. One failed check is
# ignored (nightly 23:45 London auto-restart window, transient ssh hiccup);
# 2 consecutive fails (checks are 10 min apart via cron) => RED + banner.
# Re-notifies every 6h while RED; single "recovered" banner on green.
# Read-only ssh (AUDIT_PROBE_SAFETY: no mutation, no gateway restart — fix is
# always the operator completing login on the IBKR mobile app / VPS screen).
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

LISTEN=$(ssh -o ConnectTimeout=25 -o BatchMode=yes omega-new \
  "netstat -an | findstr :4002 | findstr LISTENING" 2>/dev/null | head -1)
SSH_RC=$?

# read state: "<phase> <last_notify_epoch>"  phase in ok|fail1|red
read -r phase last_n 2>/dev/null < "$STATE" || true
phase=${phase:-ok}; last_n=${last_n:-0}

if [ -n "$LISTEN" ]; then
  if [ "$phase" = "red" ]; then
    notify "IBKR Gateway RECOVERED" "4002 listening again — exec watchdog will self-heal within 15s."
    echo "[$(ts)] RECOVERED (was red)" >> "$LOG"
  fi
  echo "ok 0" > "$STATE"
  exit 0
fi

# ssh itself failed (box unreachable) — don't alarm as login-required; log only.
if [ $SSH_RC -ne 0 ] && [ -z "$LISTEN" ] && ! ssh -o ConnectTimeout=25 -o BatchMode=yes omega-new "echo up" >/dev/null 2>&1; then
  echo "[$(ts)] SSH-UNREACHABLE (no verdict)" >> "$LOG"
  exit 0
fi

case "$phase" in
  ok)
    echo "fail1 0" > "$STATE"
    echo "[$(ts)] fail1 — 4002 not listening (waiting for 2nd confirm)" >> "$LOG"
    ;;
  fail1|red)
    if [ "$phase" = "fail1" ] || [ $((now_e - last_n)) -ge $RENOTIFY_S ]; then
      notify "IBKR LOGIN REQUIRED" "Gateway 4002 down 10+ min on omega-new — login/2FA dialog or process down. Complete login on IBKR app / VPS screen. Orders blocked until then."
      echo "red $now_e" > "$STATE"
      echo "[$(ts)] RED — notified operator" >> "$LOG"
    else
      echo "red $last_n" > "$STATE"
      echo "[$(ts)] RED — still down (renotify pending)" >> "$LOG"
    fi
    ;;
esac
