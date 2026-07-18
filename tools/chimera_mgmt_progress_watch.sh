#!/bin/bash
# chimera_mgmt_progress_watch.sh — macOS banner when chimera companion-leg
# MANAGEMENT stops ADVANCING (effect-level), not merely when the process dies.
#
# WHY (incident 2026-07-18, LiveMirrorIncident20260718): after a service restart
# the companion leg management FROZE — state file kept rewriting every 5s
# (fresh heartbeat!) but every armed leg's (peak_mfe_pct, bars_since_high)
# tuple sat unchanged for 2h while price fell through floors; no clips, no
# window flushes, and 6 REAL live-mirror holdings rode unmanaged. Every
# existing watch (executor mode, feed freshness, heartbeats, boot floor gate)
# stayed GREEN — they all checked liveness/config, none checked PROGRESS.
# This watch closes that gap: built != running != WORKING, at the live layer.
#
# Detection (read-only ssh chimera-direct, AUDIT_PROBE_SAFETY: GET/read only):
#   Snapshot armed legs as "tag|peak|bars_since_high" + state ts + live mirror
#   holding count. RED if, versus the previous cron snapshot (10 min apart):
#     * there ARE armed legs, state ts ADVANCED (engine alive & persisting),
#       every armed-leg tuple is IDENTICAL, across 2 consecutive runs
#       (>=20 min > one 15m mgmt bar), or
#     * live mirror holdings exist while chimera.service is not active.
#   Armed legs whose tuples move, or zero armed legs => OK.
# One failed compare = grace (fail1); second consecutive => RED + banner.
# Re-notifies every 6h while RED; single recovery banner on green.
# ssh-unreachable logged, not alarmed (executor-watch pattern).
#
# Self-test: PROBE_OVERRIDE_FILE=<file> substitutes the ssh probe output.
# Cron install: tools/install_chimera_mgmt_progress_watch_cron.sh
# State: /tmp/chimera_mgmt_watch.state  Snap: /tmp/chimera_mgmt_watch.snap
# Log:   /tmp/chimera_mgmt_watch.log

STATE=/tmp/chimera_mgmt_watch.state
SNAP=/tmp/chimera_mgmt_watch.snap
LOG=/tmp/chimera_mgmt_watch.log
RENOTIFY_S=$((6*3600))
now_e=$(date +%s)
ts() { date -u '+%Y-%m-%d %H:%MZ'; }
notify() { /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" 2>/dev/null; }

if [ -n "${PROBE_OVERRIDE_FILE:-}" ]; then
  PROBE=$(cat "$PROBE_OVERRIDE_FILE" 2>/dev/null); SSH_RC=$?
else
  PROBE=$(ssh -o ConnectTimeout=25 -o BatchMode=yes chimera-direct '
    act=$(systemctl is-active chimera 2>/dev/null)
    python3 - <<PYEOF
import json
try:
    d=json.load(open("/home/jo/ChimeraCrypto/data/crypto_companion_state.json"))
    legs=[]
    for L in d.get("legs",[]):
        if L.get("armed") or L.get("sublegs"):
            legs.append("%s|%.6f|%d"%(L.get("tag","?"),L.get("peak_mfe_pct",0.0),L.get("bars_since_high",0)))
    print("TS=%d"%d.get("ts",0)); print("LEGS="+";".join(sorted(legs)))
except Exception as e:
    print("TS=0"); print("LEGS=ERR:%s"%e)
try:
    m=json.load(open("/home/jo/ChimeraCrypto/data/live_mimic_positions.json"))
    print("HOLD=%d"%sum(1 for p in m.get("positions",[]) if p.get("qty",0)>0))
except Exception:
    print("HOLD=0")
PYEOF
    echo "ACT=$act"
  ' 2>/dev/null); SSH_RC=$?
fi

if [ $SSH_RC -ne 0 ] || [ -z "$PROBE" ]; then
  echo "[$(ts)] SSH-UNREACHABLE (no verdict)" >> "$LOG"
  exit 0
fi

TSV=$(printf '%s\n' "$PROBE"  | sed -n 's/^TS=//p')
LEGS=$(printf '%s\n' "$PROBE" | sed -n 's/^LEGS=//p')
HOLD=$(printf '%s\n' "$PROBE" | sed -n 's/^HOLD=//p'); HOLD=${HOLD:-0}
ACT=$(printf '%s\n' "$PROBE"  | sed -n 's/^ACT=//p')

read -r p_ts p_legs 2>/dev/null < "$SNAP" || true
printf '%s %s\n' "${TSV:-0}" "${LEGS:-}" > "$SNAP"

REASON=""
if [ "$HOLD" -gt 0 ] && [ "$ACT" != "active" ]; then
  REASON="$HOLD live mirror holding(s) while chimera.service=$ACT"
elif [ -n "$LEGS" ] && [ "$LEGS" = "${p_legs:-}" ] && [ "${TSV:-0}" -gt "${p_ts:-0}" ] 2>/dev/null; then
  n=$(printf '%s' "$LEGS" | awk -F';' '{print NF}')
  REASON="mgmt FROZEN: $n armed leg(s) unchanged >=10min while engine persists (hold=$HOLD)"
fi

read -r phase last_n 2>/dev/null < "$STATE" || true
phase=${phase:-ok}; last_n=${last_n:-0}

if [ -z "$REASON" ]; then
  if [ "$phase" = "red" ]; then
    notify "CHIMERA MGMT RECOVERED" "companion leg management advancing again."
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
      notify "CHIMERA MGMT FROZEN" "$REASON — floors/trails NOT being evaluated. Check josgp1 NOW."
      echo "red $now_e" > "$STATE"
      echo "[$(ts)] RED+NOTIFY: $REASON" >> "$LOG"
    else
      echo "[$(ts)] RED (renotify suppressed): $REASON" >> "$LOG"
    fi
    ;;
esac
exit 0
