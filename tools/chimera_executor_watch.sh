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
#     * RESTART-LOOP: >3 systemd starts of chimera in the last 30 min. Added
#       S-2026-07-18ad after the 07-18 overnight incident: 93 restarts in 14h
#       (incl. a 60-crash FATAL mode-conflict loop 05:11-05:16Z) stayed GREEN
#       because `systemctl is-active` reads "active" while systemd flaps and
#       the last "RUNTIME MODE" line greps stale from the previous good boot.
#     * CONFIG MODE-CONFLICT: live_config.json shadow_mode/mode disagrees with
#       binance_credentials.json shadow_mode — the exact FATAL-crash-loop
#       precondition, caught at the FILE level so it alarms even between boots.
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
  starts30=$(sudo -n journalctl -u chimera --since "-30 min" --no-pager 2>/dev/null | grep -c "Started chimera")
  lcs=$(grep -o "\"shadow_mode\"[[:space:]]*:[[:space:]]*[a-z]*" /home/jo/ChimeraCrypto/config/live_config.json 2>/dev/null | head -1 | grep -o "[a-z]*$")
  lcm=$(grep -o "\"mode\"[[:space:]]*:[[:space:]]*\"[a-z]*\"" /home/jo/ChimeraCrypto/config/live_config.json 2>/dev/null | tail -1 | grep -o "[a-z]*\"$" | tr -d "\"")
  crs=$(grep -o "\"shadow_mode\"[[:space:]]*:[[:space:]]*[a-z]*" /home/jo/ChimeraCrypto/config/binance_credentials.json 2>/dev/null | head -1 | grep -o "[a-z]*$")
  printf "ACT=%s\nMODE=%s\nEXEC=%s\nHALTS=%s\nSTARTS30=%s\nLCSHADOW=%s\nLCMODE=%s\nCRSHADOW=%s\n" "$act" "$mode" "$execl" "$halts" "$starts30" "$lcs" "$lcm" "$crs"
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
STARTS30=$(printf '%s\n' "$PROBE" | sed -n 's/^STARTS30=//p'); STARTS30=${STARTS30:-0}
LCSHADOW=$(printf '%s\n' "$PROBE" | sed -n 's/^LCSHADOW=//p')
LCMODE=$(printf '%s\n' "$PROBE" | sed -n 's/^LCMODE=//p')
CRSHADOW=$(printf '%s\n' "$PROBE" | sed -n 's/^CRSHADOW=//p')

REASON=""
[ "$ACT" != "active" ]                                   && REASON="chimera.service not active ($ACT)"
# RESTART-LOOP check FIRST: it is the state in which every later grep lies
# (stale RUNTIME MODE line from the previous good boot, is-active flapping).
[ -z "$REASON" ] && [ "$STARTS30" -gt 3 ]                 && REASON="RESTART-LOOP: $STARTS30 chimera starts in 30min (crash loop — det windows zeroed every bounce, fires being eaten)"
# CONFIG MODE-CONFLICT at the file level (the 05:11Z FATAL-loop precondition):
# live_config says shadow (shadow_mode true or mode!=live) while creds say live,
# or vice versa. Empty parse fields = probe couldn't read a config — flag that too.
if [ -z "$REASON" ] && [ -n "$LCSHADOW" ] && [ -n "$CRSHADOW" ]; then
  lc_live=1; [ "$LCSHADOW" = "true" ] && lc_live=0; [ -n "$LCMODE" ] && [ "$LCMODE" != "live" ] && lc_live=0
  cr_live=1; [ "$CRSHADOW" = "true" ] && cr_live=0
  [ "$lc_live" != "$cr_live" ] && REASON="CONFIG MODE-CONFLICT: live_config(shadow_mode=$LCSHADOW mode=$LCMODE) vs creds(shadow_mode=$CRSHADOW) — next restart FATAL-loops"
fi
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
