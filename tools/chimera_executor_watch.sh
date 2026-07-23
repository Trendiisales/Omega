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
# Two failed checks are ignored (restart window / ssh blip); 3 consecutive fails
# (1 min apart via cron, ibkr_login_watch 18z precedent) => RED + banner ~3min
# after true failure. Re-notifies every 6h while RED; single "recovered" banner
# on green. ssh-unreachable is logged, not alarmed.
#
# S-2026-07-18af "BETTER CHECK" (operator ultimatum after the 02:40Z banner was
# missed): every run ALSO writes /tmp/chimera_health.json {ts,ok,reason,build,
# headsha,mode,uptime_s,starts30,...} — relayed to omega-new by
# refresh_crypto_companion.sh HOP 4 and rendered as the always-on CC truth chip
# on the desk header (GUI treats stale ts as RED: a dead watch/relay counts as
# NOT-VERIFIED, never silently green). Cadence 10min -> 1min same session;
# overlap lock dir so a hung ssh cannot pile processes
# (the VPS RAM-RED ssh-pileup reaper precedent).
#
# Cron install: tools/install_chimera_executor_watch_cron.sh  (marker-line, idempotent)
# State: /tmp/chimera_executor_watch.state   Log: /tmp/chimera_executor_watch.log

STATE=/tmp/chimera_executor_watch.state
LOG=/tmp/chimera_executor_watch.log
HEALTH=/tmp/chimera_health.json
RENOTIFY_S=$((6*3600))
now_e=$(date +%s)

# overlap lock: at 1-min cadence a hung ssh must not pile processes. Stale lock
# (>10 min: holder died without cleanup) is reaped, else this run just skips.
LOCKDIR=/tmp/chimera_executor_watch.lock
if ! mkdir "$LOCKDIR" 2>/dev/null; then
  lock_age=$(( now_e - $(stat -f %m "$LOCKDIR" 2>/dev/null || echo "$now_e") ))
  if [ "$lock_age" -gt 600 ]; then
    rm -rf "$LOCKDIR"; mkdir "$LOCKDIR" 2>/dev/null || exit 0
  else
    exit 0
  fi
fi
trap 'rmdir "$LOCKDIR" 2>/dev/null' EXIT
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
  # ORDER-EXECUTION health (S-2026-07-24): count Binance order rejects, order
  # intents, and real fills SINCE BOOT. A LOT_SIZE/-1013 reject storm keeps the
  # executor LIVE + un-halted while every order bounces (qty not rounded to step)
  # -> signals fire, 0 fills, nothing trades. None of the checks above see it.
  rejects=0; intents=0; fills=0
  if [ -n "$bootln" ]; then
    rejects=$(tail -n +"$bootln" "$logf" | grep -ciE "submit failed|Filter failure|code.:-1013|code.:-1111|code.:-1013|code.:-2010")
    intents=$(tail -n +"$bootln" "$logf" | grep -ciE "TRENDROSTER-INTENT\] tag=|MIMIC-LIVE\] BUY allowlisted|governed_submit.*BUY")
    fills=$(tail -n +"$bootln" "$logf" | grep -ciE "\[LIVE-FILL\]|EXECUTOR\] FILLED|status=FILLED")
  fi
  starts30=$(sudo -n journalctl -u chimera --since "-30 min" --no-pager 2>/dev/null | grep -c "Started chimera")
  lcs=$(grep -o "\"shadow_mode\"[[:space:]]*:[[:space:]]*[a-z]*" /home/jo/ChimeraCrypto/config/live_config.json 2>/dev/null | head -1 | grep -o "[a-z]*$")
  lcm=$(grep -o "\"mode\"[[:space:]]*:[[:space:]]*\"[a-z]*\"" /home/jo/ChimeraCrypto/config/live_config.json 2>/dev/null | tail -1 | grep -o "[a-z]*\"$" | tr -d "\"")
  crs=$(grep -o "\"shadow_mode\"[[:space:]]*:[[:space:]]*[a-z]*" /home/jo/ChimeraCrypto/config/binance_credentials.json 2>/dev/null | head -1 | grep -o "[a-z]*$")
  build=$(grep -F "Tier-2 Edge Engines" "$logf" 2>/dev/null | tail -1 | grep -o "build=[a-f0-9]*" | cut -d= -f2)
  headsha=$(cd /home/jo/ChimeraCrypto && git rev-parse --short=7 HEAD 2>/dev/null)
  upts=$(systemctl show chimera -p ActiveEnterTimestamp --value 2>/dev/null)
  upe=""; [ -n "$upts" ] && upe=$(date -d "$upts" +%s 2>/dev/null)
  nowb=$(date +%s); ups=""
  case "$upe" in (*[0-9]) ups=$((nowb-upe));; esac
  printf "ACT=%s\nMODE=%s\nEXEC=%s\nHALTS=%s\nSTARTS30=%s\nLCSHADOW=%s\nLCMODE=%s\nCRSHADOW=%s\nBUILD=%s\nHEADSHA=%s\nUPTIME=%s\nREJECTS=%s\nINTENTS=%s\nFILLS=%s\n" "$act" "$mode" "$execl" "$halts" "$starts30" "$lcs" "$lcm" "$crs" "$build" "$headsha" "$ups" "$rejects" "$intents" "$fills"
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
BUILD=$(printf '%s\n' "$PROBE" | sed -n 's/^BUILD=//p')
HEADSHA=$(printf '%s\n' "$PROBE" | sed -n 's/^HEADSHA=//p')
UPTIME_S=$(printf '%s\n' "$PROBE" | sed -n 's/^UPTIME=//p'); UPTIME_S=${UPTIME_S:-0}
REJECTS=$(printf '%s\n' "$PROBE" | sed -n 's/^REJECTS=//p'); REJECTS=${REJECTS:-0}
INTENTS=$(printf '%s\n' "$PROBE" | sed -n 's/^INTENTS=//p'); INTENTS=${INTENTS:-0}
FILLS=$(printf '%s\n' "$PROBE" | sed -n 's/^FILLS=//p'); FILLS=${FILLS:-0}

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
# BUILD-MISMATCH (operator demand 2026-07-18: "see immediately if the correct
# software has loaded"): the RUNNING binary's boot-stamped build= hash must be
# a prefix-match of the repo HEAD on the box. A deploy that committed+pushed
# but restarted an old binary (or never rebuilt) alarms here instead of being
# discovered days later. Same class as Omega header-wire-incremental-stale-build.
if [ -z "$REASON" ] && [ -n "$BUILD" ] && [ -n "$HEADSHA" ]; then
  case "$HEADSHA" in
    "$BUILD"*) : ;;  # running build is HEAD (stamp may be shorter than rev-parse)
    *) case "$BUILD" in
         "$HEADSHA"*) : ;;
         *) REASON="BUILD-MISMATCH: running binary build=$BUILD but repo HEAD=$HEADSHA — wrong/stale software loaded, rebuild+restart" ;;
       esac ;;
  esac
