#!/bin/bash
# chimera_executor_watch.sh — macOS banner when the LIVE crypto executor is not
# actually live (credentials failed / executor HALTED / engine down / booted SHADOW).
#
# WHY (operator, 2026-07-18): the chimera boot registry line advertises
# "runtime reflects HALTED when credentials fail" — but nothing WATCHED for that
# state. A creds failure keeps the engine running (sleeves shadow-compute, ticks
# and heartbeats stay fresh) so every existing freshness check stays GREEN while
# zero live orders can route. Silent-fallback class (see memory
# feedback-silent-fallback-stale-feed-class): gate the PRIMARY (executor LIVE),
# don't infer from downstream liveness.
#
# Detection (read-only ssh chimera-direct, AUDIT_PROBE_SAFETY: GET/read only):
#   RED if any of:
#     * chimera.service not active
#     * last "[STARTUP] RUNTIME MODE" line is not "= LIVE"
#     * last "[EXECUTOR] Ready." line is not "shadow=NO (LIVE)"
#     * any executor halt / credential-failure line AFTER the last boot
#       (HALTED, Invalid API-key, Binance -2014/-2015, 401, signature errors)
# One failed check is ignored (restart window / ssh blip); 2 consecutive fails
# (10 min apart via cron) => RED + banner. Re-notifies every 6h while RED;
# single "recovered" banner on green. ssh-unreachable is logged, not alarmed.
#
# Cron install: tools/install_chimera_executor_watch_cron.sh  (marker-line, idempotent)
# State: /tmp/chimera_executor_watch.state   Log: /tmp/chimera_executor_watch.log

STATE=/tmp/chimera_executor_watch.state
LOG=/tmp/chimera_executor_watch.log
RENOTIFY_S=$((6*3600))
now_e=$(date +%s)
ts() { date -u '+%Y-%m-%d %H:%MZ'; }
notify() {  # $1 title  $2 body
  /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" 2>/dev/null
}

PROBE=$(ssh -o ConnectTimeout=25 -o BatchMode=yes chimera-direct '
  act=$(systemctl is-active chimera 2>/dev/null)
  logf=/home/jo/ChimeraCrypto/logs/chimera.log
  mode=$(grep -F "[STARTUP] RUNTIME MODE" "$logf" 2>/dev/null | tail -1)
  execl=$(grep -F "[EXECUTOR] Ready" "$logf" 2>/dev/null | tail -1)
  bootln=$(grep -nF "[STARTUP] RUNTIME MODE" "$logf" 2>/dev/null | tail -1 | cut -d: -f1)
  halts=0
  if [ -n "$bootln" ]; then
    # NB: exclude the boot [REGISTRY] description line, which always contains
    # "HALTED when credentials fail" as documentation text, not a state.
    halts=$(tail -n +"$bootln" "$logf" | grep -vF "[REGISTRY]" | grep -ciE "EXECUTOR.*HALT|Invalid API-key|code.:-201[45]|401 Unauthorized|[Ss]ignature for this request is not valid")
  fi
  printf "ACT=%s\nMODE=%s\nEXEC=%s\nHALTS=%s\n" "$act" "$mode" "$execl" "$halts"
' 2>/dev/null)
SSH_RC=$?

# ssh itself failed (box unreachable) — don't alarm as executor-dead; log only.
if [ $SSH_RC -ne 0 ] || [ -z "$PROBE" ]; then
  echo "[$(ts)] SSH-UNREACHABLE (no verdict)" >> "$LOG"
  exit 0
fi

ACT=$(printf '%s\n' "$PROBE"  | sed -n 's/^ACT=//p')
MODE=$(printf '%s\n' "$PROBE" | sed -n 's/^MODE=//p')
EXECL=$(printf '%s\n' "$PROBE" | sed -n 's/^EXEC=//p')
HALTS=$(printf '%s\n' "$PROBE" | sed -n 's/^HALTS=//p'); HALTS=${HALTS:-0}

REASON=""
[ "$ACT" != "active" ]                                   && REASON="chimera.service not active ($ACT)"
[ -z "$REASON" ] && ! printf '%s' "$MODE" | grep -q "RUNTIME MODE = LIVE" \
                                                          && REASON="runtime mode not LIVE: ${MODE:-<no line>}"
[ -z "$REASON" ] && ! printf '%s' "$EXECL" | grep -q "shadow=NO (LIVE)" \
                                                          && REASON="executor not LIVE: ${EXECL:-<no line>}"
[ -z "$REASON" ] && [ "$HALTS" -gt 0 ]                    && REASON="$HALTS executor-halt/credential-failure line(s) since boot"

# read state: "<phase> <last_notify_epoch>"  phase in ok|fail1|red
read -r phase last_n 2>/dev/null < "$STATE" || true
phase=${phase:-ok}; last_n=${last_n:-0}

if [ -z "$REASON" ]; then
  if [ "$phase" = "red" ]; then
    notify "CRYPTO EXECUTOR RECOVERED" "chimera live order-routing is back (executor LIVE, no halt lines)."
    echo "[$(ts)] RECOVERED (was red)" >> "$LOG"
  fi
  echo "ok 0" > "$STATE"
  exit 0
fi

case "$phase" in
  ok)
    echo "fail1 0" > "$STATE"
    echo "[$(ts)] FAIL-1 (grace): $REASON" >> "$LOG"
    ;;
  fail1|red)
    if [ "$phase" != "red" ] || [ $((now_e - last_n)) -ge $RENOTIFY_S ]; then
      notify "CRYPTO EXECUTOR NOT LIVE" "$REASON — live crypto orders are NOT routing. Check chimera-direct creds/log."
      echo "red $now_e" > "$STATE"
      echo "[$(ts)] RED+NOTIFY: $REASON" >> "$LOG"
    else
      echo "[$(ts)] RED (renotify suppressed): $REASON" >> "$LOG"
    fi
    ;;
esac
exit 0