fi
[ -z "$REASON" ] && ! printf '%s' "$MODE" | grep -q "RUNTIME MODE = LIVE" \
                                                          && REASON="runtime mode not LIVE: ${MODE:-<no line>}"
[ -z "$REASON" ] && ! printf '%s' "$EXECL" | grep -q "shadow=NO (LIVE)" \
                                                          && REASON="executor not LIVE: ${EXECL:-<no line>}"
[ -z "$REASON" ] && [ "$HALTS" -gt 0 ]                    && REASON="$HALTS executor-halt/credential-failure line(s) since boot"
# ORDER-EXECUTION HEALTH (S-2026-07-24, operator: "why are these basic errors not
# picked up... what use is the ML and checks if nothing is done"). The executor can
# be LIVE + un-halted while EVERY order bounces off a Binance filter (-1013 LOT_SIZE:
# qty not rounded to stepSize). Signals fire, 0 fills, nothing trades all night, and
# every check above stays GREEN. This is the missing OUTCOME check: are orders landing.
#   * REJECT STORM: >=3 Binance filter rejects since boot -> orders not filling.
#   * ZERO-FILL: order intents fired but 0 real fills -> effectively shadow despite LIVE.
[ -z "$REASON" ] && [ "$REJECTS" -ge 3 ]                  && REASON="ORDER-REJECT STORM: $REJECTS Binance rejects (LOT_SIZE/filter) since boot — orders bouncing, NOT filling (signals fire, 0 trades)"
[ -z "$REASON" ] && [ "$INTENTS" -ge 3 ] && [ "$FILLS" -eq 0 ] && REASON="ZERO-FILL: $INTENTS order intents but 0 real fills — executor LIVE yet nothing lands (effectively shadow; check reject/filter errors)"

# ── S-2026-07-18af health JSON (the GUI truth chip's feed) — written EVERY run,
#    green or red, at PROBE truth (no strike debounce: the chip shows raw state;
#    the banner keeps the 3-strike grace). ssh-unreachable exits above WITHOUT
#    touching this file, so the chip goes STALE->RED by ts age — a dead probe is
#    NOT-VERIFIED, never silently green. Atomic tmp+mv (relay may scp mid-write).
MODEW=$(printf '%s' "$MODE" | grep -o 'RUNTIME MODE = [A-Z]*' | awk '{print $NF}')
OK=1; [ -n "$REASON" ] && OK=0
RSAFE=$(printf '%s' "$REASON" | tr -d '"\\')
printf '{"ts":%s,"ok":%s,"reason":"%s","build":"%s","headsha":"%s","mode":"%s","act":"%s","uptime_s":%s,"starts30":%s,"halts":%s,"rejects":%s,"intents":%s,"fills":%s}\n' \
  "$now_e" "$OK" "$RSAFE" "$BUILD" "$HEADSHA" "${MODEW:-?}" "$ACT" "$UPTIME_S" "$STARTS30" "$HALTS" "$REJECTS" "$INTENTS" "$FILLS" > "$HEALTH.tmp" \
  && mv "$HEALTH.tmp" "$HEALTH"

# read state: "<phase> <last_notify_epoch>"  phase in ok|fail1|fail2|red
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
  fail1)
    echo "fail2 0" > "$STATE"
    echo "[$(ts)] FAIL-2 (grace): $REASON" >> "$LOG"
    ;;
  fail2|red)
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
